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

#include "power433_lib.h"

#define OWNER_UID	500
#define OWNER_GID	500

#ifdef POWER433_INCLUDE_TIMING_STATS
#include <signal.h>
static unsigned long ncodes, nshorts, nlongs;
static unsigned long tsync_sum, tlong_sum, tshort_sum;
static unsigned int tsync_min, tsync_avg, tsync_max;
static unsigned int tlong_min, tlong_avg, tlong_max;
static unsigned int tshort_min, tshort_avg, tshort_max;
#endif

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s {gpio}\n\n", progname);
	puts("Where:");
	puts("\tgpio\t - GPIO pin with external RF receiver data (mandatory)");
}

/* Convert 32-bit unsigned to binary */
void convertBin32(int a, char *s)
{
	unsigned int i, mask;

	mask = 1 << 31;
	for(i = 0; i < 32; i++) {
		s[i] = ( a & mask ) ? '1' : '0';
		mask = mask >> 1;
	}
	s[32] = 0;
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

/* Update min, max, sum and length for pulse array*/
void getPulseMinMaxSum(unsigned long a[], int len, unsigned int *minshort,
		       unsigned int *maxshort, unsigned long *sumshort,
		       unsigned long *nshort, unsigned int *minlong,
		       unsigned int *maxlong, unsigned long *sumlong,
		       unsigned long *nlong)
{
	int i, pt;

	for(i = 0; i < len; i++) {
		pt = Power433_classifyPulse(a[i]);
		if (pt == POWER433_PULSE_TYPE_SHORT) {
			if (a[i] < *minshort)
				*minshort = a[i];
			if (a[i] > *maxshort)
				*maxshort = a[i];
			*sumshort += a[i];
			(*nshort)++;
		}
		else if (pt == POWER433_PULSE_TYPE_LONG) {
			if (a[i] < *minlong)
				*minlong = a[i];
			if (a[i] > *maxlong)
				*maxlong = a[i];
			*sumlong += a[i];
			(*nlong)++;
		}
	}
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

#ifdef POWER433_INCLUDE_TIMING_STATS
/* Intercept TERM and INT signals */
void signalQuit(int sig)
{
	if (ncodes && nshorts && nlongs) {
		tsync_avg = tsync_sum / ncodes;
		tshort_avg = tshort_sum / nshorts;
		tlong_avg = tlong_sum / nlongs;
		printf("\nSync time (min/avg/max): %d/%d/%d microseconds\n",
		       tsync_min, tsync_avg, tsync_max);
		printf("Short time (min/avg/max): %d/%d/%d microseconds\n",
		       tshort_min, tshort_avg, tshort_max);
		printf("Long time (min/avg/max): %d/%d/%d microseconds\n",
		       tlong_min, tlong_avg, tlong_max);
		printf("Average short + long time: %d microseconds\n",
		       tshort_avg + tlong_avg);
		printf("Average long / short ratio: %.3lf\n",
		       (double)tlong_avg / tshort_avg);
		printf("Average sync / short ratio: %.3lf\n",
		       (double)tsync_avg / tshort_avg);
	} else if (!ncodes)
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
	unsigned int code;
	char bincode[33], tstring[25];
	int rfsysid, rfdevid, rfbtn;

#ifdef POWER433_INCLUDE_TIMING_STATS
	struct sigaction sa;
	unsigned long stat_sync;                 /* for statistic purposes */
	unsigned long stat_pbuf[POWER433_PULSES];	
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
	Power433_init(-1, gpio);

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
	       POWER433_BITS, gpio);

#ifdef POWER433_INCLUDE_TIMING_STATS
	/* install CRTL+C to display stats at the end */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &signalQuit;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	/* for statistics */
	ncodes = 0;
	nshorts = 0;
	nlongs = 0;
	tsync_min = 99999;
	tlong_min = 99999;
	tshort_min = 99999;
	tsync_max = 0;
	tlong_max = 0;
	tshort_max = 0;
	tsync_sum =0;
	tlong_sum =0;
	tshort_sum =0;
#endif

	/* main loop - never ends, send signal to exit */
	for(;;) {
		code = Power433_waitCode();

#ifdef POWER433_INCLUDE_TIMING_STATS
		Power433_getTimingStats(&stat_sync, stat_pbuf);
#endif
		getTimestamp(tstring);
		convertBin32(code, bincode);
		Power433_decodeCommand(code, &rfsysid, &rfdevid, &rfbtn);
		printf("%s  code = %lu , 0x%08X , %s , %d : %s%s%s%s%s : %s\n",
		       tstring, code, code, bincode, rfsysid,
		       rfdevid & POWER433_DEVICE_A ? "A" : "",
		       rfdevid & POWER433_DEVICE_B ? "B" : "",
		       rfdevid & POWER433_DEVICE_C ? "C" : "",
		       rfdevid & POWER433_DEVICE_D ? "D" : "",
		       rfdevid & POWER433_DEVICE_E ? "E" : "",
		       rfbtn ? "ON" : "OFF");
	
#ifdef POWER433_INCLUDE_TIMING_STATS
		/* statistics */	
		if (stat_sync < tsync_min)
			tsync_min = stat_sync;
		if (stat_sync > tsync_max)
			tsync_max = stat_sync;
		tsync_sum += stat_sync;
		getPulseMinMaxSum(stat_pbuf, POWER433_PULSES, &tshort_min,
				  &tshort_max, &tshort_sum, &nshorts,
				  &tlong_min, &tlong_max, &tlong_sum, &nlongs);
		ncodes++;
#endif
	}
}
