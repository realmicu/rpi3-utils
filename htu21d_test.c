#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <wiringPi.h>
#include <wiringPiI2C.h>

#include "htu21d_lib.h"

int main(void)
{
	wiringPiSetup();
	int fd = wiringPiI2CSetup(HTU21D_I2C_ADDR);
	if (fd < 0)
	{
		fprintf(stderr, "Unable to open I2C device: %s\n",
			strerror (errno));
		exit(-1);
	}

	/* Soft reset, device starts in 12-bit humidity / 14-bit temperature */
	HTU21D_softReset(fd);
	
	printf(" t = %+4.1lf deg C\n", HTU21D_getTemperature(fd));
	printf(" h = %4.1lf %%rh\n", HTU21D_getHumidity(fd));
	
	return 0;
}
