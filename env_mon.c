/*
 *  This program combines sensor output and displays all data
 *  in fullscreen mode using curses library. Currently it scans
 *  following I2C devices:
 *  - HTU21D humidity/temperature
 *  - BMP180 pressure/temperature
 *  - BH1750 luminance
*/

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <time.h>
#include <sys/time.h>

#include <ncurses.h>

#include <fcntl.h>		/* For O_* constants */
#include <semaphore.h>

#include <wiringPi.h>
#include <wiringPiI2C.h>

#include "htu21d_lib.h"
#include "bmp180_lib.h"
#include "bh1750_lib.h"

extern char *optarg;
extern int optind, opterr, optopt;

#define I2C_SEMAPHORE_NAME	"/i2c-1-lck"

#define DEFAULT_DELAY_SEC	10
#define MAX_SENSOR_NAME_LEN	6
#define MAX_VALUE_NAME_LEN	11
#define MAX_FIELD_LEN		10

#define COLMIN			(13 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN)
#define COLNOW			(18 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN \
				 + MAX_FIELD_LEN)
#define COLMAX			(23 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN \
				 + 2 * MAX_FIELD_LEN)

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s [-s] [-t N]\n\n", progname);
	puts("Where:");
	puts("\t-s\t - use system-wide user semaphore for I2C bus access (optional)");
	puts("\t-t N\t - update every N seconds (default: 10)");
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

/* Sensor initialization */
int htu21d_init(void)
{
	int fd;

	fd = wiringPiI2CSetup(HTU21D_I2C_ADDR);
	if (fd < 0) {
		fprintf(stderr, "Unable to open I2C device: %s\n",
			strerror(errno));
		exit(-1);
	}

	/* Soft reset, device starts in 12-bit humidity / 14-bit temperature */
	HTU21D_softReset(fd);

	return fd;
}

int bmp180_init(void)
{
	int fd;

	fd = wiringPiI2CSetup(BMP180_I2C_ADDR);
	if (fd < 0) {
		fprintf(stderr, "Unable to open I2C device: %s\n",
			strerror (errno));
		exit(-1);
	}

	/* Soft reset */
	BMP180_softReset(fd);

	/* Check if BMP180 responds */
	if (!BMP180_isPresent(fd)) {
		fputs("Unable to find BMP180 sensor chip.\n", stderr);
		exit(-2);
	}

	BMP180_getCalibrationData(fd);

	return fd;
}

int bh1750_init(void)
{
	int fd;

	fd = wiringPiI2CSetup(BH1750_I2C_ADDR);
	if (fd < 0) {
		fprintf(stderr, "Unable to open I2C device: %s\n",
			strerror (errno));
		exit(-1);
	}

        BH1750_powerOn(fd);
        BH1750_softReset(fd);
        BH1750_setMode(fd, BH1750_MODE_CONT, BH1750_MODE_RES_H2);

	return fd;
}

/* nCurses */
void initCurses(void)
{
        initscr();
        cbreak();
        noecho();
        curs_set(0);
}

int drawHeader(void)
{
	mvaddch(0, 0, ACS_ULCORNER);
	hline(ACS_HLINE, 24 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
	      3 * MAX_FIELD_LEN);
	mvaddch(0, 5 + MAX_SENSOR_NAME_LEN, ACS_TTEE);
	mvaddch(0, 10 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN, ACS_TTEE);
	mvaddch(0, 15 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		MAX_FIELD_LEN, ACS_TTEE);
	mvaddch(0, 20 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		2 * MAX_FIELD_LEN, ACS_TTEE);
	mvaddch(0, 25 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		3 * MAX_FIELD_LEN, ACS_URCORNER);
	mvaddch(1, 0, ACS_VLINE);
	mvaddch(1, 5 + MAX_SENSOR_NAME_LEN, ACS_VLINE);
	mvaddch(1, 10 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN, ACS_VLINE);
	mvaddstr(1, 16 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN, "Min");
	mvaddch(1, 15 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		MAX_FIELD_LEN, ACS_VLINE);
	attron(A_BOLD);
	mvaddstr(1, 21 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		 MAX_FIELD_LEN, "Now");
	attroff(A_BOLD);
	mvaddch(1, 20 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		2 * MAX_FIELD_LEN, ACS_VLINE);
	mvaddstr(1, 26 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		 2 * MAX_FIELD_LEN, "Max");
	mvaddch(1, 25 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		3 * MAX_FIELD_LEN, ACS_VLINE);
	return 2;
}


int drawSensorFrame(int ystart, const char *sensorname, int lines, char *labels[])
{
	int i, l2;

	l2 = lines << 1;
	mvaddch(ystart, 0, ACS_LTEE);
	hline(ACS_HLINE, 24 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
	      3 * MAX_FIELD_LEN);
	mvaddch(ystart, 5 + MAX_SENSOR_NAME_LEN, ACS_PLUS);
	mvaddch(ystart, 10 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN,
		ACS_PLUS);
	mvaddch(ystart, 15 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		MAX_FIELD_LEN, ACS_PLUS);
	mvaddch(ystart, 20 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		2 * MAX_FIELD_LEN, ACS_PLUS);
	mvaddch(ystart, 25 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		3 * MAX_FIELD_LEN, ACS_RTEE);
	mvaddnstr(ystart + lines, 3, sensorname, MAX_SENSOR_NAME_LEN);
	mvvline(ystart + 1, 0, ACS_VLINE, l2 - 1);
	mvvline(ystart + 1, 5 + MAX_SENSOR_NAME_LEN, ACS_VLINE, l2 - 1);
	mvvline(ystart + 1, 10 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN,
		ACS_VLINE, l2 - 1);
	mvvline(ystart + 1, 15 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		MAX_FIELD_LEN, ACS_VLINE, l2 - 1);
	mvvline(ystart + 1, 20 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		2 * MAX_FIELD_LEN, ACS_VLINE, l2 - 1);
	mvvline(ystart + 1, 25 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		3 * MAX_FIELD_LEN, ACS_VLINE, l2 - 1);
	for(i = 0; i < lines; i++) {
		attron(A_BOLD);
		mvaddnstr(ystart + (i << 1) + 1, 8 + MAX_SENSOR_NAME_LEN,
			  labels[i], MAX_VALUE_NAME_LEN);
		attroff(A_BOLD);
		mvaddch(ystart + (i << 1) + 2, 5 + MAX_SENSOR_NAME_LEN,
			ACS_LTEE);
		hline(ACS_HLINE, 19 + MAX_VALUE_NAME_LEN + 3 * MAX_FIELD_LEN);
		mvaddch(ystart + (i << 1) + 2, 10 + MAX_SENSOR_NAME_LEN + \
			MAX_VALUE_NAME_LEN, ACS_PLUS);
		mvaddch(ystart + (i << 1) + 2, 15 + MAX_SENSOR_NAME_LEN + \
			MAX_VALUE_NAME_LEN + MAX_FIELD_LEN, ACS_PLUS);
		mvaddch(ystart + (i << 1) + 2, 20 + MAX_SENSOR_NAME_LEN + \
			MAX_VALUE_NAME_LEN + 2 * MAX_FIELD_LEN, ACS_PLUS);
		mvaddch(ystart + (i << 1) + 2, 25 + MAX_SENSOR_NAME_LEN + \
			MAX_VALUE_NAME_LEN + 3 * MAX_FIELD_LEN, ACS_RTEE);
	}
	mvaddch(ystart + l2, 0, ACS_LLCORNER);
	hline(ACS_HLINE, 24 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
	      3 * MAX_FIELD_LEN);
	mvaddch(ystart + l2, 5 + MAX_SENSOR_NAME_LEN, ACS_BTEE);
	mvaddch(ystart + l2, 10 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN,
		ACS_BTEE);
	mvaddch(ystart + l2, 15 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		MAX_FIELD_LEN, ACS_BTEE);
	mvaddch(ystart + l2, 20 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		2 * MAX_FIELD_LEN, ACS_BTEE);
	mvaddch(ystart + l2, 25 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN + \
		3 * MAX_FIELD_LEN, ACS_LRCORNER);
	return ystart + l2;
}

void updateFieldDouble(int y, int x, int sign, double value, char *unit,
		       int bold)
{
	int i;
	char ibuf[MAX_FIELD_LEN + 1], obuf[MAX_FIELD_LEN + 1];

	memset(ibuf, 0, MAX_FIELD_LEN + 1);

	if (sign)
		snprintf(ibuf, MAX_FIELD_LEN + 1, "%+.1lf %s", value, unit);
	else
		snprintf(ibuf, MAX_FIELD_LEN + 1, "%.1lf %s", value, unit);

	memset(obuf, ' ', MAX_FIELD_LEN);
	obuf[MAX_FIELD_LEN] = 0;
	strcpy(obuf + (MAX_FIELD_LEN - strlen(ibuf)), ibuf);

	if (bold)
		attron(A_BOLD);

	mvaddstr(y, x, obuf);

	if (bold)
		attroff(A_BOLD);
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int opt, semflg, cntsec;
	sem_t *i2csem;
	int htu21d, bmp180, bh1750;
	int lastline;
	char *htu21d_labels[2] = { "Humidity", "Temperature" };
	char *bmp180_labels[2] = { "Pressure", "Temperature" };
	char *bh1750_labels[1] = { "Luminance" };
	double t0, t0min, t0max;
	double h, hmin, hmax;
	double t1, t1min, t1max;
	double p, pmin, pmax;
	double l, lmin, lmax;

	/* analyze command line */
	semflg = 0;
	cntsec = DEFAULT_DELAY_SEC;
	while((opt = getopt(argc, argv, "st:")) != -1) {
		if (opt == 's')
			semflg = 1;
		else if (opt == 't')
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
	wiringPiSetupGpio();

	/* sensor init */
	i2cLock(semflg, i2csem);
	htu21d = htu21d_init();
	bmp180 = bmp180_init();
	bh1750 = bh1750_init();
	i2cUnlock(semflg, i2csem);

	/* initial min/max values to overwrite */
	t0min = 9999;
	t0max = -9999;
	hmin = 100;
	hmax = 0;
	pmin = 9999;
	pmax = 0;
	t1min = 9999;
	t1max = -9999;
	lmin = 65536;
	lmax = 0;

	/* setup screen */
	initCurses();
	lastline = drawHeader();
	lastline = drawSensorFrame(lastline, "HTU21D", 2, htu21d_labels);
	lastline = drawSensorFrame(lastline, "BMP180", 2, bmp180_labels);
	lastline = drawSensorFrame(lastline, "BH1750", 1, bh1750_labels);
	refresh();

	/* main loop */
	for(;;) {
		i2cLock(semflg, i2csem);
		t0 = HTU21D_getTemperature(htu21d);
		h = HTU21D_getHumidity(htu21d);
		i2cUnlock(semflg, i2csem);
		if (t0 > t0max) t0max = t0;
		if (t0 < t0min) t0min = t0;
		if (h > hmax) hmax = h;
		if (h < hmin) hmin = h;
		updateFieldDouble(3, COLMIN, 0, hmin, "%", 0);
		updateFieldDouble(3, COLNOW, 0, h, "%", 1);
		updateFieldDouble(3, COLMAX, 0, hmax, "%", 0);
		updateFieldDouble(5, COLMIN, 1, t0min, "C", 0);
		updateFieldDouble(5, COLNOW, 1, t0, "C", 1);
		updateFieldDouble(5, COLMAX, 1, t0max, "C", 0);

		i2cLock(semflg, i2csem);
		p = BMP180_getPressureFP(bmp180, BMP180_OSS_MODE_UHR, &t1);
		i2cUnlock(semflg, i2csem);
		if (p > pmax) pmax = p;
		if (p < pmin) pmin = p;
		if (t1 > t1max) t1max = t1;
		if (t1 < t1min) t1min = t1;
		updateFieldDouble(7, COLMIN, 0, pmin, "hPa", 0);
		updateFieldDouble(7, COLNOW, 0, p, "hPa", 1);
		updateFieldDouble(7, COLMAX, 0, pmax, "hPa", 0);
		updateFieldDouble(9, COLMIN, 1, t1min, "C", 0);
		updateFieldDouble(9, COLNOW, 1, t1, "C", 1);
		updateFieldDouble(9, COLMAX, 1, t1max, "C", 0);

		i2cLock(semflg, i2csem);
		l = BH1750_getLx(bh1750);
		i2cUnlock(semflg, i2csem);
		if (l > lmax) lmax = l;
		if (l < lmin) lmin = l;
		updateFieldDouble(11, COLMIN, 0, lmin, "lx", 0);
		updateFieldDouble(11, COLNOW, 0, l, "lx", 1);
		updateFieldDouble(11, COLMAX, 0, lmax, "lx", 0);

		move(lastline + 1, 0);
		refresh();
		if (cntsec)
			sleep(cntsec);
		else
			break;
	}

	endwin();
	return 0;
}
