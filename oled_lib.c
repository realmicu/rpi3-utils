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
/* Registers / Commands */
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

/* Timing information - see datasheet */
#define SSD1306_RESET_WAIT_MS	1
#define SSD1306_SPI_CLK_HZ	16000000	/* 16 Mhz*/

/* ****************** */
/* Internal constants */
/* ****************** */
#define GPIO_PINS		28	/* number of Pi3 GPIO pins */

/* ****************** */
/* Internal variables */
/* ****************** */
static int oled_type;
static int oled_rst_pin, oled_dc_pin;
static int memsize;		/* display memory in bytes */
static unsigned char *zero;	/* for display zeroing */

/* ****************** */
/* Internal functions */
/* ****************** */
static int OLED_spiWriteCmd(int fd, unsigned char byte)
{
	return write(fd, &byte, 1);
}

/* **************** */
/* Public functions */
/* **************** */

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
			zero = (unsigned char *)malloc(memsize);
			memset(zero, 0, memsize);
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
		OLED_spiWriteCmd(fd, SSD1306_CMD_DISPLAY_OFF);

		/* set column address (low, high) */
		/* OLED_spiWriteCmd(fd, 0x00); */
		/* OLED_spiWriteCmd(fd, 0x10); */

		/* set GDDRAM page start address */
		/* OLED_spiWriteCmd(fd, 0xB0); */

		/* set memory addressing mode */
		OLED_spiWriteCmd(fd, SSD1306_CMD_ADDR_MODE);
		OLED_spiWriteCmd(fd, 0x00);	/* horizontal */

		/* set display clock divide ratio to 100 frames/sec */
		OLED_spiWriteCmd(fd, SSD1306_CMD_DIV_CLK_RATIO);
		OLED_spiWriteCmd(fd, 0x80);

		/* set multiplex ratio */
		OLED_spiWriteCmd(fd, SSD1306_CMD_MUX_RATIO);
		OLED_spiWriteCmd(fd, 0x3F);

		/* set display offset */
		/* OLED_spiWriteCmd(fd, SSD1306_CMD_DPY_OFFSET); */
		/* OLED_spiWriteCmd(fd, 0x00); */

		/* set display start line */
		/* OLED_spiWriteCmd(fd, 0x40); */

		/* set charge pump */
		OLED_spiWriteCmd(fd, SSD1306_CMD_CHARGE_PUMP);
		OLED_spiWriteCmd(fd, 0x14);	/* internal Vcc */

		/* set segment remap */
		OLED_spiWriteCmd(fd, SSD1306_CMD_SEG_REMAP_DEC);

		/* set scan direction */
		OLED_spiWriteCmd(fd, SSD1306_CMD_COM_SCAN_DEC);

		/* set contrast */		
		OLED_spiWriteCmd(fd, SSD1306_CMD_CONTRAST);
		OLED_spiWriteCmd(fd, 0xCF);	/* internal Vcc */

		/* set pre-charge period */
		OLED_spiWriteCmd(fd, SSD1306_CMD_PRECHRG_PERIOD);
		OLED_spiWriteCmd(fd, 0xF1);

		/* set COM pins */
		OLED_spiWriteCmd(fd, SSD1306_CMD_COM_PIN_CFG);
		OLED_spiWriteCmd(fd, 0x12);

		/* set VCOMH deselect level*/
		OLED_spiWriteCmd(fd, SSD1306_CMD_VCOMH_LEVEL);
		OLED_spiWriteCmd(fd, 0x40);

		/* connect display to GDDRAM */
		OLED_spiWriteCmd(fd, SSD1306_CMD_DPY_RAM_ON);

		/* set normal (no inverse) display */
		/* OLED_spiWriteCmd(fd, SSD1306_CMD_INVERSE_OFF); */

		/* display on */
		OLED_spiWriteCmd(fd, SSD1306_CMD_DISPLAY_ON);
	}
}

/* Device power off */
void OLED_powerOff(int fd)
{
	if (oled_type == ADAFRUIT_SSD1306_128_64) {
		digitalWrite(oled_dc_pin, LOW);
		OLED_spiWriteCmd(fd, SSD1306_CMD_DISPLAY_OFF);
	}
}

/* Clear display screen */
void OLED_clearDisplay(int fd)
{
	if (oled_type == ADAFRUIT_SSD1306_128_64) {
		digitalWrite(oled_dc_pin, LOW);
		OLED_spiWriteCmd(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWriteCmd(fd, 0);
		OLED_spiWriteCmd(fd, 127);
		OLED_spiWriteCmd(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWriteCmd(fd, 0);
		OLED_spiWriteCmd(fd, 7);
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
		OLED_spiWriteCmd(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWriteCmd(fd, 0);
		OLED_spiWriteCmd(fd, 127);
		OLED_spiWriteCmd(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWriteCmd(fd, 0);
		OLED_spiWriteCmd(fd, 7);
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
		OLED_spiWriteCmd(fd, SSD1306_CMD_VH_COL_RANGE);
		OLED_spiWriteCmd(fd, 0);
		OLED_spiWriteCmd(fd, 127);
		OLED_spiWriteCmd(fd, SSD1306_CMD_VH_PAGE_RANGE);
		OLED_spiWriteCmd(fd, 0);
		OLED_spiWriteCmd(fd, 7);
		digitalWrite(oled_dc_pin, HIGH);
		write(fd, &font[start * 5], memsize);

	}
}
