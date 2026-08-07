# Maker ESP32 Dog Door

A safety-first automated dog-door controller for the NULLLAB Maker ESP32. The
application serves a responsive local WebUI, provisions Wi-Fi through a setup
access point, and exposes the door and onboard LEDs to Home Assistant using
MQTT Discovery.

## Safety defaults

Stepper motion is disabled by default. Build with
`CONFIG_DOG_DOOR_ACTUATOR_ARMED=y` only after verifying the complete mechanism,
switch wiring, winding pairs, opening direction, drive duty, step rate, and
travel timeout.

The default application overlay assumes:

| Function | Connection | Electrical requirement |
| --- | --- | --- |
| Stepper | Stepper 1 connector / M1+M2 / GPIO27, 13, 4, 2 | Four-wire bipolar NEMA 17; commission with a 6 V motor supply |
| Upper limit | Servo 1 signal / GPIO32 | Internal pull-up; switch closes to ground |
| Lower limit | Servo 2 signal / GPIO33 | Internal pull-up; switch closes to ground |
| LEDs | Four onboard WS2812 LEDs / GPIO16 | Provided by the board definition |

Connect each limit switch between its servo header's signal and ground pins;
leave the 5 V pin unconnected. The application overlay reserves GPIO32 and
GPIO33 as internally pulled-up inputs instead of servo PWM outputs. Change
`boards/maker_esp32_procpu.overlay` if the actual wiring differs.

The default actuator profile targets the common Ender-3-style 42 mm bipolar
stepper: 1.8 degrees per full step, half-step sequencing, 800 half-steps per
second, and 50 percent PWM drive. The motor must connect to **Stepper 1** in
the order marked A+, A-, B+, B-. Use a multimeter to identify the two winding
pairs before connecting it; the two wires with continuity form one pair.

The Maker ESP32 driver VREF is fixed high and is not an adjustable
3D-printer-style current limit. Begin with a regulated 6 V supply on the DC
input, check winding current and motor/driver temperature, and tune
`CONFIG_DOG_DOOR_STEPPER_DRIVE_PERCENT` conservatively. Do not start with a
12 V or 16 V supply. The firmware removes winding power when stopped or
faulted, so the lift must be mechanically self-locking or otherwise prevented
from falling under gravity.

The dashboard can persistently select either the upper or lower switch as the
homing reference. A home command moves only toward the selected switch and
records the corresponding open or closed state when that switch engages.

## Build and flash

Generate a unique signing key once and keep a secure backup outside the Git
repository:

```shell
imgtool keygen -k dog-door/keys/dog-door-signing.pem -t ecdsa-p256
west build -b maker_esp32/esp32/procpu dog-door --sysbuild
west flash -d build
```

The first serial flash installs MCUboot and the signed application. Subsequent
updates can be installed from **System → Firmware update** using the generated
`zephyr.signed.bin` application image inside the sysbuild output directory.
The WebUI streams that image to the secondary slot, stops door motion, and
requests an MCUboot test boot. The previous image is retained through the swap;
the new image confirms itself only after 30 seconds of healthy startup, so a
failed update automatically rolls back on the next reboot.

Only images signed with `dog-door/keys/dog-door-signing.pem` are accepted by
the installed bootloader. The private key is ignored by Git. Losing it requires
a new bootloader and application to be installed over serial. The dashboard is
plain HTTP intended for a trusted local network; firmware authenticity does not
replace normal network isolation.

On first boot the device creates `DogDoor-Setup` with password `configureme`.
Connect to it and open `http://192.168.4.1/`. Change the setup password in
Kconfig before deployment.

## Home Assistant

Enable MQTT Discovery in the WebUI and configure the hostname or IP address of
the MQTT broker used by Home Assistant. The device publishes a cover entity,
an RGB light entity, upper/lower limit binary sensors, and availability.

## Local development UI

The dependency-free development server simulates the embedded REST API:

```shell
python3 tools/dev_server.py
```

Open `http://127.0.0.1:8080/` for the dashboard or
`http://127.0.0.1:8080/setup` for provisioning.
