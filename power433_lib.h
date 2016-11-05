#ifndef _POWER433_LIB_H_
#define _POWER433_LIB_H_

/* uncomment/define if timing statistic is required */
//#define POWER433_INCLUDE_TIMING_STATS

/* Protocol constants */
#define POWER433_BITS		24              /* bits per transmission */
#define POWER433_PULSES		(POWER433_BITS << 1)
#define POWER433_RETRANS	8               /* code retransmissions (>=3) */

/* Pulse types from classifyPulse() function*/
#define POWER433_PULSE_TYPE_SYNC	0
#define POWER433_PULSE_TYPE_SHORT	1
#define POWER433_PULSE_TYPE_LONG	2
#define POWER433_PULSE_TYPE_UNKNOWN	-1

/* Device and button constants */
#define POWER433_DEVICE_A	0
#define POWER433_DEVICE_B	1
#define POWER433_DEVICE_C	2
#define POWER433_DEVICE_D	3
#define POWER433_DEVICE_E	4
#define POWER433_BUTTON_OFF	0
#define POWER433_BUTTON_ON	1

/* Initialize library */
void Power433_init(int tx_gpio, int rx_gpio);

/* Get code if present, clear it afterwards */
unsigned int Power433_getCode(void);

/* Get code if present, clear it afterwards - code is not validated */
unsigned int Power433_getAnyCode(void);

/* Wait for code until one arrives, get it and clear */
unsigned int Power433_waitCode(void);

/* Wait for code until one arrives, get it and clear - code is not validated */
unsigned int Power433_waitAnyCode(void);

/* Send code once */
void Power433_sendCode(unsigned int code);

/* Send code n times */
void Power433_repeatCode(unsigned int code, int repeats);

/* Send command: */
/*   systemid - 5-bit System Code (0-31) */
/*   deviceid - button letter (0-A, 1-B, 2-C, 3-D, 4-E(unused)) */
/*   button   - power button (0-OFF, 1-ON) */
int Power433_sendCommand(int systemid, int deviceid, int button);

/* Decode raw code data for power switches, ie: Kemot: */
/*   systemid - 5-bit System Code (0-31) */
/*   deviceid - button letter (0-A, 1-B, 2-C, 3-D, 4-E(unused)) */
/*   button   - power button (0-OFF, 1-ON) */
int Power433_decodeCommand(unsigned int code, int *systemid,
			   int *deviceid, int *button);

/* Get pulse type: */
/*   0 - sync pulse */
/*   1 - short pulse */
/*   2 - long pulse */
/*  -1 - unclassified */
int Power433_classifyPulse(unsigned long microseconds);

#ifdef POWER433_INCLUDE_TIMING_STATS
/* Get timing data for statistics (in microseconds): */
/*   timesync - sync time (31 times zero signal) */
/*   tpbuf    - array of timings for all code pulses */
void Power433_getTimingStats(unsigned long *timesync, unsigned long tpbuf[]);
#endif

#endif
