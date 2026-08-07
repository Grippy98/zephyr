/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DOG_DOOR_LED_CONTROLLER_H_
#define DOG_DOOR_LED_CONTROLLER_H_

#include <stdint.h>

enum led_mode {
	LED_MODE_STATUS,
	LED_MODE_SOLID,
	LED_MODE_OFF,
};

struct led_snapshot {
	enum led_mode mode;
	uint8_t red;
	uint8_t green;
	uint8_t blue;
	uint8_t brightness;
	uint32_t generation;
};

int led_controller_init(void);
int led_controller_set(enum led_mode mode, uint8_t red, uint8_t green,
		       uint8_t blue, uint8_t brightness);
void led_controller_get(struct led_snapshot *snapshot);
const char *led_mode_name(enum led_mode mode);

#endif /* DOG_DOOR_LED_CONTROLLER_H_ */
