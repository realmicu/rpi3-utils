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

#include "power433_lib.h"

#define	CODE_REPEATS	8	/* at least POWER433_RETRANS times */

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s {gpio} {code0} [code1] ...\n\n", progname);
	puts("Where:");
	puts("\tgpio\t - GPIO pin with external RF transmitter connected (mandatory)");
	puts("\tcode\t - code(s) to send (at least one)");
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

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int i, gpio, ncode;
	unsigned int *codes;

	/* show help */
	if (argc < 3) {
		help(argv[0]);
		exit(0);
	}

	/* get parameters */
	sscanf(argv[1], "%d", &gpio);

	ncode = argc - 2;
	codes = (unsigned int*)malloc(ncode * sizeof(unsigned int));
	for(i = 0; i < ncode; i++)
		sscanf(argv[i + 2], "%d", &codes[i]);

        /* change scheduling priority */
        if (changeSched()) {
                fprintf(stderr, "Unable to change process scheduling priority: %s\n",
                        strerror (errno));
                exit(-1);
        }

	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();

	/* set transmission only */
	Power433_init(gpio, -1);

	/* sending codes */
	for(i = 0; i < ncode; i++) {
		printf("Transmitting code %d (0x%08X) %d times... ", codes[i],
		       codes[i], CODE_REPEATS);
		Power433_repeatCode(codes[i], CODE_REPEATS);
		puts("done.");
	}

	free(codes);
}
