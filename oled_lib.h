#ifndef _OLED_LIB_H_
#define _OLED_LIB_H_

/* OLED types */
#define ADAFRUIT_SSD1306_128_64		0

/* Default font */
#define OLED_DEFAULT_FONT		0	/* 5x7 font */

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
int OLED_initSPI(int type, int cs, int rst_pin, int dc_pin);

/* 
 * Device power on.
 */
void OLED_powerOn(int fd);

/*
 * Device power off.
 */
void OLED_powerOff(int fd);

/*
 * Clear display screen.
 */
void OLED_clearDisplay(int fd);

/*
 * Show test pattern.
 *
 *  Parameters:
 *    fd	- SPI file descriptor
 *    type	- test pattern type (0-5)
 */
void OLED_testPattern(int fd, int type);

/*
 * Show all characters for test font (ID=0, 5x7).
 *
 *  Parameters:
 *    fd	- SPI file descriptor
 *    start	- ASCII code of first character
 */
void OLED_testFont(int fd, int start);

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
int OLED_putChar(int fd, int fontid, int x, int row, int inv, char c);

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
		   const unsigned char *s);

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
int OLED_loadPsf(const unsigned char *psffile);

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
		  unsigned char *dataptr);
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
			   int *cellwidth, int *cellheight, int *byteheight);
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
unsigned char *OLED_getFontMemory(int fontid, int *bytes);

/*
 * Load bitmap (1-bit B/W, size not bigger than screen).
 * Convert it to memory layout compatible with vertical
 * addressing mode of SSD1306.
 *
 *  Parameters:
 *    bmpfile   - path to BMP file
 *
 *  Return:
 *    Bitmap ID (handler)
 */
int OLED_loadBitmap(const unsigned char *bmpfile);

/*
 * Display image.
 *
 *  Parameters:
 *    fd	- SPI file descriptor
 *    imageid	- image ID (handler)
 *    x		- x coordinate (column) in pixels
 *    row	- y coordinate in bytes (pages)
 *
 *  Return:
 *    x coordinate for next object
 */
int OLED_putImage(int fd, int imageid, int x, int row);

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
			    int *byteheight);

/*
 * Load image from memory (compatible with OLED vertical addresing mode).
 * Use this function to access images defined in header files.
 * Note: does not copy memory - uses original pointer.
 *
 *  Parameters:
 *    width             - image width in pixels
 *    height            - image height in pixels
 *    dataptr           - pointer to memory location with image
 *    maskptr           - pointer to memory location with transparency mask
 *
 *  Return:
 *    Image ID (handler)
 */
int OLED_loadImage(int width, int height, unsigned char *dataptr,
		   unsigned char *maskptr);

#endif
