#include <stdio.h>
#include <stdlib.h>

#include <wiringPi.h>
#include <lcd.h>

void main(int argc, char *argv[])
{
	int i, a, fd;

	wiringPiSetupGpio();

	fd = lcdInit(4, 20, 4, 12, 16, 5, 6, 13, 19, 0, 0, 0, 0);

	lcdClear(fd);                  //Clear the display
	lcdHome(fd);

	a=argc-1;

	if (a>4) a=4;
	
	for(i=1; i<=a; i++) {
		lcdPosition(fd, 0, i-1);
		lcdPuts(fd, argv[i]);
	}	
}
