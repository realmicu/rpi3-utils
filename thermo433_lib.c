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

#include <wiringPi.h>

#include "thermo433_lib.h"

/* number for supported devices */
#define THERMO433_DEVICES	1

/* coding schema */
#define THERMO433_CODING_HYUNDAI	0

/* noise detection */
#define THERMO433_MAX_NOISE_TIME	100	/* noise time in us */

/* device info structures */
struct thermoDesc {
	int type;		/* sensor type */	
	int coding;		/* coding schema */
	int bits;		/* bits per transmissions */
	int interval;		/* interval between transmissions (sec) */
	int repeats;		/* code repeats in single transmission */
	void *pulse_data;	/* pulse data */
};

struct hyundaiPulseDesc {
	int pulse_sync;		/* sync time length (us) - low pulse */
	int pulse_sync_min, pulse_sync_max;
	int pulse_high;		/* high pulse length (us) */
	int pulse_high_min, pulse_high_max;
	int pulse_low_short;	/* low short pulse length (us) */
	int pulse_low_short_min, pulse_low_short_max;
	int pulse_low_long;	/* low long pulse length (us) */
	int pulse_low_long_min, pulse_low_long_max;
};

/* timing data for devices */
static struct hyundaiPulseDesc hyuwsPulse = {
	8800, 7200, 9200,
	 500, 300, 700,
	2000, 1700, 2300,
	4000, 3700, 4400
};

static struct thermoDesc tDevInfo[THERMO433_DEVICES] = {
	{ THERMO433_DEVICE_HYUWSSENZOR77TH,
	  THERMO433_CODING_HYUNDAI,
	  36,
	  32,
	  4,
	  (void*)&hyuwsPulse
	}
};

/* global variables */
/* (volatile keeps variables in memory for interrupts) */
static int txgpio, rxgpio;
static struct timeval tstart;
static volatile unsigned long tsprev, tclen;
static volatile unsigned long long code;	/* must contain 36-bit value */
static unsigned long *tsbuf;
static int tsblen;
static volatile int incode, pulscount;
static volatile int npulses, maxcodetime;
static sem_t codeready;
struct thermoDesc *td;
struct hyundaiPulseDesc *pd;

#ifdef THERMO433_INCLUDE_TIMING_STATS
static unsigned long stat_sync;                 /* for statistic purposes */
static unsigned long *stat_buf;
#endif

/* ********* */
/* Functions */
/* ********* */

void handleGpioInt(void);

/* Initialize library */
int Thermo433_init(int tx_gpio, int rx_gpio, int type)
{
	/* sensor type */
	if (type < 0 || type > THERMO433_DEVICES)
		return -1;

	/* transmission GPIO pin */
	if (tx_gpio >=0 ) {
		txgpio = tx_gpio;
		pinMode(txgpio, OUTPUT);
	}
	/* receiving GPIO pin */
	if (rx_gpio >= 0) {
		gettimeofday(&tstart, NULL);
		tsprev = 0;
		code = 0;
		rxgpio = rx_gpio;
		td = &tDevInfo[type];
		if (td->coding == THERMO433_CODING_HYUNDAI) {
			pd = (struct hyundaiPulseDesc*)td->pulse_data;
			npulses = td->bits << 1;
			maxcodetime = td->bits * (pd->pulse_high_max + \
			     	      pd->pulse_low_long_max);
		} else
			return -1;
		tsblen = (td->bits * sizeof(unsigned long)) << 1;
		tsbuf = (unsigned long*)malloc(tsblen);
#ifdef THERMO433_INCLUDE_TIMING_STATS
		stat_buf = (unsigned long*)malloc(tsblen);
#endif
		pinMode(rxgpio, INPUT);
		wiringPiISR(rxgpio, INT_EDGE_BOTH, handleGpioInt);
		sem_init(&codeready, 0, 0);
	}
	return 0;
}

/* Verify parity bits for code */
/* 0 (false) if code is corrupted by means of parity */
/* NOTE: for now it is unused as checksum algorithm is unknown */
/*
static int Thermo433_checkParity(unsigned long long val)
{
	unsigned long long tmpval, ec, oc, ep, op;
	int i;

	oc = 0;
	ec = 0;
	*/ /* parity is NOT XOR */ /*
	op = 1 - ((val >> 3) & 1);
	ep = 1 - ((val >> 2) & 1);
	tmpval = val >> 4;
	for (i = 0; i < td->bits - 4; i++) {
		ec ^= tmpval & 1;
		tmpval >>= 1;
		oc ^= tmpval & 1;
		tmpval >>= 1;
	}
	return (oc == op && ec == ep);
} */

/* Retrieve code once, exit with 0 immediately if not available */
/* Code is not validated against CRC etc. */
unsigned long long Thermo433_getAnyCode(void)
{
	unsigned long long tmpcode;
	int sval;

	if (rxgpio < 0)
		return 0;

	/* do not block, decrease semaphore for ISR */
	sem_getvalue(&codeready, &sval);
	if (sval > 0)
		sem_wait(&codeready);

	tmpcode = code;
	code = 0;
	return tmpcode;
}
/* Retrieve code once, exit with 0 immediately if not available or bad */
/* NOTE: code checksum should be validated but it is not */
unsigned long long Thermo433_getCode(void)
{
	return Thermo433_getAnyCode();
}

/* Wait for any code and return only when one is available */
/* Code is not validated against CRC etc. */
unsigned long long Thermo433_waitAnyCode(void)
{
	unsigned long long tmpcode;

	if (rxgpio < 0)
		return 0;

	sem_wait(&codeready);
	tmpcode = code;
	code = 0;
	return tmpcode;
}

/* Wait for code and return only when one is available */
/* NOTE: code checksum should be validated but it is not */
unsigned long long Thermo433_waitCode(void)
{
	return Thermo433_waitAnyCode();
}

/* Decode RF data */
/* Return 0 if not supported */
/* NOTE: code checksum should be validated but it is not */
int Thermo433_decodeValues(unsigned long long val, int *ch, int *bat,
			   int *temp, int *humid, int *tdir)
{
	int i, v;
	unsigned long long mask;
	
	if (td->type == THERMO433_DEVICE_HYUWSSENZOR77TH) {
		if (ch)
			*ch = (val & 0xc0000000L) >> 30;
		if (bat)
			*bat = 1;	/* temporarily battery is OK */
		if (temp) {
			mask = 0x1000;	/* LSB of mask = MSB of data */
			v = 0;
			for(i = 0; i < 12; i++) {
				if (val & mask)
					v = (v << 1) | 1;
				else
					v <<= 1;
				mask <<= 1;
			}
			if (v & 0x800)	/* MSB for 12-bit U2 signals sign */
				v |= ~0xfff;
			*temp = v;	/* divide by 10 to get real value */
		}
		if (humid) {
			mask = 0x10;
			v = 0;
			for(i = 0; i < 8; i++) {
				if (val & mask)
					v = (v << 1) | 1;
				else
					v <<= 1;
				mask <<= 1;
			}
			v |= ~0xff;
			*humid = v;	/* add 100 to get real value */
		}
		if (tdir) {
			v = (val & 0x6000000L) >> 25;
			if (v == 3)
				*tdir = THERMO433_TEMP_TREND_INVALID;
			else
				*tdir = v;
		}
		return 1;
	}
	return 0;
}

/* Read channel, battery status, temperature and humidity */
void Thermo433_waitValues(int *ch, int *bat, int *temp, int *humid, int *tdir)
{
	Thermo433_decodeValues(Thermo433_waitCode(), ch, bat,
			       temp, humid, tdir);
}

/* Classify pulse type based on length in microseconds */
/* 0-sync, 1-short high, 2,3-low (short/long), -1-unclassified/noise */
int Thermo433_classifyPulse(unsigned long microseconds)
{
	int pulsetype;

	pulsetype = THERMO433_PULSE_TYPE_UNKNOWN;

	if (microseconds > pd->pulse_sync_min && \
	    microseconds < pd->pulse_sync_max)
		pulsetype = THERMO433_PULSE_TYPE_SYNC;
	else if (microseconds > pd->pulse_high_min && \
		 microseconds < pd->pulse_high_max)
		pulsetype = THERMO433_PULSE_TYPE_HIGH;
	else if (microseconds > pd->pulse_low_short_min && \
		 microseconds < pd->pulse_low_short_max)
		pulsetype = THERMO433_PULSE_TYPE_LOW_SHORT;
	else if (microseconds > pd->pulse_low_long_min && \
		 microseconds < pd->pulse_low_long_max)
		pulsetype = THERMO433_PULSE_TYPE_LOW_LONG;

	return pulsetype;
}

#ifdef THERMO433_INCLUDE_TIMING_STATS
/* Get timing data for statistics */
int Thermo433_getTimingStats(unsigned long *timesync, unsigned long tpbuf[])
{
	if (timesync)
		*timesync = stat_sync;

	if (tpbuf)
		memcpy(tpbuf, stat_buf, tsblen);

	return tsblen / sizeof(unsigned long);
}
#endif

/* ************* */
/* * Interrupt * */
/* ************* */

void handleGpioInt(void)
{
	struct timeval t;
	unsigned long tscur, tsdiff;
	unsigned long long tmpcode;
	int i, sval;

	gettimeofday(&t, NULL);

	tscur = (t.tv_sec - tstart.tv_sec) * 1000000 + t.tv_usec - tstart.tv_usec;
	tsdiff = tscur - tsprev;
	if (tsdiff > pd->pulse_sync_min && tsdiff < pd->pulse_sync_max) {
		/* probably end of sync period detected */
		/* next call may be start of high+low encoded bits */
		incode = 1;
		pulscount = 0;
		tclen = 0;
#ifdef THERMO433_INCLUDE_TIMING_STATS
		stat_sync = tsdiff;	/* for statistic */
#endif
	} else if (incode) {
		/* code transmission - expect high+low */
		if (pulscount < npulses) {
			/* even pulse is short high, odd pulse is low */
			if ((tsdiff > pd->pulse_high_min && \
			     tsdiff < pd->pulse_high_max && \
			     !(pulscount & 1)) || \
			    ((tsdiff > pd->pulse_low_short_min && \
			      tsdiff < pd->pulse_low_short_max) || \
			     (tsdiff > pd->pulse_low_long_min && \
			      tsdiff < pd->pulse_low_long_max) && \
			     (pulscount & 1)))
				tsbuf[pulscount++] = tsdiff; /* valid timing */
			else if (tsdiff > THERMO433_MAX_NOISE_TIME)
				incode = 0;	/* noise, discard code */
			tclen += tsdiff;

		} else if (tclen < maxcodetime) {
		/* code completed, decode */
			tmpcode = 0;
			/* only odd timings carry data */
			for(i = 1; i < pulscount; i += 2) {
				/* short low = 0 */
				if (tsbuf[i] > pd->pulse_low_short_min && \
				    tsbuf[i] < pd->pulse_low_short_max)
					tmpcode <<= 1;
				else if (tsbuf[i] > pd->pulse_low_long_min && \
					 tsbuf[i] < pd->pulse_low_long_max)
					tmpcode = (tmpcode << 1) | 1;
				else {
					/* timing does not fit, noise */
					incode = 0;
					break;
				}
			}

			if (incode && tmpcode) {
				code = tmpcode;
				incode = 0;
#ifdef THERMO433_INCLUDE_TIMING_STATS
				/* for statistic */
				memcpy(stat_buf, tsbuf, tsblen);
#endif
				/* signal that code is ready */
				/* we can store only one code */
				sem_getvalue(&codeready, &sval);
				if (sval < 1)
					sem_post(&codeready);
			}
		} else
		/* code too long (may be too much noise), discard */
			incode = 0;
	}
	tsprev = tscur;
}
