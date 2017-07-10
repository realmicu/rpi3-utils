#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
#ifdef HAS_CPUFREQ
#include <cpufreq.h>
#endif

#include <wiringPi.h>

#include "radio433_lib.h"
#include "radio433_dev.h"

extern char *optarg;
extern int optind, opterr, optopt;

/* Message format (semicolon-separated one line):
   header (<RX<)
   timestamp - seconds.milliseconds - decimal
   codetime - code length in milliseconds - decimal
   repeats - number of codes in single transmission - decimal
   interval - time between transmissions in milliseconds - decimal
   type - hex - 32-bit value (with 0x prefix)
   bits - decimal
   data - hex - 64-bit value (with 0x prefix)
   stop mark (<ZZ>)
   Example:
   <RX>1490084244.768;40;3;0;0x0101;32;0x0000000000441454;<ZZ>
   <RX>1490084239.165;128;4;33000;0x0201;36;0x00000004A03608F9;<ZZ>
 */


/* *************** */
/* *  Constants  * */
/* *************** */

#define BANNER			"Radio433Daemon v0.98 server"
#define MAX_USERNAME		32
#define MAX_NGROUPS		(NGROUPS_MAX >> 10)	/* reasonable maximum */
#define GPIO_PINS		28	/* number of Pi GPIO pins */
#define LED_BLINK_MS		100	/* minimal LED blinking time in ms */
#define SERVER_ADDR		"0.0.0.0" /* default server address */
#define SERVER_PORT		5433	/* default server TCP port */
#define MAX_MSG_SIZE		128	/* maximum message size in bytes */
#define MAX_CLIENTS		64	/* client limit */
#define MSG_HDR			"<RX>"
#define MSG_END			"<ZZ>"
#define FILE_UMASK		(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)
#define PID_DIR			"/var/run/"
#define LOG_DEBUG		"debug"
#define LOG_INFO		"info"
#define LOG_NOTICE		"notice"
#define LOG_WARN		"warn"
#define LOG_ERROR		"error"

/* ********************** */
/* *  Global variables  * */
/* ********************** */

volatile int debugflag, clntrun;
sem_t blinksem;		/* signal LED that it should blink */
pthread_t blinkthread;
int ledgpio, ledact;
sem_t clientsem;	/* to synchronize access to client fd array */
pthread_t clntaddthread;
int srvsock;		/* server socket */
int clntfd[MAX_CLIENTS];
sem_t logsem;		/* log file semaphore */
pid_t procpid;
volatile int logfd;
char progname[PATH_MAX + 1], logfname[PATH_MAX + 1], pidfname[PATH_MAX + 1];

/* *************** */
/* *  Functions  * */
/* *************** */

/* Show help */
void help(void)
{
	printf("Usage:\n\t%s -g gpio [-u user] [-d | -l logfile] [-P pidfile] [-L gpio:act] [-h ipaddr] [-p tcpport]\n\n", progname);
	puts("Where:");
	puts("\t-g gpio     - GPIO pin with external RF receiver data (mandatory)");
	puts("\t-u user     - name of the user to switch to (optional)");
	puts("\t-d          - debug mode, stay foreground and show activity (optional)");
	puts("\t-l logfile  - path to log file (optional, default is none)");
	printf("\t-P pidfile  - path to PID file (optional, default is %s%s.pid)\n", PID_DIR, progname);
	puts("\t-L g:a      - LED to signal packet receiving (optional, g is BCM GPIO number and a is 0/1 for active low/high)");
	printf("\t-h ipaddr   - IPv4 address to listen on (optional, default %s)\n", SERVER_ADDR);
	printf("\t-p tcpport  - TCP port to listen on (optional, default is %d)\n", SERVER_PORT);
	puts("\nRecognized devices - Hyundai WS Senzor 77TH, Kemot Remote Power URZ1226 compatible");
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

/* drop super-user privileges */
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
	if (setresgid(pw.pw_gid, pw.pw_gid, pw.pw_gid))
		return -1;
	if (setresuid(pw.pw_uid, pw.pw_uid, pw.pw_uid))
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

#ifdef HAS_CPUFREQ
/* verify if cpufreq governor prefers 'fixed' frequencies */
/* (gpio timings require stable system clock) */
int checkCpuFreq(void)
{
	struct cpufreq_policy *p;
	int s;

	p = cpufreq_get_policy(0);
	if (!strcmp(p->governor, "powersave") ||
	    !strcmp(p->governor, "performance"))
		s = 0;
	else
		s = 1;
	cpufreq_put_policy(p);	/* free pointer */
	return s;
}
#endif

/* blink LED */
void blinkLED(void)
{
	int semv;

	sem_getvalue(&blinksem, &semv);
	if (semv < 1)	/* semaphore 0 or 1 */
		sem_post(&blinksem);
}

/* LED blinking thread */
/* (mail loop on/off is hardly noticeable) */
void *blinkerLEDThread(void *arg)
{
	sigset_t blkset;

	sigfillset(&blkset);
	pthread_sigmask(SIG_BLOCK, &blkset, NULL);

	for(;;) {
		sem_wait(&blinksem);
		digitalWrite(ledgpio, ledact ? HIGH : LOW);
		delay(LED_BLINK_MS);
		digitalWrite(ledgpio, ledact ? LOW : HIGH);
	}
}

/* Process shutdown */
void endProcess(int status)
{
	int i;

	unlink(pidfname);

	/* terminate threads regardless of semaphores */
	if (clntrun) {
		pthread_cancel(clntaddthread);
		for(i = 0; i < MAX_CLIENTS; i++)
			if (clntfd[i] >= 0)
				close(clntfd[i]);
	}
	if (ledgpio >= 0) {
		pthread_cancel(blinkthread);
		digitalWrite(ledgpio, ledact ? LOW : HIGH);
	}
	if (srvsock >= 0)
		close(srvsock);
	if (logfd >= 0) {
		logprintf(logfd, LOG_NOTICE, "server shut down with code %d\n",
			  status);
		close(logfd);
	}
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

/* Format message */
int formatMessage(char *buf, struct timeval *ts, int type, int bits,
		  int codelen, int repeats, int interval,
		  unsigned long long code)
{
	memset(buf, 0, MAX_MSG_SIZE);
	sprintf(buf, "%s%lu.%03u;%d;%d;%d;0x%04X;%d;0x%016llX;%s\n", MSG_HDR,
		ts->tv_sec, ts->tv_usec / 1000, codelen, repeats, interval,
		type, bits, code, MSG_END);
	return strlen(buf);
}

/* Find free slot in client descriptor table, return -1 if table full */
int findFreeFd(void)
{
	int i;

	i=0;
	while(i < MAX_CLIENTS)
       		if (clntfd[i] == -1)
               		break;
		else
			i++;
	if (i == MAX_CLIENTS)
			i = -1;
	return i;
}

/* Client add thread */
/* (accept clients) */
void *clientAddThread(void *arg)
{
	int i, clfd, clen;
	struct sockaddr_in clin;
	sigset_t blkset;

	sigfillset(&blkset);
	pthread_sigmask(SIG_BLOCK, &blkset, NULL);

	for(;;) {
		clen = sizeof(struct sockaddr_in);
		clfd = accept(srvsock, (struct sockaddr *)&clin, &clen);
		sem_wait(&clientsem);
		i = findFreeFd();
		if (i >= 0)
			clntfd[i] = clfd;
		sem_post(&clientsem);
		if (i < 0)
			logprintf(logfd, LOG_WARN, "client limit reached\n");
		else
			logprintf(logfd, LOG_INFO,
				  "client %s [%d] connected successfully\n",
				  inet_ntoa(clin.sin_addr), clfd);
	}
}

/* Send code to all clients */
/* (also detect stale sockets and remove them from array) */
int updateClients(const char *buf, size_t len)
{
	int i;

	for(i = 0; i < MAX_CLIENTS; i++)
		if (clntfd[i] >= 0) {
			if (debugflag)
				logprintf(logfd, LOG_DEBUG,
					  "sending message to client [%d], %d bytes\n",
					  clntfd[i], len);
			sem_wait(&clientsem);
			if (send(clntfd[i], buf, len, MSG_NOSIGNAL) == -1) {
				logprintf(logfd, LOG_INFO,
					  "client [%d] disconnected\n", clntfd[i]);
				close(clntfd[i]);
				clntfd[i] = -1;
			}
			sem_post(&clientsem);
		}
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

/* ************ */
/* ************ */
/* **  MAIN  ** */
/* ************ */
/* ************ */

int main(int argc, char *argv[])
{
	struct timeval ts;
	unsigned long long code;
	int gpio, type, bits, opt;
	uid_t uid;
	gid_t gid;
	int srvport;
	int pidfd;
	struct sockaddr_in srvsin;
	int i, len, ena;
	int codelen, repeats, interval;
	char buf[MAX_MSG_SIZE + 1];
	char username[MAX_USERNAME + 1];
	struct sigaction sa;

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

	/* get parameters */
	gpio = -1;
	uid = 0;
	gid = 0;
	ledgpio = -1;
	ledact = 1;
	debugflag = 0;
	logfd = -1;
	srvsock = -1;
	clntrun = 0;
	memset(username, 0, MAX_USERNAME + 1);
	memset((char *)&srvsin, 0, sizeof(srvsin));
	srvsin.sin_family = AF_INET;
	inet_aton(SERVER_ADDR, &srvsin.sin_addr);
	srvport = SERVER_PORT;
	memset(logfname, 0, PATH_MAX + 1);
	memset(pidfname, 0, PATH_MAX + 1);
	strcpy(pidfname, PID_DIR);
	strcat(pidfname, progname);
	strcat(pidfname, ".pid");
	while((opt = getopt(argc, argv, "g:u:dl:P:L:h:p:")) != -1) {
		if (opt == 'g')
			sscanf(optarg, "%d", &gpio);
		else if (opt == 'u')
			strncpy(username, optarg, MAX_USERNAME);
		else if (opt == 'd')
			debugflag = 1;
		else if (opt == 'l')
			strncpy(logfname, optarg, PATH_MAX);
		else if (opt == 'P')
			strncpy(pidfname, optarg, PATH_MAX);
		else if (opt == 'L')
			sscanf(optarg, "%d:%d", &ledgpio, &ledact);
		else if (opt == 'h') {
			if (!inet_aton(optarg, &srvsin.sin_addr)) {
				dprintf(STDERR_FILENO, "Invalid IPv4 address specification.\n");
				exit(EXIT_FAILURE);
			}
		}
		else if (opt == 'p')
			sscanf(optarg, "%d", &srvport);
		else if (opt == '?' || opt == 'h') {
			help();
			exit(EXIT_FAILURE);
		}
	}

	if (getuid()) {
		dprintf(STDERR_FILENO, "Must be run by root.\n");
		exit(EXIT_FAILURE);
	}

	if (gpio < 0 || gpio > GPIO_PINS) {
		dprintf(STDERR_FILENO, "Invalid RX GPIO pin.\n");
		exit(EXIT_FAILURE);
	}

	if (ledgpio > GPIO_PINS || ledact < 0 || ledact > 1) {
		dprintf(STDERR_FILENO, "Invalid LED specification.\n");
		exit(EXIT_FAILURE);
	}

	if (debugflag && logfname[0]) {
		dprintf(STDERR_FILENO, "Flags -d and -l are mutually exclusive.\n");
		exit(EXIT_FAILURE);
	}

	srvsin.sin_port = htons(srvport);

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
	logprintf(logfd, LOG_NOTICE, "starting %s build %s (devices: %d)\n",
		  BANNER, BUILDSTAMP, RADIO433_DEVICES);
#else
	logprintf(logfd, LOG_NOTICE, "starting %s (devices: %d)\n", BANNER,
		  RADIO433_DEVICES);
#endif

	/* check if pid file exists */
	pidfd = open(pidfname, O_PATH, FILE_UMASK);
	if (pidfd >= 0) {
		/* pid file exists */
		close(pidfd);
		dprintf(STDERR_FILENO, "PID file (%s) exists, daemon already running or stale file detected.\n",
			pidfname);
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to create PID file %s: %s\n",
				  pidfname, strerror(errno));
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
	sa.sa_handler = &signalQuit;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sa.sa_handler = &signalReopenLog;
	sigaction(SIGHUP, &sa, NULL);

	/* setup server */
	srvsock = socket(AF_INET, SOCK_STREAM, 0);
	if (srvsock == -1) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to create server socket: %s\n",
				  strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to create server socket: %s\n",
				strerror(errno));
		endProcess(EXIT_FAILURE);
	}
	ena = 1;
	if (setsockopt(srvsock, SOL_SOCKET, SO_REUSEADDR, &ena, sizeof(int)) == -1) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to set flags for server socket: %s\n",
				  strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to set flags for server socket: %s\n",
				strerror(errno));
		endProcess(EXIT_FAILURE);
	}
	if (setsockopt(srvsock, SOL_SOCKET, SO_REUSEPORT, &ena, sizeof(int)) == -1) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to set flags for server socket: %s\n",
				  strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to set flags for server socket: %s\n",
				strerror(errno));
		endProcess(EXIT_FAILURE);
	}
	if (bind(srvsock, (struct sockaddr *)&srvsin, sizeof(srvsin)) == -1) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to bind to server socket: %s\n",
				  strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to bind to server socket: %s\n",
				strerror(errno));
		endProcess(EXIT_FAILURE);
	}
	if (listen(srvsock, (MAX_CLIENTS >> 4)) == -1) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to listen to server socket: %s\n",
				  strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to listen to server socket: %s\n",
				strerror(errno));
		endProcess(EXIT_FAILURE);
	}
	logprintf(logfd, LOG_NOTICE, "accepting client TCP connections on %s port %d\n",
		  inet_ntoa(srvsin.sin_addr), srvport);

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

	/* initialize WiringPi library - use BCM GPIO numbers - must be root */
	wiringPiSetupGpio();

	/* no transmission, only read (uses GPIO ISR, must be root) */
	if (Radio433_init(-1, gpio)) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to initialize radio input\n");
		else
			dprintf(STDERR_FILENO, "Unable to initialize radio input\n");
		endProcess(EXIT_FAILURE);
	}

	/* drop privileges */
	if (username[0]) {
		if (dropRootPriv(username, &uid, &gid)) {
			if (!debugflag)
				logprintf(logfd, LOG_ERROR, "unable to drop super-user privileges: %s\n",
					  strerror (errno));
			else
				dprintf(STDERR_FILENO, "Unable to drop super-user privileges: %s\n",
					strerror (errno));
			endProcess(EXIT_FAILURE);
		}
		else
			logprintf(logfd, LOG_NOTICE,
				  "dropping super-user privileges, running as UID=%ld GID=%ld\n",
				  uid, gid);
	}

	/* activate LED */
	if (ledgpio >= 0) {
		logprintf(logfd, LOG_NOTICE, "activity LED set to GPIO %d (active %s)\n",
			  ledgpio, ledact ? "high" : "low");
		digitalWrite(ledgpio, ledact ? LOW : HIGH);
		sem_init(&blinksem, 0, 0);
		if (pthread_create(&blinkthread, NULL, blinkerLEDThread, NULL)) {
			logprintf(logfd, LOG_WARN, "blinking thread cannot be started, LED disabled\n");
			ledgpio = -1;
		}
	}

	/* start network listener */
	for(i = 0; i < MAX_CLIENTS; i++)
		clntfd[i] = -1;
	sem_init(&clientsem, 0, 1);
	if (pthread_create(&clntaddthread, NULL, clientAddThread, NULL)) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "cannot start network listener thread\n");
		else
			dprintf(STDERR_FILENO, "Cannot start network listener thread.\n");
		endProcess(EXIT_FAILURE);
	}
	clntrun = 1;

#ifdef HAS_CPUFREQ
	/* warn user if system frequency is dynamic */
	if (checkCpuFreq())
		logprintf(logfd, LOG_WARN,
			  "current CPUfreq governor is not optimal for radio code timing\n");
#endif

	/* info */
	logprintf(logfd, LOG_NOTICE,
		  "starting to capture RF codes from receiver connected to GPIO pin %d\n",
		  gpio);

	/* function loop - never ends, send signal to exit */
	for(;;) {
		/* codes are buffered, so this loop can be more relaxed */
		code = Radio433_getCodeExt(&ts, &type, &bits, &codelen,
					   &repeats, &interval);
		logprintf(logfd, LOG_INFO, "radio transmission received\n");
		if (ledgpio >= 0)
			blinkLED();
		len = formatMessage(buf, &ts, type, bits, codelen,
				    repeats, interval, code);
		if (debugflag)
			logprintf(logfd, LOG_DEBUG, "sending message (%d bytes): %s", len, buf);
		updateClients(buf, len);
	}
}
