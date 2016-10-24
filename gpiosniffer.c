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

#define MAX_BUFFER_SIZE		33554432  /* buffer size in bytes */


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
	int i, n, nmax;
	int val, preval;
	unsigned long tstart;
	unsigned long ts_sec, ts_us;

	struct timeval t;
	struct sample *s;

	/* show help */
	if (argc < 3) {
		help(argv[0]);
		exit(0);
	}

	/* get parameters */
	sscanf(argv[1], "%d", &gpio);
	sscanf(argv[2], "%d", &dur);

	/* prepare buffer */
	nmax = MAX_BUFFER_SIZE / sizeof(struct sample);
	s = (struct sample*)malloc(MAX_BUFFER_SIZE);
	if (s == NULL) {
		printf("Unable to allocate %d bytes of memory.\n", MAX_BUFFER_SIZE);
		exit(-1);
	}
	memset(s, 0, MAX_BUFFER_SIZE);
	printf("Info: buffer size is %d bytes, %d maximum variable samples\n", MAX_BUFFER_SIZE, nmax);

	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();

	pinMode(gpio, INPUT);

	preval = -1;
	n = -1;
	gettimeofday(&t, NULL);
	tstart = t.tv_sec;

	for(;;) {
		if (n > nmax)
			break;

		gettimeofday(&t, NULL);

		if (t.tv_sec - tstart > dur)
			break;

		val = digitalRead(gpio);
		
		if (val != preval) {
			s[++n].value = val;
			s[n].num = 1;
			s[n].uslen = 0;
			ts_sec = t.tv_sec;
			ts_us = t.tv_usec;
			preval = val;
		} else {
			s[n].num++;
			s[n].uslen = (t.tv_sec - ts_sec) * 1000000 + t.tv_usec - ts_us ;	
		}
	}
	for(i = 0; i <= n; i++)
		printf("%d : %d samples, %lu us = %3lf sec\n", s[i].value,
		       s[i].num, s[i].uslen, s[i].uslen / 1000000.0);
	free(s);
}
