#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>		/* For O_* constants */
#include <semaphore.h>

#include <wiringPi.h>
#include <wiringPiI2C.h>

#include "bh1750_lib.h"

extern char *optarg;
extern int optind, opterr, optopt;

#define I2C_SEMAPHORE_NAME	"/i2c-1-lck"

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s [-s] [-c N]\n\n", progname);
	puts("Where:");
	puts("\t-s\t - use system-wide user semaphore for I2C bus access (optional)");
	puts("\t-c N\t - run continously every N seconds (optional)");
}

/* Semaphore operations */
void i2cLock(int semflg, sem_t *sem)
{
	if (semflg)
		sem_wait(sem);
}

void i2cUnlock(int semflg, sem_t *sem)
{
	if (semflg)
		sem_post(sem);
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int opt, semflg, cntsec;
	sem_t *i2csem;
	double lx;

	/* analyze command line */
	semflg = 0;
	cntsec = 0;
	while((opt = getopt(argc, argv, "sc:")) != -1) {
		if (opt == 's')
			semflg = 1;
		else if (opt == 'c')
			sscanf(optarg, "%d", &cntsec);
		else if (opt == '?') {
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (semflg) {
		i2csem = sem_open(I2C_SEMAPHORE_NAME, O_CREAT, 0600, 1);
		if (i2csem == SEM_FAILED) {
			fprintf(stderr, "Unable to open I2C semaphore: %s\n",
				strerror (errno));
			exit(EXIT_FAILURE);
		}
	}

	/* I2C setup */
	wiringPiSetup();
	int fd = wiringPiI2CSetup(BH1750_I2C_ADDR);
	if (fd < 0)
	{
		fprintf(stderr, "Unable to open I2C device: %s\n",
			strerror (errno));
		exit(-1);
	}

	/* initialize sensor */
	BH1750_powerOn(fd);
	BH1750_softReset(fd);
	BH1750_setMode(fd, BH1750_MODE_CONT, BH1750_MODE_RES_H);

	/* read luminance */
	for(;;)	{
		i2cLock(semflg, i2csem);
		lx = BH1750_getLx(fd);
		i2cUnlock(semflg, i2csem);
		printf("L = %.3lf lx\n", lx);
		if (cntsec)
			sleep(cntsec);
		else
			break;
	}

	return 0;
}
