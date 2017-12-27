/*
 * This program handles button events coming from 2 sources:
 * + GPIO - button is connected to GPIO pin
 * + radio - button is reported by radio433daemon via network socket
 *
 * When button (either local or remote) is pressed or released, script
 * is launched (asynchronously) with special arguments provided
 * that describe event. Asynchronous call mean that there is no wait
 * for one script instance to finish before launching another copy
 * (multiple copies can run simultaneously). No event can clog
 * the button action queue.
 *
 * Arguments (argv[]) for handler script:
 *	$1 = "radio" | "gpio"
 *	$2 = "pressed" | "released"
 *	$3 = label
 *	$4 = seconds since last state change
 *
 *	$5 = (GPIO only) pin number in BCM GPIO scheme
 *	$6 = (GPIO only) raw pin value (0 or 1)
 *
 *	$5 = (radio only) 64-bit uppercase hex code with "0x" prefix
 *	$6 = (radio only) message type as defined in radio433_types.h
 *	$7 = (radio only) valid bits (code length)
 *
 * Program design:
 *
 *  State flow for GPIO button:
 *  GPIO inactive: QSTATUS_IDLE
 *  ISR detects activation: QSTATUS_IDLE -> QSTATUS_NEW, timestamp
 *  debounce period passed: QSTATUS_NEW -> QSTATUS_BUSY, send event, poll for release
 *  button released: QSTATUS_BUSY -> QSTATUS_IDLE, send event
 *
 *  State flow for remote (radio) button:
 *  No signal: QSTATUS_IDLE
 *  First code received: QSTATUS_IDLE -> QSTATUS_NEW, timestamp
 *  codes received within TTL: QSTATUS_NEW -> QSTATUS_BUSY, send event
 *  ttl passed without valid code: QSTATUS_BUSY -> QSTATUS_IDLE, send event
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sched.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <fcntl.h>
#include <linux/limits.h>  /* for NGROUPS_MAX */
#include <pwd.h>	/* for getpwnam_r() */
#include <grp.h>	/* for getgrouplist() and setgroups() */

#include <wiringPi.h>

#include "radio433_dev.h"

/* *************** */
/* *  Constants  * */
/* *************** */

#define BANNER			"ButtonHandler v0.9"
#define MAX_USERNAME		32
#define MAX_NGROUPS		(NGROUPS_MAX >> 10)	/* reasonable maximum */
#define GPIO_PINS		28	/* number of Pi GPIO pins */
#define RADIO_PORT		5433	/* default radio433daemon TCP port */
#define RADMSG_SIZE		128	/* radio daemon message size */
#define RADBUF_SIZE		(RADMSG_SIZE * 8)
#define RADMSG_HDR		"<RX>"
#define RADMSG_EOT		"<ZZ>"
#define RECONNECT_DELAY_SEC	15
#define FILE_UMASK		(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)
#define PID_DIR			"/var/run/"
#define LOG_DEBUG		"debug"
#define LOG_INFO		"info"
#define LOG_NOTICE		"notice"
#define LOG_WARN		"warn"
#define LOG_ERROR		"error"
#define POLL_INTERVAL_MS	50	/* main loop poll interval (ms), (>debounce) */
#define GPIOBTN_DEBNC_MS	20	/* debounce interval */
#define RADBTN_CODES_NUM	2	/* wait for these number of codes to accept keypress */
#define RADBTN_ENTRY_TTL	4	/* TTL: in codelen (ms) periods */
#define MAX_RADIO_CODES		32	/* max number of recognized radio codes */
#define EVENT_QUEUE_LEN		(MAX_RADIO_CODES + GPIO_PINS)	/* event queue length */
#define BUTTON_LABEL		15
#define QSTATUS_EMPTY		0	/* spec queue: unused entry */
#define QSTATUS_IDLE		1	/* spec queue: entry is OK and idle */
#define QSTATUS_NEW		2	/* spec queue: first keypress recorded */
#define QSTATUS_BUSY		3	/* spec queue: entry after 'pressed' before 'release' */
#define EVQ_TYPE_EMPTY		0
#define EVQ_TYPE_GPIO		1
#define EVQ_TYPE_RADIO		2
#define EVQ_ACTION_PRESSED	0
#define EVQ_ACTION_RELEASED	1

#define TSDIFF(s0, m0, s1, m1)	(((s0) - (s1)) * 1000 + (m0) - (m1))
#define TSDIFFRS(s0, m0, s1, m1) ((s0) - (s1) + ((m0) - (m1) + 500) / 1000)
#define MIN(x, y)		((x) < (y) ? (x) : (y))
#define MAX(x, y)		((x) > (y) ? (x) : (y))
#define ISR_NAME(PIN)	isrGpio_ ## PIN
#define ISR_FUNC(PIN)	\
static void ISR_NAME(PIN) (void)					\
{									\
	struct gpiodentry *gd;						\
	struct timeval ts;						\
									\
	gettimeofday(&ts, NULL);					\
	gd = &gpiodesc[PIN];						\
	sem_wait(&gd->locksem);						\
	if (gd->status == QSTATUS_IDLE) {				\
		/* new keypress, may be followed by button bouncing */	\
		gd->status = QSTATUS_NEW;				\
		gd->tsec = ts.tv_sec;					\
		gd->tmsec = ts.tv_usec / 1000;				\
		/* wake up poller */					\
		sem_post(&pollsem);					\
	}								\
	sem_post(&gd->locksem);						\
}

/* ********************** */
/* *  Global variables  * */
/* ********************** */

extern char *optarg;
extern int optind, opterr, optopt;
pid_t procpid;
char progname[PATH_MAX + 1], logfname[PATH_MAX + 1], pidfname[PATH_MAX + 1];
sem_t logsem;			/* log file semaphore */
volatile int logfd, radfd;	/* files and sockets */
volatile int radflag, debugflag, netclrun, gpiodlen, raddlen;
pthread_t netclthread;
struct sockaddr_in radsin;
sem_t pollsem;	/* polling loop control */
char *eqtc[3] = { "(null)", "gpio", "radio" };
char *eqac[2] = { "pressed", "released" };

struct raddentry {
	int status;
	time_t tsec, tsecp;
	int tmsec, tmsecp;
	char label[BUTTON_LABEL + 1];	/* static once initialized */
	int codelen, repeats, interval;
	int type, bits;
	unsigned long long code;
	int nrcod, ttl;
	sem_t locksem;
} raddesc[MAX_RADIO_CODES];

struct gpiodentry {
	int status;
	time_t tsec, tsecp;
	int tmsec, tmsecp;
	int actvhigh;
	char label[BUTTON_LABEL + 1];	/* static once intialized */
	void (*isrfunc)(void);
	sem_t locksem;
} gpiodesc[GPIO_PINS];

struct eventqentry {
	int type, action;
	char *label;	/* points to label in gpio or radio struct */
	unsigned int lastchg;	/* in seconds */
	union {
		struct {	/* for GPIO button */
			int gpiopin, gpioval;
		};
		struct {	/* for radio button */
			int codetype, codebits;
			unsigned long long code;
		};
	};
} eventq[EVENT_QUEUE_LEN];

/* We must build below function table for every GPIO pin because ISR
 * routine does not know for which pin it had been fired. Therefore
 * parametrized handler is constructed that has pin number preserved.
 */

struct isrftentry {
	void (*isrfn)(void);
};

/* GPIO_PINS */
ISR_FUNC(0);
ISR_FUNC(1);
ISR_FUNC(2);
ISR_FUNC(3);
ISR_FUNC(4);
ISR_FUNC(5);
ISR_FUNC(6);
ISR_FUNC(7);
ISR_FUNC(8);
ISR_FUNC(9);
ISR_FUNC(10);
ISR_FUNC(11);
ISR_FUNC(12);
ISR_FUNC(13);
ISR_FUNC(14);
ISR_FUNC(15);
ISR_FUNC(16);
ISR_FUNC(17);
ISR_FUNC(18);
ISR_FUNC(19);
ISR_FUNC(20);
ISR_FUNC(21);
ISR_FUNC(22);
ISR_FUNC(23);
ISR_FUNC(24);
ISR_FUNC(25);
ISR_FUNC(26);
ISR_FUNC(27);

struct isrftentry isrtable[] = {
	ISR_NAME(0),
	ISR_NAME(1),
	ISR_NAME(2),
	ISR_NAME(3),
	ISR_NAME(4),
	ISR_NAME(5),
	ISR_NAME(6),
	ISR_NAME(7),
	ISR_NAME(8),
	ISR_NAME(9),
	ISR_NAME(10),
	ISR_NAME(11),
	ISR_NAME(12),
	ISR_NAME(13),
	ISR_NAME(14),
	ISR_NAME(15),
	ISR_NAME(16),
	ISR_NAME(17),
	ISR_NAME(18),
	ISR_NAME(19),
	ISR_NAME(20),
	ISR_NAME(21),
	ISR_NAME(22),
	ISR_NAME(23),
	ISR_NAME(24),
	ISR_NAME(25),
	ISR_NAME(26),
	ISR_NAME(27)
};

/* *************** */
/* *  Functions  * */
/* *************** */

/* Show help */
void help(void)
{
	printf("Usage:\n\t%s [-d | -l logfile] [-u user] [-P pidfile] [-r serverip [-p tcpport] -c codestr] [-g gpiostr] script\n\n", progname);
	puts("Where:");
	puts("\t-d          - debug mode, stay foreground and show activity (optional)");
	puts("\t-l logfile  - path to log file (optional, default is none)");
	puts("\t-u user     - name of the user to switch to (optional)");
	printf("\t-P pidfile  - path to PID file (optional, default is %s%s.pid)\n", PID_DIR, progname);
	puts("\t-r server   - IPv4 address of radio server (optional)");
	printf("\t-p tcpport  - TCP port of radio server (optional, default is %d)\n", RADIO_PORT);
	puts("\t-c codestr  - radio codes definition string (optional, see below)");
	puts("\t-g gpiostr  - GPIO buttons definition string (optional, see below)");
	puts("\tscript      - full path to program called for button events\n");
	puts("\tgpiostr     - list of comma-separated triplets: gpio_bcm_pin:active_high:text_label");
	printf("\t              that describe GPIO-connected buttons; text label is max %d chars,\n", BUTTON_LABEL);
	printf("\t              active_high is 0 or 1, up to %d GPIOs are supported\n\n", GPIO_PINS);
	puts("\tcodestr     - list of comma-separated triplets: code_hex:device_type_hex:text_label");
	printf("\t              that describe remote radio buttons; text label is max %d chars,\n", BUTTON_LABEL);
	printf("\t              up to %d different codes are supported\n", MAX_RADIO_CODES);
	puts("\nSupported radio device types:");
	puts("\t0x0101      - remote for radio-controlled power outlets (433.92 MHz, i.e. Kemot URZ series)");
	puts("\t0x0201      - remote temperature/humidity sensor (433.92 MHz, Hyundai WS Senzor 77TH)");
	puts("\nSignal actions: SIGHUP (log file truncate and reopen)\n");
}

/* Log line header */
void logprintf(int fd, const char *level, const char *fmt, ...)
{
        struct timeval ts;
        struct tm *tl;
	va_list ap;

	if (fd < 0)
		return;
	sem_wait(&logsem);
	gettimeofday(&ts, NULL);
	tl = localtime(&ts.tv_sec);
	dprintf(fd, "%d-%02d-%02d %02d:%02d:%02d.%03u %s[%d] %s: ",
		1900 + tl->tm_year, tl->tm_mon + 1, tl->tm_mday,
		tl->tm_hour, tl->tm_min, tl->tm_sec, ts.tv_usec / 1000,
		progname, procpid, level);
	va_start(ap, fmt);
	vdprintf(fd, fmt, ap);
	va_end(ap);
	sem_post(&logsem);
}

/* change ownership of log and pid files */
void chownFiles(uid_t uid, gid_t gid)
{
	if (!debugflag && logfd > 0)
		fchown(logfd, uid, gid);
	if (pidfname[0])
		chown(pidfname, uid, gid);
}

/* drop super-user privileges (euid/egid only, handler script!) */
int dropRootPriv(const char *username, uid_t *uid, gid_t *gid)
{
	struct passwd pw, *pwptr;
	char buf[1024];
	gid_t grps[MAX_NGROUPS];
	int ngrp;

	/* get user & group ID */
	errno = 0;
	pwptr = NULL;
	if (getpwnam_r(username, &pw, buf, 1024, &pwptr))
		return -1;
	if (pwptr == NULL) {
		errno = EUSERS;
		return -1;
	}
	/* chown files prior to dropping root privileges */
	chownFiles(pw.pw_uid, pw.pw_gid);
	/* set supplemental groups */
	ngrp = MAX_NGROUPS;
	if (getgrouplist(username, pw.pw_gid, grps, &ngrp) < 0)
		return -1;
	if (setgroups(ngrp, grps))
		return -1;
	/* start with GID then UID */
	if (setegid(pw.pw_gid))
		return -1;
	if (seteuid(pw.pw_uid))
		return -1;
	*uid = pw.pw_uid;
	*gid = pw.pw_gid;
	return 0;
}

/* change sheduling priority */
int changeSched(void)
{
	struct sched_param s;

	s.sched_priority = 0;
	if(sched_setscheduler(0, SCHED_BATCH, &s))
		return -1;
	return 0;
}

/* run handler script */
int callHandler(char *hprog, struct eventqentry *eq)
{
	int pid;
	char *s_argv[9];
	char sa_age[12];
	char sa_gpiopin[6], sa_gpioval[2] = { '\0', '\0' };
	char sa_codetype[8], sa_codebits[6], sa_code[12];

	pid = fork();
	if (pid == -1)
		return -1;
	else if (!pid) {
		/* child created, execute script */
		s_argv[0] = hprog;	/* $0 - program name */
		s_argv[1] = eqtc[eq->type];
		s_argv[2] = eqac[eq->action];
		s_argv[3] = eq->label;
		snprintf(sa_age, 12, "%d", eq->lastchg);
		s_argv[4] = sa_age;
		if (eq->type == EVQ_TYPE_GPIO) {
			snprintf(sa_gpiopin, 6, "%d", eq->gpiopin);
			s_argv[5] = sa_gpiopin;
			sa_gpioval[0] = '0' + eq->gpioval;
			s_argv[6] = sa_gpioval;
			s_argv[7] = (char*)NULL;
		} else if (eq->type == EVQ_TYPE_RADIO) {
			snprintf(sa_code, 12, "0x%0llX", eq->code);
			s_argv[5] = sa_code;
			snprintf(sa_codetype, 8, "0x%0X", eq->codetype);
			s_argv[6] = sa_codetype;
			snprintf(sa_codebits, 6, "%d", eq->codebits);
			s_argv[7] = sa_codebits;
		} else
			exit(EXIT_FAILURE);	/* child - terminate on error */
		s_argv[8] = (char*)NULL;
		if (execv(hprog, s_argv))
			exit(EXIT_FAILURE);	/* child - terminate on error */
	} else
		/* parent */
		return 0;
}

/* Connect/reconnect to radio daemon */
/* (this function loops indefinitely until connection is created
 *  and end process when no sockets are available)
 */
int connectRadioSrv(struct sockaddr_in *s)
{
	int fd;

	for(;;) {
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd == -1)
			break;	/* no socket available, system error */

		if (connect(fd, (struct sockaddr *)s, sizeof(*s)) == -1) {
			logprintf(logfd, LOG_WARN,
				  "connection broken with radio server %s port %d: %s\n",
				  inet_ntoa(s->sin_addr), ntohs(s->sin_port),
				  strerror(errno));
			logprintf(logfd, LOG_WARN, "retrying in %d seconds...\n",
				  RECONNECT_DELAY_SEC);
			close(fd);
			sleep(RECONNECT_DELAY_SEC);
		} else {
			logprintf(logfd, LOG_NOTICE,
				  "connected successfully to radio server %s port %d\n",
				  inet_ntoa(s->sin_addr), ntohs(s->sin_port));
			break;
		}
	}

	return fd;
}

/* Daemonize process */
int daemonize(void)
{
	pid_t pid, sid;

	/* fork then parent exits */
	pid = fork();
	if (pid < 0)
		return 1;
	if (pid > 0)
		exit(EXIT_SUCCESS);

	/* set default umask, will be overriden by open anyway */
	umask(0);

	/* create new session ID */
	sid = setsid();
	if (sid < 0)
		return 2;

	/* change current working dir to root */
	if (chdir("/") < 0)
		return 3;

	/* close standard file descriptors */
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	return 0;
}

/* Process shutdown */
void endProcess(int status)
{
	/* remove pid file */
	unlink(pidfname);

	/* stop networking thread (if any) and close socket */
	if (netclrun)
		pthread_cancel(netclthread);
	if (radfd >= 0)
		close(radfd);

	/* close logfile */
	if (logfd >= 0) {
		logprintf(logfd, LOG_NOTICE, "server shut down with code %d\n",
			  status);
		close(logfd);
	}

	/* exit process */
	exit(status);
}

/* Intercept TERM and INT signals */
void signalQuit(int sig)
{
	logprintf(logfd, LOG_NOTICE, "signal %d received\n", sig);
	endProcess(0);
}

/* SIGHUP - reopen log file, useful for logrotate */
void signalReopenLog(int sig)
{
	if (!debugflag && logfd >= 0) {
		logprintf(logfd, LOG_NOTICE, "signal %d received\n", sig);
		logprintf(logfd, LOG_NOTICE, "closing log file\n");
		fsync(logfd);	/* never hurts */
		close(logfd);
		logfd = open(logfname, O_CREAT | O_TRUNC | O_SYNC | O_WRONLY, FILE_UMASK);
		if (logfd == -1)
			endProcess(EXIT_FAILURE);
		logprintf(logfd, LOG_NOTICE, "log file flushed after receiving signal %d\n", sig);
	}
}

/* ************* */
/* *  Threads  * */
/* ************* */

void *radioDaemonThread(void *arg)
{
	char radbuf[RADBUF_SIZE + 1];
	char *msgptr, *msgend;
	int i, msglen;
	sigset_t blkset;
	unsigned long tss;
	int tsms;
	int codelen, repeats, interval;
	int type, bits;
	unsigned long long code;
	struct raddentry *rd;
	struct timeval ts;

	sigfillset(&blkset);
	pthread_sigmask(SIG_BLOCK, &blkset, NULL);

	/* function loop - never ends, send signal to exit */
	for(;;) {
		do
			msglen = recv(radfd, radbuf, RADBUF_SIZE, 0);
		while (msglen == -1 && errno == EINTR);
		if (msglen <= 0) {
			close(radfd);
			radfd = connectRadioSrv(&radsin);
			continue;
		}
		radbuf[msglen] = 0;
		msgptr = radbuf;
		do {
			msgptr = strstr(msgptr, RADMSG_HDR);
			if (msgptr == NULL)
				break;
			msgend = strstr(msgptr, RADMSG_EOT);
			if (msgend == NULL)
				break;
			if (sscanf(msgptr + 4, "%lu.%u;%d;%d;%d;0x%X;%d;0x%llX;",
			    &tss, &tsms, &codelen, &repeats, &interval, &type,
			    &bits, &code) == 8)
				for (i = 0; i < raddlen; i++)
					if (raddesc[i].status && \
					    raddesc[i].type == type && \
					    raddesc[i].code == code) {
						rd = &raddesc[i];
						sem_wait(&rd->locksem);
						if (rd->status == QSTATUS_IDLE) {
							gettimeofday(&ts, NULL);
							rd->status = QSTATUS_NEW;
							rd->codelen = codelen;
							rd->repeats = MAX(repeats, RADBTN_CODES_NUM);
							rd->interval = interval;
							rd->bits = bits;
							rd->nrcod = 0;
							rd->tsec = ts.tv_sec;
							rd->tmsec = ts.tv_usec / 1000;
						}
						rd->nrcod++;
						/* ttl window is twice tx period */
						rd->ttl = (rd->repeats * rd->codelen) << 1;
						sem_post(&rd->locksem);
						sem_post(&pollsem);
						break;
					}
			msgptr = msgend + 5;
		} while (msgptr < radbuf + msglen);
	}	/* thread main loop ends here */
}

/* ************ */
/* ************ */
/* **  MAIN  ** */
/* ************ */
/* ************ */

int main(int argc, char *argv[])
{
	int i;
	int opt;
	int radport;
	int pidfd;
	struct sigaction sa;
	char **cptr, *coptr, plbl[BUTTON_LABEL + 1];
	int pgpio, pactv, pcode, ptype;
	char username[MAX_USERNAME + 1];
	int gval;
	struct gpiodentry *gd;
	struct raddentry *rd;
	struct eventqentry *eq;
	struct timeval ts, tsl;
	unsigned int tsldiff, looptim;
	int tsms, workcnt, eqi;
	uid_t uid;
	gid_t gid;
	char hscript[PATH_MAX + 1];

	/* get process name */
	strncpy(progname, basename(argv[0]), PATH_MAX);

	/* show help */
	if (argc < 2) {
		help();
		exit(0);
	}

	/* set PID initially so logprintf() can use it, */
	/* the value will change after daemon forking */
	procpid = getpid();

	/* initialize other values */
	logfd = -1;
	radfd = -1;
	netclrun = 0;
	radflag = 0;
	gpiodlen = 0;
	raddlen = 0;
	gettimeofday(&ts, NULL);
	tsms = ts.tv_usec / 1000;

	/* get parameters */
	debugflag = 0;
	memset(username, 0, MAX_USERNAME + 1);
	memset((char *)&radsin, 0, sizeof(radsin));
	radsin.sin_family = AF_INET;
	radport = 0;
	memset(logfname, 0, PATH_MAX + 1);
	memset(pidfname, 0, PATH_MAX + 1);
	strcpy(pidfname, PID_DIR);
	strcat(pidfname, progname);
	strcat(pidfname, ".pid");
	memset((char *)raddesc, 0, sizeof(raddesc));
	memset((char *)gpiodesc, 0, sizeof(gpiodesc));

	while((opt = getopt(argc, argv, "dl:u:P:c:r:p:g:")) != -1) {
		if (opt == 'd')
			debugflag = 1;
		else if (opt == 'l')
			strncpy(logfname, optarg, PATH_MAX);
		else if (opt == 'u')
			strncpy(username, optarg, MAX_USERNAME);
		else if (opt == 'P')
			strncpy(pidfname, optarg, PATH_MAX);
		else if (opt == 'r') {
			if (!inet_aton(optarg, &radsin.sin_addr)) {
				dprintf(STDERR_FILENO, "Invalid IPv4 address specification.\n");
				exit(EXIT_FAILURE);
			}
			if (!radport)
				radport = RADIO_PORT;
			radflag = 1;
		}
		else if (opt == 'p')
			sscanf(optarg, "%d", &radport);
		else if (opt == 'g') {
			cptr = &optarg;
			while (*cptr != NULL) {
				coptr = strsep(cptr, ",");
				memset(plbl, 0, BUTTON_LABEL + 1);
				if (sscanf(coptr, "%d:%d:%15s", &pgpio, &pactv, plbl) == 3) /* BUTTON_LABEL */
					if (pgpio >= 0 && pgpio < GPIO_PINS) {
						gpiodesc[pgpio].status = QSTATUS_IDLE;
						gpiodesc[pgpio].actvhigh = pactv ? 1 : 0;
						gpiodesc[pgpio].tsecp = ts.tv_sec;
						gpiodesc[pgpio].tmsecp = tsms;
						strncpy(gpiodesc[pgpio].label, plbl,
							BUTTON_LABEL + 1);
						gpiodesc[pgpio].isrfunc = isrtable[pgpio].isrfn;
						sem_init(&gpiodesc[pgpio].locksem, 0, 1);
						gpiodlen++;
					}
			}
		}
		else if (opt == 'c') {
			cptr = &optarg;
			while (*cptr != NULL) {
				coptr = strsep(cptr, ",");
				memset(plbl, 0, BUTTON_LABEL + 1);
				if (sscanf(coptr, "0x%x:0x%x:%15s", &pcode, &ptype, plbl) == 3) /* BUTTON_LABEL */
					if (raddlen < MAX_RADIO_CODES) {
						raddesc[raddlen].status = QSTATUS_IDLE;
						raddesc[raddlen].type = ptype;
						raddesc[raddlen].code = pcode;
						raddesc[raddlen].tsecp = ts.tv_sec;
						raddesc[raddlen].tmsecp = tsms;
						strncpy(raddesc[raddlen].label, plbl,
							BUTTON_LABEL + 1);
						sem_init(&raddesc[raddlen].locksem, 0, 1);
						raddlen++;	
					}
				}
		}
		else if (opt == '?' || opt == 'h') {
			help();
			exit(EXIT_FAILURE);
		}
	}

	/* parameter logic */
	if (optind == argc) {
		help();
		exit(0);
	}
	if (raddlen && !radflag)
		raddlen = 0;
	if (getuid()) {
		dprintf(STDERR_FILENO, "Must be run by root.\n");
		exit(EXIT_FAILURE);
        }
	if (debugflag && logfname[0]) {
		dprintf(STDERR_FILENO, "Flags -d and -l are mutually exclusive.\n");
		exit(EXIT_FAILURE);
	}
	if (!radflag && radport) {
		dprintf(STDERR_FILENO, "Flag -p requires -r.\n");
		exit(EXIT_FAILURE);
	}
	if (!raddlen && !gpiodlen) {
		dprintf(STDERR_FILENO, "No event source selected, specify at least one (radio daemon, GPIO or both).\n");
		exit(EXIT_FAILURE);
	}

	/* check if script is present/runnable */
	if (eaccess(argv[optind], F_OK | X_OK)) {
		dprintf(STDERR_FILENO, "Unable to access event handler script %s: %s\n",
			argv[optind], strerror(errno));
		exit(EXIT_FAILURE);
	} else
		strncpy(hscript, argv[optind], PATH_MAX + 1);

	radsin.sin_port = htons(radport);

	/* initialize log */
	if (debugflag)
		logfd = STDOUT_FILENO;
	else {
		if (logfname[0]) {
			logfd = open(logfname, O_CREAT | O_APPEND | O_SYNC | O_WRONLY, FILE_UMASK);
			if (logfd == -1) {
				dprintf(STDERR_FILENO, "Unable to open log file '%s': %s\n",
					logfname, strerror(errno));
				exit(EXIT_FAILURE);
			}
		} else
			logfd = -1;
	}
	sem_init(&logsem, 0, 1);

	/* put banner in log */
#ifdef BUILDSTAMP
	logprintf(logfd, LOG_NOTICE, "starting %s build %s\n", BANNER, BUILDSTAMP);
#else
	logprintf(logfd, LOG_NOTICE, "starting %s\n", BANNER);
#endif

	/* check if pid file exists */
	pidfd = open(pidfname, O_PATH, FILE_UMASK);
	if (pidfd >= 0) {
		/* pid file exists */
		close(pidfd);
		dprintf(STDERR_FILENO, "PID file (%s) exists, daemon already running or stale file detected.\n",
			pidfname);
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to create PID file %s\n",
				  pidfname);
		close(logfd);
		exit(EXIT_FAILURE);
	}

	/* setup process */
	if (!debugflag)
		if (daemonize()) {
			dprintf(STDERR_FILENO, "Unable to fork to background: %s\n",
				strerror(errno));
			logprintf(logfd, LOG_ERROR, "unable to fork to background: %s\n",
				  strerror(errno));
			close(logfd);
			exit(EXIT_FAILURE);
		} else {
			procpid = getpid();
			logprintf(logfd, LOG_NOTICE, "forking to background with PID %d\n",
				  procpid);
		}

	/* populate pid file (new PID after daemonize()) */
	pidfd = open(pidfname, O_CREAT | O_WRONLY, FILE_UMASK);
	if (pidfd < 0) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to create PID file %s: %s\n",
				  pidfname, strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to create PID file %s: %s\n",
				pidfname, strerror(errno));
		endProcess(EXIT_FAILURE);
	}
	dprintf(pidfd, "%d\n", procpid);
	close(pidfd);

	/* signal handler */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGCHLD, &sa, NULL);	/* no zombies */
	sa.sa_handler = &signalQuit;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sa.sa_handler = &signalReopenLog;
	sigaction(SIGHUP, &sa, NULL);

	/* main poller semaphore set to sleep initially */
	sem_init(&pollsem, 0, 0);

	/* start network thread (optional) */
	if (raddlen) {
		if (pthread_create(&netclthread, NULL, radioDaemonThread, NULL)) {
			if (!debugflag)
				logprintf(logfd, LOG_ERROR, "cannot start network listener thread\n");
			else
				dprintf(STDERR_FILENO, "Cannot start network listener thread.\n");
			endProcess(EXIT_FAILURE);
		}
		for(i = 0; i < raddlen; i++)
			logprintf(logfd, LOG_NOTICE, "remote code 0x%llX, type 0x%X, label \"%s\" monitored\n",
				  raddesc[i].code, raddesc[i].type, raddesc[i].label);
		netclrun = 1;
	}

	/* set up ISRs for GPIO (optional) */
	if (gpiodlen) {
		/* initialize WiringPi library, use BCM GPIO numbers - must be root */
	        wiringPiSetupGpio();
		for (i = 0; i < GPIO_PINS; i++)
			if (gpiodesc[i].status && gpiodesc[i].isrfunc != NULL) {
				pinMode(i, INPUT);
				wiringPiISR(i, gpiodesc[i].actvhigh ? INT_EDGE_RISING : INT_EDGE_FALLING,
					    gpiodesc[i].isrfunc);
				logprintf(logfd, LOG_NOTICE,
					  "button connected to GPIO pin %d, active %s, label \"%s\" monitored\n",
					  i, gpiodesc[i].actvhigh ? "high" : "low",
					  gpiodesc[i].label);
			}
	}

	logprintf(logfd, LOG_NOTICE, "total %d local buttons and %d remote codes monitored\n",
		  gpiodlen, raddlen);

	/* change scheduling priority */
	if (changeSched()) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to change process scheduling priority: %s\n",
				  strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to change process scheduling priority: %s\n",
				strerror (errno));
		endProcess(EXIT_FAILURE);
	}

       /* drop privileges */
	uid = 0;
	gid = 0;
	if (username[0]) {
		if (dropRootPriv(username, &uid, &gid)) {
			if (!debugflag)
				logprintf(logfd, LOG_ERROR, "unable to drop super-user privileges: %s\n",
					  strerror (errno));
			else
				dprintf(STDERR_FILENO, "Unable to drop super-user privileges: %s\n",
					strerror (errno));
			endProcess(EXIT_FAILURE);
		} else
			logprintf(logfd, LOG_NOTICE,
				  "dropping super-user privileges, running as UID=%ld GID=%ld\n",
				  uid, gid);
	}

	/* Main Event Thread */

	looptim = 0;
	for(;;) {
		sem_wait(&pollsem);
		gettimeofday(&ts, NULL);
		tsms = ts.tv_usec / 1000;
		workcnt = 0;
		eqi = 0;

		/* check GPIO */
		if (gpiodlen)
			for(i = 0; i < GPIO_PINS; i++) {
				if (!gpiodesc[i].status)
					continue;
				gd = &gpiodesc[i];
				sem_wait(&gd->locksem);
				if (gd->status == QSTATUS_NEW) {
					if (TSDIFF(ts.tv_sec, tsms, gd->tsec, gd->tmsec) > GPIOBTN_DEBNC_MS) {
						/* debounce period passed, state should be stable now */
						gval = digitalRead(i);
						if (gval == gd->actvhigh) {
							/* button pressed */
							eq = &eventq[eqi];
							eq->type = EVQ_TYPE_GPIO;
							eq->action = EVQ_ACTION_PRESSED;
							eq->label = gd->label; /* label does not change */
							eq->lastchg = TSDIFFRS(gd->tsec, gd->tmsec, gd->tsecp, gd->tmsecp);
							eq->gpiopin = i;
							eq->gpioval = gval;
							eqi = (eqi + 1) % EVENT_QUEUE_LEN;
							gd->status = QSTATUS_BUSY;
							gd->tsecp = gd->tsec;
							gd->tmsecp = gd->tmsec;
							workcnt++;
							logprintf(logfd, LOG_INFO,
								  "GPIO button \"%s\" (pin %d) pressed after %d seconds\n",
								  eq->label, eq->gpiopin, eq->lastchg);
						} else {
							/* noise, reset */
							gd->status = QSTATUS_IDLE;
							gd->tsec = gd->tsecp;
							gd->tmsec = gd->tmsecp;
						}
					} else
						workcnt++;
				} else if (gd->status == QSTATUS_BUSY) {
					gval = digitalRead(i);
					if (gval != gd->actvhigh) {
						/* button released */
						eq = &eventq[eqi];
						eq->type = EVQ_TYPE_GPIO;
						eq->action = EVQ_ACTION_RELEASED;
						eq->label = gd->label;	/* label does not change */
						eq->lastchg = TSDIFFRS(ts.tv_sec, tsms, gd->tsec, gd->tmsec);
						eq->gpiopin = i;
						eq->gpioval = gval;
						eqi = (eqi + 1) % EVENT_QUEUE_LEN;
						gd->status = QSTATUS_IDLE;
						gd->tsec = ts.tv_sec;
						gd->tmsec = tsms;
						logprintf(logfd, LOG_INFO,
							  "GPIO button \"%s\" (pin %d) released after %d seconds\n",
							  eq->label, eq->gpiopin, eq->lastchg);
					} else
						workcnt++;
				}
				sem_post(&gd->locksem);
			}	/* GPIO: for loop */

		/* check radio */
		if (raddlen)
			for(i = 0; i < raddlen; i++) {
				rd = &raddesc[i];
				sem_wait(&rd->locksem);
				if (rd->status == QSTATUS_NEW) {
					rd->ttl -= looptim;
					if (rd->ttl >= 0 && rd->nrcod >= rd->repeats) {
						/* button pressed */
						eq = &eventq[eqi];
						eq->type = EVQ_TYPE_RADIO;
						eq->action = EVQ_ACTION_PRESSED;
						eq->label = rd->label; /* label does not change */
						eq->lastchg = TSDIFFRS(rd->tsec, rd->tmsec, rd->tsecp, rd->tmsecp);
						eq->codetype = rd->type;
						eq->codebits = rd->bits;
						eq->code = rd->code;
						eqi = (eqi + 1) % EVENT_QUEUE_LEN;
						rd->status = QSTATUS_BUSY;
						rd->tsecp = rd->tsec;
						rd->tmsecp = rd->tmsec;
						workcnt++;
						logprintf(logfd, LOG_INFO,
							  "radio button \"%s\" (code 0x%llX) pressed after %d seconds\n",
							  eq->label, eq->code, eq->lastchg);
					} else if (rd->ttl < 0) {
						/* noise, reset */
						rd->status = QSTATUS_IDLE;
						rd->tsec = rd->tsecp;
						rd->tmsec = rd->tmsecp;
					} else
						workcnt++;
				} else if (rd->status == QSTATUS_BUSY) {
					rd->ttl -= looptim;
					if (rd->ttl < 0) {
						/* button released */
						eq = &eventq[eqi];
						eq->type = EVQ_TYPE_RADIO;
						eq->action = EVQ_ACTION_RELEASED;
						eq->label = rd->label; /* label does not change */
						eq->lastchg = TSDIFFRS(ts.tv_sec, tsms, rd->tsec, rd->tmsec);
						eq->codetype = rd->type;
						eq->codebits = rd->bits;
						eq->code = rd->code;
						eqi = (eqi + 1) % EVENT_QUEUE_LEN;
						rd->status = QSTATUS_IDLE;
						rd->tsec = ts.tv_sec;
						rd->tmsec = tsms;
						logprintf(logfd, LOG_INFO,
							  "radio button \"%s\" (code 0x%llX) released after %d seconds\n",
							  eq->label, eq->code, eq->lastchg);
					} else
						workcnt++;
				}
				sem_post(&rd->locksem);
			}	/* RADIO: for loop */
	
		/* process events */
		if (eqi) {
			/* escalate privileges */
			if (setegid(0))
				logprintf(logfd, LOG_ERROR,
					  "unable to set root group privilege, handler script will not be called\n");
			else if (seteuid(0))
				logprintf(logfd, LOG_ERROR,
					  "unable to set root user privilege, handler script will not be called\n");
			else {
				for (i = 0; i < eqi; i++)
					if (callHandler(hscript, &eventq[i]))
						logprintf(logfd, LOG_ERROR, "unable to execute handler script\n");
				/* drop privileges */
				if (setegid(gid))
					logprintf(logfd, LOG_WARN,
						  "unable to drop root group privilege, process runs with GID=0\n");
				else if (seteuid(uid))
					logprintf(logfd, LOG_WARN,
						  "unable to drop root user privilege, process runs with UID=0\n");
			}
		}

		/* keep loop running (unlock semaphore), there are tasks to do */
		if (workcnt) {
			sem_post(&pollsem);
			gettimeofday(&tsl, NULL);
			tsldiff = TSDIFF(tsl.tv_sec, tsl.tv_usec / 1000, ts.tv_sec, tsms);
			looptim = MAX(tsldiff, POLL_INTERVAL_MS);
			if (tsldiff < POLL_INTERVAL_MS)
				delay(POLL_INTERVAL_MS - tsldiff);
		} else
			looptim = 0;
	}	/* MAIN LOOP ends here */
}
