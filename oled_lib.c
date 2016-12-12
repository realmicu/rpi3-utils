#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <wiringPi.h>
#include <wiringPiSPI.h>

#include "oled_lib.h"

#include "glcdfont.h"	/* from Adafruit GFX Library */
#include "psf.h"	/* from kbd-project.org */

/* *********************** */
/* *  SSD1306 SPI 128x64 * */
/* *********************** */
/* Registers / Commands / Arguments */
#define SSD1306_CMD_ADDR_MODE		0x20
#define SSD1306_CMD_VH_COL_RANGE	0x21
#define SSD1306_CMD_VH_PAGE_RANGE	0x22
#define SSD1306_CMD_CONTRAST		0x81
#define SSD1306_CMD_CHARGE_PUMP		0x8D
#define SSD1306_CMD_SEG_REMAP_INC	0xA0
#define SSD1306_CMD_SEG_REMAP_DEC	0xA1
#define SSD1306_CMD_DPY_RAM_ON		0xA4
#define SSD1306_CMD_DPY_RAM_OFF		0xA5
#define SSD1306_CMD_INVERSE_OFF		0xA6
#define SSD1306_CMD_INVERSE_ON		0xA7
#define SSD1306_CMD_MUX_RATIO		0xA8
#define SSD1306_CMD_DISPLAY_OFF		0xAE
#define SSD1306_CMD_DISPLAY_ON		0xAF
#define SSD1306_CMD_COM_SCAN_INC	0xC0
#define SSD1306_CMD_COM_SCAN_DEC	0xC8
#define SSD1306_CMD_DPY_OFFSET		0xD3
#define SSD1306_CMD_DIV_CLK_RATIO	0xD5
#define SSD1306_CMD_PRECHRG_PERIOD	0xD9
#define SSD1306_CMD_COM_PIN_CFG		0xDA
#define SSD1306_CMD_VCOMH_LEVEL		0xDB
#define SSD1306_ARG_ADDR_MODE_H		0x00
#define SSD1306_ARG_ADDR_MODE_V		0x01
#define SSD1306_ARG_ADDR_MODE_PAGE	0x02

/* Timing information - see datasheet */
#define SSD1306_RESET_WAIT_MS	1
#define SSD1306_SPI_CLK_HZ	16000000	/* 16 Mhz*/

/* ****************** */
/* Internal constants */
/* ****************** */
#define GPIO_PINS		28	/* number of Pi3 GPIO pins */
#define MAX_FONTS		16	/* maximum fonts that can be loaded */

/* ****************** */
/* Internal variables */
/* ****************** */
static int oled_type;
static int oled_rst_pin, oled_dc_pin;
static int memsize;		/* display memory in bytes */
static unsigned char *zero;	/* for display zeroing */

/* Font data */
/* index = 0 - standard 5x7(5*8) GLCD font from glcdfont.h */
/* index > 0 - dynamically loaded fonts */
/* Fonts with height larger than one byte (page) are stored as follows: */
/* upper_byte_0, lower_byte_0, upper_byte_1, lower_byte_1, ... */

struct fontDesc {
	int	fontWidth, fontHeight;	/* in pixels */
	int	fontByteH;	/* height in bytes (pages) */
	int	fontCellW;	/* cell width, allows space between fonts */
	unsigned char *fontImg;	/* font image table */
	/* internals calculated for speed as they are used frequently */
	int	fontLJustB, fontSizeB, fontCellSz;
};
static struct fontDesc fnt[MAX_FONTS];
static int fntcnt, curfnt;

/* ************************** */
/* ************************** */
/* **  Internal functions  ** */
/* ************************** */
/* ************************** */

static int OLED_spiWrite(int fd, unsigned char byte)
{
	return write(fd, &byte, 1);
}

static void OLED_fontCalc(struct fontDesc *fnd)
{
	/* how many empty vertical bytes (pages) to pad left */
	fnd->fontLJustB = ((fnd->fontCellW - fnd->fontWidth) >> 1) * \
			  fnd->fontByteH;
	/* character size in bytes */
	fnd->fontSizeB = fnd->fontWidth * fnd->fontByteH;
	/* character cell size in bytes */
	fnd->fontCellSz = fnd->fontCellW * fnd->fontByteH;
}

static void OLED_invMem(unsigned char *s, int len)
{
	int i;

	for(i = 0; i < len; i++)
		s[i] = ~s[i];
}

/* ************************ */
/* ************************ */
/* **  Public functions  ** */
/* ************************ */
/* ************************ */

/* Initialize device */
int OLED_initSPI(int type, int cs, int rst_pin, int dc_pin)
{
	int fd;

	oled_type = 0;
	oled_rst_pin = -1;
	oled_dc_pin = -1;

	if (type == ADAFRUIT_SSD1306_128_64) {

		fd = wiringPiSPISetup(cs, SSD1306_SPI_CLK_HZ);

		if (fd > 0) {
			if (rst_pin >= 0 && rst_pin < GPIO_PINS) {
				oled_rst_pin = rst_pin;
				pinMode(oled_rst_pin, OUTPUT);
			}
			if (dc_pin >= 0 && dc_pin < GPIO_PINS) {
				oled_dc_pin = dc_pin;
				pinMode(oled_dc_pin, OUTPUT);
			}
			oled_type = type;

			memsize = 1024;	/* 8 pages * 128 bytes */

			/* prepare cls framebuffer */
			zero = (unsigned char *)malloc(memsize);
			memset(zero, 0, memsize);

			/* load default font (id=0) */
			fnt[0].fontWidth = 5;
			fnt[0].fontHeight = 7;
			fnt[0].fontByteH = 1;
			fnt[0].fontCellW = 6;	/* pad to 6 pixels */
			fnt[0].fontImg = font;
			OLED_fontCalc(&fnt[0]);
			fntcnt = 1;
		}
	} else
		fd = -1;

	return fd;
}

/* Device power on */
void OLED_powerOn(int fd)
{
	if (oled_type == ADAFRUIT_SSD1306_128_64) {

		digitalWrite(oled_rst_pin, HIGH);
		delay(SSD1306_RESET_WAIT_MS);
		digitalWrite(oled_rst_pin, LOW);
		delay(SSD1306_RESET_WAIT_MS);
		digitalWrite(oled_rst_pin, HIGH);

		digitalWrite(oled_dc_pin, LOW);

		/* initialization sequence - see datasheet */
		/* values that get reset to default are omitted */

		/* display off */
		OLED_spiWrite(fd, SSD1306_CMD_DISPLAY_OFF);

		/* set column address (low, high) */
		/* OLED_spiWrite(fd, 0x00); */
		/* OLED_spiWrite(fd, 0x10); */

		/* set GDDRAM page start address */
		/* OLED_spiWrite(fd, 0xB0); */

		/* set memory addressing mode */
		OLED_spiWrite(fd, SSD1306_CMD_ADDR_MODE);
		OLED_spiWrite(fd, SSD1306_ARG_ADDR_MODE_H);

		/* set display clock divide ratio to 100 frames/sec */
		OLED_spiWrite(fd, SSD1306_CMD_DIV_CLK_RATIO);
		OLED_spiWrite(fd, 0x80);

		/* set multiplex ratio */
		OLED_spiWrite(fd, SSD1306_CMD_MUX_RATIO);
		OLED_spiWrite(fd, 0x3F);

		/* set display offset */
		/* OLED_spiWrite(fd, SSD1306_CMD_DPY_OFFSET); */
		/* OLED_spiWrite(fd, 0x00); */

		/* set display start line */
		/* OLED_spiWrite(fd, 0x40); */

		/* set charge pump */
		OLED_spiWrite(fd, SSD1306_CMD_CHARGE_PUMP);
		OLED_spiWrite(fd, 0x14);	/* internal Vcc */

		/* set segment remap */
		OLED_spiWrite(fd, SSD1306_CMD_SEG_REMAP_DEC);

		/* set scan direction */
		OLED_spiWrite(fd, SSD1306_CMD_COM_SCAN_DEC);

		/* set contrast */		
		OLED_spiWrite(fd, SSD1306_CMD_CONTRAST);
		OLED_spiWrite(fd, 0xCF);	/* internal Vcc */

		/* set pre-charge period */
		OLED_spiWrite(fd, SSD1306_CMD_PRECHRG_PERIOD);
		OLED_spiWrite(fd, 0xF1);

		/* set COM pins */
		OLED_spiWrite(fd, SSD1306_CMD_COM_PIN_CFG);
		OLED_spiWrite(fd, 0x12);

		/* set VCOMH deselect level*/
		OLED_spiWrite(fd, SSD1306_CMD_VCOMH_LEVEL);
		OLED_spiWrite(fd, 0x40);

		/* connect display to GDDRAM */
		OLED_spiWrite(fd, SSD1306_CMD_DPY_RAM_ON);

		/* set normal (no inverse) display */
		/* OLED_spiWrite(fd, SSD1306_CMD_INVERSE_OFF); */

		/* display on */
		OLED_spiWrite(fd, SSD1306_CMD_DISPLAY_ON);
	}
}

/* Device power off */
void OLED_powerOff(int fd)
{
	if (oled_type == ADAFRUIT_SSD1306_128_64) {
		digitalWrite(oled_dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_DISPLAY_OFF);
	}
}

/* *********************** */
/* Direct access functions
/* *********************** */
/* These operate on text (full byte) fields without any need
   to modify data already on screen (no transparency etc),
   therefore no framebuffer is needed. */

/* Clear display screen */
void OLED_clearDisplay(int fd)
{
	if (oled_type == ADAFRUIT_SSD1306_128_64) {
		digitalWrite(oled_dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_ADDR_MODE);
		OLED_spiWrite(fd, SSD1306_ARG_ADDR_MODE_H);
		OLED_spiWrite(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, 127);
		OLED_spiWrite(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, 7);
		digitalWrite(oled_dc_pin, HIGH);
		write(fd, zero, memsize);
	}
}

/* Show checker pattern */
void OLED_testPattern(int fd, int type)
{
	int i;
	unsigned char *pat;
 
	if (oled_type == ADAFRUIT_SSD1306_128_64) {

		pat = (unsigned char *)malloc(memsize);

		if (!type)
			/* all bits on */
			memset(pat, 0xFF, memsize);
		else if (type == 1)
			/* 1x1 checker pattern */
			for(i=0; i < memsize; i++)
				pat[i] = (i & 0x1) ? 0xAA : 0x55;
		else if (type == 2)
			/* 2x2 checker pattern */
			for(i=0; i < memsize; i++)
				pat[i] = (i & 0x2) ? 0xCC : 0x33;
		else if (type == 3)
			/* 4x4 checker pattern */
			for(i=0; i < memsize; i++)
				pat[i] = (i & 0x4) ? 0xF0 : 0x0F;
		else if (type == 4)
			/* 4-pixel-wide vertical stripes */
			for(i=0; i < memsize; i++)
				pat[i] = (i & 0x4) ? 0xFF : 0x00;
		else if (type == 5)
			/* 4-pixel-high horizontal stripes */
			for(i=0; i < memsize; i++)
				pat[i] = 0x0F;

		digitalWrite(oled_dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_ADDR_MODE);
		OLED_spiWrite(fd, SSD1306_ARG_ADDR_MODE_H);
		OLED_spiWrite(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, 127);
		OLED_spiWrite(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, 7);
		digitalWrite(oled_dc_pin, HIGH);
		write(fd, pat, memsize);

		free(pat);
	}
}

/* Show test font */
void OLED_testFont(int fd, int start)
{
	if (oled_type == ADAFRUIT_SSD1306_128_64) {
		digitalWrite(oled_dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_ADDR_MODE);
		OLED_spiWrite(fd, SSD1306_ARG_ADDR_MODE_H);
		OLED_spiWrite(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, 127);
		OLED_spiWrite(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, 7);
		digitalWrite(oled_dc_pin, HIGH);
		write(fd, &font[start * 5], memsize);
	}
}

/* Print character */
/* x - column in pixels */
/* row - y coordinate in bytes (pages!) */
/* inv - 1 for inversed background */
/* Return: x coordinate for next letter (may be wrapped) */
int OLED_putChar(int fd, int fontid, int x, int row, int inv, char c)
{
	struct fontDesc *ft;
	unsigned char *tptr, *cptr;

	ft = &fnt[fontid];

	if (oled_type == ADAFRUIT_SSD1306_128_64) {
		tptr = ft->fontImg + c * ft->fontSizeB;
		cptr = (unsigned char *)malloc(ft->fontCellSz);
		memset(cptr, 0, ft->fontCellSz);
		memcpy(cptr + ft->fontLJustB, tptr, ft->fontSizeB);
		if (inv)
			OLED_invMem(cptr, ft->fontCellSz);
		digitalWrite(oled_dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_ADDR_MODE);
		OLED_spiWrite(fd, SSD1306_ARG_ADDR_MODE_V);
		OLED_spiWrite(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWrite(fd, x);
		OLED_spiWrite(fd, (x + ft->fontCellW - 1) & 0x7F);
		OLED_spiWrite(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWrite(fd, row);
		OLED_spiWrite(fd, (row + ft->fontByteH - 1) & 0x07);
		digitalWrite(oled_dc_pin, HIGH);
		write(fd, cptr, ft->fontCellSz);
		free(cptr);

		return (x + ft->fontCellW) & 0x7F;
	}

	return -1;
}

/* Print string */
/* x - column in pixels */
/* row - y coordinate in bytes (pages!) */
/* inv - 1 for inversed background */
/* Return: x coordinate for next letter (may be wrapped) */
int OLED_putString(int fd, int fontid, int x, int row, int inv,
		   const unsigned char *s)
{
	int i, len, memlen, memw;
	struct fontDesc *ft;
	unsigned char *tptr, *cptr;

	ft = &fnt[fontid];

	len = strlen(s);
	if (!len)
		return -1;

	if (oled_type == ADAFRUIT_SSD1306_128_64) {
		memlen = len * ft->fontCellSz;
		memw = len * ft->fontCellW;
		cptr = (unsigned char *)malloc(memlen);
		memset(cptr, 0, memlen);
		for(i = 0; i < len; i++) {
			tptr = ft->fontImg + s[i] * ft->fontSizeB;
			memcpy(cptr + ft->fontLJustB + i * ft->fontCellSz,
			       tptr, ft->fontSizeB);
		}
		if (inv)
			OLED_invMem(cptr, memlen);
		digitalWrite(oled_dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_ADDR_MODE);
		OLED_spiWrite(fd, SSD1306_ARG_ADDR_MODE_V);
		OLED_spiWrite(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWrite(fd, x);
		OLED_spiWrite(fd, (x + memw - 1) & 0x7F);
		OLED_spiWrite(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWrite(fd, row);
		OLED_spiWrite(fd, (row + ft->fontByteH - 1) & 0x07);
		digitalWrite(oled_dc_pin, HIGH);
		write(fd, cptr, memlen);
		free(cptr);

		return (x + memw) & 0x7F;
	}

	return -1;
}

/* ********************* */
/* Framebuffer functions
/* ********************* */
/* Screen operates only in MOSI mode, so there is no way to
   access GDRAM. Therefore framebuffer must be implemented
   for fine-grained graphical function like pixel drawing.
   These access particular bits in already drawn bytes. */

/* Refresh display with framebuffer data */
/*
void OLED_refreshScreen(int fd, int screen_id)
{
	if (screen_id >= scrcnt)
		return;

	if (oled_type == ADAFRUIT_SSD1306_128_64) {
		digitalWrite(oled_dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, 127);
		OLED_spiWrite(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, 7);
		digitalWrite(oled_dc_pin, HIGH);
		write(fd, scr[screen_id], memsize);
	}
}
*/

/* Refresh part of display with framebuffer data */
/*
void OLED_refreshBox(int fd, int screen_id, int x, int y, int w, int h)
{
	int i, ps, pe;
	unsigned char *sptr;

	if (screen_id >= scrcnt)
		return;

	if (oled_type == ADAFRUIT_SSD1306_128_64) {
		ps = y >> 3;
		pe = (y + h - 1) >> 3;
		digitalWrite(oled_dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWrite(fd, x);
		OLED_spiWrite(fd, x + w - 1);
		OLED_spiWrite(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWrite(fd, ps);
		OLED_spiWrite(fd, pe);
		digitalWrite(oled_dc_pin, HIGH);
		sptr = scr[screen_id] + ps * xs + x;
		for(i = ps; i <= pe; i++) {
			write(fd, sptr, w);
			sptr += xs;
		}
	}
}
*/

/* Put pixel */
/*
void OLED_putPixel(int fd, int screen_id, int x, int y, int value)
{
	if (oled_type == ADAFRUIT_SSD1306_128_64) {
		if (value)
			scr[screen_id][(y >> 3) * xs + x] |= 1 << (y & 0x07);
		else
			scr[screen_id][(y >> 3) * xs + x] &= ~(1 << (y & 0x07));
	}
}
*/

/* Print one character */
/*
void OLED_putChar(int fd, int screen_id, int font_id, int row, int col,
		  int flags, char c)
{
	int i, cloff;
	unsigned char *sptr, *fptr;

	if (oled_type == ADAFRUIT_SSD1306_128_64) {
		cloff = (fnt[font_id].fontCellW - fnt[font_id].fontWidth) >> 1;
		sptr = scr[screen_id] + row * xs + fnt[font_id].fontXJust + \
		       col * fnt[font_id].fontCellW + cloff;
		fptr = fnt[font_id].fontImg + c * fnt[font_id].fontWidth * \
		       fnt[font_id].fontByteH;
		for(i = 0; i < fnt[font_id].fontByteH; i++) {
			memcpy(sptr, fptr, fnt[font_id].fontWidth);
			sptr += xs;
			fptr += fnt[font_id].fontWidth;
		}
	}
}
*/

/* Print string */
/*
void OLED_putString(int fd, int screen_id, int row, int col, int flags,
		    const char *s)
{
	if (oled_type == ADAFRUIT_SSD1306_128_64) {

	}
}
*/
