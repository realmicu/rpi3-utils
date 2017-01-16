#ifndef _RADIO433_LIB_H_
#define _RADIO433_LIB_H_

/* Supported devices */
#define RADIO433_DEVICE_KEMOTURZ1226	0
#define RADIO433_DEVICE_HYUWSSENZOR77TH	1
#define RADIO433_DEVICES		2

/* Initialize library */
int Radio433_init(int tx_gpio, int rx_gpio);

/* Get code */
unsigned long long Radio433_getCode(struct timeval *ts, int *type, int *bits);

#endif
