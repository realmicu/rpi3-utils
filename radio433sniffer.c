#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sched.h>

#include <wiringPi.h>

#include "radio433_lib.h"
#include "radio433_dev.h"

#define OWNER_UID	500
#define OWNER_GID	500

#include <signal.h>

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s {gpio}\n\n", progname);
	puts("Where:");
	puts("\tgpio\t - GPIO pin with external RF receiver data (mandatory)");
}

/* drop super-user privileges */
int dropRootPriv(int newuid, int newgid)
{
	/* start with GID */
	if (setresgid(newgid, newgid, newgid))
		return -1;
	if (setresuid(newuid, newuid, newuid))
		return -1;
	return 0;
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

/* Intercept TERM and INT signals */
void signalQuit(int sig)
{
	exit(0);
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	struct timeval ts;
	struct tm *tl;
	unsigned long long code;
	int gpio, type, bits;
	char *stype[] = { "PWR", "THM" };
	int sysid, devid, btn;
	int ch, batlow, tdir, humid;
	double temp;
	char trend[3] = { '_', '/', '\\' };

	/* show help */
	if (argc < 2) {
		help(argv[0]);
		exit(0);
	}

	/* get parameters */
	sscanf(argv[1], "%d", &gpio);

	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();

	/* no transmission, only read */
	Radio433_init(-1, gpio);

	/* change scheduling priority */
	if (changeSched()) {
		fprintf(stderr, "Unable to change process scheduling priority: %s\n",
			strerror (errno));
		exit(-1);
	}

	/* drop privileges */
	if (dropRootPriv(OWNER_UID, OWNER_GID)) {
		fprintf(stderr, "Unable to drop super-user privileges: %s\n",
			strerror (errno));
		exit(-1);
	}

	/* info */
	printf("Starting to capture RF codes from receiver connected to GPIO pin %d ...\n",
	       gpio);

	/* function loop - never ends, send signal to exit */
	for(;;) {
		code = Radio433_getCode(&ts, &type, &bits);
		tl = localtime(&ts.tv_sec);
		printf("%d-%02d-%02d %02d:%02d:%02d.%03u", 1900 + tl->tm_year,
		       tl->tm_mon + 1, tl->tm_mday, tl->tm_hour, tl->tm_min,
		       tl->tm_sec, ts.tv_usec / 1000);
		printf("  %s len = %d , code = 0x%0*llX", stype[type], bits,
		       bits >> 2, code);
		if (type == RADIO433_DEVICE_KEMOTURZ1226) {
			if (Radio433_pwrGetCommand(code, &sysid, &devid, &btn))
				printf(" , %d : %s%s%s%s%s : %s\n", sysid,
				       devid & POWER433_DEVICE_A ? "A" : "",
				       devid & POWER433_DEVICE_B ? "B" : "",
				       devid & POWER433_DEVICE_C ? "C" : "",
				       devid & POWER433_DEVICE_D ? "D" : "",
				       devid & POWER433_DEVICE_E ? "E" : "",
				       btn ? "ON" : "OFF");
		}
		else if (type == RADIO433_DEVICE_HYUWSSENZOR77TH) {
			if (Radio433_thmGetData(code, &sysid, &devid, &ch,
						&batlow, &tdir, &temp, &humid))
				printf(" , %1d , T: %+.1lf C %c , H: %d %%\n",
				       ch, temp, tdir < 0 ? '!' : trend[tdir],
				       humid);

		}
	}
}
