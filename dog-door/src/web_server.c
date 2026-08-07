/* SPDX-License-Identifier: Apache-2.0 */
#include "web_server.h"

#include "app_config.h"
#include "door_controller.h"
#include "led_controller.h"
#include "mqtt_bridge.h"
#include "network_manager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/wifi_credentials.h>

LOG_MODULE_REGISTER(web, CONFIG_DOG_DOOR_LOG_LEVEL);

#define API_BODY_MAX 768
#define API_RESPONSE_MAX 3072

enum api_kind {
	API_STATE,
	API_DOOR,
	API_LED,
	API_WIFI_SCAN,
	API_WIFI,
	API_CONFIG,
};

struct api_context {
	enum api_kind kind;
	uint8_t body[API_BODY_MAX];
	size_t cursor;
	uint8_t response[API_RESPONSE_MAX];
};

static uint8_t index_html_gz[] = {
#include "index.html.gz.inc"
};
static uint8_t app_css_gz[] = {
#include "app.css.gz.inc"
};
static uint8_t app_js_gz[] = {
#include "app.js.gz.inc"
};

static struct http_resource_detail_static index_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_encoding = "gzip",
		.content_type = "text/html",
	},
	.static_data = index_html_gz,
	.static_data_len = sizeof(index_html_gz),
};
static struct http_resource_detail_static css_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_encoding = "gzip",
		.content_type = "text/css",
	},
	.static_data = app_css_gz,
	.static_data_len = sizeof(app_css_gz),
};
static struct http_resource_detail_static js_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_encoding = "gzip",
		.content_type = "text/javascript",
	},
	.static_data = app_js_gz,
	.static_data_len = sizeof(app_js_gz),
};

static const struct http_header json_header[] = {
	{ .name = "Content-Type", .value = "application/json; charset=utf-8" },
	{ .name = "Cache-Control", .value = "no-store" },
};

static const char *find_json_value(const char *json, const char *key)
{
	char needle[72];
	const char *value;

	snprintk(needle, sizeof(needle), "\"%s\"", key);
	value = strstr(json, needle);
	if (value == NULL) {
		return NULL;
	}
	value = strchr(value + strlen(needle), ':');
	if (value == NULL) {
		return NULL;
	}
	value++;
	while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
		value++;
	}
	return value;
}

static bool json_string(const char *json, const char *key, char *output, size_t output_size)
{
	const char *value = find_json_value(json, key);
	size_t length = 0;

	if (value == NULL || *value != '"' || output_size == 0) {
		return false;
	}
	value++;
	while (value[length] != '\0' && value[length] != '"' && length + 1 < output_size) {
		if (value[length] == '\\') {
			return false;
		}
		output[length] = value[length];
		length++;
	}
	if (value[length] != '"') {
		return false;
	}
	output[length] = '\0';
	return true;
}

static bool json_number(const char *json, const char *key, long *number)
{
	const char *value = find_json_value(json, key);
	char *end;

	if (value == NULL) {
		return false;
	}
	*number = strtol(value, &end, 10);
	return end != value;
}

static bool json_boolean(const char *json, const char *key, bool *result)
{
	const char *value = find_json_value(json, key);

	if (value == NULL) {
		return false;
	}
	if (strncmp(value, "true", 4) == 0) {
		*result = true;
		return true;
	}
	if (strncmp(value, "false", 5) == 0) {
		*result = false;
		return true;
	}
	return false;
}

static void write_scan_json(char *buffer, size_t size,
			    const struct network_snapshot *network)
{
	size_t used = 0;

	used += snprintk(buffer + used, size - used, "[");
	for (size_t i = 0; i < network->scan_count && used < size; i++) {
		used += snprintk(buffer + used, size - used,
				 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,\"secure\":%s}",
				 i == 0 ? "" : ",", network->scan[i].ssid,
				 network->scan[i].rssi, network->scan[i].channel,
				 network->scan[i].security == WIFI_SECURITY_TYPE_NONE ?
				 "false" : "true");
	}
	if (used < size) {
		(void)snprintk(buffer + used, size - used, "]");
	}
}

static int state_response(struct api_context *context)
{
	struct door_snapshot door;
	struct led_snapshot led;
	struct network_snapshot network;
	struct mqtt_bridge_snapshot mqtt;
	struct app_config_data config;
	char scan_json[1024];

	door_controller_get(&door);
	led_controller_get(&led);
	network_manager_get(&network);
	mqtt_bridge_get(&mqtt);
	app_config_get(&config);
	write_scan_json(scan_json, sizeof(scan_json), &network);
	return snprintk(context->response, sizeof(context->response),
		"{\"deviceName\":\"%s\",\"uptimeSeconds\":%lld,"
		"\"door\":{\"state\":\"%s\",\"upperLimit\":%s,\"lowerLimit\":%s,"
		"\"actuatorArmed\":%s,\"servoReady\":%s,\"fault\":\"%s\"},"
		"\"led\":{\"mode\":\"%s\",\"red\":%u,\"green\":%u,\"blue\":%u,"
		"\"brightness\":%u},"
		"\"network\":{\"apActive\":%s,\"connected\":%s,\"connecting\":%s,"
		"\"scanning\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
		"\"setupSsid\":\"%s\",\"scan\":%s},"
		"\"mqtt\":{\"enabled\":%s,\"connected\":%s,\"host\":\"%s\","
		"\"port\":%u,\"username\":\"%s\"}}",
		config.device_name, k_uptime_get() / 1000,
		door_state_name(door.state), door.upper_limit ? "true" : "false",
		door.lower_limit ? "true" : "false", door.actuator_armed ? "true" : "false",
		door.servo_ready ? "true" : "false", door.fault, led_mode_name(led.mode),
		led.red, led.green, led.blue, led.brightness,
		network.ap_active ? "true" : "false", network.connected ? "true" : "false",
		network.connecting ? "true" : "false", network.scanning ? "true" : "false",
		network.ssid, network.ip_address, network.rssi, CONFIG_DOG_DOOR_SETUP_AP_SSID,
		scan_json, config.mqtt_enabled ? "true" : "false",
		mqtt.connected ? "true" : "false", config.mqtt_host, config.mqtt_port,
		config.mqtt_username);
}

static int handle_door(const char *json)
{
	char command[16];

	if (!json_string(json, "command", command, sizeof(command))) {
		return -EINVAL;
	}
	if (strcmp(command, "open") == 0) {
		return door_controller_command(DOOR_COMMAND_OPEN);
	}
	if (strcmp(command, "close") == 0) {
		return door_controller_command(DOOR_COMMAND_CLOSE);
	}
	if (strcmp(command, "stop") == 0) {
		return door_controller_command(DOOR_COMMAND_STOP);
	}
	if (strcmp(command, "home") == 0) {
		return door_controller_command(DOOR_COMMAND_HOME);
	}
	return -EINVAL;
}

static int parse_hex_color(const char *color, uint8_t *red, uint8_t *green, uint8_t *blue)
{
	unsigned int r;
	unsigned int g;
	unsigned int b;

	if (strlen(color) != 7 || color[0] != '#' ||
	    sscanf(color + 1, "%02x%02x%02x", &r, &g, &b) != 3) {
		return -EINVAL;
	}
	*red = r;
	*green = g;
	*blue = b;
	return 0;
}

static int handle_led(const char *json)
{
	struct led_snapshot current;
	char mode_text[16] = "status";
	char color[8] = "#ffffff";
	long brightness;
	enum led_mode mode;
	uint8_t red;
	uint8_t green;
	uint8_t blue;

	led_controller_get(&current);
	(void)json_string(json, "mode", mode_text, sizeof(mode_text));
	(void)json_string(json, "color", color, sizeof(color));
	if (!json_number(json, "brightness", &brightness)) {
		brightness = current.brightness;
	}
	if (strcmp(mode_text, "status") == 0) {
		mode = LED_MODE_STATUS;
	} else if (strcmp(mode_text, "solid") == 0) {
		mode = LED_MODE_SOLID;
	} else if (strcmp(mode_text, "off") == 0) {
		mode = LED_MODE_OFF;
	} else {
		return -EINVAL;
	}
	if (brightness < 0 || brightness > 100 ||
	    parse_hex_color(color, &red, &green, &blue) != 0) {
		return -EINVAL;
	}
	return led_controller_set(mode, red, green, blue, brightness);
}

static int handle_wifi(const char *json)
{
	char ssid[WIFI_SSID_MAX_LEN + 1];
	char password[WIFI_CREDENTIALS_MAX_PASSWORD_LEN + 1];
	bool secure = true;

	if (!json_string(json, "ssid", ssid, sizeof(ssid))) {
		return -EINVAL;
	}
	if (!json_string(json, "password", password, sizeof(password))) {
		password[0] = '\0';
	}
	(void)json_boolean(json, "secure", &secure);
	return network_manager_set_wifi(ssid, password,
		secure ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE);
}

static int handle_config(const char *json)
{
	struct app_config_data config;
	char temporary[DOG_DOOR_MQTT_PASSWORD_MAX + 1];
	long port;

	app_config_get(&config);
	if (json_string(json, "deviceName", temporary, sizeof(temporary))) {
		strncpy(config.device_name, temporary, sizeof(config.device_name) - 1);
	}
	(void)json_boolean(json, "mqttEnabled", &config.mqtt_enabled);
	if (json_string(json, "mqttHost", temporary, sizeof(temporary))) {
		strncpy(config.mqtt_host, temporary, sizeof(config.mqtt_host) - 1);
	}
	if (json_number(json, "mqttPort", &port)) {
		if (port < 1 || port > 65535) {
			return -EINVAL;
		}
		config.mqtt_port = port;
	}
	if (json_string(json, "mqttUsername", temporary, sizeof(temporary))) {
		strncpy(config.mqtt_username, temporary, sizeof(config.mqtt_username) - 1);
	}
	if (json_string(json, "mqttPassword", temporary, sizeof(temporary)) &&
	    temporary[0] != '\0') {
		strncpy(config.mqtt_password, temporary, sizeof(config.mqtt_password) - 1);
	}
	return app_config_set(&config);
}

static int api_handler(struct http_client_ctx *client,
		       enum http_transaction_status status,
		       const struct http_request_ctx *request_ctx,
		       struct http_response_ctx *response_ctx, void *user_data)
{
	struct api_context *context = user_data;
	int ret = 0;
	int response_len;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		context->cursor = 0;
		return 0;
	}
	if (request_ctx->data_len > 0) {
		if (context->cursor + request_ctx->data_len >= sizeof(context->body)) {
			context->cursor = 0;
			return -ENOMEM;
		}
		memcpy(context->body + context->cursor, request_ctx->data, request_ctx->data_len);
		context->cursor += request_ctx->data_len;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	context->body[context->cursor] = '\0';

	if (client->method == HTTP_GET) {
		if (context->kind == API_WIFI_SCAN) {
			ret = network_manager_scan();
			if (ret == -EALREADY) {
				ret = 0;
			}
		} else if (context->kind != API_STATE && context->kind != API_CONFIG) {
			ret = -ENOTSUP;
		}
	} else if (client->method == HTTP_POST) {
		switch (context->kind) {
		case API_DOOR: ret = handle_door((char *)context->body); break;
		case API_LED: ret = handle_led((char *)context->body); break;
		case API_WIFI: ret = handle_wifi((char *)context->body); break;
		case API_CONFIG: ret = handle_config((char *)context->body); break;
		default: ret = -ENOTSUP; break;
		}
	} else {
		ret = -ENOTSUP;
	}

	if (ret == 0) {
		response_len = state_response(context);
		response_ctx->status = HTTP_200_OK;
	} else {
		const char *message = ret == -EACCES ?
			"Actuator is disarmed in this firmware build" :
			(ret == -EINVAL ? "Invalid request" : "Command could not be completed");

		response_len = snprintk(context->response, sizeof(context->response),
					 "{\"ok\":false,\"error\":\"%s\",\"code\":%d}",
					 message, ret);
		response_ctx->status = ret == -EACCES ? HTTP_409_CONFLICT :
			(ret == -EINVAL ? HTTP_400_BAD_REQUEST : HTTP_503_SERVICE_UNAVAILABLE);
	}
	response_ctx->headers = json_header;
	response_ctx->header_count = ARRAY_SIZE(json_header);
	response_ctx->body = context->response;
	response_ctx->body_len = MAX(response_len, 0);
	response_ctx->final_chunk = true;
	context->cursor = 0;
	return 0;
}

#define API_RESOURCE(name, api_kind_value, methods) \
	static struct api_context name##_context = { .kind = api_kind_value }; \
	static struct http_resource_detail_dynamic name##_detail = { \
		.common = { .type = HTTP_RESOURCE_TYPE_DYNAMIC, \
			    .bitmask_of_supported_http_methods = methods }, \
		.cb = api_handler, .user_data = &name##_context \
	}

API_RESOURCE(state, API_STATE, BIT(HTTP_GET));
API_RESOURCE(door, API_DOOR, BIT(HTTP_POST));
API_RESOURCE(led, API_LED, BIT(HTTP_POST));
API_RESOURCE(wifi_scan, API_WIFI_SCAN, BIT(HTTP_GET));
API_RESOURCE(wifi, API_WIFI, BIT(HTTP_POST));
API_RESOURCE(config, API_CONFIG, BIT(HTTP_GET) | BIT(HTTP_POST));

static uint16_t port = CONFIG_DOG_DOOR_HTTP_PORT;
HTTP_SERVICE_DEFINE(dog_door_service, NULL, &port, CONFIG_HTTP_SERVER_MAX_CLIENTS,
		    10, NULL, NULL, NULL);

HTTP_RESOURCE_DEFINE(index_resource, dog_door_service, "/", &index_detail);
HTTP_RESOURCE_DEFINE(setup_resource, dog_door_service, "/setup", &index_detail);
HTTP_RESOURCE_DEFINE(css_resource, dog_door_service, "/app.css", &css_detail);
HTTP_RESOURCE_DEFINE(js_resource, dog_door_service, "/app.js", &js_detail);
HTTP_RESOURCE_DEFINE(state_resource, dog_door_service, "/api/state", &state_detail);
HTTP_RESOURCE_DEFINE(door_resource, dog_door_service, "/api/door", &door_detail);
HTTP_RESOURCE_DEFINE(led_resource, dog_door_service, "/api/led", &led_detail);
HTTP_RESOURCE_DEFINE(wifi_scan_resource, dog_door_service, "/api/wifi/scan", &wifi_scan_detail);
HTTP_RESOURCE_DEFINE(wifi_resource, dog_door_service, "/api/wifi", &wifi_detail);
HTTP_RESOURCE_DEFINE(config_resource, dog_door_service, "/api/config", &config_detail);

int web_server_init(void)
{
	int ret = http_server_start();

	if (ret == 0) {
		LOG_INF("HTTP dashboard started on port %u", port);
	}
	return ret;
}
