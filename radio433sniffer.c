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
#include <sched.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>

#include <wiringPi.h>

#include "radio433_lib.h"
#include "radio433_dev.h"

extern char *optarg;
extern int optind, opterr, optopt;

#define GPIO_PINS		28	/* number of Pi GPIO pins */
#define LED_BLINK_MS		100	/* minimal LED blinking time in ms */

sem_t blinksem;
pthread_t blinkthread;
int ledgpio, ledact;

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s -g gpio [-u uid:gid] [-l ledgpio:ledact]\n\n", progname);
	puts("Where:");
	puts("\tgpio\t - GPIO pin with external RF receiver data (mandatory)");
	puts("\tuid\t - user ID to switch to (optional)");
	puts("\tgid\t - user group ID to switch to (optional)");
	puts("\tledgpio\t - GPIO pin with LED to signal packet receiving (optional)");
	puts("\tledact\t - LED activity: 0 for active low, 1 for active high (optional)");
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

/* LED blinking thread */
/* (mail loop on/off is hardly noticeable) */
void *blinkLED(void *arg)
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

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	struct timeval ts;
	struct tm *tl;
	unsigned long long code;
	int gpio, type, bits;
	int opt, uid, gid, semv, tid;
	char *stype[] = { "NUL", "PWR", "THM", "RMT" };
	int sysid, devid, btn;
	int ch, batlow, tdir, humid;
	double temp;
	char trend[3] = { '_', '/', '\\' };

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
	while((opt = getopt(argc, argv, "g:u:l:")) != -1) {
		if (opt == 'g')
			sscanf(optarg, "%d", &gpio);
		else if (opt == 'u')
			sscanf(optarg, "%d:%d", &uid, &gid);
		else if (opt == 'l')
			sscanf(optarg, "%d:%d", &ledgpio, &ledact);
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
			printf("Dropping super-user privileges, running as UID=%d GID=%d.\n",
			       uid, gid);
	}

	if (ledgpio > 0) {
		printf("Activity LED set to GPIO %d (active %s).\n", ledgpio,
		       ledact ? "high" : "low");
		digitalWrite(ledgpio, ledact ? LOW : HIGH);
		sem_init(&blinksem, 0, 0);
		if (pthread_create(&blinkthread, NULL, blinkLED, NULL)) {
			fputs("Cannot start blinking thread.\n", stderr);
			exit(EXIT_FAILURE);
		}
	}

	/* info */
	printf("Starting to capture RF codes from receiver connected to GPIO pin %d ...\n",
	       gpio);

	/* function loop - never ends, send signal to exit */
	for(;;) {
		code = Radio433_getCode(&ts, &type, &bits);
		if (ledgpio > 0) {
			sem_getvalue(&blinksem, &semv);
			if (semv < 1)	/* semaphore 0 or 1 */
				sem_post(&blinksem);
		}
		tl = localtime(&ts.tv_sec);
		printf("%d-%02d-%02d %02d:%02d:%02d.%03u", 1900 + tl->tm_year,
		       tl->tm_mon + 1, tl->tm_mday, tl->tm_hour, tl->tm_min,
		       tl->tm_sec, ts.tv_usec / 1000);
		if (type & RADIO433_CLASS_POWER)
			tid = 1;
		else if (type & RADIO433_CLASS_WEATHER)
			tid = 2;
		else if (type & RADIO433_CLASS_REMOTE)
			tid = 3;
		else
			tid = 0;
		printf("  %s len = %d , code = 0x%0*llX", stype[tid] , bits,
		       bits >> 2, code);
		if (type == RADIO433_DEVICE_KEMOTURZ1226) {
			if (Radio433_pwrGetCommand(code, &sysid, &devid, &btn))
				printf(" , %d : %s%s%s%s%s : %s\n", sysid,
				       devid & POWER433_DEVICE_A ? "A" : "",
				       devid & POWER433_DEVICE_B ? "B" : "",
				       devid & POWER433_DEVICE_C ? "C" : "",
				       devid & POWER433_DEVICE_D ? "D" : "",
				       devid & POWER433_DEVICE_E ? "E" : "",
				       btn ? "ON" : "OFF");
			else
				puts("");
		}
		else if (type == RADIO433_DEVICE_HYUWSSENZOR77TH) {
			if (Radio433_thmGetData(code, &sysid, &devid, &ch,
						&batlow, &tdir, &temp, &humid))
				printf(" , %1d , T: %+.1lf C %c , H: %d %%\n",
				       ch, temp, tdir < 0 ? '!' : trend[tdir],
				       humid);
			else
				puts("");

		}
		else
			puts("");
	}
}
