#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <wiringPi.h>
#include <wiringPiI2C.h>

#include "htu21d_lib.h"
#include "bmp180_lib.h"

/* ***************** */
/*  LCDd connection 
/* ***************** */

#define LCDD_ADDRESS	"127.0.0.1"
#define LCDD_PORT	13666
#define LCDD_BUF_SIZE	256

/* ************** */
/* Misc constants */
/* ************** */

#define UPDATE_INTERVAL_MS	1000

/* ********* */
/* Functions */
/* ********* */

/* Send command to LCDd and read status */
void cmdLCDd(int fd, const char *cmd)
{
	int len;
	char buf[LCDD_BUF_SIZE];

	len = strlen(cmd);
	strncpy(buf, cmd, LCDD_BUF_SIZE - 2);
	buf[len] = '\n';
	buf[++len] = 0;
	printf(">> %s", buf);
	write(fd, cmd, len);
	memset(buf, 0, sizeof(buf));
	read(fd, buf, sizeof(buf));
	printf("<< %s", buf);
}

/* ******** */
/* * Main * */
/* ******** */

int main(void)
{
	int htu, bmp, lcddsock;
	double t, h, tmin, tmax, hmin, hmax;
	double p, t2,  pmin, pmax, t2min, t2max;
	struct sockaddr_in lcddsrv;
	char buf[LCDD_BUF_SIZE];

	/* Initialize library in BCM GPIO mode */
	wiringPiSetupGpio();

	/* Open sensors */
	htu = wiringPiI2CSetup(HTU21D_I2C_ADDR);
	if (htu < 0) {
		fprintf(stderr, "Unable to open I2C device: %s\n",
			strerror(errno));
		exit(-1);
	}

	bmp = wiringPiI2CSetup(BMP180_I2C_ADDR);
	if (bmp < 0)
	{
		fprintf(stderr, "Unable to open I2C device: %s\n",
			strerror (errno));
		exit(-1);
	}

	/* Soft reset */
	HTU21D_softReset(htu);
	BMP180_softReset(bmp);

	/* Check if BMP180 responds */
	if (!BMP180_isPresent(bmp))
	{
		fputs("Unable to find BMP180 sensor chip.\n", stderr);
		exit(-2);
	}
	BMP180_getCalibrationData(bmp);

	/* Connect to LCDd */
	lcddsock = socket(AF_INET, SOCK_STREAM, 0);
	if (lcddsock < 0) {
		fprintf(stderr, "Unable to create socket: %s\n",
			strerror(errno));
		exit(-1);
	}

	lcddsrv.sin_addr.s_addr = inet_addr(LCDD_ADDRESS);
	lcddsrv.sin_family = AF_INET;
	lcddsrv.sin_port = htons(LCDD_PORT);
	if (connect(lcddsock, (struct sockaddr *)&lcddsrv , sizeof(lcddsrv)) < 0)
	{
		fprintf(stderr, "Cannot connect to server: %s\n",
			strerror(errno));
		exit(-1);
	}
	
	cmdLCDd(lcddsock, "hello");
	cmdLCDd(lcddsock, "client_set name ENV_I2C");
	/* Add LCDd screens for HTU21D: Temperature and Humidity */
	cmdLCDd(lcddsock, "screen_add htu21d_temp");
	cmdLCDd(lcddsock, "widget_add htu21d_temp temp_title title");
	cmdLCDd(lcddsock, "widget_set htu21d_temp temp_title HTU21D.Temp");
	cmdLCDd(lcddsock, "widget_add htu21d_temp temp_cur string");
	cmdLCDd(lcddsock, "widget_add htu21d_temp temp_label_min_max string");
	cmdLCDd(lcddsock, "widget_set htu21d_temp temp_label_min_max 1 3 \" Min            Max \"");
	cmdLCDd(lcddsock, "widget_add htu21d_temp temp_value_min_max string");
	cmdLCDd(lcddsock, "screen_add htu21d_humid");
	cmdLCDd(lcddsock, "widget_add htu21d_humid humid_title title");
	cmdLCDd(lcddsock, "widget_set htu21d_humid humid_title HTU21D.Humid");
	cmdLCDd(lcddsock, "widget_add htu21d_humid humid_cur string");
	cmdLCDd(lcddsock, "widget_add htu21d_humid humid_label_min_max string");
	cmdLCDd(lcddsock, "widget_set htu21d_humid humid_label_min_max 1 3 \" Min            Max \"");
	cmdLCDd(lcddsock, "widget_add htu21d_humid humid_value_min_max string");
	/* Add LCDd screens for BMP180: Pressure and Temperature */
	cmdLCDd(lcddsock, "screen_add bmp180_press");
	cmdLCDd(lcddsock, "widget_add bmp180_press press_title title");
	cmdLCDd(lcddsock, "widget_set bmp180_press press_title BMP180.Press");
	cmdLCDd(lcddsock, "widget_add bmp180_press press_cur string");
	cmdLCDd(lcddsock, "widget_add bmp180_press press_label_min_max string");
	cmdLCDd(lcddsock, "widget_set bmp180_press press_label_min_max 1 3 \" Min            Max \"");
	cmdLCDd(lcddsock, "widget_add bmp180_press press_value_min_max string");
	cmdLCDd(lcddsock, "screen_add bmp180_temp");
	cmdLCDd(lcddsock, "widget_add bmp180_temp temp2_title title");
	cmdLCDd(lcddsock, "widget_set bmp180_temp temp2_title BMP180.Temp");
	cmdLCDd(lcddsock, "widget_add bmp180_temp temp2_cur string");
	cmdLCDd(lcddsock, "widget_add bmp180_temp temp2_label_min_max string");
	cmdLCDd(lcddsock, "widget_set bmp180_temp temp2_label_min_max 1 3 \" Min            Max \"");
	cmdLCDd(lcddsock, "widget_add bmp180_temp temp2_value_min_max string");

	/* Initial min/max values to overwrite */
	tmin = 9999;
	tmax = -9999;
	hmin = 100;
	hmax = 0;
	pmin = 9999;
	pmax = 0;
	t2min = 9999;
	t2max = -9999;


	/* Main loop */
	for(;;) {
		memset(buf, 0, sizeof(buf));

		t = HTU21D_getTemperature(htu);
		if (t > tmax) tmax = t;
		if (t < tmin) tmin = t;
		snprintf(buf, LCDD_BUF_SIZE, "widget_set htu21d_temp temp_cur 1 2 \"Temperature  %+4.1lf\260C\"", t);
		cmdLCDd(lcddsock, buf);
		snprintf(buf, LCDD_BUF_SIZE, "widget_set htu21d_temp temp_value_min_max 1 4 \"%+4.1lf\260C      %+4.1lf\260C\"", tmin, tmax);
		cmdLCDd(lcddsock, buf);

		h = HTU21D_getHumidity(htu);
		if (h > hmax) hmax = h;
		if (h < hmin) hmin = h;
		snprintf(buf, LCDD_BUF_SIZE, "widget_set htu21d_humid humid_cur 1 2 \"Humidity       %4.1lf%\"", h);
		cmdLCDd(lcddsock, buf);
		snprintf(buf, LCDD_BUF_SIZE, "widget_set htu21d_humid humid_value_min_max 1 4 \"%4.1lf%%          %4.1lf%%\"", hmin, hmax);
		cmdLCDd(lcddsock, buf);

		p = BMP180_getPressureFP(bmp, BMP180_OSS_MODE_UHR, &t2);
		if (p > pmax) pmax = p;
		if (p < pmin) pmin = p;
		if (t2 > t2max) t2max = t2;
		if (t2 < t2min) t2min = t2;
		snprintf(buf, LCDD_BUF_SIZE, "widget_set bmp180_press press_cur 1 2 \"Pressure   %5.1lfhPa\"", p);
		cmdLCDd(lcddsock, buf);
		snprintf(buf, LCDD_BUF_SIZE, "widget_set bmp180_press press_value_min_max 1 4 \"%5.1lfhPa  %5.1lfhPa\"", pmin, pmax);
		cmdLCDd(lcddsock, buf);
		snprintf(buf, LCDD_BUF_SIZE, "widget_set bmp180_temp temp2_cur 1 2 \"Temperature  %+4.1lf\260C\"", t2);
		cmdLCDd(lcddsock, buf);
		snprintf(buf, LCDD_BUF_SIZE, "widget_set bmp180_temp temp2_value_min_max 1 4 \"%+4.1lf\260C      %+4.1lf\260C\"", t2min, t2max);
		cmdLCDd(lcddsock, buf);

		delay(UPDATE_INTERVAL_MS);
	}
	return 0;
}

