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

#include <wiringPi.h>

#include "radio433_lib.h"

#define OWNER_UID	500
#define OWNER_GID	500
#define RX_GPIO		21

#include <signal.h>

/* Show help */
/*
void help(char *progname)
{
	printf("Usage:\n\t%s {gpio}\n\n", progname);
	puts("Where:");
	puts("\tgpio\t - GPIO pin with external RF receiver data (mandatory)");
}

void getTimestamp(char *s)
{
	struct timeval t;
	struct tm *tl;

	gettimeofday(&t, NULL);
	tl = localtime(&t.tv_sec);
	snprintf(s, 24, "%d-%02d-%02d %02d:%02d:%02d.%03u", 1900 + tl->tm_year,
		 tl->tm_mon + 1, tl->tm_mday, tl->tm_hour, tl->tm_min,
		 tl->tm_sec, t.tv_usec / 1000);
}
*/

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
	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();

	/* no transmission, only read */
	Radio433_init(-1, RX_GPIO);

	/* change scheduling priority */
	if (changeSched()) {
		fprintf(stderr, "Unable to change process scheduling priority: %s\n",
			strerror (errno));
		exit(-1);
	}

	/* drop privileges */
	if (dropRootPriv(OWNER_UID, OWNER_GID)) {
		fprintf(stderr, "Unable to drop super-user privileges: %s\n",
			strerror (errno));
		exit(-1);
	}

	/* function loop - never ends, send signal to exit */

	/* tempTBufDebug() should be run with analyzer thread
	   DISABLED in radio433_lib.c code (comment out
	   pthread_create()) */
	/* Radio433_tempTBufDebug(); */

	/* tempCBufDebug() should be run with analyzer thread
	   ENABLED in radio433_lib.c code (default config) */
	Radio433_tempCBufDebug();
}
