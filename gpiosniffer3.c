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

#define DELAY_MS	10

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
	int i;
	int val;
	unsigned long tstart, tustart;

	struct timeval t;

	/* show help */
	if (argc < 3) {
		help(argv[0]);
		exit(0);
	}

	/* get parameters */
	sscanf(argv[1], "%d", &gpio);
	sscanf(argv[2], "%d", &dur);

	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();

	pinMode(gpio, INPUT);

	gettimeofday(&t, NULL);
	tstart = t.tv_sec;
	tustart = t.tv_usec;

	for(;;) {
		gettimeofday(&t, NULL);

		if (t.tv_sec - tstart > dur)
			break;

		val = digitalRead(gpio);
		
		printf("%lu : %d\n",
		       (t.tv_sec - tstart) * 1000000 + t.tv_usec - tustart, val);

		delay(DELAY_MS);
	}
}
