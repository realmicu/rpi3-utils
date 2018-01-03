#ifndef _BH1750_LIB_H_
#define _BH1750_LIB_H_

/* I2C bus address */
#define BH1750_I2C_ADDR		0x23		/* for ADDR pin low */
/* #define BH1750_I2C_ADDR 	0x5C */		/* for ADDR pin high */

/* Measurement modes - see page 5 of chip specs */
#define BH1750_MODE_CONT	0x1		/* Continuous Mode */
#define BH1750_MODE_ONETIME	0x2		/* One-Time Mode */
#define BH1750_MODE_RES_H	0x0		/* H-Resolution Mode */
#define BH1750_MODE_RES_H2	0x1		/* H-Resolution Mode2 */
#define BH1750_MODE_RES_L	0x3		/* L-Resolution Mode */

/* Device initialization (for RPi I2C bus) */
/* For default address, use NULL or variable with value 0 */
/* Returns < 0 if initialization failed, otherwise fd */
/* On success, argument variable is set to I2C address */
int BH1750_initPi(int *i2caddr);

/* Device control */
void BH1750_powerDown(int fd);
void BH1750_powerOn(int fd);

/* Soft reset */
void BH1750_softReset(int fd);

/* Set measurement mode */
void BH1750_setMode(int fd, int cont, int res);

/* Get luminance */
double BH1750_getLx(int fd);

#endif
