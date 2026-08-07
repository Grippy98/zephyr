/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DOG_DOOR_APP_CONFIG_H_
#define DOG_DOOR_APP_CONFIG_H_

#include <stdbool.h>
#include <stdint.h>

#define DOG_DOOR_DEVICE_NAME_MAX 32
#define DOG_DOOR_MQTT_HOST_MAX 64
#define DOG_DOOR_MQTT_USERNAME_MAX 32
#define DOG_DOOR_MQTT_PASSWORD_MAX 64

struct app_config_data {
	char device_name[DOG_DOOR_DEVICE_NAME_MAX + 1];
	bool mqtt_enabled;
	char mqtt_host[DOG_DOOR_MQTT_HOST_MAX + 1];
	uint16_t mqtt_port;
	char mqtt_username[DOG_DOOR_MQTT_USERNAME_MAX + 1];
	char mqtt_password[DOG_DOOR_MQTT_PASSWORD_MAX + 1];
};

int app_config_init(void);
void app_config_get(struct app_config_data *config);
int app_config_set(const struct app_config_data *config);

#endif /* DOG_DOOR_APP_CONFIG_H_ */
