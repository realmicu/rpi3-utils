##########
# Global #
##########

CC = gcc
CFLAGS = -I.
PROGS = htu21d_test lcd_test lcd_env_show lcd_chars ncurstest lcdproc_env \
	lcd_env_show_fs bmp180_test pigpiobtnpoll gpiosniffer gpiosniffer2 \
	gpiosniffer3 gpiosniffint gpiosniffint3 rfkemotsniffer power433sniffer \
	power433send power433control

#################
# General rules #
#################

##%.o: %.c $(DEPS)
##	$(CC) -c -o $@ $< $(CFLAGS)

all:	$(PROGS)

.PHONY: clean all

clean:
	rm -f *.o $(PROGS)

############################################
# HTU21D Temperature/Humidity sensor (I2C) #
############################################

HTU21D_EXTRA_LIBS = -lwiringPi

htu21d_lib.o:	htu21d_lib.c
	$(CC) -c -o $@ $< $(CFLAGS)

htu21d_test:	htu21d_test.c htu21d_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(HTU21D_EXTRA_LIBS)

############################################
# BMP180 Pressure/Temperature sensor (I2C) #
############################################

BMP180_EXTRA_LIBS = -lwiringPi

bmp180_lib.o:	bmp180_lib.c
	$(CC) -c -o $@ $< $(CFLAGS)

bmp180_test:	bmp180_test.c bmp180_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(BMP180_EXTRA_LIBS)

##############################
# HD44780 LCD display (GPIO) #
##############################

HD44780_EXTRA_LIBS = -lwiringPi -lwiringPiDev

lcd_test:	lcd_test.c
	$(CC) -o $@ $< $(CFLAGS) $(HD44780_EXTRA_LIBS)

lcd_env_show:	lcd_env_show.c
	$(CC) -o $@ $< $(CFLAGS) $(HD44780_EXTRA_LIBS)

lcd_chars:	lcd_chars.c
	$(CC) -o $@ $< $(CFLAGS) $(HD44780_EXTRA_LIBS)

lcdproc_env:	lcdproc_env.c htu21d_lib.o bmp180_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(HD44780_EXTRA_LIBS) -pthread

lcd_env_show_fs:	lcd_env_show_fs.c
	$(CC) -o $@ $< $(CFLAGS) $(HD44780_EXTRA_LIBS) -lncurses

#######################
# GPIO Button Polling #
#######################

GPOLL_EXTRA_LIBS = -lwiringPi

pigpiobtnpoll:		pigpiobtnpoll.c
	$(CC) -o $@ $< $(CFLAGS) $(GPOLL_EXTRA_LIBS)

##################################
# GPIO Sniffer (for RF controls) #
##################################

GPIOSNIFFER_EXTRA_LIBS = -lwiringPi

gpiosniffer:		gpiosniffer.c
	$(CC) -o $@ $< $(CFLAGS) $(GPIOSNIFFER_EXTRA_LIBS)

gpiosniffer2:		gpiosniffer2.c
	$(CC) -o $@ $< $(CFLAGS) $(GPIOSNIFFER_EXTRA_LIBS)

gpiosniffer3:		gpiosniffer3.c
	$(CC) -o $@ $< $(CFLAGS) $(GPIOSNIFFER_EXTRA_LIBS)

gpiosniffint:		gpiosniffint.c
	$(CC) -o $@ $< $(CFLAGS) $(GPIOSNIFFER_EXTRA_LIBS)

gpiosniffint3:		gpiosniffint3.c
	$(CC) -o $@ $< $(CFLAGS) $(GPIOSNIFFER_EXTRA_LIBS)

rfkemotsniffer:		rfkemotsniffer.c
	$(CC) -o $@ $< $(CFLAGS) $(GPIOSNIFFER_EXTRA_LIBS)

##########################################
# RF control of power sockets using GPIO #
##########################################

POWER433_EXTRA_LIBS = -lwiringPi -pthread

power433_lib.o:		power433_lib.c
	$(CC) -c -o $@ $< $(CFLAGS) -pthread

power433sniffer:	power433sniffer.c power433_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(POWER433_EXTRA_LIBS)

power433send:		power433send.c power433_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(POWER433_EXTRA_LIBS)

power433control:	power433control.c power433_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(POWER433_EXTRA_LIBS)

##################
# Other programs #
##################

ncurstest:	ncurstest.c
	$(CC) -o $@ $< $(CFLAGS) -lncurses

