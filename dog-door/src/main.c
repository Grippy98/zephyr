/* SPDX-License-Identifier: Apache-2.0 */
#include "app_config.h"
#include "door_controller.h"
#include "led_controller.h"
#include "mqtt_bridge.h"
#include "network_manager.h"
#include "web_server.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(dog_door, CONFIG_DOG_DOOR_LOG_LEVEL);

int main(void)
{
	int ret;

	LOG_INF("Maker ESP32 Dog Door starting");
	LOG_WRN("Actuator is compile-time %s",
		IS_ENABLED(CONFIG_DOG_DOOR_ACTUATOR_ARMED) ? "ARMED" : "DISARMED");

	ret = app_config_init();
	if (ret != 0) {
		LOG_ERR("Configuration initialization failed: %d", ret);
	}
	ret = door_controller_init();
	if (ret != 0) {
		LOG_ERR("Door controller initialization failed: %d", ret);
	}
	ret = led_controller_init();
	if (ret != 0) {
		LOG_ERR("LED controller initialization failed: %d", ret);
	}
	ret = network_manager_init();
	if (ret != 0) {
		LOG_ERR("Network initialization failed: %d", ret);
	}
	ret = web_server_init();
	if (ret != 0) {
		LOG_ERR("Web server initialization failed: %d", ret);
	}
	ret = mqtt_bridge_init();
	if (ret != 0) {
		LOG_ERR("MQTT initialization failed: %d", ret);
	}

	while (true) {
		k_sleep(K_HOURS(1));
	}
	return 0;
}
