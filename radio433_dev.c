/*
 * **********************************************
 *  This library contains power433 and thermo433
 *  code translation functions for TX and RX
 * **********************************************
 */

/*
 * Upper layer for radio433_lib. It translates
 * device-specific codes to/from raw ones.
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

#include <wiringPi.h>

#include "radio433_dev.h"

/*
 * *********************
 * Power (Device type 0)
 * *********************
 *
 * 24-bit codes with:
 *   system id (5 bits), 0-31
 *   socket id (5 bits), A-E
 *   command (1 bit), on/off
 */

/* Code data */
#define POWER433_BADCODE_MASK	0xaaaaaaaa	/* eliminate artifact codes */
#define POWER433_SYSID_MASK	0x00554000	/* DIP switch address mask */
#define POWER433_DEVID_MASK	0x00001550	/* Device (channel) A-E mask */
#define POWER433_ON_MASK	0x00000004	/* Buttons mask */
#define POWER433_OFF_MASK	0x00000001

/* Decode power command from raw code */
/* Return 0 for bad code */
int Radio433_pwrGetCommand(unsigned long long code,
			   int *systemid, int *deviceid, int *button)
{
	unsigned long long s;

	if (code & POWER433_BADCODE_MASK)
		return 0;

	if (systemid) {
		s = (code & POWER433_SYSID_MASK) >> 14;
		*systemid = ~((s & 0x1) + ((s & 0x4) >> 1) + ((s & 0x10) >> 2) \
			    + ((s & 0x40) >> 3) + ((s & 0x100) >> 4)) & 0x1F;
	}

	if (deviceid) {
		s = (code & POWER433_DEVID_MASK) >> 4;
		*deviceid = ~((s & 0x1) + ((s & 0x4) >> 1) + ((s & 0x10) >> 2) \
			    + ((s & 0x40) >> 3) + ((s & 0x100) >> 4)) & 0x1F;
	}

	if (button) {
		if (!(code & POWER433_OFF_MASK))
			*button = POWER433_BUTTON_OFF;
		else if (!(code & POWER433_ON_MASK))
			*button = POWER433_BUTTON_ON;
		else {
			*button = -1;
			return 0;
		}
	}

	return 1;
}

/* Encode power command into raw code */
unsigned long long Radio433_pwrGetCode(int systemid, int deviceid,
                                       int button)
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

/*
 * ***************************
 * Thermometer (Device type 1)
 * ***************************
 *
 * 36-bit codes with:
 *   random system id (4 bits), 0-15
 *   sensor number (2 bits), 1-3
 *   random sensor id (2 bits), 0-3
 *   battery level (1 bit), good/weak
 *   temperature trend (2 bit), up/down/stable
 *   temperature (12 bits), reversed signed u2
 *   humidity (8 bits), reversed signed u2
 *   checksum (4 bits), unknown algorithm
 */

/* Temperature range */
#define THERMO433_MIN_TEMP	-50
#define THERMO433_MAX_TEMP	70

/* Decode sensor data */
/* Return 0 if not supported */
/* NOTE: code checksum should be validated but it is not */
int Radio433_thmGetData(unsigned long long code, int *sysid, int *thmid,
			int *ch, int *batlow, int *tdir, double *temp,
			int *humid)
{
	int i, v;
	unsigned long long mask;

	if (sysid)
		*sysid = (code & 0xf00000000L) >> 32;

	if (thmid)
		*thmid = (code & 0x030000000L) >> 28;

	if (ch)
		*ch = (code & 0x0c0000000L) >> 30;

	if (batlow)	/* guess only */
		*batlow = (code & 0x008000000L) >> 27;

	if (tdir) {
		v = (code & 0x006000000L) >> 25;
		if (v == 3)
			*tdir = THERMO433_TEMP_TREND_INVALID;
		else
			*tdir = v;
	}

	if (temp) {
		mask = 0x1000;	/* LSB of mask = MSB of data */
		v = 0;
		for(i = 0; i < 12; i++) {
			if (code & mask)
				v = (v << 1) | 1;
			else
				v <<= 1;
			mask <<= 1;
		}
		if (v & 0x800)	/* MSB for 12-bit U2 signals sign */
			v |= ~0xfff;
		*temp = v * 0.1;
	}

	if (humid) {
		mask = 0x10;
		v = 0;
		for(i = 0; i < 8; i++) {
			if (code & mask)
				v = (v << 1) | 1;
			else
				v <<= 1;
			mask <<= 1;
		}
		v |= ~0xff;
		*humid = v + 100;
	}

	if (*temp < THERMO433_MIN_TEMP || *temp > THERMO433_MAX_TEMP)
		return 0;

	if (*humid < 0 || *humid > 100)
		return 0;

	return 1;
}

/* Encode sensor data */
/* NOTE: code checksum should be generated but it is not */
unsigned long long Radio433_thmGetCode(int sysid, int thmid, int ch,
				       int batlow, int tdir, double temp,
				       int humid)
{
	int i, v;
	unsigned int mask;
	unsigned long long code;

	/* accept only sane parameters */
	if (sysid < 0 || sysid > 15 || thmid < 0 || thmid > 7 || \
	    ch < 0 || ch > 3 || batlow < 0 || batlow > 1 || \
	    tdir < 0 || tdir > 3 || temp < THERMO433_MIN_TEMP || \
	    temp > THERMO433_MAX_TEMP || humid < 0 || humid > 100)
		return 0;

	code = (sysid & 0xfULL) << 32 | (ch & 0x3ULL) << 30 | \
	       (thmid & 0x7ULL) << 28 | (batlow & 0x1ULL) << 27 | \
	       (tdir & 0x3ULL) << 25;

	v = (int)(temp * 10) & 0xfff;	/* signed */
	mask = 0x1;
	for (i = 0; i < 12; i++) {
		code |= (v & mask) << (23 - i);
		mask <<= 1;
	}

	v = (humid - 100) & 0xff;	/* signed */
	mask = 0x1;
	for (i = 0; i < 8; i++) {
		code |= (v & mask) << (11 - i);
		mask <<= 1;
	}

	/* checksum is ignored for now */

	return code;
}
