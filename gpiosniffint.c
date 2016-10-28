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
	unsigned long tsus;	/* timestamp in microseconds */
};

/* volatile keeps variables in memory for interrupts */
static struct sample *sbuf;
static volatile int n, nmax;
static volatile int gpio, intr, buflck, isrblk, isrshut;
static struct timeval tstart;

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s {gpio} {seconds} \n\n", progname);
	puts("Where:");
	puts("\tgpio\t - GPIO pin to scan for state changes (mandatory)");
	puts("\tseconds\t - duration of scan in seconds (mandatory)");
}

/* ************* */
/* * Interrupt * */
/* ************* */

void handleGpioInt(void)
{
	/* please note that digitalRead() value of GPIO pin */
	/* may not reveal true state of PIN at the time of */
	/* interrupt triggering ; rely not on this */
	
	struct timeval t;

	if (isrshut)
		return;

	intr++;
	if (buflck || n > nmax) {
		isrblk++;
		return;
	}
	gettimeofday(&t, NULL);
	buflck = 1;
	sbuf[n].value = digitalRead(gpio);
	sbuf[n++].tsus = (t.tv_sec - tstart.tv_sec) * 1000000 + t.tv_usec - tstart.tv_usec;
	buflck = 0;
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int dur, i;
	struct timeval tstop;

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
	sbuf = (struct sample*)malloc(MAX_BUFFER_SIZE);
	if (sbuf == NULL) {
		printf("Unable to allocate %d bytes of memory.\n", MAX_BUFFER_SIZE);
		exit(-1);
	}
	memset(sbuf, 0, MAX_BUFFER_SIZE);
	printf("Info: buffer size is %d bytes, %d maximum variable samples\n",
	       MAX_BUFFER_SIZE, nmax);

	n = 0;
	intr = 0;	/* number of interrupts */
	buflck = 0;	/* buffer write lock */
	isrblk = 0;	/* blocked interrupt counter*/
	isrshut = 0;	/* ISR is waiting for shutdown */

	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();

	pinMode(gpio, INPUT);

	gettimeofday(&tstart, NULL);
	wiringPiISR(gpio, INT_EDGE_BOTH, handleGpioInt);
	sleep(dur);	/* sleep here, all work is done in interrupt */
	gettimeofday(&tstop, NULL);

	isrshut = 1;	/* block interrupt to stop statistic */

	printf("%lu : %d [ 0 ] ( n/a )\n", sbuf[0].tsus, sbuf[0].value);
	for(i = 1; i < n; i++)
		printf("%lu : %d [ %d ] ( %lu )\n", sbuf[i].tsus, sbuf[i].value,
		       i % 2, sbuf[i].tsus - sbuf[i-1].tsus);

	printf("Total time: %.3lf second(s), samples: %d, interrupts: %d (%d blocked)\n",
	       tstop.tv_sec - tstart.tv_sec + (tstop.tv_usec - tstart.tv_usec) / 1000000.0,
	       n, intr, isrblk);

	free(sbuf);
}
