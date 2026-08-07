.. zephyr:board:: maker_esp32

Overview
********

The Maker ESP32 is a robotics controller from NULLLAB based on an
ESP32-WROOM-32E module. It combines the ESP32 with four TB67H450FNG H-bridge
motor drivers, four servo headers, four addressable RGB LEDs, and I2C, SPI,
ADC, and GPIO expansion headers.

Hardware
********

The board provides the following features:

- ESP32-D0WD-V3 dual-core Xtensa LX6 SoC at up to 240 MHz
- 4 MB external flash
- 2.4 GHz Wi-Fi and Bluetooth
- Four TB67H450FNG DC motor drivers
- Two four-wire stepper motor connectors shared with the DC motor outputs
- Four 5 V servo headers
- Four chained WS2812 RGB LEDs on GPIO16
- Five I2C headers on GPIO21 (SDA) and GPIO22 (SCL)
- SPI header on GPIO5 (CS), GPIO18 (SCLK), GPIO19 (MISO), and GPIO23 (MOSI)
- Four ADC inputs on GPIO34, GPIO35, GPIO39, and GPIO36
- CH340 USB-to-UART bridge for programming and the console
- 6 V to 16 V external motor power input

.. include:: ../../../espressif/common/soc-esp32-features.rst
   :start-after: espressif-soc-esp32-features

Supported Features
==================

.. zephyr:board-supported-hw::

Motor and Servo PWM Mapping
===========================

The LEDC PWM controller maps its low-speed channels to the H-bridge inputs and
its high-speed channels to the servo headers:

+--------------+-----------+------+--------------+-----------+------+
| Output       | GPIO      | PWM  | Output       | GPIO      | PWM  |
+==============+===========+======+==============+===========+======+
| Motor M1A    | 27        | 0    | Motor M1B    | 13        | 1    |
+--------------+-----------+------+--------------+-----------+------+
| Motor M2A    | 4         | 2    | Motor M2B    | 2         | 3    |
+--------------+-----------+------+--------------+-----------+------+
| Motor M3A    | 17        | 4    | Motor M3B    | 12        | 5    |
+--------------+-----------+------+--------------+-----------+------+
| Motor M4A    | 14        | 6    | Motor M4B    | 15        | 7    |
+--------------+-----------+------+--------------+-----------+------+
| Servo 1      | 32        | 8    | Servo 2      | 33        | 9    |
+--------------+-----------+------+--------------+-----------+------+
| Servo 3      | 26        | 10   | Servo 4      | 25        | 11   |
+--------------+-----------+------+--------------+-----------+------+

PWM channels 0 through 7 share a low-speed timer and channels 8 through 11
share a high-speed timer. Applications that need other timer assignments can
override the channel nodes in an application overlay.

The M3 and M4 inputs are connected to their drivers only when the four-position
``Motor and IO`` switch is set to the motor side. In the IO position, GPIO17,
GPIO12, GPIO14, and GPIO15 can be used through the GPIO expansion header.

.. warning::

   Use a suitable 6 V to 16 V supply on the DC input before driving a motor.
   Do not power motors from USB. Ensure all motor outputs are stopped before
   changing the ``Motor and IO`` switch.

System Requirements
*******************

.. include:: ../../../espressif/common/system-requirements.rst
   :start-after: espressif-system-requirements

Programming and Debugging
*************************

.. zephyr:board-supported-runners::

.. include:: ../../../espressif/common/building-flashing.rst
   :start-after: espressif-building-flashing

.. include:: ../../../espressif/common/board-variants.rst
   :start-after: espressif-board-variants

The board does not provide a dedicated JTAG connector. Several ESP32 JTAG pins
are shared with the motor circuitry, so the USB serial bootloader is the
recommended programming and debugging interface.

RGB LED Sample
**************

The four on-board WS2812 LEDs can be exercised with the LED strip sample:

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/led/led_strip
   :board: maker_esp32/esp32/procpu
   :goals: build flash

References
**********

.. target-notes::

.. _`Maker ESP32 hardware repository`: https://github.com/nulllaborg/maker-esp32
.. _`Maker ESP32 schematic`: https://github.com/nulllaborg/maker-esp32/blob/master/maker-esp32.pdf
