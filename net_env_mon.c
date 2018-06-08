/*
 *  This program connects to sensorproxy daemon and display sensor
 *  values in fullscreen mode using curses library.
*/

#define _GNU_SOURCE		/* See feature_test_macros(7) */
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <ncurses.h>
#include <linux/limits.h>	/* for PATH_MAX */
#include <fcntl.h>		/* for O_* constants */
#include <string.h>		/* for basename() */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern char *optarg;
extern int optind, opterr, optopt;

#define DEFAULT_DELAY_SEC	10
#define SPROXY_ADDR		"127.0.0.1"
#define SPROXY_PORT		5444
#define RCVBUF_SIZE		4096
#define MAX_SENSOR_NAME_LEN	6
#define MAX_VALUE_NAME_LEN	11
#define MAX_FIELD_LEN		10

#define COLMIN			(13 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN)
#define COLNOW			(18 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN \
				 + MAX_FIELD_LEN)
#define COLMAX			(23 + MAX_SENSOR_NAME_LEN + MAX_VALUE_NAME_LEN \
				 + 2 * MAX_FIELD_LEN)

struct hyuw77_dataptr {
	char *tempmin, *tempcur, *tempmax, *tempunit, *temptrnd;
	char *humidmin, *humidcur, *humidmax, *humidunit;
	char *batlow, *sigcur, *sigmax;
};

struct htu21d_dataptr {
	char *humidmin, *humidcur, *humidmax, *humidunit;
	char *tempmin, *tempcur, *tempmax, *tempunit;
};

struct bmp180_dataptr {
	char *pressmin, *presscur, *pressmax, *pressunit;
	char *tempmin, *tempcur, *tempmax, *tempunit;
};

struct bh1750_dataptr {
	char *lightmin, *lightcur, *lightmax, *lightunit;
};

struct bme280_dataptr {
	char *pressmin, *presscur, *pressmax, *pressunit;
	char *tempmin, *tempcur, *tempmax, *tempunit;
	char *humidmin, *humidcur, *humidmax, *humidunit;
};

/* Show help */
void help(char *progname)
{
	printf("\nUsage:\n\t%s [-t N] [server [port]]\n\n", progname);
	puts("Where:");
	printf("\t-t N\t - update every N seconds (optional, default: %d)\n", DEFAULT_DELAY_SEC);
	printf("\tserver\t - IPv4 address of sensor proxy server (optional, default: %s)\n", SPROXY_ADDR);
	printf("\tport\t - TCP port of sensor proxy server (optional, default: %d)\n", SPROXY_PORT);
	puts("\nRecognized devices - hyuws77th, htu21d, bmp180, bh1750, bme280\n");
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

int drawSensorFrame(int ystart, const char *sensorname, int lines, const char *labels[])
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

void printField(int y, int x, int bold, int just, const char *fmt, ...)
{
	char ibuf[MAX_FIELD_LEN + 1], obuf[MAX_FIELD_LEN + 1];
	char *ptr;
	int len;
	va_list ap;

	memset(ibuf, 0, MAX_FIELD_LEN + 1);

	va_start(ap, fmt);
	vsnprintf(ibuf, MAX_FIELD_LEN + 1, fmt, ap);
	va_end(ap);

	memset(obuf, ' ', MAX_FIELD_LEN);
	obuf[MAX_FIELD_LEN] = 0;
	ptr = obuf;
	len = strlen(ibuf);
	/* just=-1 left, just=0 center, just=1 right */
	if (!just)
		ptr = obuf + ((MAX_FIELD_LEN - len) >> 1);
	else if (just > 0)
		ptr = obuf + (MAX_FIELD_LEN - len);
	memcpy(ptr, ibuf, len);

	if (bold)
		attron(A_BOLD);

	mvaddstr(y, x, obuf);

	if (bold)
		attroff(A_BOLD);
}

int connectServer(struct sockaddr_in *s)
{
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd == -1)
		return -1;

	if (connect(fd, (struct sockaddr *)s, sizeof(*s)) == -1) {
		close(fd);
		return -1;
	}

	return fd;
}

char *findValue(const char *line, const char *value)
{
	char *ptr;

	ptr = strstr(line, value);
	if (ptr != NULL) {
		ptr = strrchr(ptr, '=');
		if (ptr != NULL)
			ptr++;
	}
	return ptr;
}

int fillData_hyuw77(const char *line, struct hyuw77_dataptr *data)
{
	char *tmpptr;

	if (findValue(line, "/timestamp=")) {
		memset(data, 0, sizeof(struct hyuw77_dataptr));
		return 0;
	} else if (findValue(line, "/index="))
		return 1;

	if (!data->tempmin) {
		tmpptr = findValue(line, "/temp/min=");
		if (tmpptr) {
			data->tempmin = tmpptr;
			return 0;
		}
	}
	if (!data->tempcur) {
		tmpptr = findValue(line, "/temp/cur=");
		if (tmpptr) {
			data->tempcur = tmpptr;
			return 0;
		}
	}
	if (!data->tempmax) {
		tmpptr = findValue(line, "/temp/max=");
		if (tmpptr) {
			data->tempmax = tmpptr;
			return 0;
		}
	}
	if (!data->tempunit) {
		tmpptr = findValue(line, "/temp/unit=");
		if (tmpptr) {
			data->tempunit = tmpptr;
			return 0;
		}
	}
	if (!data->temptrnd) {
		tmpptr = findValue(line, "/temp/trend=");
		if (tmpptr) {
			data->temptrnd = tmpptr;
			return 0;
		}
	}
	if (!data->humidmin) {
		tmpptr = findValue(line, "/humid/min=");
		if (tmpptr) {
			data->humidmin = tmpptr;
			return 0;
		}
	}
	if (!data->humidcur) {
		tmpptr = findValue(line, "/humid/cur=");
		if (tmpptr) {
			data->humidcur = tmpptr;
			return 0;
		}
	}
	if (!data->humidmax) {
		tmpptr = findValue(line, "/humid/max=");
		if (tmpptr) {
			data->humidmax = tmpptr;
			return 0;
		}
	}
	if (!data->humidunit) {
		tmpptr = findValue(line, "/humid/unit=");
		if (tmpptr) {
			data->humidunit = tmpptr;
			return 0;
		}
	}
	if (!data->batlow) {
		tmpptr = findValue(line, "/batlow=");
		if (tmpptr) {
			data->batlow = tmpptr;
			return 0;
		}
	}
	if (!data->sigmax) {
		tmpptr = findValue(line, "/signal/max=");
		if (tmpptr) {
			data->sigmax = tmpptr;
			return 0;
		}
	}
	if (!data->sigcur) {
		tmpptr = findValue(line, "/signal/cur=");
		if (tmpptr) {
			data->sigcur = tmpptr;
			return 0;
		}
	}

	return 0;
}

int fillData_htu21d(const char *line, struct htu21d_dataptr *data)
{
	char *tmpptr;

	if (findValue(line, "/timestamp=")) {
		memset(data, 0, sizeof(struct htu21d_dataptr));
		return 0;
	} else if (findValue(line, "/index="))
		return 1;

	if (!data->humidmin) {
		tmpptr = findValue(line, "/humid/min=");
		if (tmpptr) {
			data->humidmin = tmpptr;
			return 0;
		}
	}
	if (!data->humidcur) {
		tmpptr = findValue(line, "/humid/cur=");
		if (tmpptr) {
			data->humidcur = tmpptr;
			return 0;
		}
	}
	if (!data->humidmax) {
		tmpptr = findValue(line, "/humid/max=");
		if (tmpptr) {
			data->humidmax = tmpptr;
			return 0;
		}
	}
	if (!data->humidunit) {
		tmpptr = findValue(line, "/humid/unit=");
		if (tmpptr) {
			data->humidunit = tmpptr;
			return 0;
		}
	}
	if (!data->tempmin) {
		tmpptr = findValue(line, "/temp/min=");
		if (tmpptr) {
			data->tempmin = tmpptr;
			return 0;
		}
	}
	if (!data->tempcur) {
		tmpptr = findValue(line, "/temp/cur=");
		if (tmpptr) {
			data->tempcur = tmpptr;
			return 0;
		}
	}
	if (!data->tempmax) {
		tmpptr = findValue(line, "/temp/max=");
		if (tmpptr) {
			data->tempmax = tmpptr;
			return 0;
		}
	}
	if (!data->tempunit) {
		tmpptr = findValue(line, "/temp/unit=");
		if (tmpptr) {
			data->tempunit = tmpptr;
			return 0;
		}
	}

	return 0;
}

int fillData_bmp180(const char *line, struct bmp180_dataptr *data)
{
	char *tmpptr;

	if (findValue(line, "/timestamp=")) {
		memset(data, 0, sizeof(struct bmp180_dataptr));
		return 0;
	} else if (findValue(line, "/index="))
		return 1;

	if (!data->pressmin) {
		tmpptr = findValue(line, "/press/min=");
		if (tmpptr) {
			data->pressmin = tmpptr;
			return 0;
		}
	}
	if (!data->presscur) {
		tmpptr = findValue(line, "/press/cur=");
		if (tmpptr) {
			data->presscur = tmpptr;
			return 0;
		}
	}
	if (!data->pressmax) {
		tmpptr = findValue(line, "/press/max=");
		if (tmpptr) {
			data->pressmax = tmpptr;
			return 0;
		}
	}
	if (!data->pressunit) {
		tmpptr = findValue(line, "/press/unit=");
		if (tmpptr) {
			data->pressunit = tmpptr;
			return 0;
		}
	}
	if (!data->tempmin) {
		tmpptr = findValue(line, "/temp/min=");
		if (tmpptr) {
			data->tempmin = tmpptr;
			return 0;
		}
	}
	if (!data->tempcur) {
		tmpptr = findValue(line, "/temp/cur=");
		if (tmpptr) {
			data->tempcur = tmpptr;
			return 0;
		}
	}
	if (!data->tempmax) {
		tmpptr = findValue(line, "/temp/max=");
		if (tmpptr) {
			data->tempmax = tmpptr;
			return 0;
		}
	}
	if (!data->tempunit) {
		tmpptr = findValue(line, "/temp/unit=");
		if (tmpptr) {
			data->tempunit = tmpptr;
			return 0;
		}
	}

	return 0;
}

int fillData_bme280(const char *line, struct bme280_dataptr *data)
{
	char *tmpptr;

	if (findValue(line, "/timestamp=")) {
		memset(data, 0, sizeof(struct bme280_dataptr));
		return 0;
	} else if (findValue(line, "/index="))
		return 1;

	if (!data->pressmin) {
		tmpptr = findValue(line, "/press/min=");
		if (tmpptr) {
			data->pressmin = tmpptr;
			return 0;
		}
	}
	if (!data->presscur) {
		tmpptr = findValue(line, "/press/cur=");
		if (tmpptr) {
			data->presscur = tmpptr;
			return 0;
		}
	}
	if (!data->pressmax) {
		tmpptr = findValue(line, "/press/max=");
		if (tmpptr) {
			data->pressmax = tmpptr;
			return 0;
		}
	}
	if (!data->pressunit) {
		tmpptr = findValue(line, "/press/unit=");
		if (tmpptr) {
			data->pressunit = tmpptr;
			return 0;
		}
	}
	if (!data->tempmin) {
		tmpptr = findValue(line, "/temp/min=");
		if (tmpptr) {
			data->tempmin = tmpptr;
			return 0;
		}
	}
	if (!data->tempcur) {
		tmpptr = findValue(line, "/temp/cur=");
		if (tmpptr) {
			data->tempcur = tmpptr;
			return 0;
		}
	}
	if (!data->tempmax) {
		tmpptr = findValue(line, "/temp/max=");
		if (tmpptr) {
			data->tempmax = tmpptr;
			return 0;
		}
	}
	if (!data->tempunit) {
		tmpptr = findValue(line, "/temp/unit=");
		if (tmpptr) {
			data->tempunit = tmpptr;
			return 0;
		}
	}
	if (!data->humidmin) {
		tmpptr = findValue(line, "/humid/min=");
		if (tmpptr) {
			data->humidmin = tmpptr;
			return 0;
		}
	}
	if (!data->humidcur) {
		tmpptr = findValue(line, "/humid/cur=");
		if (tmpptr) {
			data->humidcur = tmpptr;
			return 0;
		}
	}
	if (!data->humidmax) {
		tmpptr = findValue(line, "/humid/max=");
		if (tmpptr) {
			data->humidmax = tmpptr;
			return 0;
		}
	}
	if (!data->humidunit) {
		tmpptr = findValue(line, "/humid/unit=");
		if (tmpptr) {
			data->humidunit = tmpptr;
			return 0;
		}
	}

	return 0;
}

int fillData_bh1750(const char *line, struct bh1750_dataptr *data)
{
	char *tmpptr;

	if (findValue(line, "/timestamp=")) {
		memset(data, 0, sizeof(struct bh1750_dataptr));
		return 0;
	} else if (findValue(line, "/index="))
		return 1;

	if (!data->lightmin) {
		tmpptr = findValue(line, "/light/min=");
		if (tmpptr) {
			data->lightmin = tmpptr;
			return 0;
		}
	}
	if (!data->lightcur) {
		tmpptr = findValue(line, "/light/cur=");
		if (tmpptr) {
			data->lightcur = tmpptr;
			return 0;
		}
	}
	if (!data->lightmax) {
		tmpptr = findValue(line, "/light/max=");
		if (tmpptr) {
			data->lightmax = tmpptr;
			return 0;
		}
	}
	if (!data->lightunit) {
		tmpptr = findValue(line, "/light/unit=");
		if (tmpptr) {
			data->lightunit = tmpptr;
			return 0;
		}
	}

	return 0;
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int opt, cntsec, spxport, lastline;
	char progname[PATH_MAX + 1];
	struct sockaddr_in spxsin;
	int clfd, msglen;
	char *bufptr, *bufptrn;
	char buf[RCVBUF_SIZE];
	char spcbuf[MAX_FIELD_LEN + 1];
	const char *htu21d_labels[2] = { "Humidity", "Temperature" };
	const char *bmp180_labels[2] = { "Pressure", "Temperature" };
	const char *bh1750_labels[1] = { "Luminance" };
	const char *hyuw77_labels[3] = { "Temperature", "Humidity", "Information" };
	const char *bme280_labels[3] = { "Pressure", "Temperature", "Humidity" };
	struct hyuw77_dataptr hyuw77_dptr;
	struct htu21d_dataptr htu21d_dptr;
	struct bmp180_dataptr bmp180_dptr;
	struct bh1750_dataptr bh1750_dptr;
	struct bme280_dataptr bme280_dptr;
	int hyuw77_ok, htu21d_ok, bmp180_ok, bh1750_ok, bme280_ok;

	/* clear field */
	memset(spcbuf, ' ', MAX_FIELD_LEN);
	spcbuf[MAX_FIELD_LEN] = 0;

	/* get process name */
	strncpy(progname, basename(argv[0]), PATH_MAX);

	/* analyze command line */
	cntsec = DEFAULT_DELAY_SEC;
	while((opt = getopt(argc, argv, "t:")) != -1) {
		if (opt == 't')
			sscanf(optarg, "%d", &cntsec);
		else if (opt == '?') {
			help(progname);
			exit(EXIT_FAILURE);
		}
	}

	/* prepare connection data */
	memset((char *)&spxsin, 0, sizeof(spxsin));
	spxsin.sin_family = AF_INET;
	if (optind == argc)
		inet_aton(SPROXY_ADDR, &spxsin.sin_addr);
	else {
		if (!inet_aton(argv[optind++], &spxsin.sin_addr)) {
			dprintf(STDERR_FILENO, "Invalid IPv4 address specification.\n");
			exit(EXIT_FAILURE);
		}
	}
	if (optind == argc)
		spxport = SPROXY_PORT;
	else
		sscanf(argv[optind], "%d", &spxport);
	spxsin.sin_port = htons(spxport);

	/* setup screen */
	initCurses();
	lastline = drawHeader();
	lastline = drawSensorFrame(lastline, "HTU21D", 2, htu21d_labels);
	lastline = drawSensorFrame(lastline, "BMP180", 2, bmp180_labels);
	lastline = drawSensorFrame(lastline, "BME280", 3, bme280_labels);
	lastline = drawSensorFrame(lastline, "BH1750", 1, bh1750_labels);
	lastline = drawSensorFrame(lastline, "HYUW77", 3, hyuw77_labels);
	refresh();

	/* main loop */
	for(;;) {
		hyuw77_ok = 0;
		htu21d_ok = 0;
		bmp180_ok = 0;
		bh1750_ok = 0;
		bme280_ok = 0;
		clfd = connectServer(&spxsin);
		if (clfd >= 0) {
			memset(&hyuw77_dptr, 0, sizeof(struct hyuw77_dataptr));
			memset(&htu21d_dptr, 0, sizeof(struct htu21d_dataptr));
			memset(&bmp180_dptr, 0, sizeof(struct bmp180_dataptr));
			memset(&bh1750_dptr, 0, sizeof(struct bh1750_dataptr));
			memset(&bme280_dptr, 0, sizeof(struct bme280_dataptr));
			msglen = recv(clfd, buf, RCVBUF_SIZE, 0);
			if (msglen > 0) {
				bufptrn = buf;
				do {
					bufptr = strsep(&bufptrn, "\n");
					if (strstr(bufptr, "/radio/hyuws77th@")) {
						if (fillData_hyuw77(bufptr, &hyuw77_dptr)) {
							printField(19, COLMIN, 0, 1, "%s %s",
								   hyuw77_dptr.tempmin, hyuw77_dptr.tempunit);
							printField(19, COLNOW, 1, 1, "%s %s",
								   hyuw77_dptr.tempcur, hyuw77_dptr.tempunit);
							printField(19, COLMAX, 0, 1, "%s %s",
								   hyuw77_dptr.tempmax, hyuw77_dptr.tempunit);
							printField(21, COLMIN, 0, 1, "%s %s",
								   hyuw77_dptr.humidmin, hyuw77_dptr.humidunit);
							printField(21, COLNOW, 1, 1, "%s %s",
								   hyuw77_dptr.humidcur, hyuw77_dptr.humidunit);
							printField(21, COLMAX, 0, 1, "%s %s",
								   hyuw77_dptr.humidmax, hyuw77_dptr.humidunit);
							printField(23, COLMIN, 0, 0, "Sig: %s/%s",
								   hyuw77_dptr.sigcur, hyuw77_dptr.sigmax);
							printField(23, COLNOW, 0, 0, "Temp: %s",
								   hyuw77_dptr.temptrnd);
							printField(23, COLMAX, 0, 0, "BatLow: %s",
								   hyuw77_dptr.batlow);
							hyuw77_ok = 1;
						}
					} else if (strstr(bufptr, "/i2c/htu21d@")) {
						if (fillData_htu21d(bufptr, &htu21d_dptr)) {
							printField(3, COLMIN, 0, 1, "%s %s",
								   htu21d_dptr.humidmin, htu21d_dptr.humidunit);
							printField(3, COLNOW, 1, 1, "%s %s",
								   htu21d_dptr.humidcur, htu21d_dptr.humidunit);
							printField(3, COLMAX, 0, 1, "%s %s",
								   htu21d_dptr.humidmax, htu21d_dptr.humidunit);
							printField(5, COLMIN, 0, 1, "%s %s",
								   htu21d_dptr.tempmin, htu21d_dptr.tempunit);
							printField(5, COLNOW, 1, 1, "%s %s",
								   htu21d_dptr.tempcur, htu21d_dptr.tempunit);
							printField(5, COLMAX, 0, 1, "%s %s",
								   htu21d_dptr.tempmax, htu21d_dptr.tempunit);
							htu21d_ok = 1;
						}
					} else if (strstr(bufptr, "/i2c/bmp180@")) {
						if (fillData_bmp180(bufptr, &bmp180_dptr)) {
							printField(7, COLMIN, 0, 1, "%s %s",
								   bmp180_dptr.pressmin, bmp180_dptr.pressunit);
							printField(7, COLNOW, 1, 1, "%s %s",
								   bmp180_dptr.presscur, bmp180_dptr.pressunit);
							printField(7, COLMAX, 0, 1, "%s %s",
								   bmp180_dptr.pressmax, bmp180_dptr.pressunit);
							printField(9, COLMIN, 0, 1, "%s %s",
								   bmp180_dptr.tempmin, bmp180_dptr.tempunit);
							printField(9, COLNOW, 1, 1, "%s %s",
								   bmp180_dptr.tempcur, bmp180_dptr.tempunit);
							printField(9, COLMAX, 0, 1, "%s %s",
								   bmp180_dptr.tempmax, bmp180_dptr.tempunit);
							bmp180_ok = 1;
						}
					} else if (strstr(bufptr, "/i2c/bme280@")) {
						if (fillData_bme280(bufptr, &bme280_dptr)) {
							printField(11, COLMIN, 0, 1, "%s %s",
								   bme280_dptr.pressmin, bme280_dptr.pressunit);
							printField(11, COLNOW, 1, 1, "%s %s",
								   bme280_dptr.presscur, bme280_dptr.pressunit);
							printField(11, COLMAX, 0, 1, "%s %s",
								   bme280_dptr.pressmax, bme280_dptr.pressunit);
							printField(13, COLMIN, 0, 1, "%s %s",
								   bme280_dptr.tempmin, bme280_dptr.tempunit);
							printField(13, COLNOW, 1, 1, "%s %s",
								   bme280_dptr.tempcur, bme280_dptr.tempunit);
							printField(13, COLMAX, 0, 1, "%s %s",
								   bme280_dptr.tempmax, bme280_dptr.tempunit);
							printField(15, COLMIN, 0, 1, "%s %s",
								   bme280_dptr.humidmin, bme280_dptr.humidunit);
							printField(15, COLNOW, 1, 1, "%s %s",
								   bme280_dptr.humidcur, bme280_dptr.humidunit);
							printField(15, COLMAX, 0, 1, "%s %s",
								   bme280_dptr.humidmax, bme280_dptr.humidunit);
							bme280_ok = 1;
						}
					} else if (strstr(bufptr, "/i2c/bh1750@")) {
						if (fillData_bh1750(bufptr, &bh1750_dptr)) {
							printField(17, COLMIN, 0, 1, "%s %s",
								   bh1750_dptr.lightmin, bh1750_dptr.lightunit);
							printField(17, COLNOW, 1, 1, "%s %s",
								   bh1750_dptr.lightcur, bh1750_dptr.lightunit);
							printField(17, COLMAX, 0, 1, "%s %s",
								   bh1750_dptr.lightmax, bh1750_dptr.lightunit);
							bh1750_ok = 1;
						}
					}
				} while (bufptrn != NULL && bufptrn - buf <= msglen);
			}
			move(lastline + 1, 0);
			refresh();
			close(clfd); /* should be closed by remote end by now */
		}

		if (!hyuw77_ok) {
			mvaddstr(19, COLMIN, spcbuf);
			mvaddstr(19, COLNOW, spcbuf);
			mvaddstr(19, COLMAX, spcbuf);
			mvaddstr(21, COLMIN, spcbuf);
			mvaddstr(21, COLNOW, spcbuf);
			mvaddstr(21, COLMAX, spcbuf);
			mvaddstr(23, COLMIN, spcbuf);
			mvaddstr(23, COLNOW, spcbuf);
			mvaddstr(23, COLMAX, spcbuf);
		}

		if (!htu21d_ok) {
			mvaddstr(3, COLMIN, spcbuf);
			mvaddstr(3, COLNOW, spcbuf);
			mvaddstr(3, COLMAX, spcbuf);
			mvaddstr(5, COLMIN, spcbuf);
			mvaddstr(5, COLNOW, spcbuf);
			mvaddstr(5, COLMAX, spcbuf);
		}

		if (!bmp180_ok) {
			mvaddstr(7, COLMIN, spcbuf);
			mvaddstr(7, COLNOW, spcbuf);
			mvaddstr(7, COLMAX, spcbuf);
			mvaddstr(9, COLMIN, spcbuf);
			mvaddstr(9, COLNOW, spcbuf);
			mvaddstr(9, COLMAX, spcbuf);
		}

		if (!bme280_ok) {
			mvaddstr(11, COLMIN, spcbuf);
			mvaddstr(11, COLNOW, spcbuf);
			mvaddstr(11, COLMAX, spcbuf);
			mvaddstr(13, COLMIN, spcbuf);
			mvaddstr(13, COLNOW, spcbuf);
			mvaddstr(13, COLMAX, spcbuf);
			mvaddstr(15, COLMIN, spcbuf);
			mvaddstr(15, COLNOW, spcbuf);
			mvaddstr(15, COLMAX, spcbuf);
		}

		if (!bh1750_ok) {
			mvaddstr(17, COLMIN, spcbuf);
			mvaddstr(17, COLNOW, spcbuf);
			mvaddstr(17, COLMAX, spcbuf);
		}

		if (!hyuw77_ok || !htu21d_ok || !bmp180_ok || !bh1750_ok || !bme280_ok) {
			move(lastline + 1, 0);
			refresh();
		}

		if (cntsec)
			sleep(cntsec);
		else
			break;
	}

	endwin();
	return 0;
}
