#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>

#include <wiringPi.h>

#define SIGNAL_NOISE_US		0  /* noise if less microseconds*/


struct sample {
	int value;	/* 0 or 1 */
	int num;	/* number of samples in a row */
	unsigned long uslen;	/* length of series in microseconds */
};

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s {gpio} {seconds} \n\n", progname);
	puts("Where:");
	puts("\tgpio\t - GPIO pin to scan for state changes (mandatory)");
	puts("\tseconds\t - duration of scan in seconds (mandatory)");
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int gpio, dur;
	int i, n;
	int val, preval;
	unsigned long tstart;
	unsigned long ts_sec, ts_us;

	struct timeval t;
	struct sample s;

	/* show help */
	if (argc < 3) {
		help(argv[0]);
		exit(0);
	}

	/* get parameters */
	sscanf(argv[1], "%d", &gpio);
	sscanf(argv[2], "%d", &dur);

	memset(&s, 0, sizeof(s));

	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();

	pinMode(gpio, INPUT);

	preval = -1;
	gettimeofday(&t, NULL);
	tstart = t.tv_sec;

	for(;;) {
		gettimeofday(&t, NULL);

		if (t.tv_sec - tstart > dur)
			break;

		val = digitalRead(gpio);
		
		if (val != preval) {
			if (preval >= 0 && s.uslen > SIGNAL_NOISE_US)
				printf("%d : %d samples, %lu us = %3lf sec\n",
				       s.value, s.num, s.uslen,
				       s.uslen / 1000000.0);
			s.value = val;
			s.num = 1;
			s.uslen = 0;
			ts_sec = t.tv_sec;
			ts_us = t.tv_usec;
			preval = val;
		} else {
			s.num++;
			s.uslen = (t.tv_sec - ts_sec) * 1000000 + t.tv_usec - ts_us ;	
		}
	}
}
