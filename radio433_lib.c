/*
 * ***************************************************
 *  This library combines power433 and thermo433 into
 *  common radio packet transmitter/analyzer
 * ***************************************************
 */

/*
 * Receiver call flow:
 *
 * GPIO Interrupt (handleGpioInt())
 *   - performs very simple packet timing analysis
 *   - fills timing ring buffer
 *   - raises 'timingready' semaphore
 * Code analyzer thread (codeAnalyzerThread())
 *   - woken by ISR via 'timingready' semaphore
 *   - performs packet classification of data in ring buffer
 *   - if valid packet, raise 'codeready' semaphore
 * User functions that waits for raw code (Radio433_getCode() family)
 *   - sleeps waiting for 'codeready' semaphore
 *   - determine code type and return raw code value
 * External user function that decodes information
 *   - based on code type, retrieve information (command, temp/humid etc.)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <semaphore.h>
#include <pthread.h>

#include <wiringPi.h>

#include "radio433_lib.h"

/* coding schema */
#define RADIO433_CODING_HIGHLOW	0
#define RADIO433_CODING_LOWVAR	1

/* ring buffer size for timings */
#define RADIO433_RING_BUFFER_ENTRIES	32

/* noise detection - smaller spikes will not affect pulse recording */
#define RADIO433_MAX_NOISE_TIME		105	/* noise time in us */

/* device info structures */
struct deviceDesc {
	int type;		/* device */
	int coding;		/* coding schema (determines pulse_data) */
	int bits;		/* bits per transmissions */
	int interval;		/* interval between transmissions (msec) */
	int repeats;		/* code repeats in single transmission */
	void *pulse_data;	/* pulse data */
};

/*	  __   _
 *	 |  | | |
 * ______|  |_| |__|
 *  Sync : 1  :  0 :
 *
 * Constant length, bits encoded by high/low ratio.
 */
struct highlowPulseDesc {
	int pulse_sync;		/* sync time length (us) - low pulse */
	int pulse_sync_min, pulse_sync_max;
	int pulse_short;	/* short pulse length (us) */
	int pulse_short_min, pulse_short_max;
	int pulse_long;		/* long pulse length (us) */
	int pulse_long_min, pulse_long_max;
};
#define STRUCT_HIGHLOW_PTR(s)	((struct highlowPulseDesc *)(s))

/*	  _	 _
 *	 | |	| |
 * ______| |____| |__|
 *  Sync : : 1  : : 0:
 *
 * Variable length, bits encoded by low timing.
 */
struct lowvarPulseDesc {
	int pulse_sync;		/* sync time length (us) - low pulse */
	int pulse_sync_min, pulse_sync_max;
	int pulse_high;		/* high pulse length (us) */
	int pulse_high_min, pulse_high_max;
	int pulse_low_short;	/* low short pulse length (us) */
	int pulse_low_short_min, pulse_low_short_max;
	int pulse_low_long;	/* low long pulse length (us) */
	int pulse_low_long_min, pulse_low_long_max;
};
#define STRUCT_LOWVAR_PTR(s)	((struct lowvarPulseDesc *)(s))

/* timing data for devices */
static struct highlowPulseDesc kemotPulse = {
	9700, 9500, 9800,
	 300, 170, 450,
	 900, 800, 1100,
};

static struct lowvarPulseDesc hyuwsPulse = {
	8800, 7200, 9200,
	 500, 300, 700,
	2000, 1700, 2300,
	4000, 3700, 4400
};

static struct deviceDesc tDevInfo[RADIO433_DEVICES] = {
	{ RADIO433_DEVICE_KEMOTURZ1226,
	  RADIO433_CODING_HIGHLOW,
	  24,
	  0,
	  3,
	  (void*)&kemotPulse
	},
	{ RADIO433_DEVICE_HYUWSSENZOR77TH,
	  RADIO433_CODING_LOWVAR,
	  36,
	  33000,
	  4,
	  (void*)&hyuwsPulse
	}
};

/*
 * ****************
 * Global variables
 * ****************
 */
/* (volatile keeps selected variables in memory for interrupts) */
struct timingBuf {
	struct timeval timestamp;
	int pulses;
	unsigned long codetime;	/* total code length in us */
	unsigned long synctime;
	unsigned long *timbuf;
} tbuf[RADIO433_RING_BUFFER_ENTRIES];
struct codeBuf {
	struct timeval timestamp;
	int devidx;		/* index in tDevInfo array */
	unsigned long codetime;	/* total code length in us */
	unsigned long long code;
} cbuf[RADIO433_RING_BUFFER_ENTRIES];
static volatile int tri, twi, cri, cwi;	/* read and write buffer idx */
static volatile int synctmin, synctmax;	/* time range: sync pulses */
static volatile int npulsemin, npulsemax;  /* qty range: non-sync pulses */
static volatile int pulsetmin, pulsetmax;  /* time range: non-sync pulses */
static volatile int codetmin, codetmax; /* time range: codes */
static volatile unsigned long tsprev, tclen;
static volatile int incode, pulscount;
static int txgpio, rxgpio;
static struct timeval tstart;
static sem_t timingready, codeready;
static pthread_t codeanalyzer;

/*
 *  ******************
 *  Internal functions
 *  ******************
 */

/* Set global variables used mainly by ISR to speed-up */
/* signal analysis (although they are used elsewhere also)*/
static void Radio433_setTimingVars(void)
{
	int i, np, smin, smax, ptmin, ptmax, ctmin, ctmax;
	struct deviceDesc *td;

	/* min/max values for ISR */
	synctmin = 99999;
	synctmax = 0;
	npulsemin = 99999;
	npulsemax = 0;
	pulsetmin = 99999;
	pulsetmax = 0;
	codetmin = 99999;
	codetmax = 0;
	/* find min and max values for all codes (ISR speedup) */
	for(i = 0; i < RADIO433_DEVICES; i++) {
		td = &tDevInfo[i];
		if (td->coding == RADIO433_CODING_HIGHLOW) {
			np = td->bits << 1;	/* 2 * bits (no sync) */
			smin = STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_sync_min;
			smax = STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_sync_max;
			ptmin = STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_short_min;
			ptmax = STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_long_max;
			ctmin = smin + td->bits * \
				(STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_long_min + \
				STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_short_min);
			ctmax = smax + td->bits * \
				(STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_long_max + \
				STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_short_max);
		} else if (td->coding == RADIO433_CODING_LOWVAR) {
			np = td->bits << 1;	/* 2 * bits (no sync) */
			smin = STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_sync_min;
			smax = STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_sync_max;
			ptmin = STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_high_min;
			ptmax = STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_low_long_max;
			ctmin = smin + td->bits * \
				(STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_high_min + \
				STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_low_short_min);
			ctmax = smax + td->bits * \
				(STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_high_max + \
				STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_low_long_max);
		}
		if (synctmin > smin)
			synctmin = smin;
		if (synctmax < smax)
			synctmax = smax;
		if (npulsemin > np)
			npulsemin = np;
		if (npulsemax < np)
			npulsemax = np;
		if (pulsetmin > ptmin)
			pulsetmin = ptmin;
		if (pulsetmax < ptmax)
			pulsetmax = ptmax;
		if (codetmin > ctmin)
			codetmin = ctmin;
		if (codetmax < ctmax)
			codetmax = ctmax;
	}
}

/*
 * ****************
 * Public functions
 * ****************
 */

static void handleGpioInt(void);
static void *codeAnalyzerThread(void *);

/* Initialize library */
int Radio433_init(int tx_gpio, int rx_gpio)
{
	int i;

	/* transmission GPIO pin */
	if (tx_gpio >=0 ) {
		txgpio = tx_gpio;
		pinMode(txgpio, OUTPUT);
	}
	/* receiving GPIO pin */
	if (rx_gpio >= 0) {
		rxgpio = rx_gpio;
		gettimeofday(&tstart, NULL);
		/* initialize variables for ISR */
		tsprev = 0;
		pulscount = 0;
		tclen = 0;
		incode = 0;
		Radio433_setTimingVars();
		/* initialize buffers */
		tri = 0;
		twi = 0;
		cri = 0;
		cwi = 0;
		memset(tbuf, 0, sizeof(tbuf));
		memset(cbuf, 0, sizeof(cbuf));
		for(i = 0; i < RADIO433_RING_BUFFER_ENTRIES; i++)
			tbuf[i].timbuf = (unsigned long*)malloc(npulsemax * sizeof(unsigned long));
		/* initialize system structures */
		if (pthread_create(&codeanalyzer, NULL, codeAnalyzerThread, NULL))
			return -1;
		pinMode(rxgpio, INPUT);
		wiringPiISR(rxgpio, INT_EDGE_BOTH, handleGpioInt);
		sem_init(&timingready, 0, 0);
		sem_init(&codeready, 0, 0);
	}
	return 0;
}

/* Get code */
unsigned long long Radio433_getCode(struct timeval *ts,
				    int *type, int *bits)
{
	struct codeBuf *cb;

	sem_wait(&codeready);
	cb = &cbuf[cri];
	if (ts)
		memcpy(ts, &cb->timestamp, sizeof(struct timeval));
	if (type)
		*type = tDevInfo[cb->devidx].type;
	if (bits)
		*bits = tDevInfo[cb->devidx].bits;
	cri = (cri + 1) % RADIO433_RING_BUFFER_ENTRIES;
	return cb->code;
}

/* Get code with extended information */
unsigned long long Radio433_getCodeExt(struct timeval *ts,
				       int *type, int *bits, int *codetime,
				       int *repeats, int *interval)
{
	struct codeBuf *cb;

	sem_wait(&codeready);
	cb = &cbuf[cri];
	if (ts)
		memcpy(ts, &cb->timestamp, sizeof(struct timeval));
	if (type)
		*type = tDevInfo[cb->devidx].type;
	if (bits)
		*bits = tDevInfo[cb->devidx].bits;
	if (codetime)
		*codetime = (cb->codetime + 999) / 1000;	/* us to ms */
	if (repeats)
		*repeats = tDevInfo[cb->devidx].repeats;
	if (interval)
		*interval = tDevInfo[cb->devidx].interval;
	cri = (cri + 1) % RADIO433_RING_BUFFER_ENTRIES;
	return cb->code;
}

/* Transmit timed pulses from an array pulses[len] beginning with starthigh key */
int Radio433_pulseCode(const unsigned long *pulses, int len, int starthigh)
{
	int i, p;

	if (txgpio < 0)
		return -1;

	if (len < 0 || starthigh < 0)
		return -2;

	p = starthigh ? 1 : 0;

	for (i = 0; i < len; i++) {
		digitalWrite(txgpio, p);
		delayMicroseconds(pulses[i]);
		p = 1 - p;
	}

	return 0;
}

/* Send raw code (can be any length up to 64 bits) */
int Radio433_sendRawCode(unsigned long long code, int coding, int bits, int repeats)
{
	int i, j, txlen;
	unsigned long long codemask;
	unsigned long *txbuf;
	unsigned long eotpulse;

	if (txgpio < 0)
		return -1;

	if (bits < 0 || bits > (sizeof(unsigned long long) << 3) || repeats <= 0)
		return -2;

	/* construct timing table */
	txlen = 2 + (bits << 1);	/* sync + 2 * bits */
	txbuf = (unsigned long*)malloc(txlen);
	codemask = 1 << ( bits - 1);
	if (coding == RADIO433_CODING_HIGHLOW) {
		/* sync */
		txbuf[0] = kemotPulse.pulse_short;
		txbuf[1] = kemotPulse.pulse_sync;
		/* data bits: high-low */
		for (i = 0; i < bits << 1; i += 2) {
			if (code & codemask) {
				txbuf[2 + i] = kemotPulse.pulse_long;
				txbuf[3 + i] = kemotPulse.pulse_short;
			} else {
				txbuf[2 + i] = kemotPulse.pulse_short;
				txbuf[3 + i] = kemotPulse.pulse_long;
			}
			codemask >>= 1;
		}
		eotpulse = kemotPulse.pulse_short;
	} else if (coding == RADIO433_CODING_LOWVAR) {
		/* sync */
		txbuf[0] = hyuwsPulse.pulse_high;
		txbuf[1] = hyuwsPulse.pulse_sync;
		/* data bits: high-low */
		for (i = 0; i < bits << 1; i += 2) {
			txbuf[2 + i] = hyuwsPulse.pulse_high;
			if (code & codemask)
				txbuf[3 + i] = hyuwsPulse.pulse_low_long;
			else
				txbuf[3 + i] = hyuwsPulse.pulse_low_short;
			codemask >>= 1;
		}
		eotpulse = hyuwsPulse.pulse_high;
	} else {
		free(txbuf);
		return -3;
	}

	/* generate code */
	for (j = 0; j < repeats; j++)
		Radio433_pulseCode(txbuf, txlen, 1);

	/* code sequence always ends with low signal which may last
	   for unknown length - till nearest noise peak, so pulse it
	   to end within predefined timing */
	digitalWrite(txgpio, HIGH);
	delayMicroseconds(eotpulse);
	digitalWrite(txgpio, LOW);
	delayMicroseconds(eotpulse);

	free(txbuf);
	return 0;
}

/* Send device-specific code (repeats == 0 - use default number of packets) */
int Radio433_sendDeviceCode(unsigned long long code, int type, int repeats)
{
	int i;

	if (txgpio < 0)
		return -1;

	if (repeats < 0 || type < 0)
		return -2;

	/* find device description */
	for (i = 0; i < RADIO433_DEVICES; i++)
		if (tDevInfo[i].type == type)
			break;

	/* device not defined, exit */
	if (i == RADIO433_DEVICES)
		return -3;

	return Radio433_sendRawCode(code, tDevInfo[i].coding, tDevInfo[i].bits,
				    repeats ? repeats : tDevInfo[i].repeats);
}

/*
 * *********************
 * Code analyzing thread
 * *********************
 */

static void *codeAnalyzerThread(void *arg)
{
	/*
	 * Pulse to code translation:
	 * 0. Each device starts with metric 0.
	 * 1. For correct sync pulse, metric of device is increased by
	 *    (npulsemax + 1), so it is not possible to choose correct
	 *    metric without good sync sequence.
	 * 2a. For each consecutive correct pulse, device gets +1 to metric.
	 * 2b. If pulse timing is wrong, device metric resets to 0.
	 * 3. After all pulses are processed, device with highest metric
	 *    is chosen. This promotes longest correctly-timed code.
	 */

	int i, j, bp, mmax, dmax;
	int metric[RADIO433_DEVICES];
	unsigned long long tmpcode[RADIO433_DEVICES];
	struct deviceDesc *td;
	struct timingBuf *tb;
	struct codeBuf *cb;

	/* endless loop, sleeps on semaphore */
	for(;;) {
		sem_wait(&timingready);
		/* timing available - process it */
		tb = &tbuf[tri];
		cb = &cbuf[cwi];
		memset(metric, 0, sizeof(metric));
		memset(tmpcode, 0, sizeof(tmpcode));
		/* check timing */
		for(i = 0; i < RADIO433_DEVICES; i++) {
			td = &tDevInfo[i];
			if (td->coding == RADIO433_CODING_HIGHLOW) {
				/* HIGH-LOW coding, 2 pulses per bit */
				bp = td->bits << 1;
				if (tb->pulses < bp)
					continue;
				if (tb->synctime >=
					STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_sync_min
				    && tb->synctime <=
					STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_sync_max)
					metric[i] += npulsemax + 1;
				for(j = 0; j < bp; j += 2)
					if (tb->timbuf[j] >=
						STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_short_min
					    && tb->timbuf[j] <=
						STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_short_max
					    && tb->timbuf[j+1] >=
						STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_long_min
					    && tb->timbuf[j+1] <=
						STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_long_max) {
						tmpcode[i] <<= 1;
						metric[i] += 2;
					} else if (tb->timbuf[j] >=
							STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_long_min
						   && tb->timbuf[j] <=
							STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_long_max
						   && tb->timbuf[j+1] >=
							STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_short_min
						   && tb->timbuf[j+1] <=
							STRUCT_HIGHLOW_PTR(td->pulse_data)->pulse_short_max) {
						tmpcode[i] = (tmpcode[i] << 1) | 1;
						metric[i] += 2;
					} else {
						metric[i] = 0;
						break;
					}
			} else if (td->coding == RADIO433_CODING_LOWVAR) {
				/* LOW VARIABLE coding, 1 start pulse
				   plus 1 data pulse per bit */
				bp = td->bits << 1;
				if (tb->pulses < bp)
					continue;
				if (tb->synctime >= STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_sync_min
				    && tb->synctime <= STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_sync_max)
					metric[i] += npulsemax + 1;
				for(j = 0; j < bp; j += 2)
					if (tb->timbuf[j] >=
						STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_high_min
					    && tb->timbuf[j] <=
						STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_high_max
					    && tb->timbuf[j+1] >=
						STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_low_short_min
					    && tb->timbuf[j+1] <=
						STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_low_short_max) {
						tmpcode[i] <<= 1;
						metric[i] += 2;
					} else if (tb->timbuf[j] >=
							STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_high_min
						   && tb->timbuf[j] <=
							STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_high_max
						   && tb->timbuf[j+1] >=
							STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_low_long_min
						   && tb->timbuf[j+1] <=
							STRUCT_LOWVAR_PTR(td->pulse_data)->pulse_low_long_max) {
						tmpcode[i] = (tmpcode[i] << 1) | 1;
						metric[i] += 2;
					} else {
						metric[i] = 0;
						break;
					}
			}
		}
		/* find longest correct code */
		mmax = 0;
		dmax = -1;
		for(i = 0; i < RADIO433_DEVICES; i++)
			if (metric[i] > mmax) {
				mmax = metric[i];
				dmax = i;
			}
		if (dmax >= 0) {
			/* signal that code is OK and ready */
			cb->devidx = dmax;
			cb->codetime = tb->codetime;
			cb->code = tmpcode[dmax];
			memcpy(&cb->timestamp, &tb->timestamp,
			       sizeof(struct timeval));
			sem_post(&codeready);
			cwi = (cwi + 1) % RADIO433_RING_BUFFER_ENTRIES;
		}
		tri = (tri + 1) % RADIO433_RING_BUFFER_ENTRIES;
	}
}

/*
 * *********
 * Interrupt
 * *********
*/

/* Keep it fast and simple, code analysis is performed in separate thread */
static void handleGpioInt(void)
{
	struct timeval t;
	unsigned long tscur, tsdiff;
	struct timingBuf *tptr;

	gettimeofday(&t, NULL);

	tscur = (t.tv_sec - tstart.tv_sec) * 1000000 + t.tv_usec - tstart.tv_usec;
	tsdiff = tscur - tsprev;
	if (tsdiff >= synctmin && tsdiff <= synctmax) {
		/* probably end of sync period detected */
		/* next call may be start of high+low encoded bits */
		if (incode && pulscount >= npulsemin && pulscount <= npulsemax
		    && tclen >= codetmin && tclen <= codetmax) {
			/* if we were 'incode', mark it as complete */
			sem_post(&timingready);
			twi = (twi + 1) % RADIO433_RING_BUFFER_ENTRIES;
		}
		incode = 1;
		tptr = &tbuf[twi];
		memcpy(&tptr->timestamp, &t, sizeof(struct timeval));
		tptr->pulses = 0;
		tptr->synctime = tsdiff;
		tptr->codetime = 0;
		pulscount = 0;
		tclen = tsdiff;
	} else if (incode && tsdiff > RADIO433_MAX_NOISE_TIME) {
		/* code transmission - expect high and low */
		if (pulscount < npulsemax && tsdiff >= pulsetmin
		    && tsdiff <= pulsetmax) {
			/* capture in progress (until noise or max length ) */
			tptr = &tbuf[twi];
			tptr->pulses++;
			tptr->timbuf[pulscount++] = tsdiff;
			tclen += tsdiff;
			tptr->codetime = tclen;
		} else {
			if (tclen >= codetmin && tclen <= codetmax) {
				/* code completed */
				sem_post(&timingready);
				twi = (twi + 1) % RADIO433_RING_BUFFER_ENTRIES;
			}
			/* we're done, code OK or too much noise */
			incode = 0;
		}
	}
	tsprev = tscur;
}
