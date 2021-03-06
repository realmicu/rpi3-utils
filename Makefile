##########
# Global #
##########

CC = gcc
CFLAGS = -I. -Wunused -Wshadow
PROGS = htu21d_test lcd_test lcd_env_show lcd_chars ncurstest lcdproc_env \
	lcd_env_show_fs bmp180_test pigpiobtnpoll gpiosniffer gpiosniffer2 \
	gpiosniffer3 gpiosniffint gpiosniffint3 rfkemotsniffer power433sniffer \
	power433send power433ctrlite bh1750_test env_mon ssd1306_test \
	ssd1306_font ssd1306_psf2ch ssd1306_bmp thermo433sniffer \
	radio433sniffer radio433daemon radio433client sensorproxy \
	net_env_mon power433control buttonhandler radiodump bme280_test

BUILDSTAMP = $(shell echo `date '+%Y%m%d-git@'``git log --oneline -1 | cut -d' ' -f1`)

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

#####################################
# BH1750 Ambient Light sensor (I2C) #
#####################################

BH1750_EXTRA_LIBS = -lwiringPi

bh1750_lib.o:	bh1750_lib.c
	$(CC) -c -o $@ $< $(CFLAGS)

bh1750_test:	bh1750_test.c bh1750_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(BH1750_EXTRA_LIBS) -pthread

#####################################################
# BME280 Pressure/Temperature/Humidity sensor (I2C) #
#####################################################

BME280_EXTRA_LIBS = -lwiringPi

bme280_lib.o:	bme280_lib.c
	$(CC) -c -o $@ $< $(CFLAGS)

bme280_test:	bme280_test.c bme280_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(BME280_EXTRA_LIBS)

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

power433ctrlite:	power433ctrlite.c power433_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(POWER433_EXTRA_LIBS)

##################
# Sensor monitor #
##################

ENVMON_EXTRA_LIBS = -lwiringPi -lncurses -pthread

env_mon:	env_mon.c htu21d_lib.o bmp180_lib.o bh1750_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(ENVMON_EXTRA_LIBS)

#####################
# OLED screen (SPI) #
#####################

OLED_EXTRA_LIBS = -lwiringPi

oled_lib.o:	oled_lib.c
	$(CC) -c -o $@ $< $(CFLAGS)

ssd1306_test:	ssd1306_test.c oled_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(OLED_EXTRA_LIBS)

ssd1306_font:	ssd1306_font.c oled_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(OLED_EXTRA_LIBS)

ssd1306_psf2ch:	ssd1306_psf2ch.c oled_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(OLED_EXTRA_LIBS)

ssd1306_bmp:	ssd1306_bmp.c oled_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(OLED_EXTRA_LIBS)

###########################################
# Remote Thermometer RF access using GPIO #
###########################################

THERMO433_EXTRA_LIBS = -lwiringPi -pthread

thermo433_lib.o:	thermo433_lib.c
	$(CC) -c -o $@ $< $(CFLAGS) -pthread

thermo433sniffer:	thermo433sniffer.c thermo433_lib.o
	$(CC) -o $@ $^ $(CFLAGS) $(THERMO433_EXTRA_LIBS)

##################################
# Universal RF access using GPIO #
##################################

RADIO433_EXTRA_LIBS = -lwiringPi -pthread

radio433_lib.o:	radio433_lib.c radio433_lib.h radio433_types.h
	$(CC) -c -o $@ $< $(CFLAGS) $(RADIO433_EXTRA_LIBS)

radio433_dev.o:	radio433_dev.c radio433_dev.h radio433_types.h
	$(CC) -c -o $@ $< $(CFLAGS)

radio433sniffer: radio433sniffer.c radio433_lib.o radio433_dev.o
	$(CC) -o $@ $^ $(CFLAGS) $(RADIO433_EXTRA_LIBS)

radio433daemon:	radio433daemon.c radio433_lib.o radio433_dev.o
	$(CC) -o $@ $^ $(CFLAGS) $(RADIO433_EXTRA_LIBS) -DHAS_CPUFREQ -lcpufreq -DBUILDSTAMP=\"$(BUILDSTAMP)\"

radio433client:	radio433client.c radio433_dev.o
	$(CC) -o $@ $^ $(CFLAGS)

power433control:	power433control.c radio433_lib.o radio433_dev.o
	$(CC) -o $@ $^ $(CFLAGS) $(RADIO433_EXTRA_LIBS) -DBUILDSTAMP=\"$(BUILDSTAMP)\"

radiodump:	radiodump.c
	$(CC) -o $@ $< $(CFLAGS) $(RADIO433_EXTRA_LIBS) -DHAS_CPUFREQ -lcpufreq -DBUILDSTAMP=\"$(BUILDSTAMP)\"

##################################
# Networked environment monitors #
##################################

sensorproxy:	sensorproxy.c radio433_dev.o htu21d_lib.o bmp180_lib.o bh1750_lib.o bme280_lib.o
	$(CC) -o $@ $^ $(CFLAGS) -lwiringPi -pthread -DBUILDSTAMP=\"$(BUILDSTAMP)\"

net_env_mon:	net_env_mon.c
	$(CC) -o $@ $^ $(CFLAGS) -lncurses

###################
# Button handlers #
###################

buttonhandler:	buttonhandler.c
	$(CC) -o $@ $< $(CFLAGS) -lwiringPi -pthread -DBUILDSTAMP=\"$(BUILDSTAMP)\"

##################
# Other programs #
##################

ncurstest:	ncurstest.c
	$(CC) -o $@ $< $(CFLAGS) -lncurses

