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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sched.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <fcntl.h>
#include <linux/limits.h>
#ifdef HAS_CPUFREQ
#include <cpufreq.h>
#endif

#include <wiringPi.h>

extern char *optarg;
extern int optind, opterr, optopt;

/* *************** */
/* *  Constants  * */
/* *************** */

#define BANNER			"radiodump v0.6"
#define GPIO_PINS		28	/* number of Pi GPIO pins */
#define RING_BUFFER_ENTRIES	256
#define FILE_UMASK		(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)
#define SYSFS_GPIO_UNEXPORT	"/sys/class/gpio/unexport"
#define TUSDIFF(sec_e, usec_e, sec_s, usec_s)	(((sec_e) - (sec_s)) * 1000000UL + (usec_e) - (usec_s))

/* ********************** */
/* *  Global variables  * */
/* ********************** */

char progname[PATH_MAX + 1];
int brkflag, isrshut;
static volatile int bufsize;
static volatile int bri, bwi;	/* read and write buffer idx */
static volatile int tsyncmin, tsyncmax, tnoise;
static volatile unsigned long numpkts, numttotal, numtnoise, numtmiss;
static struct timeval tvprev;
static unsigned long *tbuf;	/* timing ring buffer */
static sem_t semtrdy;		/* buffer access semaphore */
static sem_t semisrdown;	/* unblocked if ISR is shut down and ready for exit */

/* *************** */
/* *  Functions  * */
/* *************** */

/* Show help */
void help(void)
{
	printf("Usage:\n\t%s -g gpio [-C] [-o outfile] [-b buffersize] [-s syncmin[,syncmax]] [-e noisetime] [-t timelimit] [-c packets]\n\n", progname);
	puts("Where:");
	puts("\t-g gpio       - GPIO pin with external RF receiver data (mandatory)");
	puts("\t-C            - generate CSV-friendly output (optional, format \"time,0,1,duration\")");
	puts("\t-o outfile    - output file name (optional)");
	printf("\t-b buffersize - size of signal processing buffer (in entries, optional, default is %d)\n",
	       RING_BUFFER_ENTRIES);
	puts("\t-s min[,max]  - wait for low signal that marks packet start (in microseconds, optional, default is none)");
	puts("\t-e noisetime  - treat signal as noise and ignore (in microseconds, optional, default is record all)");
	puts("\t-t timelimit  - capture duration (in seconds, optional, default is indefinite)");
	puts("\t-c packets    - number of packets to capture (optional, default is indefinite, valid only with -s)\n");
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

#ifdef HAS_CPUFREQ
/* verify if cpufreq governor prefers 'fixed' frequencies */
/* (gpio timings require stable system clock) */
int checkCpuFreq(void)
{
	struct cpufreq_policy *p;
	int s;

	p = cpufreq_get_policy(0);
	if (!strcmp(p->governor, "powersave") ||
	    !strcmp(p->governor, "performance"))
		s = 0;
	else
		s = 1;
	cpufreq_put_policy(p);	/* free pointer */
	return s;
}
#endif

/* unexport GPIO: remove GPIO exported in /sys when ISR is used */
int unexportSysfsGPIO(int gpio)
{
	int sysfd, ws;
	char gtxt[6];

	sysfd = open(SYSFS_GPIO_UNEXPORT, O_WRONLY);
	if (sysfd == -1)
		return -1;
	memset(gtxt, 0, sizeof(gtxt));
	snprintf(gtxt, sizeof(gtxt), "%d", gpio);
	ws = write(sysfd, gtxt, strlen(gtxt));
	close(sysfd);
	return ws < 0 ? -1 : 0;
}

/* ************ */
/* *  Signal  * */
/* ************ */

/* Intercept TERM and INT signals */
void signalQuit(int sig)
{
	brkflag = 1;
	sem_post(&semtrdy);	/* unblock main thread to exit immediately */
}

/* *************** */
/* *  Interrupt  * */
/* *************** */

/* Handle rising and falling edges on GPIO line */
static void handleGpioInt(void)
{
	struct timeval t;
	unsigned long tsdiff;

	if (isrshut) {
		sem_post(&semisrdown);
		return;
	}

	if (!tvprev.tv_sec) {
		/* first pass, initialize timestamp and exit */
		gettimeofday(&tvprev, NULL);
		return;
	}

	gettimeofday(&t, NULL);
	tsdiff = TUSDIFF(t.tv_sec, t.tv_usec, tvprev.tv_sec, tvprev.tv_usec);

	if (!numpkts && ((tsyncmin && tsdiff < tsyncmin) ||
	    (tsyncmax && tsdiff > tsyncmax)))
		/* waiting for sync that begins first packet */
		return;

	if (tsdiff <= tnoise) {
		/* tnoise may be 0 but it works anyway */
		if (tnoise)
			numtnoise++;
		return;
	}

	/* packet sync timing found */
	if (tsyncmin && tsdiff >= tsyncmin && (!tsyncmax || tsdiff <= tsyncmax))
		numpkts++;

	if ((bwi - bri + bufsize) % bufsize == 1) {
		/* no space left, signal overflow */
		numtmiss++;		
	} else {
		/* timing ok */
		tbuf[bwi] = tsdiff;
		sem_post(&semtrdy);
		bwi = (bwi + 1) % bufsize;
	}
	numttotal++;
	tvprev.tv_sec = t.tv_sec;
	tvprev.tv_usec = t.tv_usec;
}

/* ************ */
/* ************ */
/* **  MAIN  ** */
/* ************ */
/* ************ */

int main(int argc, char *argv[])
{
	struct timeval tstart, ts;
	int opt;
	struct sigaction sa;
	int ofd, gpio, timlim, pktlim;
	char outfile[PATH_MAX + 1];
	unsigned long blen, tlus, td, tbval;
	int csvflag, csvbit;
	unsigned long csvtime;

	/* get process name */
	strncpy(progname, basename(argv[0]), PATH_MAX);

	/* show help */
	if (argc < 2) {
		help();
		exit(0);
	}

	/* get parameters */
	gpio = -1;
	memset(outfile, 0, PATH_MAX + 1);
	bufsize = RING_BUFFER_ENTRIES;
	tsyncmin = 0;
	tsyncmax = 0;
	tnoise = 0;
	timlim = 0;
	pktlim = 0;
	csvflag = 0;
	while((opt = getopt(argc, argv, "g:o:b:s:e:t:c:C")) != -1) {
		if (opt == 'g')
			sscanf(optarg, "%d", &gpio);
		else if (opt == 'o')
			strncpy(outfile, optarg, PATH_MAX);
		else if (opt == 'b')
			sscanf(optarg, "%d", &bufsize);
		else if (opt == 's')
			sscanf(optarg, "%d,%d", &tsyncmin, &tsyncmax);
		else if (opt == 'e')
			sscanf(optarg, "%d", &tnoise);
		else if (opt == 't')
			sscanf(optarg, "%d", &timlim);
		else if (opt == 'c')
			sscanf(optarg, "%d", &pktlim);
		else if (opt == 'C')
			csvflag = 1;
		else if (opt == '?' || opt == 'h') {
			help();
			exit(EXIT_FAILURE);
		}
	}

	/* parameter logic */
	if (getuid()) {
		dprintf(STDERR_FILENO, "Error: must be run by root.\n");
		exit(EXIT_FAILURE);
	}

	if (gpio < 0 || gpio > GPIO_PINS) {
		dprintf(STDERR_FILENO, "Error: invalid RX GPIO pin.\n");
		exit(EXIT_FAILURE);
	}

	if (!tsyncmin && pktlim) {
		dprintf(STDERR_FILENO, "Error: cannot detect packets when no sync time is specified (-c requires -s).\n");
		exit(EXIT_FAILURE);
	}

	if (tnoise && tsyncmin && tnoise >= tsyncmin) {
		dprintf(STDERR_FILENO, "Error: noise time >= sync time.\n");
		exit(EXIT_FAILURE);
	}

	if (tsyncmax && tsyncmin >= tsyncmax) {
		dprintf(STDERR_FILENO, "Error: sync time min >= sync time max.\n");
		exit(EXIT_FAILURE);
	}

	if (tsyncmin && timlim)
		dprintf(STDERR_FILENO, "Warning: program will not time out until at least one sync pulse is received (both -s and -t are specified).\n");

#ifdef HAS_CPUFREQ
	/* warn user if system frequency is dynamic */
	if (checkCpuFreq())
		dprintf(STDERR_FILENO, "Warning: current CPUfreq governor is not optimal for radio code timing.\n");
#endif

	/* try to change scheduling priority */
	if (changeSched())
		dprintf(STDERR_FILENO, "Warning: unable to change process scheduling priority.\n");

	/* init buffer */
	blen = bufsize * sizeof(unsigned long);
	tbuf = (unsigned long*)malloc(blen);
	if (tbuf == NULL) {
		dprintf(STDERR_FILENO, "Error: unable to allocate buffer (%d bytes requested).\n", blen);
		exit(EXIT_FAILURE);
	}
	memset(tbuf, 0, blen);

	/* open output file if specified */
	if (outfile[0]) {
		ofd = open(outfile, O_CREAT | O_TRUNC | \
			   O_APPEND | O_SYNC | O_WRONLY, FILE_UMASK);
		if (ofd == -1) {
			dprintf(STDERR_FILENO,
				"Error: unable to open output file '%s': %s\n",
				outfile, strerror(errno));
			exit(EXIT_FAILURE);
		}
	} else
		ofd = STDOUT_FILENO;

	/* init globals */
	memset(&tvprev, 0, sizeof(struct timeval));	/* set to 0 */
	brkflag = 0;
	isrshut = 0;
	tlus = timlim * 1000000UL;
	td = 0;
	numpkts = 0;
	numttotal = 0;
	numtnoise = 0;
	numtmiss = 0;
	bri = 0;
	bwi = 0;
	csvbit = 0;
	csvtime = 0;
	sem_init(&semtrdy, 0, 0);
	sem_init(&semisrdown, 0, 0);
	gettimeofday(&tstart, NULL);

        /* signal handler */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &signalQuit;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	dprintf(STDERR_FILENO, "<Press Ctrl-C to end program>\n");

	/* put banner in log */
#ifdef BUILDSTAMP
	dprintf(ofd, "# %s build %s\n", BANNER, BUILDSTAMP);
#else
	dprintf(ofd, "# %s\n", BANNER);
#endif

	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();
	pinMode(gpio, INPUT);
	wiringPiISR(gpio, INT_EDGE_BOTH, handleGpioInt);

	/* info */
	dprintf(ofd, "# GPIO pin number: %d\n", gpio);
	dprintf(ofd, "# Timing buffer size: %d entries\n", bufsize);
	dprintf(ofd, "# Packet limit: %d %s\n", pktlim, pktlim ? "" : "(unlimited)");
	dprintf(ofd, "# Capture duration: %d %s\n", timlim, timlim ? "second(s)" : "(unlimited)");
	dprintf(ofd, "# Packet sync time min: %d %s\n", tsyncmin, tsyncmin ? "microsecond(s)" : "(not set)");
	dprintf(ofd, "# Packet sync time max: %d %s\n", tsyncmax, tsyncmax ? "microsecond(s)" : "(not set)");
	dprintf(ofd, "# Noise max time: %d %s\n", tnoise, tnoise ? "microsecond(s)" : "(not set)");
	dprintf(ofd, "# START\n");

	/* main function loop (break if time is up
	   or packet limit reached or Ctrl+C pressed) */
	for(;;) {
		sem_wait(&semtrdy);
		gettimeofday(&ts, NULL);
		td = TUSDIFF(ts.tv_sec, ts.tv_usec, tstart.tv_sec, tstart.tv_usec);
		if (brkflag || (timlim && td > tlus) || (pktlim && numpkts > pktlim)) {
			isrshut = 1;
			break;
		}
		/* read timings from buffer and display */
		tbval = tbuf[bri];
		bri = (bri + 1) % bufsize;
		if (csvflag) {
			dprintf(ofd, "%lu,%d,%d,%lu\n",
				csvtime, csvbit, 1 - csvbit, tbval);
			csvbit = 1 - csvbit;
			csvtime += tbval;
		}
		else
			dprintf(ofd, "%lu\n", tbval);
	}

	/* wait for ISR to shut down */
	sem_wait(&semisrdown);

	/* display statistics */
	dprintf(ofd, "\n# END\n");
	dprintf(ofd, "# Total pulses received: %lu\n", numttotal);
	dprintf(ofd, "# Pulses missed (buffer full): %lu (%.2lf %%)\n",
		numtmiss, (double)numtmiss/numttotal);
	dprintf(ofd, "# Noise pulses: %lu (%.2lf %%)\n",
		numtnoise, (double)numtnoise / (numttotal + numtnoise));
	dprintf(ofd, "# Packets (sync pulses): %lu\n", numpkts);
	dprintf(ofd, "# Capture duration: %lu second(s)\n", td / 1000000UL);

	/* the end */
	free(tbuf);
	if (ofd != STDOUT_FILENO)
		close(ofd);
	unexportSysfsGPIO(gpio);
	return 0;
}
