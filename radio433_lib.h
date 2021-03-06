#ifndef _RADIO433_LIB_H_
#define _RADIO433_LIB_H_

#include "radio433_types.h"

/* Initialize library */
int Radio433_init(int tx_gpio, int rx_gpio);

/* Get code */
unsigned long long Radio433_getCode(struct timeval *ts, int *type, int *bits);

/* Get code (with additional timing data in miliseconds) */
unsigned long long Radio433_getCodeExt(struct timeval *ts, int *type, int *bits,
				       int *codetime, int *repeats, int *interval);

/* Pulse code: transmit timed pulses ('len' values in us, 'starthigh' == 0/1) */
int Radio433_pulseCode(const unsigned long *pulses, int len, int starthigh);

/* Send raw code: use selected modulation and length in bits */
int Radio433_sendRawCode(unsigned long long code, int coding, int bits, int repeats);

/* Send device-specific code (repeats == 0 - use default number of packets) */
int Radio433_sendDeviceCode(unsigned long long code, int type, int repeats);

#endif
