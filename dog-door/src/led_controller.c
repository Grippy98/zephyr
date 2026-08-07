/* SPDX-License-Identifier: Apache-2.0 */
#include "led_controller.h"

#include "door_controller.h"

#include <errno.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(leds, CONFIG_DOG_DOOR_LOG_LEVEL);

#define STRIP_NODE DT_ALIAS(led_strip)
#define LED_COUNT DT_PROP(STRIP_NODE, chain_length)

BUILD_ASSERT(DT_NODE_HAS_STATUS(STRIP_NODE, okay), "led-strip alias is required");

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_snapshot state = {
	.mode = LED_MODE_STATUS,
	.brightness = 70,
};
static struct k_work_delayable update_work;
K_MUTEX_DEFINE(led_lock);

const char *led_mode_name(enum led_mode mode)
{
	switch (mode) {
	case LED_MODE_STATUS: return "status";
	case LED_MODE_SOLID: return "solid";
	case LED_MODE_OFF: return "off";
	default: return "status";
	}
}

static struct led_rgb status_color(enum door_state door_state)
{
	switch (door_state) {
	case DOOR_STATE_OPEN: return (struct led_rgb){ .r = 46, .g = 117, .b = 182 };
	case DOOR_STATE_CLOSED: return (struct led_rgb){ .r = 47, .g = 125, .b = 74 };
	case DOOR_STATE_OPENING:
	case DOOR_STATE_CLOSING:
	case DOOR_STATE_HOMING: return (struct led_rgb){ .r = 224, .g = 149, .b = 44 };
	case DOOR_STATE_FAULT: return (struct led_rgb){ .r = 194, .g = 57, .b = 52 };
	default: return (struct led_rgb){ .r = 112, .g = 79, .b = 146 };
	}
}

static void update_handler(struct k_work *work)
{
	struct led_snapshot snapshot;
	struct door_snapshot door;
	struct led_rgb color;
	struct led_rgb pixels[LED_COUNT];

	ARG_UNUSED(work);
	led_controller_get(&snapshot);
	door_controller_get(&door);

	if (snapshot.mode == LED_MODE_OFF) {
		color = (struct led_rgb){0};
	} else if (snapshot.mode == LED_MODE_STATUS) {
		color = status_color(door.state);
	} else {
		color = (struct led_rgb){ .r = snapshot.red, .g = snapshot.green,
					  .b = snapshot.blue };
	}
	color.r = ((uint16_t)color.r * snapshot.brightness) / 100U;
	color.g = ((uint16_t)color.g * snapshot.brightness) / 100U;
	color.b = ((uint16_t)color.b * snapshot.brightness) / 100U;
	for (size_t i = 0; i < ARRAY_SIZE(pixels); i++) {
		pixels[i] = color;
	}
	if (device_is_ready(strip)) {
		(void)led_strip_update_rgb(strip, pixels, ARRAY_SIZE(pixels));
	}
	k_work_reschedule(&update_work, K_MSEC(250));
}

int led_controller_init(void)
{
	if (!device_is_ready(strip)) {
		LOG_ERR("WS2812 strip is not ready");
		return -ENODEV;
	}
	k_work_init_delayable(&update_work, update_handler);
	k_work_schedule(&update_work, K_NO_WAIT);
	return 0;
}

int led_controller_set(enum led_mode mode, uint8_t red, uint8_t green,
		       uint8_t blue, uint8_t brightness)
{
	if (mode > LED_MODE_OFF || brightness > 100) {
		return -EINVAL;
	}
	k_mutex_lock(&led_lock, K_FOREVER);
	state.mode = mode;
	state.red = red;
	state.green = green;
	state.blue = blue;
	state.brightness = brightness;
	state.generation++;
	k_mutex_unlock(&led_lock);
	k_work_reschedule(&update_work, K_NO_WAIT);
	return 0;
}

void led_controller_get(struct led_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return;
	}
	k_mutex_lock(&led_lock, K_FOREVER);
	*snapshot = state;
	k_mutex_unlock(&led_lock);
}
