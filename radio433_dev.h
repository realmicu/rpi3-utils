#ifndef _RADIO433_DEV_H_
#define _RADIO433_DEV_H_

#include "radio433_types.h"

/* Decode power command from raw code */
int Radio433_pwrGetCommand(unsigned long long code,
                           int *systemid, int *deviceid, int *button);

/* Encode power command into raw code */
unsigned long long Radio433_pwrGetCode(int systemid, int deviceid,
				       int button);

/* Decode sensor data */
/* NOTE: code checksum should be validated but it is not */
int Radio433_thmGetData(unsigned long long code, int *sysid, int *thmid,
                        int *ch, int *batlow, int *tdir, double *temp,
			int *humid);

/* Encode sensor data */
/* NOTE: code checksum should be generated but it is not */
unsigned long long Radio433_thmGetCode(int sysid, int thmid, int ch,
				       int batlow, int tdir, double temp,
				       int humid);

#endif
