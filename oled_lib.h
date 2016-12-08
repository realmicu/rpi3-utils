#ifndef _OLED_LIB_H_
#define _OLED_LIB_H_

/* OLED types */
#define ADAFRUIT_SSD1306_128_64		1

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

#endif
