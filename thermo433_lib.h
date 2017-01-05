#ifndef _THERMO433_LIB_H_
#define _THERMO433_LIB_H_

/* uncomment/define if timing statistic is required */
#define THERMO433_INCLUDE_TIMING_STATS

/* Remote thermometer types */
#define THERMO433_DEVICE_HYUWSSENZOR77TH	0  /* Hyundai WS Senzor 77 TH */

/* Pulse types from classifyPulse() function*/
#define THERMO433_PULSE_TYPE_SYNC	0
#define THERMO433_PULSE_TYPE_HIGH	1
#define THERMO433_PULSE_TYPE_LOW_SHORT	2
#define THERMO433_PULSE_TYPE_LOW_LONG	3
#define THERMO433_PULSE_TYPE_UNKNOWN	-1

/* Temperature trend (direction of change) */
#define THERMO433_TEMP_TREND_DOWN	2
#define THERMO433_TEMP_TREND_UP		1
#define THERMO433_TEMP_TREND_STABLE	0
#define THERMO433_TEMP_TREND_INVALID	-1

int Thermo433_init(int tx_gpio, int rx_gpio, int type);

unsigned long long Thermo433_getAnyCode(void);

unsigned long long Thermo433_getCode(void);

unsigned long long Thermo433_waitAnyCode(void);

unsigned long long Thermo433_waitCode(void);
int Thermo433_decodeValues(unsigned long long val, int *ch, int *bat,
			   int *temp, int *humid, int *tdir);

void Thermo433_waitValues(int *ch, int *bat, int *temp, int *humid, int *tdir);

int Thermo433_classifyPulse(unsigned long microseconds);

#ifdef THERMO433_INCLUDE_TIMING_STATS
/* Get timing data for statistics */
int Thermo433_getTimingStats(unsigned long *timesync, unsigned long tpbuf[]);
#endif

#endif
