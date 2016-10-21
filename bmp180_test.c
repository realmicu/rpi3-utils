#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wiringPi.h>
#include <wiringPiI2C.h>

#include "bmp180_lib.h"

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(void)
{

	double p, t;
	double p2, t2;

	wiringPiSetup();
	int fd = wiringPiI2CSetup(BMP180_I2C_ADDR);
	if (fd < 0)
	{
		fprintf(stderr, "Unable to open I2C device: %s\n",
			strerror (errno));
		exit(-1);
	}

	/* Soft reset */
	BMP180_softReset(fd);

	/* Check if BMP180 responds */
	if (!BMP180_isPresent(fd))
	{
		fputs("Unable to find BMP180 sensor chip.\n", stderr);
		exit(-2);
	}

	BMP180_getCalibrationData(fd);

	/* BMP180_dumpCalibrationData(); */

	p = BMP180_getPressure(fd, BMP180_OSS_MODE_UHR, &t);
	
	printf(" p = %5.1lf hPa\n", p);
	printf(" t = %+4.1lf deg C\n", t);

	p2 = BMP180_getPressureFP(fd, BMP180_OSS_MODE_UHR, &t2);
	
	printf("p2 = %5.1lf hPa\n", p2);
	printf("t2 = %+4.1lf deg C\n", t2);
	
	return 0;
}
