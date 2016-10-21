#include <stdio.h>
#include <stdlib.h>

#include <wiringPi.h>
#include <lcd.h>

void main(void)
{
	int i, fd;

	wiringPiSetupGpio();

	fd = lcdInit(4, 20, 4, 12, 16, 5, 6, 13, 19, 0, 0, 0, 0);

	lcdClear(fd);
	lcdHome(fd);

	for(i = 0; i <= 255; i++) {
		lcdPutchar(fd, (char)i);
		if (i % 80 == 79) getchar();
	}

	lcdClear(fd);
}
