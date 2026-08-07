# Maker ESP32 Dog Door

A safety-first automated dog-door controller for the NULLLAB Maker ESP32. The
application serves a responsive local WebUI, provisions Wi-Fi through a setup
access point, and exposes the door and onboard LEDs to Home Assistant using
MQTT Discovery.

## Safety defaults

Servo motion is disabled by default. Build with
`CONFIG_DOG_DOOR_ACTUATOR_ARMED=y` only after verifying the complete mechanism,
switch wiring, PWM direction, neutral pulse, travel pulses, and travel timeout.

The default application overlay assumes:

| Function | Connection | Electrical requirement |
| --- | --- | --- |
| Servo | Servo header 1 / GPIO32 / LEDC channel 8 | Separate suitable 5 V supply with common ground |
| Upper limit | ADC header GPIO34 | External 10 kOhm pull-up to 3.3 V; switch closes to ground |
| Lower limit | ADC header GPIO35 | External 10 kOhm pull-up to 3.3 V; switch closes to ground |
| LEDs | Four onboard WS2812 LEDs / GPIO16 | Provided by the board definition |

GPIO34 and GPIO35 do not have internal pull resistors. Never operate the door
with floating limit inputs. Change `boards/maker_esp32_procpu.overlay` if the
actual wiring differs.

## Build and flash

```shell
west build -b maker_esp32/esp32/procpu dog-door
west flash
```

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
