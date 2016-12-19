#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <time.h>
#include <sys/time.h>

#include <wiringPi.h>

#include "oled_lib.h"

#define OLED_SPI_CS		0
#define OLED_GPIO_RST		12
#define OLED_GPIO_DC		16

/* Show help */

void help(char *progname)
{
	printf("Usage:\n\t%s [-u] bmpfile\n\n", progname);
	puts("Where:");
	puts("\t-u\t - update screen - do not initialize and clear (optional)");
	puts("\tbmpfile\t - bitmap to display (mandatory)");
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int fd;
	int n, bmpid, xw, bh;

	wiringPiSetupGpio();

	fd = OLED_initSPI(ADAFRUIT_SSD1306_128_64, OLED_SPI_CS, OLED_GPIO_RST,
			  OLED_GPIO_DC);
	if (fd < 0) {
		fprintf(stderr, "Unable to open SPI device: %s\n",
			strerror (errno));
		exit(-1);
	}

	if (argc < 2 || (argc > 1 && !strcmp(argv[1], "--?"))) {
		help(argv[0]);
		exit(0);
	}

	if (strcmp(argv[1], "-u")) {
		OLED_powerOn(fd);
		OLED_clearDisplay(fd);
		n = 1;
	} else
		n = 2;

	if (!argv[n])
		exit(0);

	bmpid = OLED_loadBitmap(argv[n]);
	if (bmpid < 0) {
		fprintf(stderr, "Error opening bitmap file: %s\n", argv[n]);
		exit(-1);
	}

	OLED_getImageScreenSize(bmpid, &xw, NULL, &bh);

	OLED_putImage(fd, bmpid, (128 - xw) >> 1, (8 - bh) >> 1);

	return 0;
}
