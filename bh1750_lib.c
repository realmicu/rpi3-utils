#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <wiringPi.h>
#include <wiringPiI2C.h>

#include "bh1750_lib.h"

/* Registers */
#define BH1750_REG_POWER_DOWN	0x00
#define BH1750_REG_POWER_ON	0x01
#define BH1750_REG_RESET	0x07

/* Timing information - see sensor datasheet */
#define	BH1750_RESET_MAX_TIME_MS	15	/* not in datasheet, assumed */
#define	BH1750_PWR_ONOFF_MAX_TIME_MS	15	/* not in datasheet, assumed */
#define BH1750_LX_RES_L_MAX_TIME_MS	24
#define BH1750_LX_RES_H_MAX_TIME_MS	180

/* Scaling factor - see sensor datasheet */
#define BH1750_SCALING_FACTOR	1.2	/* denominator */

/* Delay values for different Resolution settings */
static const int reswait[4] = { BH1750_LX_RES_H_MAX_TIME_MS, \
				BH1750_LX_RES_H_MAX_TIME_MS, \
				0, \
				BH1750_LX_RES_L_MAX_TIME_MS };

static int datawait;	/* current wait time for data */

/* BH1750: device initialization (for RPi I2C bus) */
int BH1750_initPi(int i2caddr)
{
	int fd;

	fd = wiringPiI2CSetup(i2caddr > 0 ? i2caddr : BH1750_I2C_ADDR);
	return fd < 0 ? -1 : fd;
}

/* BH1750: device control */
void BH1750_powerDown(int fd)
{
	wiringPiI2CWrite(fd, BH1750_REG_POWER_DOWN);
	delay(BH1750_PWR_ONOFF_MAX_TIME_MS);
	datawait = 0;
}

void BH1750_powerOn(int fd)
{
	wiringPiI2CWrite(fd, BH1750_REG_POWER_ON);
	delay(BH1750_PWR_ONOFF_MAX_TIME_MS);
	datawait = 0;
}

/* BH1750: soft reset */
void BH1750_softReset(int fd)
{
	wiringPiI2CWrite(fd, BH1750_REG_RESET);
	delay(BH1750_RESET_MAX_TIME_MS);
}

/* BH1750: set measurement mode */
void BH1750_setMode(int fd, int cont, int res)
{
	if (cont < BH1750_MODE_CONT || cont > BH1750_MODE_ONETIME || \
	    res < BH1750_MODE_RES_H || res > BH1750_MODE_RES_L)
		return;

	wiringPiI2CWrite(fd, ((cont << 4) | res) & 0x33);
	datawait = reswait[res];
}

/* BH1750: get luminance value */
double BH1750_getLx(int fd)
{
	unsigned int lx;
	unsigned char buf[2];

	if (!datawait)
		return -1.0;

	delay(datawait);
	read(fd, buf, 2);
	lx = (buf [0] << 8) | buf [1];
	/* Convert sensor reading into luminosity.
	   See page 7 of the datasheet */
	return lx / BH1750_SCALING_FACTOR;
}
