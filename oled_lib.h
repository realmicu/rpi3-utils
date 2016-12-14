#ifndef _OLED_LIB_H_
#define _OLED_LIB_H_

/* OLED types */
#define ADAFRUIT_SSD1306_128_64		1

/* Default font */
#define OLED_DEFAULT_FONT		0	/* 5x7 font */

/* Initialize device */
int OLED_initSPI(int type, int cs, int rst_pin, int dc_pin);

/* Device power on/off */
void OLED_powerOn(int fd);
void OLED_powerOff(int fd);

/* Clear display screen */
void OLED_clearDisplay(int fd);

/* Show test patterns */
/* type: */
/* 	0 - all bits on */
/* 	1 - 1x1 checker pattern */
/* 	2 - 2x2 checker pattern */
/* 	3 - 4x4 checker pattern */
/* 	4 - 4-pixel-wide vertical stripes */
/* 	5 - 4-pixel-high horizontal stripes */
void OLED_testPattern(int fd, int type);

/* Show test font */
/* start - ASCII code of first character */
void OLED_testFont(int fd, int start);

/* Print character */
/* x is column in pixels and row is y coordinate in bytes (pages!) */
/* function returns x column of next character */
int OLED_putChar(int fd, int fontid, int x, int row, int inv, char c);

/* Print string */
/* x - column in pixels */
/* row - y coordinate in bytes (pages!) */
/* inv - 1 for inversed background */
/* Return: x coordinate for next letter (may be wrapped) */
int OLED_putString(int fd, int fontid, int x, int row, int inv,
                   const unsigned char *s);

/* Load PSF font file (256 chars, no Unicode) and return fontid */
int OLED_loadPsf(const unsigned char *psffile);

/* Get font screen dimensions */
/* Return: character size in bytes */
int OLED_getFontScreenSize(int fontid, int *width, int *height,
			   int *cellwidth, int *cellheight, int *byteheight);

/* Returns pointer to font memory area and its size in bytes */
unsigned char *OLED_getFontImage(int fontid, int *bytes);

#endif
