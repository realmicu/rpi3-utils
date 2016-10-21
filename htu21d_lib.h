#ifndef _HTU21D_LIB_H_
#define _HTU21D_LIB_H_

#define	HTU21D_I2C_ADDR		0x40

/* Soft reset */
void HTU21D_softReset(int fd);

/* Get temperature */
/* Unit: Celsius */
double HTU21D_getTemperature(int fd);

/* Get relative humidity */
/* Unit: Percent (0-100%) */
double HTU21D_getHumidity(int fd);

#endif