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
#include <sched.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
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
#define LOG_INFO		"info"
#define LOG_NOTICE		"notice"
#define LOG_WARN		"warn"
#define LOG_ERROR		"error"

int vflag;
sem_t blinksem;		/* signal LED that it should blink */
pthread_t blinkthread;
int ledgpio, ledact;
sem_t clientsem;	/* to synchronize access to client fd array */
pthread_t clntaddthread;
int srvsock;		/* server socket */
int clntfd[MAX_CLIENTS];

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s -g gpio [-v] [-u user] [-l ledgpio:ledact] [-h ipaddr] [-p tcpport]\n\n", progname);
	puts("Where:");
	puts("\tgpio\t - GPIO pin with external RF receiver data (mandatory)");
	puts("\t-v\t - be verbose, print radio activity and network connections (optional)");
	puts("\tuser\t - name of the user to switch to (optional)");
	puts("\tledgpio\t - GPIO pin with LED to signal packet receiving (optional)");
	puts("\tledact\t - LED activity: 0 for active low, 1 for active high (optional)");
	puts("\tipaddr\t - IPv4 address to listen on (optional, default any = 0.0.0.0)");
	puts("\ttcpport\t - TCP port to listen on (optional, default is 5433)");
}

/* Log line header */
void logprintf(FILE *fd, const char *level, const char *fmt, ...)
{
        struct timeval ts;
        struct tm *tl;
	va_list ap;

	gettimeofday(&ts, NULL);
	tl = localtime(&ts.tv_sec);
	fprintf(fd, "%d-%02d-%02d %02d:%02d:%02d.%03u %s: ", 1900 + tl->tm_year,
                tl->tm_mon + 1, tl->tm_mday, tl->tm_hour, tl->tm_min,
                tl->tm_sec, ts.tv_usec / 1000, level);
	va_start(ap, fmt);
	vfprintf(fd, fmt, ap);
	va_end(ap);
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
	for(;;) {
		sem_wait(&blinksem);
		digitalWrite(ledgpio, ledact ? HIGH : LOW);
		delay(LED_BLINK_MS);
		digitalWrite(ledgpio, ledact ? LOW : HIGH);
	}
}

/* Intercept TERM and INT signals */
void signalQuit(int sig)
{
	exit(0);
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

	for(;;) {
		clen = sizeof(struct sockaddr_in);
		clfd = accept(srvsock, (struct sockaddr *)&clin, &clen);
		sem_wait(&clientsem);
		i = findFreeFd();
		if (i >= 0)
			clntfd[i] = clfd;
		sem_post(&clientsem);
		if (vflag) {
			if (i < 0)
				logprintf(stdout, LOG_INFO, "client limit reached\n");
			else
				logprintf(stdout, LOG_INFO,
					  "client %s [%d] connected successfully\n",
					  inet_ntoa(clin.sin_addr), clfd);
		}
	}
}

/* Send code to all clients */
/* (also detect stale sockets and remove them from array) */
int updateClients(const char *buf, size_t len)
{
	int i;

	for(i = 0; i < MAX_CLIENTS; i++)
		if (clntfd[i] >= 0) {
			if (vflag)
				logprintf(stdout, LOG_INFO,
					  "sending message to client [%d]\n", clntfd[i]);
			sem_wait(&clientsem);
			if (send(clntfd[i], buf, len, MSG_NOSIGNAL) == -1) {
				if (vflag)
					logprintf(stdout, LOG_INFO,
						  "client [%d] disconnected\n", clntfd[i]);
				close(clntfd[i]);
				clntfd[i] = -1;
			}
			sem_post(&clientsem);
		}
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	struct timeval ts;
	unsigned long long code;
	int gpio, type, bits, opt;
	uid_t uid;
	gid_t gid;
	int srvport;
	char srvaddrstr[16];
	struct sockaddr_in sinsrv;
	int i, len;
	int codelen, repeats, interval;
	char buf[MAX_MSG_SIZE + 1];
	char username[MAX_USERNAME + 1];

	/* show help */
	if (argc < 2) {
		help(argv[0]);
		exit(0);
	}

	/* get parameters */
	gpio = -1;
	uid = 0;
	gid = 0;
	ledgpio = -1;
	ledact = 1;
	vflag = 0;
	memset(username, 0, MAX_USERNAME + 1);
	memset(srvaddrstr, 0, 16);
	strncpy(srvaddrstr, SERVER_ADDR, 15);
	srvport = SERVER_PORT;
	while((opt = getopt(argc, argv, "vg:u:l:h:p:")) != -1) {
		if (opt == 'g')
			sscanf(optarg, "%d", &gpio);
		else if (opt == 'u')
			strncpy(username, optarg, MAX_USERNAME);
		else if (opt == 'l')
			sscanf(optarg, "%d:%d", &ledgpio, &ledact);
		else if (opt == 'h')
			strncpy(srvaddrstr, optarg, 15);
		else if (opt == 'p')
			sscanf(optarg, "%d", &srvport);
		else if (opt == 'v')
			vflag = 1;
		else if (opt == '?') {
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (getuid()) {
		fputs("Must be run by root.\n", stderr);
		exit(EXIT_FAILURE);
	}

	if (gpio < 0 || gpio > GPIO_PINS) {
		fputs("Invalid RX GPIO pin.\n", stderr);
		exit(EXIT_FAILURE);
	}

	if (ledgpio > GPIO_PINS || ledact < 0 || ledact > 1) {
		fputs("Invalid LED specification.\n", stderr);
		exit(EXIT_FAILURE);
	}

	/* setup server */
	srvsock = socket(AF_INET, SOCK_STREAM, 0);
	if (srvsock == -1) {
		fprintf(stderr, "Unable to create socket: %s\n",
			strerror (errno));
		exit(EXIT_FAILURE);
	}
	memset((char *)&sinsrv, 0, sizeof(sinsrv));
	if (!inet_aton(srvaddrstr, &sinsrv.sin_addr)) {
		fputs("Invalid IPv4 address specification.\n", stderr);
		exit(EXIT_FAILURE);
	}
	sinsrv.sin_family = AF_INET;
	sinsrv.sin_port = htons(srvport);
	if (bind(srvsock, (struct sockaddr *)&sinsrv, sizeof(sinsrv)) == -1) {
		fprintf(stderr, "Unable to bind to socket: %s\n",
			strerror (errno));
		exit(EXIT_FAILURE);
	}
	if (listen(srvsock, (MAX_CLIENTS >> 4)) == -1) {
		fprintf(stderr, "Unable to listen to socket: %s\n",
			strerror (errno));
		exit(EXIT_FAILURE);
	}
	logprintf(stdout, LOG_NOTICE, "accepting client TCP connections on %s:%d\n",
		  srvaddrstr, srvport);

	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();

	/* no transmission, only read */
	Radio433_init(-1, gpio);

	/* change scheduling priority */
	if (changeSched()) {
		fprintf(stderr, "Unable to change process scheduling priority: %s\n",
			strerror (errno));
		exit(-1);
	}

	/* drop privileges */
	if (username[0]) {
		if (dropRootPriv(username, &uid, &gid)) {
			fprintf(stderr, "Unable to drop super-user privileges: %s\n",
				strerror (errno));
			exit(EXIT_FAILURE);
		}
		else
			logprintf(stdout, LOG_NOTICE,
				  "dropping super-user privileges, running as UID=%ld GID=%ld\n",
				  uid, gid);
	}

	/* activate LED */
	if (ledgpio > 0) {
		logprintf(stdout, LOG_NOTICE, "activity LED set to GPIO %d (active %s)\n",
			  ledgpio, ledact ? "high" : "low");
		digitalWrite(ledgpio, ledact ? LOW : HIGH);
		sem_init(&blinksem, 0, 0);
		if (pthread_create(&blinkthread, NULL, blinkerLEDThread, NULL)) {
			fputs("Cannot start blinking thread.\n", stderr);
			exit(EXIT_FAILURE);
		}
	}

	/* start network listener */
	for(i = 0; i < MAX_CLIENTS; i++)
		clntfd[i] = -1;
	sem_init(&clientsem, 0, 1);
	if (pthread_create(&clntaddthread, NULL, clientAddThread, NULL)) {
		fputs("Cannot start network listener thread.\n", stderr);
		exit(EXIT_FAILURE);
	}

#ifdef HAS_CPUFREQ
	/* warn user if system frequency is dynamic */
	if (checkCpuFreq())
		logprintf(stdout, LOG_WARN,
			  "current cpufreq governor is not optimal for radio code timing");
#endif

	/* info */
	logprintf(stdout, LOG_NOTICE,
		  "starting to capture RF codes from receiver connected to GPIO pin %d\n",
		  gpio);

	/* function loop - never ends, send signal to exit */
	for(;;) {
		/* codes are buffered, so this loop can be more relaxed */
		code = Radio433_getCodeExt(&ts, &type, &bits, &codelen,
					   &repeats, &interval);
		if (ledgpio > 0)
			blinkLED();
		len = formatMessage(buf, &ts, type, bits, codelen,
				    repeats, interval, code);
		if (vflag)
			logprintf(stdout, LOG_INFO, "MSG(%d): %s", len, buf);
		updateClients(buf, len);
	}
}
