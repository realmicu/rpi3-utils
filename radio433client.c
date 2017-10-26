#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "radio433_types.h"
#include "radio433_dev.h"

#define RADIO433_DEFAULT_HOST	"127.0.0.1"
#define RADIO433_DEFAULT_PORT	5433
#define RECONNECT_DELAY_SEC	10
#define RADMSG_SIZE		128     /* radio daemon message size */
#define RADBUF_SIZE		(RADMSG_SIZE * 8)
#define MSG_HDR			"<RX>"
#define MSG_EOT			"<ZZ>"

extern char *optarg;
extern int optind, opterr, optopt;

/* Show help */
void help(char *progname)
{
	printf("Usage:\n\t%s [-w] [-r ipaddr] [-p tcpport]\n\n", progname);
	puts("Where:");
	puts("\t-w       \t - wait for server (optional)");
	puts("\t-r ipaddr\t - IPv4 address of radio433daemon server (optional)");
	puts("\t-p tcpport\t - TCP port of radio433daemon server (optional)");
	printf("When no parameter is specified, client tries to connect to server %s on port %d.\n",
	       RADIO433_DEFAULT_HOST, RADIO433_DEFAULT_PORT);
}

/* ********** */
/* *  MAIN  * */
/* ********** */

int main(int argc, char *argv[])
{
	time_t tss;
	unsigned int tsms;
	struct tm *tl;
	unsigned long long code;
	int opt, tid, type, bits;
        char *stype[] = { "NUL", "PWR", "THM", "RMT" };
	int sysid, devid, btn;
	int ch, batlow, tdir, humid;
	double temp;
	char trend[3] = { '_', '/', '\\' };
	int port, clfd, msglen;
	struct sockaddr_in clntsin;
	char buf[RADBUF_SIZE + 1];
	char *msgptr, *msgend;
	int waitflag;
	int codelen, repeats, interval;

	/* get parameters */
	memset((char *)&clntsin, 0, sizeof(clntsin));
	clntsin.sin_family = AF_INET;
	inet_aton(RADIO433_DEFAULT_HOST, &clntsin.sin_addr);
	port = RADIO433_DEFAULT_PORT;
	waitflag = 0;
	while((opt = getopt(argc, argv, "hwr:p:")) != -1) {
		if (opt == 'r') {
			if (!inet_aton(optarg, &clntsin.sin_addr)) {
				fputs("Invalid IPv4 address specification.\n", stderr);
				exit(EXIT_FAILURE);
			}
		}
		else if (opt == 'p')
			sscanf(optarg, "%d", port);
		else if (opt == 'w')
			waitflag = 1;
		else if (opt == '?' || opt == 'h') {
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
	}
	clntsin.sin_port = htons(port);

	/* connect to server */
	for(;;) {
		clfd = socket(AF_INET, SOCK_STREAM, 0);
        	if (clfd == -1) {
        	        fprintf(stderr, "Unable to create socket: %s\n",
        	                strerror (errno));
        	        exit(EXIT_FAILURE);
        	}
        	if (connect(clfd, (struct sockaddr *)&clntsin, sizeof(clntsin)) == -1) {
        	        fprintf(stderr, "Unable to connect to server: %s\n",
        	                strerror (errno));
			if (waitflag) {
				fprintf(stderr, "Retrying in %d seconds...\n",
					RECONNECT_DELAY_SEC);
				sleep(RECONNECT_DELAY_SEC);
				continue;
			}
			else
        	        	exit(EXIT_FAILURE);
        	} else
			break;
	}
	printf("Connected to server %s port %d. Awaiting messages...\n",
	       inet_ntoa(clntsin.sin_addr), port);

	/* function loop - never ends, send signal to exit */
	for(;;) {
		do
			msglen = recv(clfd, buf, RADBUF_SIZE, 0);
		while (msglen == -1 && errno == EINTR);
		if (msglen < 0) {
                	fprintf(stderr, "Error receiving data from server: %s\n",
                        	strerror (errno));
                	exit(EXIT_FAILURE);
		} else if (!msglen) {
			fputs("Server has closed connection.\n", stderr);
			exit(EXIT_FAILURE);
		}
		buf[msglen] = 0;
		msgptr = buf;
		do {
			msgptr = strstr(msgptr, MSG_HDR);
			if (msgptr == NULL)
				break;
			msgend = strstr(msgptr, MSG_EOT);
			if (msgend == NULL)
				break;
			if (sscanf(msgptr + 4, "%lu.%u;%d;%d;%d;0x%X;%d;0x%llX;",
			    &tss, &tsms, &codelen, &repeats, &interval, &type,
			    &bits, &code) != 8)
				continue;
			tl = localtime(&tss);
			printf("%d-%02d-%02d %02d:%02d:%02d.%03u",
			       1900 + tl->tm_year, tl->tm_mon + 1,
			       tl->tm_mday, tl->tm_hour, tl->tm_min,
			       tl->tm_sec, tsms);
			if (type & RADIO433_CLASS_POWER)
				tid = 1;
			else if (type & RADIO433_CLASS_WEATHER)
				tid = 2;
			else if (type & RADIO433_CLASS_REMOTE)
				tid = 3;
			else
				tid = 0;
			printf("  %s len = %d , code = 0x%0*llX", stype[tid], bits,
			       (bits + 3) >> 2, code);
			if (type == RADIO433_DEVICE_KEMOTURZ1226) {
				if (Radio433_pwrGetCommand(code, &sysid, &devid, &btn))
					printf(" , %d : %s%s%s%s%s : %s\n", sysid,
					       devid & POWER433_DEVICE_A ? "A" : "",
					       devid & POWER433_DEVICE_B ? "B" : "",
					       devid & POWER433_DEVICE_C ? "C" : "",
					       devid & POWER433_DEVICE_D ? "D" : "",
					       devid & POWER433_DEVICE_E ? "E" : "",
					       btn ? "ON" : "OFF");
				else
					puts("");
			} else if (type == RADIO433_DEVICE_HYUWSSENZOR77TH) {
				if (Radio433_thmGetData(code, &sysid, &devid, &ch,
							&batlow, &tdir, &temp, &humid))
					printf(" , %1d , T: %+.1lf C %c , H: %d %% %c\n",
					       ch, temp, tdir < 0 ? '!' : trend[tdir],
					       humid, batlow ? 'b' : ' ');
				else
					puts("");
			} else
				puts("");
			msgptr = msgend + 5;
		} while (msgptr < buf + msglen);
	}	/* main loop ends here */
}
