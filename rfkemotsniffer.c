#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>

#include <wiringPi.h>

/* uncomment to get statistics */
#define INCLUDE_KEMOT_TIMING_STATS

#define KEMOT_BITS		24		/* bits per transmission */
#define KEMOT_PULSES		(KEMOT_BITS << 1) /* bits per transmission */
#define KEMOT_MIN_RETRANS	3		/* code retransmissions */
#define KEMOT_MIN_SYNC_TIME	9500		/*  sync time in us */
#define KEMOT_MAX_SYNC_TIME	9800
#define KEMOT_MIN_SHORT_TIME	250		/*  short time in us */
#define KEMOT_MAX_SHORT_TIME	400	
#define KEMOT_MIN_LONG_TIME	850		/*  long time in us */
#define KEMOT_MAX_LONG_TIME	1000

#define KEMOT_SYSID_MASK	0x00554000	/* DIP switch address mask */
#define KEMOT_A_MASK		0x00001000	/* Remote A-D mask */
#define KEMOT_B_MASK		0x00000400
#define KEMOT_C_MASK		0x00000100
#define KEMOT_D_MASK		0x00000040
#define KEMOT_ON_MASK		0x00000004	/* Buttons mask */
#define KEMOT_OFF_MASK		0x00000001

#ifdef INCLUDE_KEMOT_TIMING_STATS
#include <signal.h>
#endif

/* volatile keeps variables in memory for interrupts */
static struct timeval tstart;
static volatile unsigned long tsprev, code;
static unsigned long tsbuf[KEMOT_PULSES];	/* 2 * bit timing (high, low)*/
static volatile int incode, pulscount;
#ifdef INCLUDE_KEMOT_TIMING_STATS
static volatile int gotbrk;
static unsigned long stat_sync;			/* for statistic purposes */
static unsigned long stat_buf[KEMOT_PULSES];	/* for statistic purposes */
#endif

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s {gpio}\n\n", progname);
	puts("Where:");
	puts("\tgpio\t - GPIO pin with external RF receiver data (mandatory)");
}

/* ************* */
/* * Interrupt * */
/* ************* */

void handleGpioInt(void)
{
	struct timeval t;
	unsigned long tscur, tsdiff, tmpcode;
	int i;

	gettimeofday(&t, NULL);

	tscur = (t.tv_sec - tstart.tv_sec) * 1000000 + t.tv_usec - tstart.tv_usec;
	tsdiff = tscur - tsprev;
	if (tsdiff > KEMOT_MIN_SYNC_TIME && tsdiff < KEMOT_MAX_SYNC_TIME) {
		/* probably end of sync period detected */
		/* next call may be start of high+low encoded bits */
		incode = 1;
		pulscount = 0;
#ifdef INCLUDE_KEMOT_TIMING_STATS
		stat_sync = tsdiff;	/* for statistic */
#endif
	} else if (incode) {
		/* code transmission - expect high+low bits */
		if (pulscount < KEMOT_PULSES) {
			if ((tsdiff > KEMOT_MIN_SHORT_TIME && tsdiff < KEMOT_MAX_SHORT_TIME) || \
			    (tsdiff > KEMOT_MIN_LONG_TIME && tsdiff < KEMOT_MAX_LONG_TIME))
				tsbuf[pulscount++] = tsdiff; /* valid 0 or 1 */
			else 
				incode = 0;	/* noise, discard code */
		} else {
		/* code completed, decode */
			tmpcode = 0;
			for(i = 0; i < pulscount; i += 2) {
				if (tsbuf[i] > KEMOT_MIN_SHORT_TIME && \
				    tsbuf[i] < KEMOT_MAX_SHORT_TIME && \
				    tsbuf[i + 1] > KEMOT_MIN_LONG_TIME && \
				    tsbuf[i + 1] < KEMOT_MAX_LONG_TIME)
					tmpcode = tmpcode << 1;
				else if (tsbuf[i] > KEMOT_MIN_LONG_TIME && \
					 tsbuf[i] < KEMOT_MAX_LONG_TIME && \
					 tsbuf[i + 1] > KEMOT_MIN_SHORT_TIME && \
					 tsbuf[i + 1] < KEMOT_MAX_SHORT_TIME)
					tmpcode = (tmpcode << 1) | 1;
				else {
					/* high+low sequence unknown, noise */
					incode = 0;
					break;
				}
			}

			if (incode && tmpcode) {
				code = tmpcode;
				incode = 0;
#ifdef INCLUDE_KEMOT_TIMING_STATS
				/* for statistic */
				memcpy(stat_buf, tsbuf, sizeof(tsbuf));
#endif
			}
		}
	}
	tsprev = tscur;
}

#ifdef INCLUDE_KEMOT_TIMING_STATS
/* Intercept TERM and INT signals */
void signalQuit(int sig)
{
	gotbrk = 1;
}
#endif

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

/* Decode RF keycode */
int decodeKeyData(unsigned long code, int *systemid,
		   int *deviceid, int *button)
{
	unsigned int s;

	s = (code & KEMOT_SYSID_MASK) >> 14;
	*systemid = (s & 0x1) + ((s & 0x4) >> 1) + ((s & 0x10) >> 2) + \
		    ((s & 0x40) >> 3) + ((s & 0x100) >> 4);

	if (!(code & KEMOT_A_MASK))
		*deviceid = 0;
	else if (!(code & KEMOT_B_MASK))
		*deviceid = 1;
	else if (!(code & KEMOT_C_MASK))
		*deviceid = 2;
	else if (!(code & KEMOT_D_MASK))
		*deviceid = 3;
	else {
		*deviceid = -1;
		*button = -1;
		return 0;
	}

	if (!(code & KEMOT_OFF_MASK))
		*button = 0;
	else if (!(code & KEMOT_ON_MASK))
		*button = 1;
	else {
		*button = -1;
		return 0;
	}

	return 1;
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int gpio;
	char bincode[33];
	int rfsysid, rfdevid, rfbtn;

#ifdef INCLUDE_KEMOT_TIMING_STATS
	struct sigaction sa;
	int i;
	unsigned long ncodes, nshorts, nlongs;
	unsigned long tsync_sum, tlong_sum, tshort_sum;	
	unsigned int tsync_min, tsync_avg, tsync_max;
	unsigned int tlong_min, tlong_avg, tlong_max;
	unsigned int tshort_min, tshort_avg, tshort_max;
#endif

	/* show help */
	if (argc < 2) {
		help(argv[0]);
		exit(0);
	}

	/* get parameters */
	sscanf(argv[1], "%d", &gpio);

#ifdef INCLUDE_KEMOT_TIMING_STATS
	/* install CRTL+C to display stats at the end */
	gotbrk = 0;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &signalQuit;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
#endif

	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();

	pinMode(gpio, INPUT);

	printf("Starting to capture %d-bit codes from RF receiver connected to GPIO pin %d ...\n",
	       KEMOT_BITS, gpio);

	gettimeofday(&tstart, NULL);
	tsprev = 0;
	wiringPiISR(gpio, INT_EDGE_BOTH, handleGpioInt);

#ifdef INCLUDE_KEMOT_TIMING_STATS
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
#endif

	/* main loop - never ends, send signal to exit */
	for(;;) {
#ifdef INCLUDE_KEMOT_TIMING_STATS
		if (gotbrk)
			break;
#endif
		if (code) {
			if (!(code & 0xaaaaaaaaL)) {
				convertBin32(code, bincode);
				decodeKeyData(code, &rfsysid, &rfdevid, &rfbtn);
				printf("code = %lu , 0x%08X , %s , %d : %c : %s\n", code, code, bincode, rfsysid, 'A' + rfdevid, rfbtn ? "ON" : "OFF");
	
#ifdef INCLUDE_KEMOT_TIMING_STATS
				/* statistics */	
				if (stat_sync < tsync_min)
					tsync_min = stat_sync;
				if (stat_sync > tsync_max)
					tsync_max = stat_sync;
				tsync_sum += stat_sync;
				for(i = 0; i < KEMOT_PULSES; i++) {
					if (stat_buf[i] > KEMOT_MIN_SHORT_TIME && \
					    stat_buf[i] < KEMOT_MAX_SHORT_TIME) {
						tshort_sum += stat_buf[i];
						if (stat_buf[i] < tshort_min)
							tshort_min = stat_buf[i];
						if (stat_buf[i] > tshort_max)
							tshort_max = stat_buf[i];
						nshorts++;
					}
					else if (stat_buf[i] > KEMOT_MIN_LONG_TIME &&
						 stat_buf[i] < KEMOT_MAX_LONG_TIME) {
							tlong_sum += stat_buf[i];
							if (stat_buf[i] < tlong_min)
								tlong_min = stat_buf[i];
							if (stat_buf[i] > tlong_max)
								tlong_max = stat_buf[i];
							nlongs++;
					}
				}
				ncodes++;
#endif
			}
			code = 0;
		}
	}

#ifdef INCLUDE_KEMOT_TIMING_STATS
	if (gotbrk && ncodes && nshorts && nlongs ) {
		tsync_avg = tsync_sum / ncodes;
		tshort_avg = tshort_sum / nshorts;
		tlong_avg = tlong_sum / nlongs;
		printf("Sync time (min/avg/max): %d/%d/%d microseconds\n",
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
	}
#endif
}
