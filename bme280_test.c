#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wiringPi.h>
#include <wiringPiI2C.h>

#include "bme280_lib.h"

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(void)
{
	int addr, fd;
	double p, t, h;

	wiringPiSetup();
	addr = BME280_I2C_ALT_ADDR;
	fd = BME280_initPi(&addr);
	if (fd < 0) {
		fprintf(stderr, "Unable to open I2C device: %s\n",
			strerror (errno));
		exit(-1);
	}

	/* Soft reset */
	BME280_softReset(fd);

	/* Check if BME280 responds */
	if (!BME280_isPresent(fd)) {
		fprintf(stderr,
			"Unable to find BME280 sensor chip at 0x%02x.\n", addr);
		exit(-2);
	} else
		printf("BME280 sensor found at 0x%02x.\n", addr);

	BME280_setupFullMode(fd);

	BME280_getCalibrationData(fd);

	#if 0
	BME280_dumpCalibrationData();
	puts("");
	#endif

	if (!BME280_getSensorData(fd, &t, &p, &h)) {
		printf(" t = %+4.1lf deg C\n", t);
		printf(" p = %5.1lf hPa\n", p);
		printf(" h = %4.1lf %%rh\n", h);
	}

	return 0;
}
