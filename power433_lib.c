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

#include "power433_lib.h"

/* timing data - transmission */
#define POWER433_TX_BIT_TIME	1250
#define POWER433_TX_SHORT_TIME	(POWER433_TX_BIT_TIME >> 2)
#define POWER433_TX_LONG_TIME	(POWER433_TX_BIT_TIME - POWER433_TX_SHORT_TIME)
#define POWER433_TX_SYNC_TIME	(POWER433_TX_SHORT_TIME * 31)
/* timing data - receiving */
#define POWER433_MIN_SYNC_TIME	9500		/* sync time in us */
#define POWER433_MAX_SYNC_TIME	9800
#define POWER433_MIN_SHORT_TIME	170		/* short time in us */
#define POWER433_MAX_SHORT_TIME	450
#define POWER433_MIN_LONG_TIME	800		/* long time in us */
#define POWER433_MAX_LONG_TIME	1100
#define POWER433_MAX_NOISE_TIME	100		/* noise time in us */
#define POWER433_MAX_CODE_TIME	(POWER433_BITS * POWER433_TX_BIT_TIME * 6 / 5)

/* code data */
#define POWER433_BADCODE_MASK	0xaaaaaaaa	/* eliminate artifact codes */
#define POWER433_SYSID_MASK	0x00554000	/* DIP switch address mask */
#define POWER433_DEVID_MASK	0x00001550	/* Device (channel) A-E mask */
#define POWER433_ON_MASK	0x00000004	/* Buttons mask */
#define POWER433_OFF_MASK	0x00000001

/* global variables */
/* (volatile keeps variables in memory for interrupts) */
static int txgpio, rxgpio;
static struct timeval tstart;
static volatile unsigned long tsprev, tclen, code;
static unsigned long tsbuf[POWER433_PULSES];	/* 2 * bit timing (high, low)*/
static volatile int incode, pulscount;
static sem_t codeready;

#ifdef POWER433_INCLUDE_TIMING_STATS
static unsigned long stat_sync;                 /* for statistic purposes */
static unsigned long stat_buf[POWER433_PULSES];
#endif

/* ********* */
/* Functions */
/* ********* */

void handleGpioInt(void);

/* Initialize library */
void Power433_init(int tx_gpio, int rx_gpio)
{
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
		pinMode(rxgpio, INPUT);
		wiringPiISR(rxgpio, INT_EDGE_BOTH, handleGpioInt);
		sem_init(&codeready, 0, 0);
	}
}

/* Retrieve code once, exit with 0 immediately if not available */
unsigned int Power433_getAnyCode(void)
{
	unsigned int tmpcode;
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

/* Retrieve code once, exit with 0 immediately if not available */
/* Only valid codes are returned */
unsigned int Power433_getCode(void)
{
	unsigned int tmpcode;

	if (rxgpio < 0)
		return 0;

	tmpcode = Power433_getAnyCode();
	if (tmpcode & POWER433_BADCODE_MASK)
		tmpcode = 0;

	return tmpcode;
}

/* Wait for code and return only when one is available */
unsigned int Power433_waitAnyCode(void)
{
	unsigned int tmpcode;

	if (rxgpio < 0)
		return 0;

	sem_wait(&codeready);
	tmpcode = code;
	code = 0;
	return tmpcode;
}

/* Wait for code and return only when one is available */
/* Only valid codes are returned */
unsigned int Power433_waitCode(void)
{
	unsigned int tmpcode;

	if (rxgpio < 0)
		return 0;

	for(;;) {
		tmpcode = Power433_waitAnyCode();
		if (!(tmpcode & POWER433_BADCODE_MASK))
			break;
	}
	return tmpcode;
}

/* Send code once */
void Power433_sendCode(unsigned int txcode)
{
	int i;
	unsigned int codemask;
	unsigned long txbuf[POWER433_PULSES];

	if (txgpio < 0)
		return;

	/* prepare timings for sending code */
	codemask = 1 << (POWER433_BITS - 1);
	for(i = 0; i < POWER433_PULSES; i += 2) {
		if (txcode & codemask) {
			/* 1 - 3 * high + 1 * low */
			txbuf[i] = POWER433_TX_LONG_TIME;
			txbuf[i + 1] = POWER433_TX_SHORT_TIME;
		} else {
			/* 0 - 1 * high + 3 * low */
			txbuf[i] = POWER433_TX_SHORT_TIME;
			txbuf[i + 1] = POWER433_TX_LONG_TIME;
		}
		codemask >>= 1;
	}

	/* send sync signal - high pulse + 31 low pulses */
	digitalWrite(txgpio, HIGH);
	delayMicroseconds(POWER433_TX_SHORT_TIME);
	digitalWrite(txgpio, LOW);
	delayMicroseconds(POWER433_TX_SYNC_TIME);

	/* code transmission */
	for(i = 0; i < POWER433_PULSES; i += 2) {
		digitalWrite(txgpio, HIGH);
		delayMicroseconds(txbuf[i]);
		digitalWrite(txgpio, LOW);
		delayMicroseconds(txbuf[i + 1]);
	}
}

/* Send code N times */
void Power433_repeatCode(unsigned int txcode, int n)
{
	int i;

	if (txgpio < 0)
		return;

	for(i = 0; i < n; i++)
		Power433_sendCode(txcode);

	/* code sequence always ends with low signal which may last
	   for unknown length - till nearest noise peak, so pulse it
	   to end within predefined timing */
	digitalWrite(txgpio, HIGH);
	delayMicroseconds(POWER433_TX_SHORT_TIME);
	digitalWrite(txgpio, LOW);
	delayMicroseconds(POWER433_TX_SHORT_TIME);
}

/* Decode RF keycode */
int Power433_decodeCommand(unsigned int code, int *systemid,
			   int *deviceid, int *button)
{
	unsigned int s;

	s = (code & POWER433_SYSID_MASK) >> 14;
	*systemid = ~((s & 0x1) + ((s & 0x4) >> 1) + ((s & 0x10) >> 2) + \
		    ((s & 0x40) >> 3) + ((s & 0x100) >> 4)) & 0x1F;

	s = (code & POWER433_DEVID_MASK) >> 4;
	*deviceid = ~((s & 0x1) + ((s & 0x4) >> 1) + ((s & 0x10) >> 2) + \
		    ((s & 0x40) >> 3) + ((s & 0x100) >> 4)) & 0x1F; 

	if (!(code & POWER433_OFF_MASK))
		*button = POWER433_BUTTON_OFF;
	else if (!(code & POWER433_ON_MASK))
		*button = POWER433_BUTTON_ON;
	else {
		*button = -1;
		return -1;
	}

	return 0;
}

/* Contruct RF keycode */
unsigned int Power433_encodeCommand(unsigned int systemid,
                                    unsigned int deviceid,
                                    unsigned int button)
{
	unsigned int tmpsysid, tmpdevid;

	tmpsysid = ~systemid & 0x1F;
	tmpdevid = ~deviceid & 0x1F;
	return ((((tmpsysid & 0x10) << 4) + ((tmpsysid & 0x8) << 3) + \
	       ((tmpsysid & 0x4) << 2) + ((tmpsysid & 0x2) << 1) + \
	       (tmpsysid & 0x1)) << 14) + ((((tmpdevid & 0x10) << 4) + \
	       ((tmpdevid & 0x8) << 3) + ((tmpdevid & 0x4) << 2) + \
	       ((tmpdevid & 0x2) << 1) + (tmpdevid & 0x1)) << 4) + \
	       (0x1 << ((~button & 0x1) << 1));
}

/* Send command */
int Power433_sendCommand(unsigned int systemid, unsigned int deviceid,
			 unsigned int button)
{
	unsigned int tmpcode;

	if (txgpio < 0)
		return -1;

	tmpcode = Power433_encodeCommand(systemid, deviceid, button);

	/* send code a few times */
	Power433_repeatCode(tmpcode, POWER433_RETRANS);

	return 0;
}

/* Classify pulse type based on length in microseconds */
/* 0-sync, 1-short, 2-long, -1-unclassified/noise */
int Power433_classifyPulse(unsigned long microseconds)
{
	int pulsetype;

	pulsetype = POWER433_PULSE_TYPE_UNKNOWN;

	if (microseconds > POWER433_MIN_SYNC_TIME && \
	    microseconds < POWER433_MAX_SYNC_TIME)
		return POWER433_PULSE_TYPE_SYNC;
	else if (microseconds > POWER433_MIN_SHORT_TIME && \
		 microseconds < POWER433_MAX_SHORT_TIME)
		return POWER433_PULSE_TYPE_SHORT;
	else if (microseconds > POWER433_MIN_LONG_TIME && \
		 microseconds < POWER433_MAX_LONG_TIME)
		return POWER433_PULSE_TYPE_LONG;

	return pulsetype;
}

#ifdef POWER433_INCLUDE_TIMING_STATS
/* Get timing data for statistics */
void Power433_getTimingStats(unsigned long *timesync, unsigned long tpbuf[])
{
	*timesync = stat_sync;
	memcpy(tpbuf, stat_buf, sizeof(stat_buf));	
}
#endif

/* ************* */
/* * Interrupt * */
/* ************* */

void handleGpioInt(void)
{
	struct timeval t;
	unsigned long tscur, tsdiff, tmpcode;
	int i, sval;

	gettimeofday(&t, NULL);

	tscur = (t.tv_sec - tstart.tv_sec) * 1000000 + t.tv_usec - tstart.tv_usec;
	tsdiff = tscur - tsprev;
	if (tsdiff > POWER433_MIN_SYNC_TIME && tsdiff < POWER433_MAX_SYNC_TIME) {
		/* probably end of sync period detected */
		/* next call may be start of high+low encoded bits */
		incode = 1;
		pulscount = 0;
		tclen = 0;
#ifdef POWER433_INCLUDE_TIMING_STATS
		stat_sync = tsdiff;	/* for statistic */
#endif
	} else if (incode) {
		/* code transmission - expect high+low bits */
		if (pulscount < POWER433_PULSES) {
			if ((tsdiff > POWER433_MIN_SHORT_TIME && tsdiff < POWER433_MAX_SHORT_TIME) || \
			    (tsdiff > POWER433_MIN_LONG_TIME && tsdiff < POWER433_MAX_LONG_TIME))
				tsbuf[pulscount++] = tsdiff; /* valid 0 or 1 */
			else if (tsdiff > POWER433_MAX_NOISE_TIME)
				incode = 0;	/* noise, discard code */
			tclen += tsdiff;
		} else if (tclen < POWER433_MAX_CODE_TIME) {
		/* code completed, decode */
			tmpcode = 0;
			for(i = 0; i < pulscount; i += 2) {
				if (tsbuf[i] > POWER433_MIN_SHORT_TIME && \
				    tsbuf[i] < POWER433_MAX_SHORT_TIME && \
				    tsbuf[i + 1] > POWER433_MIN_LONG_TIME && \
				    tsbuf[i + 1] < POWER433_MAX_LONG_TIME)
					tmpcode <<= 1;
				else if (tsbuf[i] > POWER433_MIN_LONG_TIME && \
					 tsbuf[i] < POWER433_MAX_LONG_TIME && \
					 tsbuf[i + 1] > POWER433_MIN_SHORT_TIME && \
					 tsbuf[i + 1] < POWER433_MAX_SHORT_TIME)
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
#ifdef POWER433_INCLUDE_TIMING_STATS
				/* for statistic */
				memcpy(stat_buf, tsbuf, sizeof(tsbuf));
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
