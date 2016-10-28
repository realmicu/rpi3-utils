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

#define MAX_BUFFER_LEN	1024	/* circular buffer size for timing events */
#define DISPLAY_DELAY	10	/* main display loop in miliseconds */

struct sample {
	int value;
	unsigned long uslen;
};

/* volatile keeps variables in memory for interrupts */
static volatile struct sample sbuf[MAX_BUFFER_LEN];
static volatile int gpio, intr, buflck, isrblk, rptr, wptr;
static struct timeval tstart;

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s {gpio} {seconds} \n\n", progname);
	puts("Where:");
	puts("\tgpio\t - GPIO pin to scan for state changes (mandatory)");
	puts("\tseconds\t - duration of scan in seconds (optional)");
}

/* ************* */
/* * Interrupt * */
/* ************* */

void handleGpioInt(void)
{
	int v;
	struct timeval t;

	intr++;
	if (buflck) {
		isrblk++;
		return;
	}
	gettimeofday(&t, NULL);
	buflck = 1;
	sbuf[wptr].value = digitalRead(gpio);
	sbuf[wptr].uslen = (t.tv_sec - tstart.tv_sec) * 1000000 + t.tv_usec - tstart.tv_usec;
	wptr = (wptr + 1) % MAX_BUFFER_LEN;
	buflck = 0;
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int i, dur;
	struct timeval t;

	/* show help */
	if (argc < 2) {
		help(argv[0]);
		exit(0);
	}

	/* get parameters */
	sscanf(argv[1], "%d", &gpio);
	if (argc > 2)
		sscanf(argv[2], "%d", &dur);
	else
		dur = 0;

	/* initialize global variables */
	intr = 0;	/* number of interrupts */
	buflck = 0;	/* buffer write lock */
	isrblk = 0;	/* blocked interrupt counter*/
	rptr = 0;	/* read pointer */
	wptr = 0;	/* write pointer */
	gettimeofday(&tstart, NULL);

	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();

	pinMode(gpio, INPUT);

	wiringPiISR(gpio, INT_EDGE_BOTH, handleGpioInt);

	for(;;) {
		if (dur > 0 && time(NULL) - tstart.tv_sec > dur)
			break;
		while(rptr != wptr) {
			printf("%lu : %d\n", sbuf[rptr].uslen, sbuf[rptr].value);
			rptr = (rptr + 1) % MAX_BUFFER_LEN;
		}
		delay(DISPLAY_DELAY);
	}
	gettimeofday(&t, NULL);
	printf("Total time: %.3lf second(s), total interrupts: %d (%d blocked)\n",
	       t.tv_sec - tstart.tv_sec + (t.tv_usec - tstart.tv_usec) / 1000000.0,
	       intr, isrblk);
}
