/* SPDX-License-Identifier: Apache-2.0 */
#include "app_config.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(app_config, CONFIG_DOG_DOOR_LOG_LEVEL);

#define CONFIG_SETTINGS_KEY "dogdoor/config"
#define CONFIG_VERSION 2U

struct app_config_data_v1 {
	char device_name[DOG_DOOR_DEVICE_NAME_MAX + 1];
	bool mqtt_enabled;
	char mqtt_host[DOG_DOOR_MQTT_HOST_MAX + 1];
	uint16_t mqtt_port;
	char mqtt_username[DOG_DOOR_MQTT_USERNAME_MAX + 1];
	char mqtt_password[DOG_DOOR_MQTT_PASSWORD_MAX + 1];
};

struct stored_config_v1 {
	uint32_t version;
	struct app_config_data_v1 data;
};

struct stored_config {
	uint32_t version;
	struct app_config_data data;
};

static struct stored_config current;
K_MUTEX_DEFINE(config_lock);

static void load_defaults(void)
{
	memset(&current, 0, sizeof(current));
	current.version = CONFIG_VERSION;
	strcpy(current.data.device_name, "Mudroom Door");
	current.data.mqtt_port = 1883;
	current.data.home_to_upper = true;
}

static int config_settings_set(const char *name, size_t len,
			       settings_read_cb read_cb, void *cb_arg)
{
	struct stored_config loaded;
	struct stored_config_v1 loaded_v1;
	ssize_t read_len;

	if (strcmp(name, "config") != 0) {
		return -ENOENT;
	}

	if (len == sizeof(loaded_v1)) {
		read_len = read_cb(cb_arg, &loaded_v1, sizeof(loaded_v1));
		if (read_len != sizeof(loaded_v1) || loaded_v1.version != 1U) {
			LOG_WRN("Ignoring incompatible stored configuration");
			return 0;
		}

		k_mutex_lock(&config_lock, K_FOREVER);
		memcpy(&current.data, &loaded_v1.data, sizeof(loaded_v1.data));
		current.version = CONFIG_VERSION;
		current.data.home_to_upper = true;
		k_mutex_unlock(&config_lock);
		LOG_INF("Migrated configuration; homing defaults to the upper limit");
		return 0;
	}

	if (len != sizeof(loaded)) {
		return -ENOENT;
	}
	read_len = read_cb(cb_arg, &loaded, sizeof(loaded));
	if (read_len != sizeof(loaded) || loaded.version != CONFIG_VERSION) {
		LOG_WRN("Ignoring incompatible stored configuration");
		return 0;
	}

	k_mutex_lock(&config_lock, K_FOREVER);
	current = loaded;
	current.data.device_name[DOG_DOOR_DEVICE_NAME_MAX] = '\0';
	current.data.mqtt_host[DOG_DOOR_MQTT_HOST_MAX] = '\0';
	current.data.mqtt_username[DOG_DOOR_MQTT_USERNAME_MAX] = '\0';
	current.data.mqtt_password[DOG_DOOR_MQTT_PASSWORD_MAX] = '\0';
	k_mutex_unlock(&config_lock);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dogdoor, "dogdoor", NULL, config_settings_set,
			       NULL, NULL);

int app_config_init(void)
{
	int ret;

	load_defaults();
	ret = settings_subsys_init();
	if (ret != 0 && ret != -EALREADY) {
		LOG_ERR("Settings init failed: %d", ret);
		return ret;
	}

	ret = settings_load_subtree("dogdoor");
	if (ret != 0) {
		LOG_ERR("Settings load failed: %d", ret);
	}
	return ret;
}

void app_config_get(struct app_config_data *config)
{
	if (config == NULL) {
		return;
	}
	k_mutex_lock(&config_lock, K_FOREVER);
	*config = current.data;
	k_mutex_unlock(&config_lock);
}

int app_config_set(const struct app_config_data *config)
{
	struct stored_config snapshot;

	if (config == NULL || config->device_name[0] == '\0' ||
	    (config->mqtt_enabled && config->mqtt_host[0] == '\0') ||
	    config->mqtt_port == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&config_lock, K_FOREVER);
	current.data = *config;
	current.version = CONFIG_VERSION;
	current.data.device_name[DOG_DOOR_DEVICE_NAME_MAX] = '\0';
	current.data.mqtt_host[DOG_DOOR_MQTT_HOST_MAX] = '\0';
	current.data.mqtt_username[DOG_DOOR_MQTT_USERNAME_MAX] = '\0';
	current.data.mqtt_password[DOG_DOOR_MQTT_PASSWORD_MAX] = '\0';
	snapshot = current;
	k_mutex_unlock(&config_lock);

	return settings_save_one(CONFIG_SETTINGS_KEY, &snapshot, sizeof(snapshot));
}
