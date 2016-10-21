#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <wiringPi.h>

#define GPIO_PINS		28	/* number of Pi GPIO pins */
#define MAX_BTN_LABEL		16	/* maximum length of button label */
#define MAX_SCRIPT_PATH		256	/* maximum length of script path */
#define POLL_INTERVAL_MS	20	/* GPIO polling interval in ms */
#define DEBOUNCE_INT_NUM	 3	/* how many polling intervals button
					   can bounce (its state floats) */
struct btninfo {	/* table index is GPIO pin number */
	int activelow;	/* 1 if activelow - pull-up is used */
	char label[MAX_BTN_LABEL + 1];  /* label for handler script */
	int pressed;	/* 1 if currently pressed */
	unsigned long lastchg;	/* timestamp of last state change */
	int bouncecnt;	/* how long until bounce period expires */
};

static struct btninfo *btn[GPIO_PINS];

static char hscript[MAX_SCRIPT_PATH + 1];

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s {script} {btn0} [btn1] ...\n\n",
	       progname);
	puts("Where:");
	puts("\tscript\t - points to button handler script (mandatory)");
	puts("\tbtn\t - defined as gpio:activelow:label (at least one)");
}

/* Run handler script */
void callScript(char *button, int gpio, char *action, int age)
{
	int pid;
	char s_gpio[4], s_age[12];
	char *s_argv[6];

	pid = fork();
	if (pid == -1)
		fprintf(stderr, "Unable to execute handler: %s: %s\n", hscript,
			strerror(errno));
	else if (!pid) {
		/* child created, execute script */
		snprintf(s_gpio, 4, "%d", gpio);
		snprintf(s_age, 12, "%d", age);
		s_argv[0]=hscript;
		s_argv[1]=button;
		s_argv[2]=s_gpio;
		s_argv[3]=action;
		s_argv[4]=s_age;
		s_argv[5]=(char*)NULL;
		printf(">> executing button handler script: %s \"%s\" \"%s\" \"%s\" \"%s\"\n",
		       hscript, s_argv[1], s_argv[2], s_argv[3], s_argv[4]);
		if (execv(hscript, s_argv))
			fprintf(stderr, "Unable to execute handler: %s: %s\n",
				hscript, strerror(errno));
	}
}

/* Intercept TERM and INT signals */
void signalQuit(int sig)
{
	callScript("", 0, "stop", 0);
	exit(0);
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	int i, s;
	int par_gpio, par_al;
	char par_lbl[MAX_BTN_LABEL + 1];
	unsigned long t;
	struct sigaction sa; 

	/* show help */
	if (argc < 3) {
		help(argv[0]);
		exit(0);
	}

	memset(btn, 0, sizeof(btn));
	memset(hscript, 0, sizeof(hscript));

	/* store path to script - parameter 1 */
	strncpy(hscript, argv[1], MAX_SCRIPT_PATH);
	printf(">> added button handler script: \"%s\"\n", hscript);

	/* initialize button table - parameters 2..N */
	for(i = 2; i < argc; i++)
		if (sscanf(argv[i], "%d:%d:%s",
			   &par_gpio, &par_al, par_lbl) == 3) {
			if (par_gpio < 0 || par_gpio > GPIO_PINS ||
			    par_al < 0 || par_al > 1)
				continue;
			btn[par_gpio] = malloc(sizeof(struct btninfo));
			if (btn[par_gpio] == NULL)
				break;
			btn[par_gpio]->activelow = par_al;
			strncpy(btn[par_gpio]->label, par_lbl, MAX_BTN_LABEL);
			printf(">> added button[%d]: gpio=%d activelow=%d label=%s\n",
				i - 2, par_gpio, btn[par_gpio]->activelow,
				btn[par_gpio]->label);
		} else
			continue;
	/* register signal handler */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &signalQuit;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	/* initialize WiringPi library - use BCM GPIO numbers */
	wiringPiSetupGpio();

	/* run script with start parameter */
	callScript("", 0, "start", 0);

	/* finish button initialization, read initial state
	   and call handler script with "on/off" actions */
	for(i = 0; i < GPIO_PINS; i++) {
		if (!btn[i])
			continue;
		pinMode(i, INPUT);
		btn[i]->pressed = digitalRead(i) ?
				  !btn[i]->activelow : btn[i]->activelow;
		btn[i]->lastchg = time(NULL);
		btn[i]->bouncecnt = 0;
		callScript(btn[i]->label, i, btn[i]->pressed ? "on" : "off", 0);
	}

	/* MAIN LOOP */
	/* (no exit, send signal to stop) */
	for(;;) {
		for(i = 0; i < GPIO_PINS; i++) {
			if (!btn[i])
				continue;
			if (btn[i]->bouncecnt) {
				btn[i]->bouncecnt--;
				continue;
			}
			s = digitalRead(i) ?
			    !btn[i]->activelow : btn[i]->activelow;
			if (s == btn[i]->pressed)
				continue;
			btn[i]->pressed = s;
			t = time(NULL) - btn[i]->lastchg;
			btn[i]->lastchg += t;
			btn[i]->bouncecnt = DEBOUNCE_INT_NUM;
			callScript(btn[i]->label, i,
				   btn[i]->pressed ? "pressed" : "released", t);
		}
		waitpid(-1, NULL, WNOHANG); /* 'un-zombify' children scripts */
		delay(POLL_INTERVAL_MS);
	}
}
