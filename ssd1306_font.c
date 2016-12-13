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
	printf("Usage:\n\t%s [-u | -a] font string0 ... stringN\n\n", progname);
	puts("Where:");
	puts("\t-u\t - update screen - do not initialize and clear (optional)");
	puts("\t-a\t - display all characters (optional)");
	puts("\tfont\t - path to PSF font file - no Unicode, 256 chars (mandatory)");
	puts("\tstringN\t - text to display on row N (optional)");
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int fd;
	int i, n, a, fontid, hb, noclr;
	int allch, cw, x, row;
	unsigned char buf[256];

	wiringPiSetupGpio();

	fd = OLED_initSPI(ADAFRUIT_SSD1306_128_64, OLED_SPI_CS, OLED_GPIO_RST,
			  OLED_GPIO_DC);
	if (fd < 0) {
		fprintf(stderr, "Unable to open SPI device: %s\n",
			strerror (errno));
		exit(-1);
	}

	if (argc < 2 || !strcmp(argv[1], "--?")) {
		help(argv[0]);
		exit(0);
	}

	if (!strcmp(argv[1], "-u")) {
		n = 2;
		noclr = 1;
		allch = 0;
	} else if (!strcmp(argv[1], "-a")) {
		n = 2;
		allch = 1;
		noclr = 0;
	} else {
		n = 1;
		noclr = 0;
		allch = 0;
	}

	fontid = OLED_loadPsf(argv[n]);
	if (fontid < 0) {
		fprintf(stderr, "Unable to open font file: %s\n", argv[n]);
		exit(-1);
	}

	if (allch) {
		/* all character dump mode */
		OLED_powerOn(fd);
		OLED_clearDisplay(fd);
		hb = OLED_getFontScreenSize(fontid, &cw, NULL);
		x = 0;
		row = 0;
		for(i = 0; i < 256; i++) {
			OLED_putChar(fd, fontid, x, row, 0, i);
			x += cw;	
			if (x >= 128) {
				x = 0;
				row += hb;
			}
			if (row >= 8) {
				x = 0;
				row = 0;
				sleep(8);
			}
		}
	} else {
		/* display mode */
		a = argc - n - 1;
		if (a) {
			if (a > 8)
				a = 8;
			hb = OLED_getFontScreenSize(fontid, NULL, NULL);
			if (!noclr) {
				OLED_powerOn(fd);
				OLED_clearDisplay(fd);
			}
			for(i = 0; i < a; i++) {
				OLED_putString(fd, fontid, 0, i * hb, 0,
				       argv[i + n + 1]);
			}
		}
	}

	return 0;
}
