/*
 * This program connects to radio433daemon and i2c bus, picks up digital sensor
 * messages and allows clients to read them in decoded form.
 *
 * Supported devices so far:
 * + Radio
 *   hyuws77th - Hyundai WS SENZOR 77TH 433.92 MHz temperature/humidity
 * + I2C
 *   htu21d - HTU21D temperature/humidity
 *   bmp180 - BMP180 temperature/pressure
 *   bh1750 - BH1750 light sensor
 *
 *  Message format:
 *  /BUS/DEVICE/PROPERTY=VALUE
 *
 *  NOTE: min and max values are always calculated by daemon not devices!
 *
 *  For radio (wireless 433 MHz) sensors:
 *
 *  /radio/hyuws77th@CHANNEL,SYSID:DEVID/PROPERTY
 *  where property can be:
 *    timestamp - timestamp as seconds.milliseconds
 *    interval - transmission interval in ms
 *    code - raw code in hex
 *    batlow - battery low indicator
 *    signal/{cur,max} - number of packets total vs received
 *    temp/{cur,min,max,unit} - temperature in Celsius, signed integer (with +/-)
 *    temp/trend - temperature trend calculated by device ('\', '_', '/')
 *    humid/{cur,min,max,unit} - air humidity in percent, integer [0-100]
 *    index - index in table
 *
 *  For I2C (locally connected) sensors:
 *
 *  /i2c/DEVICE@I2C_BUS,I2C_ID/PROPERTY
 *  where property can be:
 *    timestamp - timestamp as seconds.milliseconds
 *    interval - probe interval in ms
 *    {temp,humid,press,light}/{cur,min,max,unit} - values
 *    index - index in table
 *
 *  NOTE 0: device section ALWAYS starts with 'timestamp' value
 *  NOTE 1: device section ALWAYS ends with 'index' attribute
 *          that may serve as a marker that sensor info is complete
 *
 *  Execution flow:
 *  + main thread accepts clients, immediately sends info and closes socket
 *  + optional client_thread listens to radio server and updates sensor list
 *  + optional i2c_thread reads from I2C sensors and updates sensor list
 *
 *  Sensor list utilizes lazy timing and housekeeping, it means that obsolete
 *  entries are removed only during access (updates and sending to clients)
 */

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
#include <sys/stat.h>
#include <sys/socket.h>
#include <sched.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <linux/limits.h>
#include <fcntl.h>
#include <linux/limits.h>  /* for NGROUPS_MAX */
#include <pwd.h>        /* for getpwnam_r() */
#include <grp.h>        /* for getgrouplist() and setgroups() */


#include <wiringPi.h>
#include <wiringPiI2C.h>

#include "radio433_dev.h"
#include "htu21d_lib.h"
#include "bmp180_lib.h"
#include "bh1750_lib.h"

/* *************** */
/* *  Constants  * */
/* *************** */

#define BANNER			"SensorProxy v0.98.5 (radio+i2c) server"
#define MAX_USERNAME		32
#define MAX_NGROUPS		(NGROUPS_MAX >> 10)	/* reasonable maximum */
#define RADIO_PORT		5433	/* default radio433daemon TCP port */
#define SERVER_PORT		5444
#define SERVER_ADDR		"0.0.0.0"
#define RPI3I2C_BUS		1
#define RADMSG_SIZE		128	/* radio daemon message size */
#define RADBUF_SIZE		(RADMSG_SIZE * 8)
#define BUFFER_SIZE		4096	/* output buffer size */
#define MAX_CLNT_QUEUE		16	/* client backlog limit */
#define RECONNECT_DELAY_SEC	15
#define MAX_SENSORS		256
#define SENSOR_LABEL		16
#define BUS_LABEL		8
#define UNIT_LABEL		4
#define SENSOR_ENTRY_TTL	4	/* failed communication limit */
#define RADMSG_HDR		"<RX>"
#define RADMSG_EOT		"<ZZ>"
#define TXTMSG_HDR		"BEGIN"
#define TXTMSG_EOT		"END"
#define FILE_UMASK		(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)
#define PID_DIR			"/var/run/"
#define LOG_DEBUG		"debug"
#define LOG_INFO		"info"
#define LOG_NOTICE		"notice"
#define LOG_WARN		"warn"
#define LOG_ERROR		"error"

#define TSDIFF(s0, m0, s1, m1)	(((s0) - (s1)) * 1000 + (m0) - (m1))
#define MIN(x, y)		((x) < (y) ? (x) : (y))
#define MAX(x, y)		((x) > (y) ? (x) : (y))

/* Labels */
#define SL_HYUWSSENZOR77TH	"hyuws77th"
#define SL_HTU21D		"htu21d"
#define SL_BMP180		"bmp180"
#define SL_BH1750		"bh1750"

/* Sensor bus */
#define SB_NULL			0
#define SB_RADIO		1
#define SB_I2C			2

/* I2C sensor type (address is not unique, see BMP180 and BME280) */
#define ST_I2C_NULL		0
#define ST_I2C_HTU21D		1
#define ST_I2C_BMP180		2
#define ST_I2C_BH1750		3

/* ********************** */
/* *  Global variables  * */
/* ********************** */

const char *busname[] = { "(null)", "radio", "i2c" };
const char trend[3] = { '_', '/', '\\' };
extern char *optarg;
extern int optind, opterr, optopt;
volatile int debugflag, i2cdelay, netclrun, i2ctrun;
volatile int mrstflag, rdelflag;
pthread_t netcltthread, i2cthread;
pid_t procpid;
char progname[PATH_MAX + 1], logfname[PATH_MAX + 1], pidfname[PATH_MAX + 1];
volatile int logfd, srvfd, radfd; /* files and sockets */
sem_t sensem;		/* semaphore for sensor list updates */
sem_t logsem;		/* log file semaphore */
struct sockaddr_in radsin;

struct radmsg {
	time_t tsec;
	unsigned int tmsec;
	int codelen, repeats, interval;
	int type, bits;
	unsigned long long code;
};

struct sensorentry {
	time_t tsec;
	unsigned int tmsec;
	int interval;			/* interval between readings in ms */
	int bus, type;
	char label[SENSOR_LABEL];
	void *data;			/* pointer to model-specific struct */
} senstbl[MAX_SENSORS];

struct fvalentry {
	double cur, min, max;
	char unit[UNIT_LABEL];
};

struct ivalentry {
	int cur, min, max;
	char unit[UNIT_LABEL];
};

struct radioentry {
	unsigned long long code;	/* raw code for quick comparisons */
	int ch, sysid, devid;
	int sigcur, sigmax;
};

struct i2centry {
	int bus, id;
	int fd;
};

struct datahyuws77th {
	struct radioentry radio;
	struct fvalentry temp;
	struct ivalentry humid;
	int trend, batlow;
};

struct datahtu21d {
	struct i2centry i2c;
	struct fvalentry humid, temp;
};

struct databmp180 {
	struct i2centry i2c;
	struct fvalentry press, temp;
};

struct databh1750 {
	struct i2centry i2c;
	struct fvalentry light;
};

/* *************** */
/* *  Functions  * */
/* *************** */

/* Show help */
void help(void)
{
	printf("Usage:\n\t%s [-V] [-i i2cint] [-u username] [-d | -l logfile] [-P pidfile] [-r radioip [-t radioport]] [-h address] [-p tcpport]\n\n", progname);
	puts("Where:");
	puts("\t-i i2cint     - read I2C sensors with specified interval in seconds (optional, default is skip)");
	puts("\t-u username   - name of the user to switch to (optional, valid only if run by root)");
	puts("\t-d            - debug mode, stay foreground and show activity (optional)");
	puts("\t-l logfile    - path to log file (optional, default is none)");
	printf("\t-P pidfile    - path to PID file (optional, default is %s%s.pid)\n", PID_DIR, progname);
	printf("\t-r radioip    - IPv4 address of radio server (optional)\n");
	printf("\t-t radioport  - TCP port of radio server (optional, default is %d)\n", RADIO_PORT);
	printf("\t-h address    - IPv4 address to listen on (optional, default %s)\n", SERVER_ADDR);
	printf("\t-p tcpport    - TCP port to listen on (optional, default is %d)\n", SERVER_PORT);
	puts("\t-V            - show version and exit");
	puts("\nSupported source devices:");
	puts("\thyuws77th (radio) - temperature/humidity 433.92 MHz radio sensor Hyundai WS Senzor 77TH");
	puts("\thtu21d      (i2c) - humidity/temperature local sensor HTU21D");
	puts("\tbmp180      (i2c) - pressure/temperature local sensor BMP180");
	puts("\tbh1750      (i2c) - light level local sensor BH1750");
	puts("\nSignal actions: SIGHUP (log file truncate and reopen)");
	puts("                SIGUSR1 (reset min and max values for all sensors)");
	puts("                SIGUSR2 (delete all radio sensors, initiate re-discover)\n");
}

/* Show version */
void verShow(void)
{
#ifdef BUILDSTAMP
	printf("%s build %s\n", BANNER, BUILDSTAMP);
#else
	printf("%s\n", BANNER);
#endif
}

/* Log line header */
void logprintf(int fd, const char *level, const char *fmt, ...)
{
        struct timeval ts;
        struct tm *tl;
	va_list ap;

	if (fd < 0)
		return;
	sem_wait(&logsem);
	gettimeofday(&ts, NULL);
	tl = localtime(&ts.tv_sec);
	dprintf(fd, "%d-%02d-%02d %02d:%02d:%02d.%03u %s[%d] %s: ",
		1900 + tl->tm_year, tl->tm_mon + 1, tl->tm_mday,
		tl->tm_hour, tl->tm_min, tl->tm_sec, ts.tv_usec / 1000,
		progname, procpid, level);
	va_start(ap, fmt);
	vdprintf(fd, fmt, ap);
	va_end(ap);
	sem_post(&logsem);
}

/* Connect/reconnect to radio daemon */
/* (this function loops indefinitely until connection is created
 *  and end process when no sockets are available)
 */
int connectRadioSrv(struct sockaddr_in *s)
{
	int fd;

	for(;;) {
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd == -1)
			break;	/* no socket available */

		if (connect(fd, (struct sockaddr *)s, sizeof(*s)) == -1) {
			logprintf(logfd, LOG_WARN,
				  "connection broken with radio server %s port %d: %s\n",
				  inet_ntoa(s->sin_addr), ntohs(s->sin_port),
				  strerror(errno));
			logprintf(logfd, LOG_WARN, "retrying in %d seconds...\n",
				  RECONNECT_DELAY_SEC);
			close(fd);
			sleep(RECONNECT_DELAY_SEC);
		} else {
			logprintf(logfd, LOG_NOTICE,
				  "connected successfully to radio server %s port %d\n",
				  inet_ntoa(s->sin_addr), ntohs(s->sin_port));
			break;
		}
	}

	return fd;
}

/* Daemonize process */
int daemonize(void)
{
	pid_t pid, sid;

	/* fork then parent exits */
	pid = fork();
	if (pid < 0)
		return 1;
	if (pid > 0)
		exit(EXIT_SUCCESS);

	/* set default umask, will be overriden by open anyway */
	umask(0);

	/* create new session ID */
	sid = setsid();
	if (sid < 0)
		return 2;

	/* change current working dir to root */
	if (chdir("/") < 0)
		return 3;

	/* close standard file descriptors */
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	return 0;
}

/* Process shutdown */
void endProcess(int status)
{
	int i;
	struct sensorentry *s;
	struct i2centry *c;

	unlink(pidfname);
	if (netclrun)
		pthread_cancel(netcltthread);
	if (i2ctrun) {
		pthread_cancel(i2cthread);
		/* we do not use semaphore here, as both threads
		 * should finish by now, and furthermore one of
		 * them can block semaphore before being interrupted */
		for(i = 0; i < MAX_SENSORS; i++) {
			s = &senstbl[i];
			if (s->bus == SB_I2C) {
				c = (struct i2centry *)(s->data);
				if (c->id == HTU21D_I2C_ADDR &&
				    c->fd >= 0) {
					HTU21D_softReset(c->fd);
					close(c->fd);
				} else if (c->id == BMP180_I2C_ADDR &&
					   c->fd >= 0) {
					BMP180_softReset(c->fd);
					close(c->fd);
				} else if (c->id == BH1750_I2C_ADDR &&
					   c->fd >= 0) {
					BH1750_softReset(c->fd);
					BH1750_powerDown(c->fd);
					close(c->fd);
				}
			}
		}
	}
	if (srvfd >= 0)
		close(srvfd);
	if (radfd >= 0)
		close(radfd);
	if (logfd >= 0) {
		logprintf(logfd, LOG_NOTICE, "server shut down with code %d\n",
			  status);
		close(logfd);
	}
	exit(status);
}

/* Intercept TERM and INT signals */
void signalQuit(int sig)
{
	logprintf(logfd, LOG_NOTICE, "signal %d received\n", sig);
	endProcess(0);
}

/* SIGHUP - reopen log file, useful for logrotate */
void signalReopenLog(int sig)
{
	if (!debugflag && logfd >= 0) {
		logprintf(logfd, LOG_NOTICE, "signal %d received\n", sig);
		logprintf(logfd, LOG_NOTICE, "closing log file\n");
		fsync(logfd);	/* never hurts */
		close(logfd);
		logfd = open(logfname, O_CREAT | O_TRUNC | O_SYNC | O_WRONLY, FILE_UMASK);
		if (logfd == -1)
			endProcess(EXIT_FAILURE);
		logprintf(logfd, LOG_NOTICE, "log file flushed after receiving signal %d\n", sig);
	}
}

/* SIGUSR1 - signal to reset min and max values before client transmission */
void signalRstMinMax(int sig)
{
	logprintf(logfd, LOG_NOTICE, "signal %d received\n", sig);
	logprintf(logfd, LOG_NOTICE, "min and max values will be reset with next read\n", sig);
	mrstflag = 1;
}

/* SIGUSR2 - signal to remove all radio sensors before client transmission */
void signalDelRadio(int sig)
{
	logprintf(logfd, LOG_NOTICE, "signal %d received\n", sig);
	logprintf(logfd, LOG_NOTICE, "all radio sensors will be deleted with next read\n", sig);
	rdelflag = 1;
}

/* drop super-user privileges */
int dropRootPriv(const char *username, uid_t *uid, gid_t *gid)
{
	struct passwd pw, *pwptr;
	char buf[1024];
	gid_t grps[MAX_NGROUPS];
	int ngrp;

	/* get user & group ID */
	errno = 0;
	pwptr = NULL;
	if (getpwnam_r(username, &pw, buf, 1024, &pwptr))
		return -1;
	if (pwptr == NULL) {
		errno = EUSERS;
		return -1;
        }
	/* set supplemental groups */
	ngrp = MAX_NGROUPS;
	if (getgrouplist(username, pw.pw_gid, grps, &ngrp) < 0)
		return -1;
	if (setgroups(ngrp, grps))
		return -1;
	/* start with GID then UID */
	if (setresgid(pw.pw_gid, pw.pw_gid, pw.pw_gid))
		return -1;
	if (setresuid(pw.pw_uid, pw.pw_uid, pw.pw_uid))
		return -1;
	*uid = pw.pw_uid;
	*gid = pw.pw_gid;
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

/* Format network message */
int formatMessage(char *buf, size_t len)
{
	int i;
	char dbuf[SENSOR_LABEL + BUS_LABEL + 32];
	char *bufptr;
	struct sensorentry *s;
	struct radioentry *r;
	struct i2centry *c;
	struct datahyuws77th *dhs;
	struct datahtu21d *dht2;
	struct databmp180 *dbm1;
	struct databh1750 *dbh1;

        memset(buf, 0, len);
	bufptr = buf;
	sem_wait(&sensem);
#ifdef BUILDSTAMP
	bufptr += sprintf(bufptr, "#INFO: %s build %s\n", BANNER, BUILDSTAMP);
#else
	bufptr += sprintf(bufptr, "#INFO: %s\n", BANNER);
#endif
	bufptr += sprintf(bufptr, "#%s\n", TXTMSG_HDR);
	for(i = 0; i < MAX_SENSORS; i++) {
		s = &senstbl[i];
		if (s->bus == SB_RADIO) {
			r = (struct radioentry *)(s->data); /* radio begins every struct here */
			sprintf(dbuf, "/%s/%s@%d,%02X:%02X", busname[SB_RADIO],
				s->label, r->ch, r->sysid, r->devid);
			bufptr += sprintf(bufptr, "%s/timestamp=%lu.%03u\n",
					  dbuf, s->tsec, s->tmsec);
			bufptr += sprintf(bufptr, "%s/interval=%d\n", dbuf, s->interval);
			bufptr += sprintf(bufptr, "%s/code=0x%016llX\n", dbuf, r->code);
			bufptr += sprintf(bufptr, "%s/signal/cur=%d\n", dbuf, r->sigcur);
			bufptr += sprintf(bufptr, "%s/signal/max=%d\n", dbuf, r->sigmax);
			if (s->type == RADIO433_DEVICE_HYUWSSENZOR77TH) {
				dhs = (struct datahyuws77th *)(s->data);
				bufptr += sprintf(bufptr, "%s/batlow=%d\n", dbuf, dhs->batlow);
				bufptr += sprintf(bufptr, "%s/temp/min=%+.1lf\n", dbuf, dhs->temp.min);
				bufptr += sprintf(bufptr, "%s/temp/cur=%+.1lf\n", dbuf, dhs->temp.cur);
				bufptr += sprintf(bufptr, "%s/temp/max=%+.1lf\n", dbuf, dhs->temp.max);
				bufptr += sprintf(bufptr, "%s/temp/unit=%s\n", dbuf, dhs->temp.unit);
				bufptr += sprintf(bufptr, "%s/temp/trend=%c\n", dbuf, trend[dhs->trend]);
				bufptr += sprintf(bufptr, "%s/humid/min=%d\n", dbuf, dhs->humid.min);
				bufptr += sprintf(bufptr, "%s/humid/cur=%d\n", dbuf, dhs->humid.cur);
				bufptr += sprintf(bufptr, "%s/humid/max=%d\n", dbuf, dhs->humid.max);
				bufptr += sprintf(bufptr, "%s/humid/unit=%s\n", dbuf, dhs->humid.unit);
				bufptr += sprintf(bufptr, "%s/index=%d\n", dbuf, i);
			} else
				continue;
		} else if (s->bus == SB_I2C) {
			c = (struct i2centry *)(s->data); /* i2c info begins every struct here */
			sprintf(dbuf, "/%s/%s@%d,%02X", busname[SB_I2C],
				s->label, c->bus, c->id);
			bufptr += sprintf(bufptr, "%s/timestamp=%lu.%03u\n",
					  dbuf, s->tsec, s->tmsec);
			bufptr += sprintf(bufptr, "%s/interval=%d\n", dbuf, s->interval);
			if (s->type == ST_I2C_HTU21D) {
				dht2 = (struct datahtu21d *)(s->data);
				bufptr += sprintf(bufptr, "%s/humid/min=%.1lf\n", dbuf, dht2->humid.min);
				bufptr += sprintf(bufptr, "%s/humid/cur=%.1lf\n", dbuf, dht2->humid.cur);
				bufptr += sprintf(bufptr, "%s/humid/max=%.1lf\n", dbuf, dht2->humid.max);
				bufptr += sprintf(bufptr, "%s/humid/unit=%s\n", dbuf, dht2->humid.unit);
				bufptr += sprintf(bufptr, "%s/temp/min=%+.1lf\n", dbuf, dht2->temp.min);
				bufptr += sprintf(bufptr, "%s/temp/cur=%+.1lf\n", dbuf, dht2->temp.cur);
				bufptr += sprintf(bufptr, "%s/temp/max=%+.1lf\n", dbuf, dht2->temp.max);
				bufptr += sprintf(bufptr, "%s/temp/unit=%s\n", dbuf, dht2->temp.unit);
			} else if (s->type == ST_I2C_BMP180) {
				dbm1 = (struct databmp180 *)(s->data);
				bufptr += sprintf(bufptr, "%s/press/min=%.1lf\n", dbuf, dbm1->press.min);
				bufptr += sprintf(bufptr, "%s/press/cur=%.1lf\n", dbuf, dbm1->press.cur);
				bufptr += sprintf(bufptr, "%s/press/max=%.1lf\n", dbuf, dbm1->press.max);
				bufptr += sprintf(bufptr, "%s/press/unit=%s\n", dbuf, dbm1->press.unit);
				bufptr += sprintf(bufptr, "%s/temp/min=%+.1lf\n", dbuf, dbm1->temp.min);
				bufptr += sprintf(bufptr, "%s/temp/cur=%+.1lf\n", dbuf, dbm1->temp.cur);
				bufptr += sprintf(bufptr, "%s/temp/max=%+.1lf\n", dbuf, dbm1->temp.max);
				bufptr += sprintf(bufptr, "%s/temp/unit=%s\n", dbuf, dbm1->temp.unit);
			} else if (s->type == ST_I2C_BH1750) {
				dbh1 = (struct databh1750 *)(s->data);
				bufptr += sprintf(bufptr, "%s/light/min=%.1lf\n", dbuf, dbh1->light.min);
				bufptr += sprintf(bufptr, "%s/light/cur=%.1lf\n", dbuf, dbh1->light.cur);
				bufptr += sprintf(bufptr, "%s/light/max=%.1lf\n", dbuf, dbh1->light.max);
				bufptr += sprintf(bufptr, "%s/light/unit=%s\n", dbuf, dbh1->light.unit);
			}
			bufptr += sprintf(bufptr, "%s/index=%d\n", dbuf, i);
		} else
			continue;
	}
	bufptr += sprintf(bufptr, "#%s\n", TXTMSG_EOT);
	sem_post(&sensem);
        return strlen(buf);
}

/* Find free slot in sensor table */
int findFreeSlot(void)
{
	int i;

	for(i = 0; i < MAX_SENSORS; i++)
		if (!senstbl[i].type)
			break;

	return i < MAX_SENSORS ? i : -1;
}

/* Remove sensor entry by index */
void sensorEntryDelete(int i)
{
	struct sensorentry *s;
	struct i2centry *c;

	if (i < 0 || i >= MAX_SENSORS)
		return;

	s = &senstbl[i];
	if (s->data) {
		if (s->bus == SB_I2C) {
			c = (struct i2centry *)(s->data);
			if (c->fd >= 0)
				close(c->fd);
		}
		free(s->data);
	}
	memset(s, 0, sizeof(struct sensorentry));
}

/* Remove all radio sensors from table */
void sensorRadioRemoveAll(void)
{
        int i;
        struct sensorentry *s;

        sem_wait(&sensem);
        for(i = 0; i < MAX_SENSORS; i++) {
                s = &senstbl[i];
                if (s->bus == SB_RADIO)
                                sensorEntryDelete(i);
        }
        sem_post(&sensem);
}

/* Remove stale sensors from table */
/* entry is expired if time difference is more
 * than SENSOR_ENTRY_TTL intervals */
void sensorTableClean(void)
{
	int i, l;
	struct timeval t;
	struct sensorentry *s;

	gettimeofday(&t, NULL);
	sem_wait(&sensem);
	for(i = 0; i < MAX_SENSORS; i++) {
		s = &senstbl[i];
		if (s->type) {
			l = TSDIFF(t.tv_sec, t.tv_usec / 1000, s->tsec, s->tmsec);
			if (l > SENSOR_ENTRY_TTL * s->interval) {
				logprintf(logfd, LOG_NOTICE,
					  "removing sensor \"%s\" [%d] due to timeout (%lu ms)\n",
					  s->label, i, l);
				sensorEntryDelete(i);
			}
		}
	}
	sem_post(&sensem);
}

/* Reset min/max for all active sensors */
void sensorResetMinMax(void)
{
        int i;
        struct sensorentry *s;
        struct datahyuws77th *dhs;
	struct datahtu21d *dht2;
	struct databmp180 *dbm1;
	struct databh1750 *dbh1;

	sem_wait(&sensem);
	for(i = 0; i < MAX_SENSORS; i++) {
		s = &senstbl[i];
		if (s->type == RADIO433_DEVICE_HYUWSSENZOR77TH) {
			dhs = (struct datahyuws77th *)(s->data);
			dhs->temp.min = dhs->temp.cur;
			dhs->temp.max = dhs->temp.cur;
			dhs->humid.min = dhs->humid.cur;
			dhs->humid.max = dhs->humid.cur;
		} else if (s->bus == SB_I2C) {
			if (s->type == ST_I2C_HTU21D) {
				dht2 = (struct datahtu21d *)(s->data);
				dht2->humid.min = dht2->humid.cur;
				dht2->humid.max = dht2->humid.cur;
				dht2->temp.min = dht2->temp.cur;
				dht2->temp.max = dht2->temp.cur;
			} else if (s->type == ST_I2C_BMP180) {
				dbm1 = (struct databmp180 *)(s->data);
				dbm1->press.min = dbm1->press.cur;
				dbm1->press.max = dbm1->press.cur;
				dbm1->temp.min = dbm1->temp.cur;
				dbm1->temp.max = dbm1->temp.cur;
			} else if (s->type == ST_I2C_BH1750) {
				dbh1 = (struct databh1750 *)(s->data);
				dbh1->light.min = dbh1->light.cur;
				dbh1->light.max = dbh1->light.cur;
			}
		}
        }
        sem_post(&sensem);
}

/* Update remote sensor in table */
void sensorRadioUpdate(struct radmsg *rm)
{
	int i;
	struct sensorentry *s;
	struct radioentry *r;
	struct datahyuws77th *dhs;
	int sysid, devid, ch, batlow, tdir, humid;
	double temp;
	int codestatus; /* 0-new, 1-update ts + data, 2-update only ts */

	if (rm->type != RADIO433_DEVICE_HYUWSSENZOR77TH)
		return;

	if (!Radio433_thmGetData(rm->code, &sysid, &devid, &ch,
				 &batlow, &tdir, &temp, &humid))
		return;

	/* first check if signal is part of multi-signal transmission */
	sem_wait(&sensem);
	codestatus = 0;
	for(i = 0; i < MAX_SENSORS; i++) {
		s = &senstbl[i];
		if (s->bus == SB_RADIO && s->type == rm->type) {
			r = (struct radioentry *)(s->data); /* radio begins every struct */
			if (r->code == rm->code)
				codestatus = 2;
			else if (r->ch == ch && r->sysid == sysid && r->devid == devid)
				codestatus = 1;
			if (codestatus > 0) {
				if (TSDIFF(rm->tsec, rm->tmsec, s->tsec, s->tmsec) <=
				    (r->sigmax - r->sigcur + 1) * rm->codelen) {
					if (r->sigcur < r->sigmax)
						r->sigcur++;
					else {
						sem_post(&sensem);
						return;
					}
				} else
					r->sigcur = 1;
				break;
			}
		}
	}

	if (!codestatus) {
		i = findFreeSlot();
		if (i < 0) {
			sem_post(&sensem);
			return;
		}
		s = &senstbl[i];
		s->interval = rm->interval;
		s->bus = SB_RADIO;
		s->type = rm->type;
		if (rm->type == RADIO433_DEVICE_HYUWSSENZOR77TH) {
			strncpy(s->label, SL_HYUWSSENZOR77TH, SENSOR_LABEL);
			s->data = malloc(sizeof(struct datahyuws77th));
			dhs = (struct datahyuws77th *)(s->data);
		} else
			return;
		dhs->radio.ch = ch;
		dhs->radio.sysid = sysid;
		dhs->radio.devid = devid;
		dhs->radio.sigcur = 1;
		dhs->radio.sigmax = rm->repeats;
		dhs->temp.min = temp;
		dhs->temp.max = temp;
		strncpy(dhs->temp.unit, "C", UNIT_LABEL);
		dhs->humid.min = humid;
		dhs->humid.max = humid;
		strncpy(dhs->humid.unit, "%", UNIT_LABEL);
		logprintf(logfd, LOG_NOTICE,
			  "radio sensor \"%s\" [%d] added to monitor\n",
			  s->label, i);
	} else {
		s = &senstbl[i];
		if (rm->type == RADIO433_DEVICE_HYUWSSENZOR77TH)
			dhs = (struct datahyuws77th *)(s->data);
	}

	s->tsec = rm->tsec;
	s->tmsec = rm->tmsec;

	if (codestatus < 2) {
		dhs->radio.code = rm->code;
		dhs->temp.cur = temp;
		dhs->temp.min = MIN(temp, dhs->temp.min);
		dhs->temp.max = MAX(temp, dhs->temp.max);
		dhs->humid.cur = humid;
		dhs->humid.min = MIN(humid, dhs->humid.min);
		dhs->humid.max = MAX(humid, dhs->humid.max);
		dhs->trend = tdir;
		dhs->batlow = batlow;
	}
	sem_post(&sensem);
}

/* ************* */
/* *  Threads  * */
/* ************* */

/* Radio daemon client thread */
void *radioDaemonThread(void *arg)
{
	char radbuf[RADBUF_SIZE + 1];
	char *msgptr, *msgend;
	int msglen;
	sigset_t blkset;
	struct radmsg rm;

	sigfillset(&blkset);
	pthread_sigmask(SIG_BLOCK, &blkset, NULL);

	/* function loop - never ends, send signal to exit */
	for(;;) {
		do
			msglen = recv(radfd, radbuf, RADBUF_SIZE, 0);
		while (msglen == -1 && errno == EINTR);
		if (msglen <= 0) {
			close(radfd);
			radfd = connectRadioSrv(&radsin);
			continue;
		}
		radbuf[msglen] = 0;
		msgptr = radbuf;
		do {
			msgptr = strstr(msgptr, RADMSG_HDR);
			if (msgptr == NULL)
				break;
			msgend = strstr(msgptr, RADMSG_EOT);
			if (msgend == NULL)
				break;
			if (sscanf(msgptr + 4, "%lu.%u;%d;%d;%d;0x%X;%d;0x%llX;",
			    &rm.tsec, &rm.tmsec, &rm.codelen, &rm.repeats,
			    &rm.interval, &rm.type, &rm.bits, &rm.code) != 8)
				continue;
			sensorTableClean();
			sensorRadioUpdate(&rm);
			msgptr = msgend + 5;
		} while (msgptr < radbuf + msglen);
		logprintf(logfd, LOG_INFO, "received update packet from server\n");
	}	/* thread main loop ends here */
}

/* I2C sensor reading thread */
void *i2cSensorThread(void *arg)
{
	sigset_t blkset;
	int i;
	struct timeval ts;
        struct sensorentry *s;
	struct i2centry *c;
	struct datahtu21d *dht2;
	struct databmp180 *dbm1;
	struct databh1750 *dbh1;
	double v0, v1;

	sigfillset(&blkset);
	pthread_sigmask(SIG_BLOCK, &blkset, NULL);

	for(;;) {
		sem_wait(&sensem);
		for(i = 0; i < MAX_SENSORS; i++) {
			s = &senstbl[i];
			if (s->bus == SB_I2C) {
				c = (struct i2centry *)(s->data);
				if (s->type == ST_I2C_HTU21D) {
					gettimeofday(&ts, NULL);
					v0 = HTU21D_getHumidity(c->fd);
					v1 = HTU21D_getTemperature(c->fd);
					if (v0 < 0.0 || v0 > 100.0 ||
					    v1 < -40.0 || v1 > 125.0)
						continue;
					s->tsec = ts.tv_sec;
					s->tmsec = ts.tv_usec / 1000;
					dht2 = (struct datahtu21d *)(s->data);
					dht2->humid.cur = v0;
					dht2->humid.min = MIN(v0, dht2->humid.min);
					dht2->humid.max = MAX(v0, dht2->humid.max);
					dht2->temp.cur = v1;
					dht2->temp.min = MIN(v1, dht2->temp.min);
					dht2->temp.max = MAX(v1, dht2->temp.max);
					if (debugflag)
						logprintf(logfd, LOG_DEBUG,
							  "sensor \"%s\" [%d] read complete (h=%.1lf, t=%.1lf)\n",
							  s->label, i, v0, v1);
				} else if (s->type == ST_I2C_BMP180) {
					gettimeofday(&ts, NULL);
					v0 = BMP180_getPressureFP(c->fd, BMP180_OSS_MODE_UHR, &v1);
					if (v0 < 300.0 || v0 > 1100.0 ||
					    v1 < -40.0 || v1 > 85.0)
						continue;
					s->tsec = ts.tv_sec;
					s->tmsec = ts.tv_usec / 1000;
					dbm1 = (struct databmp180 *)(s->data);
					dbm1->press.cur = v0;
					dbm1->press.min = MIN(v0, dbm1->press.min);
					dbm1->press.max = MAX(v0, dbm1->press.max);
					dbm1->temp.cur = v1;
					dbm1->temp.min = MIN(v1, dbm1->temp.min);
					dbm1->temp.max = MAX(v1, dbm1->temp.max);
					if (debugflag)
						logprintf(logfd, LOG_DEBUG,
							  "sensor \"%s\" [%d] read complete (p=%.1lf, t=%.1lf)\n",
							  s->label, i, v0, v1);
				} else if (s->type == ST_I2C_BH1750) {
					gettimeofday(&ts, NULL);
					v0 = BH1750_getLx(c->fd);
					if (v0 < 0.0)
						continue;
					s->tsec = ts.tv_sec;
					s->tmsec = ts.tv_usec / 1000;
					dbh1 = (struct databh1750 *)(s->data);
					dbh1->light.cur = v0;
					dbh1->light.min = MIN(v0, dbh1->light.min);
					dbh1->light.max = MAX(v0, dbh1->light.max);
					if (debugflag)
						logprintf(logfd, LOG_DEBUG,
							  "sensor \"%s\" [%d] read complete (l=%.1lf)\n",
							  s->label, i, v0);
				}
			}
		}
		sem_post(&sensem);
		sleep(i2cdelay);
	}
}

/* ************************* */
/* *  I2C sensor routines  * */
/* ************************* */

/* Sensor initialization */
int initHTU21D(int idx)
{
	int fd;
	struct timeval ts;
        struct sensorentry *s;
	struct datahtu21d *dht2;
	double h, t;

	/* Initialize device with default I2C address */
	fd = HTU21D_initPi(NULL);
	if (fd < 0)
		return -1;

	/* soft reset, device starts in 12-bit humidity / 14-bit temperature */
	HTU21D_softReset(fd);

	/* initialize structure */
	do {
		h = HTU21D_getHumidity(fd);
		t = HTU21D_getTemperature(fd);
	} while (h < 0.0 || h > 100.0 || t < -40.0 || t > 125.0);

	gettimeofday(&ts, NULL);
	s = &senstbl[idx];
	s->tsec = ts.tv_sec;
	s->tmsec = ts.tv_usec / 1000;
	s->interval = i2cdelay * 1000;
	s->bus = SB_I2C;
	s->type = ST_I2C_HTU21D;
	strncpy(s->label, SL_HTU21D, SENSOR_LABEL);
	s->data = malloc(sizeof(struct datahtu21d));
	dht2 = (struct datahtu21d *)(s->data);
	dht2->i2c.bus = RPI3I2C_BUS;
	dht2->i2c.id = HTU21D_I2C_ADDR;
	dht2->i2c.fd = fd;
	dht2->humid.cur = h;
	dht2->humid.min = h;
	dht2->humid.max = h;
	strncpy(dht2->humid.unit, "%", UNIT_LABEL);
	dht2->temp.cur = t;
	dht2->temp.min = t;
	dht2->temp.max = t;
	strncpy(dht2->temp.unit, "C", UNIT_LABEL);

	return fd;
}

int initBMP180(int idx)
{
	int fd;
	struct timeval ts;
        struct sensorentry *s;
	struct databmp180 *dbm1;
	double p, t;

	/* Initialize device with default I2C address */
	fd = BMP180_initPi(NULL);
	if (fd < 0)
		return -1;

	/* soft reset */
	BMP180_softReset(fd);

        /* check if BMP180 responds */
	if (!BMP180_isPresent(fd))
		return -2;

	BMP180_getCalibrationData(fd);

	/* initialize structure */
	p = BMP180_getPressureFP(fd, BMP180_OSS_MODE_UHR, &t);

	gettimeofday(&ts, NULL);
	s = &senstbl[idx];
	s->tsec = ts.tv_sec;
	s->tmsec = ts.tv_usec / 1000;
	s->interval = i2cdelay * 1000;
	s->bus = SB_I2C;
	s->type = ST_I2C_BMP180;
	strncpy(s->label, SL_BMP180, SENSOR_LABEL);
	s->data = malloc(sizeof(struct databmp180));
	dbm1 = (struct databmp180 *)(s->data);
	dbm1->i2c.bus = RPI3I2C_BUS;
	dbm1->i2c.id = BMP180_I2C_ADDR;
	dbm1->i2c.fd = fd;
	dbm1->press.cur = p;
	dbm1->press.min = p;
	dbm1->press.max = p;
	strncpy(dbm1->press.unit, "hPa", UNIT_LABEL);
	dbm1->temp.cur = t;
	dbm1->temp.min = t;
	dbm1->temp.max = t;
	strncpy(dbm1->temp.unit, "C", UNIT_LABEL);

	return fd;
}

int initBH1750(int idx)
{
	int fd;
	struct timeval ts;
        struct sensorentry *s;
	struct databh1750 *dbh1;
	double l;

	/* Initialize device with default I2C address */
	fd = BH1750_initPi(NULL);
	if (fd < 0)
		return -1;

	BH1750_powerOn(fd);
	BH1750_softReset(fd);
	BH1750_setMode(fd, BH1750_MODE_CONT, BH1750_MODE_RES_H2);

	/* initialize structure */
	l = BH1750_getLx(fd);

	gettimeofday(&ts, NULL);
	s = &senstbl[idx];
	s->tsec = ts.tv_sec;
	s->tmsec = ts.tv_usec / 1000;
	s->interval = i2cdelay * 1000;
	s->bus = SB_I2C;
	s->type = ST_I2C_BH1750;
	strncpy(s->label, SL_BH1750, SENSOR_LABEL);
	s->data = malloc(sizeof(struct databh1750));
	dbh1 = (struct databh1750 *)(s->data);
	dbh1->i2c.bus = RPI3I2C_BUS;
	dbh1->i2c.id = BH1750_I2C_ADDR;
	dbh1->i2c.fd = fd;
	dbh1->light.cur = l;
	dbh1->light.min = l;
	dbh1->light.max = l;
	strncpy(dbh1->light.unit, "lx", UNIT_LABEL);

	return fd;
}

/* ************ */
/* ************ */
/* **  MAIN  ** */
/* ************ */
/* ************ */

int main(int argc, char *argv[])
{
	char msgbuf[BUFFER_SIZE + 1];
	int opt;
	int srvport, radport;
	struct sockaddr_in srvsin, clin;
	int pidfd;
	struct sigaction sa;
	int radflag, idx, ena;
	int clfd, clen, len;
	uid_t uid;
	gid_t gid;
	char username[MAX_USERNAME + 1];

	/* get process name */
	strncpy(progname, basename(argv[0]), PATH_MAX);

	/* set PID initially so logprintf() can use it, */
	/* the value will change after daemon forking */
	procpid = getpid();

	/* initialize other values */
	logfd = -1;
	srvfd = -1;
	radfd = -1;
	netclrun = 0;
	i2ctrun = 0;
	mrstflag = 0;
	rdelflag = 0;

	/* get parameters */
	debugflag = 0;
	radflag = 0;
	i2cdelay = 0;
	memset(username, 0, MAX_USERNAME + 1);
	memset((char *)&srvsin, 0, sizeof(srvsin));
	srvsin.sin_family = AF_INET;
	inet_aton(SERVER_ADDR, &srvsin.sin_addr);
	srvport = SERVER_PORT;
	memset((char *)&radsin, 0, sizeof(radsin));
	radsin.sin_family = AF_INET;
	radport = 0;
	memset(logfname, 0, PATH_MAX + 1);
	memset(pidfname, 0, PATH_MAX + 1);
	strcpy(pidfname, PID_DIR);
	strcat(pidfname, progname);
	strcat(pidfname, ".pid");

	while((opt = getopt(argc, argv, "du:i:l:P:r:t:h:p:V")) != -1) {
		if (opt == 'd')
			debugflag = 1;
		else if (opt == 'u')
			strncpy(username, optarg, MAX_USERNAME);
		else if (opt == 'i')
			sscanf(optarg, "%d", &i2cdelay);
		else if (opt == 'l')
			strncpy(logfname, optarg, PATH_MAX);
		else if (opt == 'P')
			strncpy(pidfname, optarg, PATH_MAX);
		else if (opt == 'r') {
			if (!inet_aton(optarg, &radsin.sin_addr)) {
				dprintf(STDERR_FILENO, "Invalid IPv4 address specification.\n");
				exit(EXIT_FAILURE);
			}
			if (!radport)
				radport = RADIO_PORT;
			radflag = 1;
		}
		else if (opt == 't')
			sscanf(optarg, "%d", &radport);
		else if (opt == 'h') {
			if (!inet_aton(optarg, &srvsin.sin_addr)) {
				dprintf(STDERR_FILENO, "Invalid IPv4 address specification.\n");
				exit(EXIT_FAILURE);
			}
		}
		else if (opt == 'p')
			sscanf(optarg, "%d", &srvport);
		else if (opt == 'V') {
			verShow();
			exit(EXIT_SUCCESS);
		}
		else if (opt == '?' || opt == 'h') {
			help();
			exit(EXIT_FAILURE);
		}
	}

	/* parameter logic */
	uid = getuid();
	gid = getgid();
	if (username[0]) {
		if (!uid)
			dropRootPriv(username, &uid, &gid);
		else {
			dprintf(STDERR_FILENO, "Flag -u is valid only when started by root.\n");
			exit(EXIT_FAILURE);
		}
	}

	if (debugflag && logfname[0]) {
		dprintf(STDERR_FILENO, "Flags -d and -l are mutually exclusive.\n");
		exit(EXIT_FAILURE);
	}

	if (!radflag && radport) {
		dprintf(STDERR_FILENO, "Flag -p requires -r.\n");
		exit(EXIT_FAILURE);
	}

	if (!radflag && !i2cdelay) {
		dprintf(STDERR_FILENO, "No sensor data source selected, specify at least one (radio daemon, I2C or both).\n");
		exit(EXIT_FAILURE);
	}

	srvsin.sin_port = htons(srvport);
	radsin.sin_port = htons(radport);

	/* initialize log */
	if (debugflag)
		logfd = STDOUT_FILENO;
	else {
		if (logfname[0]) {
			logfd = open(logfname, O_CREAT | O_APPEND | O_SYNC | O_WRONLY, FILE_UMASK);
			if (logfd == -1) {
				dprintf(STDERR_FILENO, "Unable to open log file '%s': %s\n",
					logfname, strerror(errno));
				exit(EXIT_FAILURE);
			}
		} else
			logfd = -1;
	}
	sem_init(&logsem, 0, 1);

	/* change scheduling priority */
	if (changeSched()) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to change process scheduling priority: %s\n",
				  strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to change process scheduling priority: %s\n",
				strerror (errno));
		endProcess(EXIT_FAILURE);
	}

	/* put banner in log */
#ifdef BUILDSTAMP
	logprintf(logfd, LOG_NOTICE, "starting %s build %s\n", BANNER, BUILDSTAMP);
#else
	logprintf(logfd, LOG_NOTICE, "starting %s\n", BANNER);
#endif

	/* show UID/GID */
	logprintf(logfd, LOG_NOTICE, "running as UID=%ld GID=%ld\n", uid, gid);

	/* check if pid file exists */
	pidfd = open(pidfname, O_PATH, FILE_UMASK);
	if (pidfd >= 0) {
		/* pid file exists */
		close(pidfd);
		dprintf(STDERR_FILENO, "PID file (%s) exists, daemon already running or stale file detected.\n",
			pidfname);
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to create PID file %s: %s\n",
				  pidfname, strerror(errno));
		close(logfd);
		exit(EXIT_FAILURE);
	}

	/* setup process */
	if (!debugflag)
		if (daemonize()) {
			dprintf(STDERR_FILENO, "Unable to fork to background: %s\n",
				strerror(errno));
			logprintf(logfd, LOG_ERROR, "unable to fork to background: %s\n",
				  strerror(errno));
			close(logfd);
			exit(EXIT_FAILURE);
		} else {
			procpid = getpid();
			logprintf(logfd, LOG_NOTICE, "forking to background with PID %d\n",
				  procpid);
		}

	/* populate pid file (new PID after daemonize()) */
	pidfd = open(pidfname, O_CREAT | O_WRONLY, FILE_UMASK);
	if (pidfd < 0) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to create PID file %s: %s\n",
				  pidfname, strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to create PID file %s: %s\n",
				pidfname, strerror(errno));
		endProcess(EXIT_FAILURE);
	}
	dprintf(pidfd, "%d\n", procpid);
	close(pidfd);

	/* signal handler */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigaction(SIGQUIT, &sa, NULL);
	sa.sa_handler = &signalQuit;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sa.sa_handler = &signalReopenLog;
	sigaction(SIGHUP, &sa, NULL);
	sa.sa_handler = &signalRstMinMax;
	sigaction(SIGUSR1, &sa, NULL);
	sa.sa_handler = &signalDelRadio;
	sigaction(SIGUSR2, &sa, NULL);

	/* setup proxy server */
	srvfd = socket(AF_INET, SOCK_STREAM, 0);
	if (srvfd == -1) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to create server socket: %s\n",
				  strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to create server socket: %s\n",
				strerror(errno));

		endProcess(EXIT_FAILURE);
	}
	ena = 1;
	if (setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &ena, sizeof(int)) == -1) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to set flags for server socket: %s\n",
				  strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to set flags for server socket: %s\n",
				strerror(errno));
		endProcess(EXIT_FAILURE);
	}
	if (setsockopt(srvfd, SOL_SOCKET, SO_REUSEPORT, &ena, sizeof(int)) == -1) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to set flags for server socket: %s\n",
				  strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to set flags for server socket: %s\n",
				strerror(errno));
		endProcess(EXIT_FAILURE);
	}
	if (bind(srvfd, (struct sockaddr *)&srvsin, sizeof(srvsin)) == -1) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to bind to server socket: %s\n",
				  strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to bind to server socket: %s\n",
				strerror(errno));
		endProcess(EXIT_FAILURE);
	}
	if (listen(srvfd, MAX_CLNT_QUEUE) == -1) {
		if (!debugflag)
			logprintf(logfd, LOG_ERROR, "unable to listen to server socket: %s\n",
				  strerror(errno));
		else
			dprintf(STDERR_FILENO, "Unable to listen to server socket: %s\n",
				strerror(errno));
		endProcess(EXIT_FAILURE);
	}
	logprintf(logfd, LOG_NOTICE, "accepting client TCP connections on %s port %d\n",
		  inet_ntoa(srvsin.sin_addr), srvport);

	/* initialize sensor list */
	memset(senstbl, 0, MAX_SENSORS * sizeof(struct sensorentry));
	sem_init(&sensem, 0, 1);

	/* start network client thread (optional) */
	if (radflag) {
		if (pthread_create(&netcltthread, NULL, radioDaemonThread, NULL)) {
			if (!debugflag)
				logprintf(logfd, LOG_ERROR, "cannot start network client thread\n");
			else
				dprintf(STDERR_FILENO, "Cannot start network client thread.\n");
			endProcess(EXIT_FAILURE);
		}
		netclrun = 1;
	}

	/* start I2C sensor read thread (optional) */
	if (i2cdelay) {
		idx = 0;
		wiringPiSetupGpio();
		if (initHTU21D(idx) >= 0) {
			logprintf(logfd, LOG_NOTICE, "htu21d I2C sensor at %d,0x%02X [%d] added to monitor\n",
				  RPI3I2C_BUS, HTU21D_I2C_ADDR, idx);
			idx++;
		}
		if (initBMP180(idx) >= 0) {
			logprintf(logfd, LOG_NOTICE, "bmp180 I2C sensor at %d,0x%02X [%d] added to monitor\n",
				  RPI3I2C_BUS, BMP180_I2C_ADDR, idx);
			idx++;
		}
		if (initBH1750(idx) >= 0) {
			logprintf(logfd, LOG_NOTICE, "bh1750 I2C sensor at %d,0x%02X [%d] added to monitor\n",
				  RPI3I2C_BUS, BH1750_I2C_ADDR, idx);
			idx++;
		}
		if (pthread_create(&i2cthread, NULL, i2cSensorThread, NULL)) {
			logprintf(logfd, LOG_WARN, "cannot start I2C thread, skipping I2C devices\n");
			memset(senstbl, 0, MAX_SENSORS * sizeof(struct sensorentry));
			i2cdelay = 0;
		} else
			i2ctrun = 1;
	}

	/* function loop - never ends, send signal to exit */
	for(;;) {
		/* clients are accepted, updated and disconnected immediately
		 * one by one so listen backlog should be big enough to keep
		 * them all in queue */
		clen = sizeof(struct sockaddr_in);
		clfd = accept(srvfd, (struct sockaddr *)&clin, &clen);
		logprintf(logfd, LOG_INFO,
			  "client %s [%d] connected successfully\n",
			  inet_ntoa(clin.sin_addr), clfd);
		if (rdelflag) {
			sensorRadioRemoveAll();
			rdelflag = 0;
		}
		sensorTableClean();
		if (mrstflag) {
			sensorResetMinMax();
			mrstflag = 0;
		}
		len = formatMessage(msgbuf, BUFFER_SIZE);
		if (debugflag)
			logprintf(logfd, LOG_DEBUG,
				  "sending message to client [%d], %d bytes\n", clfd, len);
		if (send(clfd, msgbuf, len, MSG_NOSIGNAL) == -1) {
			logprintf(logfd, LOG_WARN,
				  "client [%d] write error\n", clfd);
		}
		close(clfd);
		logprintf(logfd, LOG_INFO,
			  "client [%d] disconnected\n", clfd);
	}
}
