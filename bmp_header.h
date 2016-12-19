/* bmp_header.h */

/*
 * Simple BMP header definition
 */

#ifndef _BMP_HEADER_H
#define _BMP_HEADER_H

#define BMP_MAGIC0	'B'
#define BMP_MAGIC1	'M'

#pragma pack(push, 2)
struct bmp_header {
	unsigned char magic[2];
	unsigned int filesize;
	unsigned int reserved0;
	unsigned int imgoffset;
	unsigned int hdrsize;
	unsigned int width, height;
	unsigned short clrplanes;
	unsigned short bpp;
};
#pragma pack(pop)

#define BMP_MAGIC_OK(x)	((x)[0]==BMP_MAGIC0 && (x)[1]==BMP_MAGIC1)

#endif	/* _BMP_HEADER_H */

