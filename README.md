
-----------------------------------------------------------
All programs here are licensed under GPL v3 (see GPL file).
-----------------------------------------------------------


Programs require WiringPi.


NON-ROOT SETUP:

0. Set and export WIRINGPI_GPIOMEM=1 in user's profile.

1. Enable access to GPIO for special group 'gpio':

   # groupadd gpio
   # cat > /etc/udev/rules.d/99-gpiomem-access.rules < EOF
   SUBSYSTEM=="bcm2835-gpiomem", KERNEL=="gpiomem", GROUP="gpio", MODE="0660"
   EOF

2. Enable access to I2C devices for special group 'i2c':

   # groupadd i2c
   # cat > /etc/udev/rules.d/99-i2c-access.rules < EOF
   SUBSYSTEM=="i2c-dev", KERNEL=="i2c-[0-9]*", GROUP="i2c", MODE="0660"
   EOF

3. Enable access to SPI devices for special group 'spi':

   # groupadd spi
   # cat > /etc/udev/rules.d/99-spi-access.rules < EOF
   SUBSYSTEM=="spidev", KERNEL=="spidev[0-9].[0-9]", GROUP="spi", MODE="0660"
   EOF

4. Add user to secondary groups gpio, i2c and spi.

5. Force reload of udev configuration:

   # udevadm control --reload-rules && udevadm trigger

6. (optional) Enable gpio utility for user by adding entry to /etc/sudoers:

   user ALL = (root) NOPASSWD: /usr/bin/gpio


For radio signals capture, use latest tool:

  radiodump

  (replaces gpiosniffint)

For contacting power sockets, use latest tool:

  power433control

  (replaces power433ctrlite)

