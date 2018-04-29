#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <wiringPi.h>
#include <wiringPiI2C.h>

#include "bme280_lib.h"

/*
 * Datasheet: https://cdn-shop.adafruit.com/product-files/2652/2652.pdf
 * Also based on https://github.com/BoschSensortec/BME280_driver
 */

/* Calibration registers */
#define BME280_REG_DIG_T1	0x88	/* unsigned short */
#define BME280_REG_DIG_T2	0x8A	/* signed short */
#define BME280_REG_DIG_T3	0x8C	/* signed short */
#define BME280_REG_DIG_P1	0x8E	/* unsigned short */
#define BME280_REG_DIG_P2	0x90	/* signed short */
#define BME280_REG_DIG_P3	0x92	/* signed short */
#define BME280_REG_DIG_P4	0x94	/* signed short */
#define BME280_REG_DIG_P5	0x96	/* signed short */
#define BME280_REG_DIG_P6	0x98	/* signed short */
#define BME280_REG_DIG_P7	0x9A	/* signed short */
#define BME280_REG_DIG_P8	0x9C	/* signed short */
#define BME280_REG_DIG_P9	0x9E	/* signed short */
#define BME280_REG_DIG_H1	0xA1	/* unsigned char */
#define BME280_REG_DIG_H2	0xE1	/* signed short */
#define BME280_REG_DIG_H3	0xE3	/* unsigned char */
#define BME280_REG_DIG_H4	0xE4	/* signed short */
#define BME280_REG_DIG_H5	0xE6	/* signed short */
#define BME280_REG_DIG_H6	0xE7	/* signed char */

/* Control registers */
#define BME280_REG_CHIP_ID	0xD0	/* 0x60 if chip is present */
#define BME280_REG_SOFT_RST	0xE0	/* send 0xb6 to reset chip */
#define BME280_REG_CTRL_HUM	0xF2
#define BME280_REG_STATUS	0xF3
#define BME280_REG_CTRL_MEAS	0xF4
#define BME280_REG_CONFIG	0xF5
#define BME280_REG_PRESS_MSB	0xF7
#define BME280_REG_PRESS_LSB	0xF8
#define BME280_REG_PRESS_XLSB	0xF9
#define BME280_REG_TEMP_MSB	0xFA
#define BME280_REG_TEMP_LSB	0xFB
#define BME280_REG_TEMP_XLSB	0xFC
#define BME280_REG_HUM_MSB	0xFD
#define BME280_REG_HUM_LSB	0xFE

/* Register values */
#define	BME280_VAL_ID		0x60
#define	BME280_VAL_RST		0xB6

/* Timing information (rounded) - see sensor datasheet */
#define	BME280_STARTUP_TIME_MS		2

/* BME280 calibration values, see datasheet pages 22-23 for details */
static signed short dig_T2, dig_T3, dig_P2, dig_P3, dig_P4, dig_P5, dig_P6,
		    dig_P7, dig_P8, dig_P9, dig_H2, dig_H4, dig_H5;
static unsigned short dig_T1, dig_P1;
static unsigned char dig_H1, dig_H3;
static signed char dig_H6;
static int calflag;

/* BME280: device initialization (for RPi I2C bus) */
int BME280_initPi(int *i2caddr)
{
	int addr, fd;

	addr = BME280_I2C_DEF_ADDR;

	if (i2caddr != NULL)
		if (*i2caddr > 0)
			addr = *i2caddr;

	fd = wiringPiI2CSetup(addr);

	if (fd < 0)
		return -1;

	if (i2caddr != NULL)
		*i2caddr = addr;

	calflag = 0;	/* calibration variables not initialized yet */

	return fd;
}

/* BME280: check if sensor chip is available */
int BME280_isPresent(int fd)
{
	int id;

	id = wiringPiI2CReadReg8(fd, BME280_REG_CHIP_ID);
	return(id == BME280_VAL_ID ? 1 : 0);
}

/* BME280: soft reset */
void BME280_softReset(int fd)
{
	wiringPiI2CWriteReg8(fd, BME280_REG_SOFT_RST, BME280_VAL_RST);
        delay(BME280_STARTUP_TIME_MS);
}

/* BME280: get calibration data */
void BME280_getCalibrationData(int fd)
{
	unsigned int lsb, msb;

	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_T1);
	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_T1 + 1);
	dig_T1 = (unsigned short)((msb << 8) + lsb);

	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_T2);
	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_T2 + 1);
	dig_T2 = (signed short)((msb << 8) + lsb);

	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_T3);
	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_T3 + 1);
	dig_T3 = (signed short)((msb << 8) + lsb);

	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P1);
	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P1 + 1);
	dig_P1 = (unsigned short)((msb << 8) + lsb);

	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P2);
	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P2 + 1);
	dig_P2 = (signed short)((msb << 8) + lsb);

	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P3);
	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P3 + 1);
	dig_P3 = (signed short)((msb << 8) + lsb);

	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P4);
	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P4 + 1);
	dig_P4 = (signed short)((msb << 8) + lsb);

	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P5);
	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P5 + 1);
	dig_P5 = (signed short)((msb << 8) + lsb);

	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P6);
	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P6 + 1);
	dig_P6 = (signed short)((msb << 8) + lsb);

	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P7);
	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P7 + 1);
	dig_P7 = (signed short)((msb << 8) + lsb);

	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P8);
	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P8 + 1);
	dig_P8 = (signed short)((msb << 8) + lsb);

	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P9);
	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_P9 + 1);
	dig_P9 = (signed short)((msb << 8) + lsb);

	dig_H1 = (unsigned char)wiringPiI2CReadReg8(fd, BME280_REG_DIG_H1);

	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_H2);
	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_H2 + 1);
	dig_H2 = (signed short)((msb << 8) + lsb);

	dig_H3 = (unsigned char)wiringPiI2CReadReg8(fd, BME280_REG_DIG_H3);

	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_H4);
	lsb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_H4 + 1);
	dig_H4 = (signed short)((msb << 4) + (lsb & 0x0f));

	msb = wiringPiI2CReadReg8(fd, BME280_REG_DIG_H5);
	dig_H5 = (signed short)((msb << 4) + ((lsb & 0xf0) >> 4));

	dig_H6 = (signed char)wiringPiI2CReadReg8(fd, BME280_REG_DIG_H6);

	calflag = 1;
}

/* BME280: display calibration data */
void BME280_dumpCalibrationData(void)
{
	if (!calflag)
		return;

	printf("dig_T1 = %d\n", dig_T1);
	printf("dig_T2 = %d\n", dig_T2);
	printf("dig_T3 = %d\n", dig_T3);
	printf("dig_P1 = %d\n", dig_P1);
	printf("dig_P2 = %d\n", dig_P2);
	printf("dig_P3 = %d\n", dig_P3);
	printf("dig_P4 = %d\n", dig_P4);
	printf("dig_P5 = %d\n", dig_P5);
	printf("dig_P6 = %d\n", dig_P6);
	printf("dig_P7 = %d\n", dig_P7);
	printf("dig_P8 = %d\n", dig_P8);
	printf("dig_P9 = %d\n", dig_P9);
	printf("dig_H1 = %d\n", dig_H1);
	printf("dig_H2 = %d\n", dig_H2);
	printf("dig_H3 = %d\n", dig_H3);
	printf("dig_H4 = %d\n", dig_H4);
	printf("dig_H5 = %d\n", dig_H5);
	printf("dig_H6 = %d\n", dig_H6);
}

/* BME280: setup device - all measurements in forced mode, filter off,
 * OSS is x1, values are 16-bit (weather station preset)
 */
void BME280_setupFullMode(int fd)
{
	unsigned char b8;

	if (!calflag)
		BME280_getCalibrationData(fd);

	/* set Hum/Temp/Press OSS to x1 and enable forced mode */
	wiringPiI2CWriteReg8(fd, BME280_REG_CTRL_HUM, BME280_OVERSAMPLING_1X);
	wiringPiI2CWriteReg8(fd, BME280_REG_CTRL_MEAS, (BME280_OVERSAMPLING_1X << 5) \
			     | (BME280_OVERSAMPLING_1X << 2) | BME280_FORCED_MODE );

	/* disable filter */
	b8 = wiringPiI2CReadReg8(fd, BME280_REG_CONFIG);
	wiringPiI2CWriteReg8(fd, BME280_REG_CONFIG, (b8 & 0x1c) | (BME280_FILTER_COEFF_OFF << 2));
}

/* BME280: get values using official Bosch floating point formula */
int BME280_getSensorData(int fd, double *temp, double *press, double *humid)
{
	/* Sources:
	 * https://github.com/BoschSensortec/BME280_driver/blob/master/bme280.c
	 * BME280 datasheet, section 8.1, page 49
	 */

	unsigned char msb, lsb, xlsb;
	unsigned long uv;
	double v0, v1, v2, v3, v4, v5, ut, cv;

	if (!calflag)
		return -1;

	/* temperature (deg C) */
	msb = wiringPiI2CReadReg8(fd, BME280_REG_TEMP_MSB);
        lsb = wiringPiI2CReadReg8(fd, BME280_REG_TEMP_LSB);
        xlsb = wiringPiI2CReadReg8(fd, BME280_REG_TEMP_XLSB);
	uv = ((unsigned long)msb << 12) + ((unsigned long)lsb << 4) + \
	     (((unsigned long)xlsb & 0xf0) >> 4);

	v0 = ((double)uv / 16384.0 - (double)dig_T1 / 1024.0) * (double)dig_T2;
	v1 = (double)uv / 131072.0 - (double)dig_T1 / 8192.0;
	v1 = v1 * v1 * (double)dig_T3;
	ut = v0 + v1;
	cv = (v0 + v1) / 5120.0;

	if (cv < BME280_TEMP_MIN)
		cv = BME280_TEMP_MIN;
	else if (cv > BME280_TEMP_MAX)
		cv = BME280_TEMP_MAX;

	if (temp !=NULL)
		*temp = cv;

	/* pressure (hPa) */
	msb = wiringPiI2CReadReg8(fd, BME280_REG_PRESS_MSB);
        lsb = wiringPiI2CReadReg8(fd, BME280_REG_PRESS_LSB);
        xlsb = wiringPiI2CReadReg8(fd, BME280_REG_PRESS_XLSB);
	uv = ((unsigned long)msb << 12) + ((unsigned long)lsb << 4) + \
	     (((unsigned long)xlsb & 0xf0) >> 4);

	v0 = ut / 2.0 - 64000.0;
	v1 = v0 * v0 * (double)dig_P6 / 32768.0;
	v1 = v1 + v0 * (double)dig_P5 * 2.0;
	v1 = v1 / 4.0 + (double)dig_P4 * 65536.0;
	v2 = (double)dig_P3 * v0 * v0 / 524288.0;
	v0 = (v2 + (double)dig_P2 * v0) / 524288.0;
	v0 = (1.0 + v0 / 32768.0) * (double)dig_P1;
	if (v0 != 0.0) {	/* avoid exception caused by division by zero */
		cv = 1048576.0 - (double)uv;
		cv = (cv - v1 / 4096.0) * 6250.0 / v0;
		v0 = (double)dig_P9 * cv * cv / 2147483648.0;
		v1 = cv * (double)dig_P8 / 32768.0;
		cv = cv + (v0 + v1 + (double)dig_P7) / 16.0;

		if (cv < BME280_PRESS_MIN)
			cv = BME280_PRESS_MIN;
		else if (cv > BME280_PRESS_MAX)
			cv = BME280_PRESS_MAX;
	} else /* Invalid case */
		cv = BME280_PRESS_MIN;

	if (cv < BME280_PRESS_MIN)
		cv = BME280_PRESS_MIN;
	else if (cv > BME280_PRESS_MAX)
		cv = BME280_PRESS_MAX;

	if (press != NULL)
		*press = cv / 100.0;

	/* humidity (rH%) */
	msb = wiringPiI2CReadReg8(fd, BME280_REG_HUM_MSB);
        lsb = wiringPiI2CReadReg8(fd, BME280_REG_HUM_LSB);
	uv = ((unsigned long)msb << 8) + (unsigned long)lsb;

	v0 = ut - 76800.0;
	v1 = ((double)dig_H4 * 64.0 + ((double)dig_H5 / 16384.0) * v0);
	v2 = (double)uv - v1;
	v3 = (double)dig_H2 / 65536.0;
	v4 = 1.0 + ((double)dig_H3 / 67108864.0) * v0;
	v5 = 1.0 + ((double)dig_H6 / 67108864.0) * v0 * v4;
	v5 = v2 * v3 * v4 * v5;
	cv = v5 * (1.0 - (double)dig_H1 * v5 / 524288.0);

	if (cv < BME280_HUMID_MIN)
		cv = BME280_HUMID_MIN;
	else if (cv > BME280_HUMID_MAX)
		cv = BME280_HUMID_MAX;

	if (humid != NULL)
		*humid = cv;

	return 0;
}
