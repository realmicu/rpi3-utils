#ifndef _BME280_LIB_H_
#define _BME280_LIB_H_

/* I2C bus address */
#define BME280_I2C_DEF_ADDR	0x77
#define BME280_I2C_ALT_ADDR	0x76

/* Sensor power modes */
#define BME280_SLEEP_MODE	0x00
#define BME280_FORCED_MODE	0x01
#define BME280_NORMAL_MODE	0x03

/* Oversampling */
#define BME280_NO_OVERSAMPLING		0x00
#define BME280_OVERSAMPLING_1X		0x01
#define BME280_OVERSAMPLING_2X		0x02
#define BME280_OVERSAMPLING_4X		0x03
#define BME280_OVERSAMPLING_8X		0x04
#define BME280_OVERSAMPLING_16X		0x05

/* Standby duration */
#define BME280_STANDBY_TIME_1_MS	0x00
#define BME280_STANDBY_TIME_62_5_MS	0x01
#define BME280_STANDBY_TIME_125_MS	0x02
#define BME280_STANDBY_TIME_250_MS	0x03
#define BME280_STANDBY_TIME_500_MS	0x04
#define BME280_STANDBY_TIME_1000_MS	0x05
#define BME280_STANDBY_TIME_10_MS	0x06
#define BME280_STANDBY_TIME_20_MS	0x07

/* Filter coefficient */
#define BME280_FILTER_COEFF_OFF		0x00
#define BME280_FILTER_COEFF_2		0x01
#define BME280_FILTER_COEFF_4		0x02
#define BME280_FILTER_COEFF_8		0x03
#define BME280_FILTER_COEFF_16		0x04

/* Ranges */
#define BME280_PRESS_MIN		 30000.0
#define BME280_PRESS_MAX		110000.0
#define BME280_TEMP_MIN			-40.0
#define BME280_TEMP_MAX			 85.0
#define BME280_HUMID_MIN		  0.0
#define BME280_HUMID_MAX		100.0

/* Device initialization (for RPi I2C bus) */
/* For default address, use NULL or variable with value 0 */
/* Returns < 0 if initialization failed, otherwise fd */
/* On success, argument variable is set to I2C address */
int BME280_initPi(int *i2caddr);

/* Check if sensor chip is available */
int BME280_isPresent(int fd);

/* Soft reset */
void BME280_softReset(int fd);

/* Get calibration data */
void BME280_getCalibrationData(int fd);

/* Display calibration data */
void BME280_dumpCalibrationData(void);

/* Setup device - all measurements in forced mode, filter off,
 * OSS is x1, values are 16-bit (weather station preset)
 */
void BME280_setupFullMode(int fd);

/* Get all 3 sensor values using official Bosch floating point formula */
/* Pressure in hPa, temperature in Celsius, humidity in percent */
/* Return 0 if readings are OK */
int BME280_getSensorData(int fd, double *temp, double *press, double *humid);

#endif
