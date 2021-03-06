#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <wiringPi.h>
#include <wiringPiSPI.h>

#include "oled_lib.h"

#include "glcdfont.h"	/* from Adafruit GFX Library */
#include "psf_header.h"	/* from kbd-project.org */
#include "bmp_header.h"

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
#define OLED_DEVICES		1	/* number of supported devices */
#define GPIO_PINS		28	/* number of Pi3 GPIO pins */
#define MAX_FONTS		16	/* maximum fonts that can be loaded */
#define MAX_IMAGES		256	/* maximum images that can be loaded */
#define OLED_CHIP_SSD1306	0	/* chip is SSD1306 */
#define OLED_CHIP_SH1106	1	/* chip is SH1106 */

/* ****************** */
/* Internal variables */
/* ****************** */
struct oledDesc {
	int type;		/* screen type */
	int chip;		/* chip family */
	int memsize;		/* memory size in bytes */
	int xsize, ysize;	/* screen dimensions in pixels */
	void *chip_data;	/* chip-specific data */
};

struct ssd1306Desc {
	int rst_pin;		/* BCM GPIO for reset signal */
	int dc_pin;		/* BCM GPIO for Data/Command */
	int byteh;		/* height in bytes (pages) */
};

static struct ssd1306Desc ssd1306Info = {
	-1, -1, 8
};

static struct oledDesc oledInfo[OLED_DEVICES] = {
	{
		ADAFRUIT_SSD1306_128_64,
		OLED_CHIP_SSD1306,
		1024,
		128,
		64,
		(void*)&ssd1306Info
	}
};

static unsigned char *zero;	/* for display zeroing */
static struct oledDesc *od;

/*
 * Font data.
 *
 * index = 0 - standard 5x7(5*8) GLCD font from glcdfont.h
 * index > 0 - dynamically loaded fonts
 * Fonts with height larger than one byte (page) are stored as follows:
 * upper_byte_0, lower_byte_0, upper_byte_1, lower_byte_1, ...
 */

struct fontDesc {
	int	fontWidth, fontHeight;	/* in pixels */
	int	fontByteH;	/* height in bytes (pages) */
	int	fontCellW;	/* cell width, allows space between fonts */
	unsigned char *fontImg;	/* font image table */
	/* internals calculated for speed as they are used frequently */
	int	fontLJustB, fontSizeB, fontCellSz;
};
static struct fontDesc fnt[MAX_FONTS];
static int fntcnt;

/*
 * Image data.
 *
 * Similar to fonts, all images get an ID when loaded and stay in memory
 * until process ends. Images loaded directly to screen do not use mask
 * (no read access to GDRAM).
 */

struct imgDesc {
	int imgHeight, imgWidth;
	unsigned char *imgData;
	unsigned char *maskData;
	/* internals calculated for speed as they are used frequently */
	int imgByteH, imgSizeB;
};
static struct imgDesc img[MAX_IMAGES];
static int imgcnt;

/* ************************** */
/* ************************** */
/* **  Internal functions  ** */
/* ************************** */
/* ************************** */

/* Basic byte write */
static int OLED_spiWrite(int fd, unsigned char byte)
{
	return write(fd, &byte, 1);
}

/* Calculate speedup values for font */
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

/* Invert (negate) image */
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

/*
 * Initialize device.
 *
 *   Parameters:
 *     type 	- screen type
 *     cs	- SPI slave number (0/1 on Rpi3)
 *     rst_pin	- reset GPIO pin (BCM number)
 *     dc_pin	- Data/Command GPIO pin (BCM number)
 *
 *   Return:
 *     File descriptor of SPI device or -1 if error
 */
int OLED_initSPI(int type, int cs, int rst_pin, int dc_pin)
{
	int fd;
	struct ssd1306Desc *sd;

	od = NULL;

	if (type < 0 || type >= OLED_DEVICES)
		return -1;

	od = &oledInfo[type];		/* set global pointer */

	if (type == ADAFRUIT_SSD1306_128_64) {

		sd = (struct ssd1306Desc*)od->chip_data;

		fd = wiringPiSPISetup(cs, SSD1306_SPI_CLK_HZ);

		if (fd > 0) {
			if (rst_pin >= 0 && rst_pin < GPIO_PINS) {
				sd->rst_pin = rst_pin;
				pinMode(sd->rst_pin, OUTPUT);
			}
			if (dc_pin >= 0 && dc_pin < GPIO_PINS) {
				sd->dc_pin = dc_pin;
				pinMode(sd->dc_pin, OUTPUT);
			}

			/* prepare cls framebuffer */
			zero = (unsigned char *)malloc(od->memsize);
			memset(zero, 0, od->memsize);

			/* load default font (id=0), size 5x7, */
			/* width padded to 6 pixels, 1-byte high */
			fntcnt = 0;
			OLED_loadFont(5, 7, 6, 1, font);

			/* no predefined images */
			imgcnt = 0;
		}
	} else
		fd = -1;

	return fd;
}

/* 
 * Device power on.
 */
void OLED_powerOn(int fd)
{
	struct ssd1306Desc *sd;

	if (!od)
		return;

	if (od->chip == OLED_CHIP_SSD1306) {

		sd = (struct ssd1306Desc*)od->chip_data;

		digitalWrite(sd->rst_pin, HIGH);
		delay(SSD1306_RESET_WAIT_MS);
		digitalWrite(sd->rst_pin, LOW);
		delay(SSD1306_RESET_WAIT_MS);
		digitalWrite(sd->rst_pin, HIGH);

		digitalWrite(sd->dc_pin, LOW);

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

/*
 * Device power off.
 */
void OLED_powerOff(int fd)
{
	struct ssd1306Desc *sd;

	if (!od)
		return;

	if (od->chip == OLED_CHIP_SSD1306) {
		sd = (struct ssd1306Desc*)od->chip_data;
		digitalWrite(sd->dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_DISPLAY_OFF);
	}
}

/*
 * ***********************
 * Direct access functions
 * ***********************
 *
 * These operate on text (full byte) fields without any need
 * to modify data already on screen (no transparency etc),
 * therefore no framebuffer is needed.
 */

/*
 * Clear display screen.
 */
void OLED_clearDisplay(int fd)
{
	struct ssd1306Desc *sd;

	if (!od)
		return;

	if (od->chip == OLED_CHIP_SSD1306) {
		sd = (struct ssd1306Desc*)od->chip_data;
		digitalWrite(sd->dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_ADDR_MODE);
		OLED_spiWrite(fd, SSD1306_ARG_ADDR_MODE_H);
		OLED_spiWrite(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, od->xsize - 1);
		OLED_spiWrite(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, sd->byteh - 1);
		digitalWrite(sd->dc_pin, HIGH);
		write(fd, zero, od->memsize);
	}
}

/*
 * Show test pattern.
 *
 *  Parameters:
 *    fd	- SPI file descriptor
 *    type	- test pattern type (0-5)
 */
void OLED_testPattern(int fd, int type)
{
	int i;
	unsigned char *pat;
	struct ssd1306Desc *sd;

	if (!od)
		return;

	if (od->chip == OLED_CHIP_SSD1306) {

		sd = (struct ssd1306Desc*)od->chip_data;

		pat = (unsigned char *)malloc(od->memsize);

		if (!type)
			/* all bits on */
			memset(pat, 0xFF, od->memsize);
		else if (type == 1)
			/* 1x1 checker pattern */
			for(i=0; i < od->memsize; i++)
				pat[i] = (i & 0x1) ? 0xAA : 0x55;
		else if (type == 2)
			/* 2x2 checker pattern */
			for(i=0; i < od->memsize; i++)
				pat[i] = (i & 0x2) ? 0xCC : 0x33;
		else if (type == 3)
			/* 4x4 checker pattern */
			for(i=0; i < od->memsize; i++)
				pat[i] = (i & 0x4) ? 0xF0 : 0x0F;
		else if (type == 4)
			/* 4-pixel-wide vertical stripes */
			for(i=0; i < od->memsize; i++)
				pat[i] = (i & 0x4) ? 0xFF : 0x00;
		else if (type == 5)
			/* 4-pixel-high horizontal stripes */
			for(i=0; i < od->memsize; i++)
				pat[i] = 0x0F;

		digitalWrite(sd->dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_ADDR_MODE);
		OLED_spiWrite(fd, SSD1306_ARG_ADDR_MODE_H);
		OLED_spiWrite(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, od->xsize - 1);
		OLED_spiWrite(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, sd->byteh - 1);
		digitalWrite(sd->dc_pin, HIGH);
		write(fd, pat, od->memsize);

		free(pat);
	}
}

/*
 * Show all characters for test font (ID=0, 5x7).
 *
 *  Parameters:
 *    fd	- SPI file descriptor
 *    start	- ASCII code of first character
 */
void OLED_testFont(int fd, int start)
{
	struct ssd1306Desc *sd;

	if (!od)
		return;

	if (od->chip == OLED_CHIP_SSD1306) {
		sd = (struct ssd1306Desc*)od->chip_data;
		digitalWrite(sd->dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_ADDR_MODE);
		OLED_spiWrite(fd, SSD1306_ARG_ADDR_MODE_H);
		OLED_spiWrite(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, od->xsize - 1);
		OLED_spiWrite(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWrite(fd, 0);
		OLED_spiWrite(fd, sd->byteh - 1);
		digitalWrite(sd->dc_pin, HIGH);
		write(fd, &font[start * 5], od->memsize);
	}
}

/*
 * Print character.
 *
 *  Parameters:
 *    fd	- SPI file descriptor
 *    fontid	- font handler
 *    x		- column in pixels
 *    row	- y coordinate in bytes (pages!)
 *    inv	- 1 for inversed background
 *    c		- character
 *
 *  Return:
 *    x coordinate for next letter
 */
int OLED_putChar(int fd, int fontid, int x, int row, int inv, char c)
{
	struct fontDesc *ft;
	unsigned char *tptr, *cptr;
	struct ssd1306Desc *sd;

	if (!od)
		return -1;

	if (fontid >= fntcnt)
		return -1;

	ft = &fnt[fontid];

	if (od->chip == OLED_CHIP_SSD1306) {
		sd = (struct ssd1306Desc*)od->chip_data;
		/* we do not clip on-screen */
		if (row < 0 || row + ft->fontByteH > sd->byteh ||
		    x < 0 || x + ft->fontCellW > od->xsize)
			return -1;
		tptr = ft->fontImg + c * ft->fontSizeB;
		cptr = (unsigned char *)malloc(ft->fontCellSz);
		memset(cptr, 0, ft->fontCellSz);
		memcpy(cptr + ft->fontLJustB, tptr, ft->fontSizeB);
		if (inv)
			OLED_invMem(cptr, ft->fontCellSz);
		digitalWrite(sd->dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_ADDR_MODE);
		OLED_spiWrite(fd, SSD1306_ARG_ADDR_MODE_V);
		OLED_spiWrite(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWrite(fd, x);
		OLED_spiWrite(fd, x + ft->fontCellW - 1);
		OLED_spiWrite(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWrite(fd, row);
		OLED_spiWrite(fd, row + ft->fontByteH - 1);
		digitalWrite(sd->dc_pin, HIGH);
		write(fd, cptr, ft->fontCellSz);
		free(cptr);

		return (x + ft->fontCellW) & 0x7F;
	}

	return -1;
}

/*
 * Print string.
 *
 *  Parameters:
 *    fd	- SPI file descriptor
 *    fontid	- font handler
 *    x		- column in pixels
 *    row	- y coordinate in bytes (pages!)
 *    inv	- 1 for inverted background
 *    s		- string
 *
 *  Return:
 *    x coordinate for next letter
 */
int OLED_putString(int fd, int fontid, int x, int row, int inv,
		   const unsigned char *s)
{
	int i, len, memlen, memw;
	struct fontDesc *ft;
	unsigned char *tptr, *cptr;
	struct ssd1306Desc *sd;

	if (!od)
		return -1;

	if (fontid >= fntcnt)
		return -1;

	ft = &fnt[fontid];

	len = strlen(s);
	if (!len)
		return -1;
	if (row < 0 || x < 0)
		return -1;

	if (od->chip == OLED_CHIP_SSD1306) {
		sd = (struct ssd1306Desc*)od->chip_data;
		if (row + ft->fontByteH > sd->byteh)
			return -1;	/* we do not clip vertically */
		if (x + len * ft->fontCellW > od->xsize)
			len = (od->xsize - x) / ft->fontCellW;
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
		digitalWrite(sd->dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_ADDR_MODE);
		OLED_spiWrite(fd, SSD1306_ARG_ADDR_MODE_V);
		OLED_spiWrite(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWrite(fd, x);
		OLED_spiWrite(fd, x + memw < od->xsize ?
			      x + memw - 1 : od->xsize - 1);
		OLED_spiWrite(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWrite(fd, row);
		OLED_spiWrite(fd, row + ft->fontByteH < sd->byteh ?
			      row + ft->fontByteH - 1 : sd->byteh - 1);
		digitalWrite(sd->dc_pin, HIGH);
		write(fd, cptr, memlen);
		free(cptr);

		return x + memw < od->xsize ? x + memw : od->xsize;
	}

	return -1;
}

/*
 * Load PSF font file (256 chars, ASCII, no Unicode).
 * Convert it to memory layout compatible with vertical
 * addressing mode of SSD1306.
 *
 *  Parameters:
 *    psffile	- path to PSFv1/v2 font file
 *
 *  Return:
 *    Font ID (handler)
 */
int OLED_loadPsf(const unsigned char *psffile)
{
	int psfd;
	struct psf1_header ph1;
	struct psf2_header ph2;
	unsigned int hdr_numchars, hdr_charsize;
	unsigned char magic[4], *buf;
	struct fontDesc *ft;
	int c, i, j, obh, tjust, ibw, ilw;
	unsigned char omask, imask;

	if (fntcnt == MAX_FONTS)
		return -1;

	psfd = open(psffile, O_RDONLY);
	if (psfd < 0)
		return -1;

	ft = &fnt[fntcnt];

	read(psfd, &magic, 4);
	lseek(psfd, 0, SEEK_SET);
	if (PSF1_MAGIC_OK(magic)) {
		/* PSF v1 - always 8-bit wide */
		read(psfd, &ph1, sizeof(struct psf1_header));
		if (ph1.mode)
			return -1;	/* wrong mode */
		ft->fontWidth = 8;
		ft->fontHeight = ph1.charsize;
		ft->fontCellW = 8;
		ft->fontByteH = (ph1.charsize + 7) >> 3;
		hdr_numchars = 256;
		hdr_charsize = ph1.charsize;
	} else if (PSF2_MAGIC_OK(magic)) {
		/* PSF v2 - newer and more flexible */
		read(psfd, &ph2, sizeof(struct psf2_header));
		if (ph2.version > PSF2_MAXVERSION)
			return -1;	/* wrong version */
		if (ph2.flags || ph2.length != 256)
			return -1;	/* only ASCII charset supported */
		ft->fontWidth = ph2.width;
		ft->fontHeight = ph2.height;
		ft->fontCellW = ph2.width;
		ft->fontByteH = (ph2.height + 7) >> 3;
		hdr_numchars = ph2.length;
		hdr_charsize = ph2.charsize;
	} else
		return -1;

	OLED_fontCalc(ft);
	ft->fontImg = (unsigned char *)malloc(ft->fontCellSz * hdr_numchars);
	memset(ft->fontImg, 0, ft->fontCellSz * hdr_numchars);
	ilw = (ft->fontWidth + 7) >> 3;
	tjust = ((ft->fontByteH << 3) - ft->fontHeight + 1) >> 1;
	buf = (unsigned char *)malloc(hdr_charsize);
	for(c = 0; c < 256; c++) {
		read(psfd, buf, hdr_charsize);
		for(i = 0; i < ft->fontHeight; i++) {
			omask = 1 << ((i + tjust) & 0x07);
			obh = (i + tjust) >> 3;
			for(j = 0; j < ft->fontWidth; j++) {
				imask = 1 << (7 - (j & 0x07));
				ibw = j >> 3;
				if (buf[i * ilw + ibw] & imask)
					ft->fontImg[c * ft->fontCellSz + \
					+ j * ft->fontByteH + obh] |= omask;
			}
		}
	}

	free(buf);
	close(psfd);

	return fntcnt++;
}

/*
 * Load font from memory (compatible with OLED vertical addresing mode).
 * Use this function to access fonts defined in header files.
 * Note: does not copy memory - uses original pointer.
 *
 *  Parameters:
 *    width		- font width in pixels
 *    height		- font height in pixels
 *    cellwidth		- font box (cell) width in pixels (extra spacing)
 *    byteheight	- font height in bytes (pages)
 *    dataptr		- pointer to memory location with fonts images
 *
 *  Return:
 *    Font ID (handler)
 */
int OLED_loadFont(int width, int height, int cellwidth, int byteheight,
		  unsigned char *dataptr)
{
	struct fontDesc *ft;

	if (fntcnt == MAX_FONTS)
		return -1;

	ft = &fnt[fntcnt];

	ft->fontWidth = width;
	ft->fontHeight = height;
	ft->fontCellW = cellwidth;
	ft->fontByteH = byteheight;
	ft->fontImg = dataptr;
	OLED_fontCalc(ft);

	return fntcnt++;
}

/*
 * Get font screen dimensions.
 *
 *  Parameters:
 *    fontid	 - font handler
 *    width	 - set to font width in pixels
 *    height	 - set to font height in pixels
 *    cellwidth	 - set to font box (cell) width in pixels (extra spacing)
 *    byteheight - set to font height in bytes (pages)
 *
 *  Return:
 *    Font size in bytes (pages)
 */
int OLED_getFontScreenSize(int fontid, int *width, int *height,
			   int *cellwidth, int *cellheight, int *byteheight)
{
	if (fontid >= fntcnt)
		return -1;

	if (width)
		*width = fnt[fontid].fontWidth;
	if (height)
		*height = fnt[fontid].fontHeight;
	if (cellwidth)
		*cellwidth = fnt[fontid].fontCellW;
	if (cellheight)
		*cellheight = fnt[fontid].fontByteH << 3;
	if (byteheight)
		*byteheight = fnt[fontid].fontByteH;

	return fnt[fontid].fontSizeB;
}

/*
 *  Get font memory address.
 *
 *  Parameters:
 *    fontid	- font handler
 *    bytes	- set to font memory size in bytes
 *
 *  Return:
 *    Pointer to font memory area
 */
unsigned char *OLED_getFontMemory(int fontid, int *bytes)
{
	if (fontid >= fntcnt)
		return NULL;

	if (bytes)
		*bytes = fnt[fontid].fontSizeB << 8;

	return fnt[fontid].fontImg;
}

/*
 * Load bitmap (1-bit B/W, size not bigger than screen).
 * Convert it to memory layout compatible with vertical
 * addressing mode of SSD1306.
 *
 *  Parameters:
 *    bmpfile	- path to BMP file
 *
 *  Return:
 *    Bitmap ID (handler)
 */
int OLED_loadBitmap(const unsigned char *bmpfile)
{
	int bfd;
	char magic[2];
	struct bmp_header bh;
	struct imgDesc *im;
	unsigned char *buf;
	int llen, i, j;
	int omask, obi, imask, ibi;

	if (imgcnt == MAX_IMAGES)
		return -1;

	if (!od)
		return -1;

	bfd = open(bmpfile, O_RDONLY);
	if (bfd < 0)
		return -1;

	read(bfd, &magic, 2);
	lseek(bfd, 0, SEEK_SET);

	if (!BMP_MAGIC_OK(magic))
		return -1;

	read(bfd, &bh, sizeof(struct bmp_header));

	if (bh.width > od->xsize || bh.height > od->ysize)
		return -1;	/* image too large */
	if (bh.bpp != 1)
		return -1;	/* image is not 1-bit */

	im = &img[imgcnt];
	im->imgWidth = bh.width;
	im->imgHeight = bh.height;
	im->imgByteH = (bh.height + 7) >> 3;
	im->imgSizeB = im->imgByteH * im->imgWidth;
	im->imgData = (unsigned char *)malloc(im->imgSizeB);
	im->maskData = NULL;
	memset(im->imgData, 0, im->imgSizeB);

	lseek(bfd, bh.imgoffset, SEEK_SET);
	llen = ((bh.width >> 3) + 3) & ~0x03;	/* lines are dword-aligned */
	buf = (unsigned char *)malloc(llen);

	/* bitmaps are oriented bottom-top */
	for(i = im->imgHeight - 1; i >= 0; i--) {
		read(bfd, buf, llen);
		omask = 1 << (i & 0x07);	/* output mask */
		obi = i >> 3;	/* output (vertical) byte index */
		for(j = 0; j < im->imgWidth; j++) {
			imask = 1 << (7 - (j & 0x07));	/* input mask */
			ibi = j >> 3;	/* input (horizontal) byte index */
			if (buf[ibi] & imask)
				im->imgData[j * im->imgByteH + obi] |= omask;
		}
	}

	close(bfd);
	free(buf);

	return imgcnt++;
}

/*
 * Display image.
 *
 *  Parameters:
 *    fd	 - SPI file descriptor
 *    imageid	 - image ID (handler)
 *    x		 - x coordinate (column) in pixels
 *    row	 - y coordinate in bytes (pages)
 *
 *  Return:
 *    x coordinate for next object
 */
int OLED_putImage(int fd, int imageid, int x, int row)
{
	struct imgDesc *im;
	struct ssd1306Desc *sd;

	if (!od)
		return -1;

	if (imageid >= imgcnt)
		return -1;

	if (row < 0 || x < 0)
		return -1;

	im = &img[imageid];

	if (od->chip == OLED_CHIP_SSD1306) {
		sd = (struct ssd1306Desc*)od->chip_data;
		if (row + im->imgByteH > sd->byteh ||
		    x + im->imgWidth > od->xsize)
			return -1;	/* we do not clip on-screen */

		digitalWrite(sd->dc_pin, LOW);
		OLED_spiWrite(fd, SSD1306_CMD_ADDR_MODE);
		OLED_spiWrite(fd, SSD1306_ARG_ADDR_MODE_V);
		OLED_spiWrite(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWrite(fd, x);
		OLED_spiWrite(fd, x + im->imgWidth - 1);
		OLED_spiWrite(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWrite(fd, row);
		OLED_spiWrite(fd, row + im->imgByteH - 1);
		digitalWrite(sd->dc_pin, HIGH);
		write(fd, im->imgData, im->imgSizeB);

		return x + im->imgWidth < od->xsize ?
		       x + im->imgWidth : od->xsize;
	}

	return -1;
}

/*
 * Get image screen dimensions.
 *
 *  Parameters:
 *    imageid	 - font handler
 *    width	 - set to image width in pixels
 *    height	 - set to image height in pixels
 *    byteheight - set to image height in bytes (pages)
 *
 *  Return:
 *    Image size in bytes (pages)
 */
int OLED_getImageScreenSize(int imageid, int *width, int *height,
			    int *byteheight)
{
	if (imageid >= fntcnt)
		return -1;

	if (width)
		*width = img[imageid].imgWidth;
	if (height)
		*height = img[imageid].imgHeight;
	if (byteheight)
		*byteheight = img[imageid].imgByteH;

	return img[imageid].imgSizeB;
}

/*
 * Load image from memory (compatible with OLED vertical addresing mode).
 * Use this function to access images defined in header files.
 * Note: does not copy memory - uses original pointer.
 *
 *  Parameters:
 *    width		- image width in pixels
 *    height		- image height in pixels
 *    dataptr		- pointer to memory location with image
 *    maskptr		- pointer to memory location with transparency mask
 *
 *  Return:
 *    Image ID (handler)
 */
int OLED_loadImage(int width, int height, unsigned char *dataptr,
		   unsigned char *maskptr)
{
	struct imgDesc *im;

	if (imgcnt == MAX_IMAGES)
		return -1;

	im = &img[imgcnt];

	im->imgWidth = width;
	im->imgHeight = height;
	im->imgByteH = (height + 7) >> 3;
	im->imgSizeB = im->imgByteH * im->imgWidth;
	im->imgData = dataptr;
	im->maskData = maskptr;

	return imgcnt++;
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
