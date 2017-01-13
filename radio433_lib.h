#ifndef _RADIO433_LIB_H_
#define _RADIO433_LIB_H_

/* Supported devices */
#define RADIO433_DEVICE_KEMOTURZ1226	0
#define RADIO433_DEVICE_HYUWSSENZOR77TH	1
#define RADIO433_DEVICES		2

/* Uncomment to enable debugging */
#define RADIO433_DEBUG

/* Initialize library */
int Radio433_init(int tx_gpio, int rx_gpio);

#ifdef RADIO433_DEBUG
/* Dump timing ring buffer */
void Radio433_tempTBufDebug(void);

/* Dump code ring buffer */
void Radio433_tempCBufDebug(void);
#endif

#endif
