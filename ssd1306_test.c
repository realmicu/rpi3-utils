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

/*
void help(char *progname)
{
	printf("Usage:\n\t%s [-s] [-t] [-c N]\n\n", progname);
	puts("Where:");
	puts("\t-s\t - use system-wide user semaphore for I2C bus access (optional)");
	puts("\t-t\t - include timestamp (optional)");
	puts("\t-c N\t - run continously every N seconds (optional)");
} */

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int fd;
	int i, a;

	wiringPiSetupGpio();

	fd = OLED_initSPI(ADAFRUIT_SSD1306_128_64, OLED_SPI_CS, OLED_GPIO_RST,
			  OLED_GPIO_DC);
	if (fd < 0)
	{
		fprintf(stderr, "Unable to open SPI device: %s\n",
			strerror (errno));
		exit(-1);
	}

	a = argc - 1;

	if (a) {
		if (a > 8)
			a = 8;
		OLED_powerOn(fd);
		OLED_clearDisplay(fd);
		for(i = 0; i < a; i++)
			OLED_putString(fd, OLED_DEFAULT_FONT, 0, i, 0,
				       argv[i + 1]);
	}
	else {
		/* no arguments - predefined test procedures */
		puts("Powering on display.");
		OLED_powerOn(fd);

		puts("OK, waiting 5 seconds...");
		sleep(5);

		puts("Zeroing video memory.");
		OLED_clearDisplay(fd);

		puts("OK, waiting 5 seconds...");
		sleep(5);

		for(i = 0; i < 6; i++) {
			printf("Test pattern %d.\n", i);
			OLED_testPattern(fd, i);
			puts("OK, waiting 5 seconds...");
			sleep(5);
		}
	
		puts("Test font.");
		OLED_testFont(fd, 0);

		puts("OK, waiting 5 seconds...");
		sleep(5);

		puts("Displaying some characters.");
		OLED_clearDisplay(fd);
		for(i = 0; i < 8; i++) {
			OLED_putChar(fd, OLED_DEFAULT_FONT, 0 + (i << 3), i, i & 1, 'T');
			OLED_putChar(fd, OLED_DEFAULT_FONT, 6 + (i << 3), i, i & 1,  'e');
			OLED_putChar(fd, OLED_DEFAULT_FONT, 12 + (i << 3), i, i & 1, 's');
			OLED_putChar(fd, OLED_DEFAULT_FONT, 18 + (i << 3), i, i & 1, 't');
		}
		puts("OK, waiting 5 seconds...");
		sleep(5);

		puts("Powering off display.");
		OLED_powerOff(fd);
	}

	return 0;
}
