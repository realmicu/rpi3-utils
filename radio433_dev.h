#ifndef _RADIO433_DEV_H_
#define _RADIO433_DEV_H_

/* Device and button constants */
#define POWER433_DEVICE_A	0x10
#define POWER433_DEVICE_B	0x08
#define POWER433_DEVICE_C	0x04
#define POWER433_DEVICE_D	0x02
#define POWER433_DEVICE_E	0x01
#define POWER433_BUTTON_OFF	0
#define POWER433_BUTTON_ON	1

/* Decode power command from raw code */
int Radio433_pwrGetCommand(unsigned long long code,
                           int *systemid, int *deviceid, int *button);

/* Temperature trend (direction of change) */
#define THERMO433_TEMP_TREND_DOWN	2
#define THERMO433_TEMP_TREND_UP		1
#define THERMO433_TEMP_TREND_STABLE	0
#define THERMO433_TEMP_TREND_INVALID	-1

/* Decode sensor data */
/* NOTE: code checksum should be validated but it is not */
int Radio433_thmGetData(unsigned long long code, int *sysid, int *thmid,
                        int *ch, int *batlow, int *tdir, double *temp,
			int *humid);

#endif
