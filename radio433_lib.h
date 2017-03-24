#ifndef _RADIO433_LIB_H_
#define _RADIO433_LIB_H_

#include "radio433_types.h"

/* Initialize library */
int Radio433_init(int tx_gpio, int rx_gpio);

/* Get code */
unsigned long long Radio433_getCode(struct timeval *ts, int *type, int *bits);

#endif
