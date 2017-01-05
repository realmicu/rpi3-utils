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

#include "thermo433_lib.h"

#define OWNER_UID	500
#define OWNER_GID	500

#define THERMO_BITS	36
#define THERMO_PULSES	(THERMO_BITS << 1)

#ifdef THERMO433_INCLUDE_TIMING_STATS
#include <signal.h>
static volatile int gotbrk;
#endif

#ifdef THERMO433_INCLUDE_TIMING_STATS
static unsigned long tstats[4][4];	/* 0-count, 1-sum, 2-min, 3-max */
#endif

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s {gpio}\n\n", progname);
	puts("Where:");
	puts("\tgpio\t - GPIO pin with external RF receiver data (mandatory)");
}

/* Convert 36-bit unsigned to binary string */
void convertBin36(unsigned long long a, char *s)
{
	unsigned int i, j;
	unsigned long long mask;

	mask = 1ULL << (THERMO_BITS - 1);
	j = 0;
	for(i = 0; i < THERMO_BITS; i++) {
		if (i && !(i & 0x3))
			s[j++] = ' ';
		s[j++] = ( a & mask ) ? '1' : '0';
		mask >>= 1;
	}
	s[j] = 0;
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

#ifdef THERMO433_INCLUDE_TIMING_STATS
/* Intercept TERM and INT signals */
void signalQuit(int sig)
{
	if (tstats[THERMO433_PULSE_TYPE_SYNC][0] && \
	    tstats[THERMO433_PULSE_TYPE_HIGH][0] && \
	    tstats[THERMO433_PULSE_TYPE_LOW_SHORT][0] && \
	    tstats[THERMO433_PULSE_TYPE_LOW_LONG][0]) {
		printf("\nSync time (min/avg/max): %d/%d/%d microseconds",
		       tstats[THERMO433_PULSE_TYPE_SYNC][2],
		       tstats[THERMO433_PULSE_TYPE_SYNC][1] / tstats[THERMO433_PULSE_TYPE_SYNC][0],
		       tstats[THERMO433_PULSE_TYPE_SYNC][3]);
		printf("\nHigh time (min/avg/max): %d/%d/%d microseconds",
		       tstats[THERMO433_PULSE_TYPE_HIGH][2],
		       tstats[THERMO433_PULSE_TYPE_HIGH][1] / tstats[THERMO433_PULSE_TYPE_HIGH][0],
		       tstats[THERMO433_PULSE_TYPE_HIGH][3]);
		printf("\n0's (low short) time (min/avg/max): %d/%d/%d microseconds",
		       tstats[THERMO433_PULSE_TYPE_LOW_SHORT][2],
		       tstats[THERMO433_PULSE_TYPE_LOW_SHORT][1] / tstats[THERMO433_PULSE_TYPE_LOW_SHORT][0],
		       tstats[THERMO433_PULSE_TYPE_LOW_SHORT][3]);
		printf("\n1's (low long) time (min/avg/max): %d/%d/%d microseconds\n",
		       tstats[THERMO433_PULSE_TYPE_LOW_LONG][2],
		       tstats[THERMO433_PULSE_TYPE_LOW_LONG][1] / tstats[THERMO433_PULSE_TYPE_LOW_LONG][0],
		       tstats[THERMO433_PULSE_TYPE_LOW_LONG][3]);
	} else if (!tstats[THERMO433_PULSE_TYPE_SYNC][0])
		puts("\nNo codes received - timing statistics unavailable.");

	exit(0);
}
#endif

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int gpio;
	unsigned long long code;
	char bincode[THERMO_BITS + 9], tstring[25];
	int temp, humid, ch, bat, tdir, xorok;
	char trend[3] = { '_', '/', '\\' };

#ifdef THERMO433_INCLUDE_TIMING_STATS
	struct sigaction sa;
	int i, pi;
	unsigned long stat_sync;                 /* for statistic purposes */
	unsigned long stat_pbuf[THERMO_PULSES];
#endif

	/* show help */
	if (argc < 2) {
		help(argv[0]);
		exit(0);
	}

	/* get parameters */
	sscanf(argv[1], "%d", &gpio);

	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();

	/* no transmission, only read */
	Thermo433_init(-1, gpio, THERMO433_DEVICE_HYUWSSENZOR77TH);

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

	/* info */
	printf("Starting to capture %d-bit codes from RF receiver connected to GPIO pin %d ...\n",
	       THERMO_BITS, gpio);

#ifdef THERMO433_INCLUDE_TIMING_STATS
	/* install CRTL+C to display stats at the end */
	gotbrk = 0;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &signalQuit;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	/* for statistics */
	for(i = 0; i < 4; i++) {
		tstats[i][0] = 0;	/* count */
		tstats[i][1] = 0;	/* sum */
		tstats[i][2] = 99999;	/* min */
		tstats[i][3] = 0;	/* max */
	}
#endif

	/* main loop - never ends, send signal to exit */
	for(;;) {
		code = Thermo433_waitAnyCode();

#ifdef THERMO433_INCLUDE_TIMING_STATS
		Thermo433_getTimingStats(&stat_sync, stat_pbuf);
#endif

		getTimestamp(tstring);
		convertBin36(code, bincode);
		xorok = Thermo433_decodeValues(code, &ch, &bat,
			&temp, &humid, &tdir);
		/* NOTE: when below is combined into one line, segfault! */
		printf("%s  code = 0x%09llX , ", tstring, code);
		printf("%s , %1d , T: %+.1lf C %c , H: %d %% , ", bincode, ch,
		       temp * 0.1, tdir < 0 ? '!' : trend[tdir], humid + 100);
		printf("%s %c\n", bat ? "B" : "b", xorok ? 'C' : 'e');
	
#ifdef THERMO433_INCLUDE_TIMING_STATS
		/* statistics */	
		if (stat_sync < tstats[THERMO433_PULSE_TYPE_SYNC][2])
			tstats[THERMO433_PULSE_TYPE_SYNC][2] = stat_sync;
		if (stat_sync > tstats[THERMO433_PULSE_TYPE_SYNC][3])
			tstats[THERMO433_PULSE_TYPE_SYNC][3] = stat_sync;
		tstats[THERMO433_PULSE_TYPE_SYNC][1] += stat_sync;
		tstats[THERMO433_PULSE_TYPE_SYNC][0]++;
		for(i = 0; i < THERMO_PULSES; i++) {
			pi = Thermo433_classifyPulse(stat_pbuf[i]);
			if (stat_pbuf[i] < tstats[pi][2])
				tstats[pi][2] = stat_pbuf[i];
			if (stat_pbuf[i] > tstats[pi][3])
				tstats[pi][3] = stat_pbuf[i];
			tstats[pi][1] += stat_pbuf[i];
			tstats[pi][0]++;
		}
		
#endif
	}
}
