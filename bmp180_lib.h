#ifndef _BMP180_LIB_H_
#define _BMP180_LIB_H_

/* I2C bus address */
#define BMP180_I2C_ADDR		0x77

/* Oversampling modes */
#define BMP180_OSS_MODE_ULP	0		/* ultra low power */
#define BMP180_OSS_MODE_STD	1		/* standard */
#define BMP180_OSS_MODE_HR	2		/* high resolution */
#define BMP180_OSS_MODE_UHR	3		/* ultra high resolution */

/* Check if sensor chip is available */
int BMP180_isPresent(int fd);

/* Soft reset */
void BMP180_softReset(int fd);

/* Get calibration data */
void BMP180_getCalibrationData(int fd);

/* Display calibration data */
void BMP180_dumpCalibrationData(void);

/* Calculate pressure using official Bosch formula */
/* Optionally stores calculated temperature in temp var */
/* Pressure unit: hPa */
/* Temperature unit: Celsius */
double BMP180_getPressure(int fd, int oss, double *temp);

/* Calculate pressure using alternate (floating point) formula */
/* Optionally stores calculated temperature in temp var */
/* Pressure unit: hPa */
/* Temperature unit: Celsius */
double BMP180_getPressureFP(int fd, int oss, double *temp);

#endif
