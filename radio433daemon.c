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

#include <wiringPi.h>

#include "radio433_lib.h"
#include "radio433_dev.h"

extern char *optarg;
extern int optind, opterr, optopt;

/* Message format (semicolon-separated one line):
   header (<RX<)
   timestamp - seconds.miliseconds - decimal
   type - decimal
   bits - decimal
   data - hex - 64-bit value (with 0x prefix)
   stop mark (<ZZ>)
   Example:
   <RX>1490084244.768;0;32;0x0000000000441454;<ZZ>
   <RX>1490084239.165;1;36;0x00000004A03608F9;<ZZ>
 */

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
	printf("Usage:\n\t%s -g gpio [-v] [-u uid:gid] [-l ledgpio:ledact] [-h ipaddr] [-p tcpport]\n\n", progname);
	puts("Where:");
	puts("\tgpio\t - GPIO pin with external RF receiver data (mandatory)");
	puts("\t-v\t - be verbose, print radio activity and network connections (optional)");
	puts("\tuid\t - user ID to switch to (optional)");
	puts("\tgid\t - user group ID to switch to (optional)");
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
int dropRootPriv(int newuid, int newgid)
{
	/* start with GID */
	if (setresgid(newgid, newgid, newgid))
		return -1;
	if (setresuid(newuid, newuid, newuid))
		return -1;
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
int formatMessage(char *buf, struct timeval *ts, int type,
		   int bits, unsigned long long code)
{
	memset(buf, 0, MAX_MSG_SIZE);
	sprintf(buf, "%s%lu.%lu;%d;%d;0x%016llX;%s\n", MSG_HDR, ts->tv_sec,
		ts->tv_usec / 1000, type, bits, code, MSG_END);
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
	int gpio, type, bits;
	int opt, uid, gid;
	int srvport;
	char srvaddrstr[16];
	struct sockaddr_in sinsrv;
	int i, len;
	char buf[MAX_MSG_SIZE + 1];

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
	strncpy(srvaddrstr, SERVER_ADDR, 15);
	srvport = SERVER_PORT;
	while((opt = getopt(argc, argv, "vg:u:l:h:p:")) != -1) {
		if (opt == 'g')
			sscanf(optarg, "%d", &gpio);
		else if (opt == 'u')
			sscanf(optarg, "%d:%d", &uid, &gid);
		else if (opt == 'l')
			sscanf(optarg, "%d:%d", &ledgpio, &ledact);
		else if (opt == 'h')
			sscanf(optarg, "%15s", srvaddrstr);
		else if (opt == 'p')
			sscanf(optarg, "%d", &srvport);
		else if (opt == 'v')
			vflag = 1;
		else if (opt == '?') {
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
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
	if (uid > 0 && gid > 0) {
		if (dropRootPriv(uid, gid)) {
			fprintf(stderr, "Unable to drop super-user privileges: %s\n",
				strerror (errno));
			exit(EXIT_FAILURE);
		}
		else
			logprintf(stdout, LOG_NOTICE,
				  "dropping super-user privileges, running as UID=%d GID=%d\n",
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

	/* info */
	logprintf(stdout, LOG_NOTICE,
		  "starting to capture RF codes from receiver connected to GPIO pin %d\n",
		  gpio);

	/* function loop - never ends, send signal to exit */
	for(;;) {
		/* codes are buffered, so this loop can be more relaxed */
		code = Radio433_getCode(&ts, &type, &bits);
		if (ledgpio > 0)
			blinkLED();
		len = formatMessage(buf, &ts, type, bits, code);
		if (vflag)
			logprintf(stdout, LOG_INFO, "MSG(%d): %s", len, buf);
		updateClients(buf, len);
	}
}
