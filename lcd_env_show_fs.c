#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ncurses.h>

#include <wiringPi.h>
#include <wiringPiI2C.h>
#include <lcd.h>

/* ************** */
/* Misc constants */
/* ************** */

#define UPDATE_INTERVAL_MS		1000
#define TEMP_SHOW_INTERVALS		  12
#define CLOCK_SHOW_INTERVALS		   8
#define SCREEN_SHOWS_TEMP		   0
#define SCREEN_SHOWS_CLOCK		   1
#define BORDER_CLR_ID			   1
#define SCREEN_CLR_ID			   2

/* *************************** */
/* HTU21D Sensor configuration */
/* *************************** */

/* I2C bus address */
#define	HTU21D_I2C_ADDR	0x40

/* Registers (No Hold master) */
#define	HTU21D_TEMP_NH			0xf3
#define	HTU21D_HUMID_NH			0xf5
#define	HTU21D_SOFT_RST			0xfe

/* Max measuring times - see page 3, 5 and 12 of the datasheet */
#define	HTU21D_TEMP_MAX_TIME_MS		50
#define	HTU21D_HUMID_MAX_TIME_MS	16
#define	HTU21D_SOFT_RST_MAX_TIME_MS	15

#define HTU21D_EXTRA_DELAY_MS		10
#define HTU21D_TEMP_WAIT_MS		\
		( HTU21D_TEMP_MAX_TIME_MS + HTU21D_EXTRA_DELAY_MS )
#define HTU21D_HUMID_WAIT_MS		\
		( HTU21D_HUMID_MAX_TIME_MS + HTU21D_EXTRA_DELAY_MS )
#define HTU21D_RESET_WAIT_MS		\
		( HTU21D_SOFT_RST_MAX_TIME_MS + HTU21D_EXTRA_DELAY_MS )

/* ************************* */
/* HD44780 LCD configuration */
/* ************************* */

/* Display dimensions in chars */
#define	LCD_ROWS	 4
#define	LCD_COLS	20

/* Communication bits: 4 or 8 */
#define LCD_COMM_BITS	 4

/* RPi assigned GPIO pins */
#define LCD_RS_GPIO	12
#define LCD_E_GPIO	16
#define LCD_D0_GPIO	 5
#define LCD_D1_GPIO	 6
#define LCD_D2_GPIO	13
#define LCD_D3_GPIO	19

/* **************** */
/* Global variables */
/* **************** */

char scr[LCD_ROWS][LCD_COLS + 1];
int xw, yw;

/* ********* */
/* Functions */
/* ********* */

/* Soft reset */
void HTU21D_softReset(int fd)
{
        wiringPiI2CWrite(fd, HTU21D_SOFT_RST);
        delay(HTU21D_RESET_WAIT_MS);
}

/* Get temperature */
double getTemperature(int fd)
{
        unsigned char buf[4];
        wiringPiI2CWrite(fd, HTU21D_TEMP_NH);
        delay(HTU21D_TEMP_WAIT_MS);
        read(fd, buf, 3);
        unsigned int temp = (buf [0] << 8 | buf [1]) & 0xFFFC;
        /* Convert sensor reading into temperature.
           See page 14 of the datasheet */
        double tSensorTemp = temp / 65536.0;
        return -46.85 + (175.72 * tSensorTemp);
}

/* Get humidity */
double getHumidity(int fd)
{
        unsigned char buf[4];
        wiringPiI2CWrite(fd, HTU21D_HUMID_NH);
        delay(HTU21D_HUMID_WAIT_MS);
        read(fd, buf, 3);
        unsigned int humid = (buf [0] << 8 | buf [1]) & 0xFFFC;
        /* Convert sensor reading into humidity.
           See page 15 of the datasheet */
        double tSensorHumid = humid / 65536.0;
        return -6.0 + (125.0 * tSensorHumid);
}

/* Fill remaining screen with spaces */
void fillSpaces(void)
{
        int i, j, l;

	for(i = 0; i < LCD_ROWS; i++) {
		l = strlen(scr[i]);
		for(j = l; j < LCD_COLS; j++) scr[i][j] = ' ';
	}
}


/* Init ncurses */
void initCurses(void)
{
	int xs, ys;

	initscr();
	cbreak();
	noecho();
	curs_set(0);
	start_color();
	init_pair(BORDER_CLR_ID, COLOR_BLACK, COLOR_GREEN);
	init_pair(SCREEN_CLR_ID, COLOR_WHITE, COLOR_BLUE);
	getmaxyx(stdscr, ys, xs);
	yw = (ys - LCD_ROWS) >> 1;
	xw = (xs - LCD_COLS) >> 1;

	/* draw frame */
	attron(COLOR_PAIR(BORDER_CLR_ID));
	mvaddch(yw, xw, '+');
	hline('-', LCD_COLS);
	mvaddch(yw, xw + LCD_COLS + 1, '+');
	mvaddch(yw + LCD_ROWS + 1, xw, '+');
	hline('-', LCD_COLS);
	mvaddch(yw + LCD_ROWS + 1, xw + LCD_COLS + 1, '+');
	mvvline(yw + 1, xw, '|', LCD_ROWS);
	mvvline(yw + 1, xw + LCD_COLS + 1, '|', LCD_ROWS);
	attroff(COLOR_PAIR(BORDER_CLR_ID));
	refresh();
}

/* Show on terminal */
void displayOnTerm(void)
{
	int i;

	printf("+");
	for(i = 0; i < LCD_COLS; i++) printf("-");
	printf("+\n");
	for(i = 0; i < LCD_ROWS; i++) printf("|%s|\n", scr[i]);
	printf("+");
	for(i = 0; i < LCD_COLS; i++) printf("-");
	printf("+\n");
}

/* Show ncurses fullscreen (best use with screen) */
void displayCurses(void)
{
	int i;

	attron(COLOR_PAIR(SCREEN_CLR_ID));

	for(i = 0; i < LCD_ROWS; i++)
		mvprintw(yw + 1 + i, xw + 1, "%s", scr[i]);

	attroff(COLOR_PAIR(SCREEN_CLR_ID));
	refresh();
}

/* Display screen on LCD */
void displayScreen(int fd)
{
	int i;

	for(i = 0; i < LCD_ROWS; i++) {
		lcdPosition(fd, 0, i);
		lcdPuts(fd, scr[i]);
	}
}

void printTempHumid(double t, double tmin, double tmax, double h, double hmin,
		    double hmax)
{
	char buf[LCD_COLS + 1];

	buf[LCD_COLS] = 0;

	snprintf(buf, LCD_COLS + 1, "Temperature  %+4.1lf\337C", t);
	sprintf(scr[0], "%-*s", LCD_COLS, buf);
	snprintf(buf, LCD_COLS + 1, " %+4.1lf\337C .. %+4.1lf\337C ", tmin,
		 tmax);
	sprintf(scr[1], "%-*s", LCD_COLS, buf);

	snprintf(buf, LCD_COLS + 1, "Humidity      %4.1lf %%", h);
	sprintf(scr[2], "%-*s", LCD_COLS, buf);
	snprintf(buf, LCD_COLS + 1, "  %4.1lf %% ... %4.1lf %%", hmin, hmax);
	sprintf(scr[3], "%-*s", LCD_COLS, buf);
}

void printClock(void)
{
	time_t tt;
	struct tm *lt;
	char buf[LCD_COLS + 1];

	buf[LCD_COLS] = 0;

	time(&tt);
	lt = localtime(&tt);

	sprintf(scr[0], "%-*s", LCD_COLS, " ");
	snprintf(buf, LCD_COLS + 1, "     %04d-%02d-%02d", 1900 + lt->tm_year,
 		 lt->tm_mon + 1, lt->tm_mday);
	sprintf(scr[1], "%-*s", LCD_COLS, buf);
	snprintf(buf, LCD_COLS + 1, "      %02d:%02d:%02d", lt->tm_hour,
 		 lt->tm_min, lt->tm_sec);
	sprintf(scr[2], "%-*s", LCD_COLS, buf);
	sprintf(scr[3], "%-*s", LCD_COLS, " ");
}

/* ******** */
/* * Main * */
/* ******** */

int main(void)
{
	int htu, lcd;
	int i, wos, cnt;
	double t, h, tmin, tmax, hmin, hmax;

	/* Initialize library in BCM GPIO mode */
	wiringPiSetupGpio();

	/* Open sensor */
	htu = wiringPiI2CSetup(HTU21D_I2C_ADDR);
	if (htu < 0) {
		fprintf(stderr, "Unable to open I2C device: %s\n",
			strerror(errno));
		exit(-1);
	}

	/* Open display */
	lcd = lcdInit(LCD_ROWS, LCD_COLS, LCD_COMM_BITS, LCD_RS_GPIO,
		      LCD_E_GPIO, LCD_D0_GPIO, LCD_D1_GPIO, LCD_D2_GPIO,
		      LCD_D3_GPIO, 0, 0, 0, 0);
	if (lcd < 0) {
		fprintf(stderr, "Unable to open LCD device: %s\n",
			strerror(errno));
		exit(-1);
	}

	/* Init ncurses */
	initCurses();

	/* Soft reset, device starts in 12-bit humidity / 14-bit temperature */
	HTU21D_softReset(htu);

	/* Clear display */
	lcdClear(lcd);
	lcdHome(lcd);

	/* Put NULL char at the end of each row */
	for(i = 0; i < LCD_ROWS; i++) scr[i][LCD_COLS] = 0;

	/* Initial min/max values to overwrite */
	tmin = 9999;
	tmax = -9999;
	hmin = 100;
	hmax = 0;

	/* Begin with Temp/Humid on-screen */
	wos = SCREEN_SHOWS_TEMP;
	cnt = TEMP_SHOW_INTERVALS;

	/* Main loop */
	for(;;) {
	
		if (wos == SCREEN_SHOWS_TEMP) {	
			t = getTemperature(htu);
			if (t > tmax) tmax = t;
			if (t < tmin) tmin = t;

			h = getHumidity(htu);
			if (h > hmax) hmax = h;
			if (h < hmin) hmin = h;

			printTempHumid(t, tmin, tmax, h, hmin, hmax);

			if (! --cnt) {
				wos = SCREEN_SHOWS_CLOCK;
				cnt = CLOCK_SHOW_INTERVALS;
			}
		}
		else if (wos == SCREEN_SHOWS_CLOCK) {
			printClock();
			if (! --cnt) {
				wos = SCREEN_SHOWS_TEMP;
				cnt = TEMP_SHOW_INTERVALS;
			}
		}
		else break;

		fillSpaces();

		displayScreen(lcd);
		/* displayOnTerm(); */
		displayCurses();

		delay(UPDATE_INTERVAL_MS);
	}

	endwin();
	return 0;
}

