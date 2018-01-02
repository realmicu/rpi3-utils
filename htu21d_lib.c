#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <wiringPi.h>
#include <wiringPiI2C.h>

#include "htu21d_lib.h"

/* *************************** */
/* HTU21D Sensor configuration */
/* *************************** */

/* Registers (No Hold master) */
#define	HTU21D_REG_TEMP_NH		0xF3
#define	HTU21D_REG_HUMID_NH		0xF5
#define	HTU21D_REG_SOFT_RST		0xFE

/* Max measuring times - see page 3, 5 and 12 of the datasheet */
#define	HTU21D_TEMP_MAX_TIME_MS		50
#define	HTU21D_HUMID_MAX_TIME_MS	16
#define	HTU21D_SOFT_RST_MAX_TIME_MS	15

/* ********* */
/* Functions */
/* ********* */

/* HTU21D: device initialization (for RPi I2C bus) */
int HTU21D_initPi(int i2caddr)
{
	int fd;

	fd = wiringPiI2CSetup(i2caddr > 0 ? i2caddr : HTU21D_I2C_ADDR);
	return fd < 0 ? -1 : fd;
}

/* Soft reset */
void HTU21D_softReset(int fd)
{
        wiringPiI2CWrite(fd, HTU21D_REG_SOFT_RST);
        delay(HTU21D_SOFT_RST_MAX_TIME_MS);
}

/* Get temperature */
double HTU21D_getTemperature(int fd)
{
	unsigned int temp;
	double tSensorTemp;
        unsigned char buf[4];

	wiringPiI2CWrite(fd, HTU21D_REG_TEMP_NH);
	delay(HTU21D_TEMP_MAX_TIME_MS);
	read(fd, buf, 3);
	temp = (buf [0] << 8 | buf [1]) & 0xFFFC;
        /* Convert sensor reading into temperature.
	   See page 14 of the datasheet */
	tSensorTemp = temp / 65536.0;
	return -46.85 + (175.72 * tSensorTemp);
}

/* Get humidity */
double HTU21D_getHumidity(int fd)
{
	unsigned int humid;
	double tSensorHumid;
        unsigned char buf[4];

        wiringPiI2CWrite(fd, HTU21D_REG_HUMID_NH);
        delay(HTU21D_HUMID_MAX_TIME_MS);
        read(fd, buf, 3);
        humid = (buf [0] << 8 | buf [1]) & 0xFFFC;
        /* Convert sensor reading into humidity.
           See page 15 of the datasheet */
        tSensorHumid = humid / 65536.0;
        return -6.0 + (125.0 * tSensorHumid);
}

