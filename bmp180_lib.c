#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <wiringPi.h>
#include <wiringPiI2C.h>

#include "bmp180_lib.h"

/* Registers */
#define BMP180_REG_AC1		0xAA
#define BMP180_REG_AC2		0xAC
#define BMP180_REG_AC3		0xAE
#define BMP180_REG_AC4		0xB0
#define BMP180_REG_AC5		0xB2
#define BMP180_REG_AC6		0xB4
#define BMP180_REG_B1		0xB6
#define BMP180_REG_B2		0xB8
#define BMP180_REG_MB		0xBA
#define BMP180_REG_MC		0xBC
#define BMP180_REG_MD		0xBE
#define BMP180_REG_ID		0xD0		/* 0x55 if chip is present */
#define BMP180_REG_SOFT_RST	0xE0		/* send 0xb6 to reset chip */
#define BMP180_REG_MEAS_CTL	0xF4
#define BMP180_REG_OUT_MSB	0xF6
#define BMP180_REG_OUT_LSB	0xF7
#define BMP180_REG_OUT_XLSB	0xF8

/* Register values */
#define	BMP180_VAL_ID		0x55
#define	BMP180_VAL_RST		0xB6
#define BMP180_VAL_TEMP		0x2E
#define BMP180_VAL_PRESS	0x34

/* Timing information (rounded) - see sensor datasheet */
#define	BMP180_SOFT_RST_MAX_TIME_MS	15	/* not in datasheet, assumed */
#define BMP180_TEMP_MAX_TIME_MS		 5
#define BMP180_PRESS_ULP_MAX_TIME_MS	 5
#define BMP180_PRESS_STD_MAX_TIME_MS	 8
#define BMP180_PRESS_HR_MAX_TIME_MS	14 
#define BMP180_PRESS_UHR_MAX_TIME_MS	26

/* Delay values for different OSS settings */
static const int osswait[4] = { BMP180_PRESS_ULP_MAX_TIME_MS, \
				BMP180_PRESS_STD_MAX_TIME_MS, \
				BMP180_PRESS_HR_MAX_TIME_MS, \
				BMP180_PRESS_UHR_MAX_TIME_MS };

/* BMP180 calibration values, see datasheet pages 13-15 for details */
static short cal_AC1, cal_AC2, cal_AC3, cal_B1, cal_B2, cal_MB, cal_MC, cal_MD;
static unsigned short cal_AC4, cal_AC5, cal_AC6;

/* BMP180: check if sensor chip is available */
int BMP180_isPresent(int fd)
{
	int id;

	id = wiringPiI2CReadReg8(fd, BMP180_REG_ID);
	return(id == BMP180_VAL_ID ? 1 : 0);
}

/* BMP180: soft reset */
void BMP180_softReset(int fd)
{
	wiringPiI2CWriteReg8(fd, BMP180_REG_SOFT_RST, BMP180_VAL_RST);
        delay(BMP180_SOFT_RST_MAX_TIME_MS);
}

/* BMP180: get calibration data */
void BMP180_getCalibrationData(int fd)
{
	int msb, lsb;

	msb = wiringPiI2CReadReg8(fd, BMP180_REG_AC1);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_AC1 + 1);
	cal_AC1 = (short)((msb << 8) + lsb);

	msb = wiringPiI2CReadReg8(fd, BMP180_REG_AC2);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_AC2 + 1);
	cal_AC2 = (short)((msb << 8) + lsb);

	msb = wiringPiI2CReadReg8(fd, BMP180_REG_AC3);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_AC3 + 1);
	cal_AC3 = (short)((msb << 8) + lsb);

	msb = wiringPiI2CReadReg8(fd, BMP180_REG_AC4);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_AC4 + 1);
	cal_AC4 = (unsigned short)((msb << 8) + lsb);

	msb = wiringPiI2CReadReg8(fd, BMP180_REG_AC5);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_AC5 + 1);
	cal_AC5 = (unsigned short)((msb << 8) + lsb);

	msb = wiringPiI2CReadReg8(fd, BMP180_REG_AC6);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_AC6 + 1);
	cal_AC6 = (unsigned short)((msb << 8) + lsb);

	msb = wiringPiI2CReadReg8(fd, BMP180_REG_B1);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_B1 + 1);
	cal_B1 = (short)((msb << 8) + lsb);

	msb = wiringPiI2CReadReg8(fd, BMP180_REG_B2);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_B2 + 1);
	cal_B2 = (short)((msb << 8) + lsb);

	msb = wiringPiI2CReadReg8(fd, BMP180_REG_MB);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_MB + 1);
	cal_MB = (short)((msb << 8) + lsb);

	msb = wiringPiI2CReadReg8(fd, BMP180_REG_MC);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_MC + 1);
	cal_MC = (short)((msb << 8) + lsb);

	msb = wiringPiI2CReadReg8(fd, BMP180_REG_MD);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_MD + 1);
	cal_MD = (short)((msb << 8) + lsb);
}

/* BMP180: display calibration data */
void BMP180_dumpCalibrationData(void)
{
	printf("AC1 = %d\n", cal_AC1);
	printf("AC2 = %d\n", cal_AC2);
	printf("AC3 = %d\n", cal_AC3);
	printf("AC4 = %d\n", cal_AC4);
	printf("AC5 = %d\n", cal_AC5);
	printf("AC6 = %d\n", cal_AC6);
	printf(" B1 = %d\n", cal_B1);
	printf(" B2 = %d\n", cal_B2);
	printf(" MB = %d\n", cal_MB);
	printf(" MC = %d\n", cal_MC);
	printf(" MD = %d\n", cal_MD);
}

/* BMP180: get values using official Bosch formula */
double BMP180_getPressure(int fd, int oss, double *temp)
{
	int msb, lsb, xlsb;
	int ut, up;
	int x1, x2, x3, b3, b5, b6, b8, p;
	unsigned int b4, b7;

	wiringPiI2CWriteReg8(fd, BMP180_REG_MEAS_CTL, BMP180_VAL_TEMP);
	delay(BMP180_TEMP_MAX_TIME_MS);
	msb = wiringPiI2CReadReg8(fd, BMP180_REG_OUT_MSB);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_OUT_LSB);
	ut = (msb << 8) + lsb;

	wiringPiI2CWriteReg8(fd, BMP180_REG_MEAS_CTL, \
			     BMP180_VAL_PRESS + (oss << 6));
	delay(osswait[oss]);
	msb = wiringPiI2CReadReg8(fd, BMP180_REG_OUT_MSB);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_OUT_LSB);
	xlsb = wiringPiI2CReadReg8(fd, BMP180_REG_OUT_XLSB);
	up = ((msb << 16) + (lsb << 8) + xlsb) >> (8 - oss);

	/* temperature calculation */
	x1 = ((ut - cal_AC6) * cal_AC5) >> 15;
	x2 = (cal_MC << 11) / (x1 + cal_MD);
	b5 = x1 + x2;
	if (temp != NULL)
		*temp = b5 / 160.0;	/* (b5 + 8) >> 4) / 10 */

	/* pressure calculation */
	b6 = b5 - 4000;
	b8 = (b6 * b6) >> 12;
	x1 = (cal_B2 * b8) >> 11;
	x2 = (cal_AC2 * b6) >> 11;
	x3 = x1 + x2;
	b3 = ((((cal_AC1 << 2) + x3) << oss) + 2) >> 2;
	x1 = (cal_AC3 * b6) >> 13;
	x2 = (cal_B1 * b8) >> 16;
	x3 = ((x1 + x2) + 2) >> 2;
	b4 = (cal_AC4 * (x3 + 32768)) >> 15;
	b7 = (up - b3) * (50000 >> oss);
	p = b7 < 0x80000000 ? (b7 << 1) / b4 : (b7 / b4) << 1;
	x1 = (p >> 8) * (p >> 8);
	x1 = (x1 * 3038) >> 16;
	x2 = (-7357 * p) >> 16;

	return (p + (x1 + x2 + 3791) / 16.0) / 100.0;
}

/* BMP180: get pressure using alternate (floating point) formula */
double BMP180_getPressureFP(int fd, int oss, double *temp)
{
	int msb, lsb, xlsb;
	double tu, pu, alpha, t, s, x, y, z;
	double c3, c4, b1, c5, c6, mc, md;
	double x0, x1, x2, y0, y1, y2, p0, p1, p2;

	wiringPiI2CWriteReg8(fd, BMP180_REG_MEAS_CTL, BMP180_VAL_TEMP);
	delay(BMP180_TEMP_MAX_TIME_MS);
	msb = wiringPiI2CReadReg8(fd, BMP180_REG_OUT_MSB);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_OUT_LSB);
	tu = msb * 256.0 + lsb;

	wiringPiI2CWriteReg8(fd, BMP180_REG_MEAS_CTL, \
			     BMP180_VAL_PRESS + (oss << 6));
	delay(osswait[oss]);
	msb = wiringPiI2CReadReg8(fd, BMP180_REG_OUT_MSB);
	lsb = wiringPiI2CReadReg8(fd, BMP180_REG_OUT_LSB);
	xlsb = wiringPiI2CReadReg8(fd, BMP180_REG_OUT_XLSB);
	pu = 256.0 * msb + lsb + xlsb / 256.0;

	c3 = 0.0048828125 * cal_AC3;	/* 160 * 2^-15 * AC3 */
	c4 = 0.000000030517578125 * cal_AC4;  /* 10^-3 * 2^-15 * AC4 */
	b1 = 0.000023841857910016 * cal_B1;  /* 160^2 * 2^-30 * B1 */

	c5 = 0.00000019073486328125 * cal_AC5;  /* 2^-15 / 160 * AC5 */
	c6 = cal_AC6;
	mc = 0.08 * cal_MC;  /* 2^11 / 160^2 * MC */
	md = cal_MD / 160.0;

	x0 = cal_AC1;
	x1 = 0.01953125 * cal_AC2;  /* 160 * 2^-13 * AC2 */
	x2 = 0.000762939453124864 * cal_B2;  /* 160^2 * 2^-25 * B2 */

	y0 = c4 * 32768;
	y1 = c4 * c3;
	y2 = c4 * b1;

	p0 = 2.364375;  /* (3791 - 8) / 1600 */
	p1 = 0.99298381805419921875;  /* 1 - (7357 * 2^-20) */
	p2 = 0.000004420871843836;  /* 3038 * 100 * 2^-36 */

	alpha = c5 * (tu - c6);
	t = alpha + mc / (alpha + md);

	s = t - 25;
	x = x2 * s * s + x1 * s + x0;
	y = y2 * s * s + y1 * s + y0;
	z = (pu - x) / y;

	if (temp != NULL)
		*temp = t;

	return p2 * z * z + p1 * z + p0;
}

