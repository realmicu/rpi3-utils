#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sched.h>

#include <wiringPi.h>

#include "radio433_lib.h"
#include "radio433_dev.h"

#define	CODE_RETRANS	12	/* set to >0 override default */

/*
 Based on original power433control.c .
 This version used radio433 framework and is currently supported.
 */

struct cmd {
	unsigned int sys, dev, oper;
};

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s {gpio} {oper0} [oper1] ...\n\n", progname);
	puts("Where:");
	puts("\tgpio\t - GPIO pin with external RF transmitter connected (mandatory)");
	puts("\toper\t - operation defined as system:device:{on|off} (at least one)");
}

/* change sheduling priority */
int changeSched(void)
{
        struct sched_param s;

        s.sched_priority = 0;
        if(sched_setscheduler(0, SCHED_BATCH, &s))
                return -1;
        return 0;
}

/* finds first ':' in s, terminates s and returns pointer to rest of string */
char *splitArg(char *s)
{
	int i;

	for(i = 0; i < strlen(s); i++)
		if (s[i] == ':') {
			s[i] = 0;
			if (s[i+1])
				return s+i+1;
			else
				return NULL;
	}
	return s;
}

/* read number from string into variable - it supports binary as well */
unsigned int getSysNum(char *s)
{
	unsigned int a;

	if (strlen(s) == 5)
		a = ((s[0] - '0') << 4) + ((s[1] - '0') << 3) + \
		    ((s[2] - '0') << 2) + ((s[3] - '0') << 1) + \
		    (s[4] - '0');
	else if (s[1] == 'x' || s[1] == 'X')
	/* hex */
		sscanf(s+2, "%x", &a);
	else
	/* decimal */
		sscanf(s, "%d", &a);

	return a;
}

/* read number from string into variable - it supports binary as well */
unsigned int getDevMask(char *s)
{
	int i;
	unsigned int a;

	a = 0;
	for(i = 0; i < strlen(s); i++)
		a |= 1 << ((s[i] & 0x20 ? 'e' : 'E') - s[i]);

	return a;
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int i, gpio, ncode;
	struct cmd *codes;
	char parmbuf[16];
	char *pdev, *pbtn;

	/* show help */
	if (argc < 3) {
		help(argv[0]);
		exit(0);
	}

	/* get parameters */
	sscanf(argv[1], "%d", &gpio);

	/* get command codes */
	ncode = 0;
	codes = (struct cmd *)malloc((argc - 2) * sizeof(struct cmd));
	for(i = 0; i < argc - 2; i++) {
		memset(parmbuf, 0, 16);
		strncpy(parmbuf, argv[i + 2], 15);
		pdev = splitArg(parmbuf);
		if (!pdev)
			continue;
		pbtn = splitArg(pdev);
		if (!pbtn)
			continue;
		codes[ncode].sys = getSysNum(parmbuf);
		codes[ncode].dev = getDevMask(pdev);
		if ((pbtn[0] | 0x20) == 'o') {
			if ((pbtn[1] | 0x20) == 'n' && !pbtn[2])
				codes[ncode].oper = POWER433_BUTTON_ON;
			else if ((pbtn[1] | 0x20) == 'f' &&
				(pbtn[2] | 0x20) == 'f')
				codes[ncode].oper = POWER433_BUTTON_OFF;
			else
				continue;
		} else
			continue;

		ncode++;
	}

        /* change scheduling priority */
        if (changeSched()) {
                fprintf(stderr, "Unable to change process scheduling priority: %s\n",
                        strerror (errno));
                exit(-1);
        }

	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();

	/* set transmission only */
	Radio433_init(gpio, -1);

	/* sending codes */
	for(i = 0; i < ncode; i++) {
		printf("Setting socket %s%s%s%s%s in group %d to %s.\n",
		       codes[i].dev & POWER433_DEVICE_A ? "A" : "",
		       codes[i].dev & POWER433_DEVICE_B ? "B" : "",
		       codes[i].dev & POWER433_DEVICE_C ? "C" : "",
		       codes[i].dev & POWER433_DEVICE_D ? "D" : "",
		       codes[i].dev & POWER433_DEVICE_E ? "E" : "",
		       codes[i].sys, codes[i].oper ? "ON" : "OFF");
		Radio433_sendDevCode(Radio433_pwrGetCode(codes[i].sys, codes[i].dev, codes[i].oper),
				     RADIO433_DEVICE_KEMOTURZ1226, CODE_RETRANS);
	}

	free(codes);
}
