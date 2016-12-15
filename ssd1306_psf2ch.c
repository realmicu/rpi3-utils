/* 						    */
/* This program converts PSFv1 and PSFv2 font files */
/* (256 characters without Unicode tables only) to  */
/* array in C-program source code		    */
/* 						    */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <time.h>
#include <sys/time.h>

#include <wiringPi.h>

#include "oled_lib.h"

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s infile\n\n", progname);
	puts("Where:");
	puts("\tinfile\t - path to PSF font file - no Unicode, 256 chars (mandatory)");
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int fontid;
	unsigned char *fi;
	int fl;
	int i, j, w, h, cw, ch, bh, chsz;

	if (argc < 2 || !strcmp(argv[1], "--?")) {
		help(argv[0]);
		exit(0);
	}

	fontid = OLED_loadPsf(argv[1]);
	if (fontid < 0) {
		fprintf(stderr, "Unable to open font file: %s\n", argv[1]);
		exit(-1);
	}

	fi = OLED_getFontImage(fontid, &fl);
	if (!fi)
		exit(-1);
	chsz = OLED_getFontScreenSize(fontid, &w, &h, &cw, &ch, &bh);

	puts("#ifndef FONT_FILE\n#define FONT_FILE\n");
	puts("/*\n    Font information:\n");
	printf("    File name: %s\n", argv[1]);
	puts("    Characters: 256");
	printf("    Font width in pixels: %d\n", w);
	printf("    Font height in pixels: %d\n", h);
	printf("    Font box width in pixels: %d\n", cw);
	printf("    Font box height in pixels: %d\n", ch);
	printf("    Font box height in bytes (pages): %d\n", bh);
	printf("    Font size in bytes: %d\n", chsz);
	puts("*/\n\nstatic unsigned char font[] = {");

	for(i = 0; i < 256; i++)
		for(j = 0; j < chsz; j++) {
			/* print tab at beginning of each 8th byte */
			if (!(j & 0x07))
				printf("\t");
			/* print byte */
			printf("0x%02X", fi[i * chsz + j]);
			fl--;
			if (!fl)
				break; /* if end of image, exit */
			/* print separator */
			printf(", ");
			/* print character in comment for 1st line */
			if (j == 7) {
				if (i >= 0x20 && i <= 0x7a)
					printf("\t/* 0x%02X = \'%c\' */", i, i);
				else
					printf("\t/* 0x%02X */", i);
			}
			/* newline after max 8 bytes */
			if ((j & 0x07) == 7 || j == chsz - 1)
				puts("");
		}

	puts("\n};\n\n#endif");
	return 0;
}
