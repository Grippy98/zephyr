/* SPDX-License-Identifier: Apache-2.0 */
#include "mqtt_bridge.h"

#include "app_config.h"
#include "door_controller.h"
#include "led_controller.h"
#include "network_manager.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>

LOG_MODULE_REGISTER(mqtt_bridge, CONFIG_DOG_DOOR_LOG_LEVEL);

#define MQTT_RX_BUFFER_SIZE 2048
#define MQTT_TX_BUFFER_SIZE 2048
#define MQTT_PAYLOAD_MAX 384
#define MQTT_RECONNECT_SECONDS 5

#define TOPIC_AVAILABILITY "dogdoor/availability"
#define TOPIC_COVER_COMMAND "dogdoor/cover/set"
#define TOPIC_COVER_STATE "dogdoor/cover/state"
#define TOPIC_LIGHT_COMMAND "dogdoor/light/set"
#define TOPIC_LIGHT_STATE "dogdoor/light/state"
#define TOPIC_LIMITS "dogdoor/limits"
#define TOPIC_DIAGNOSTIC "dogdoor/state"

static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buffer[MQTT_RX_BUFFER_SIZE];
static uint8_t tx_buffer[MQTT_TX_BUFFER_SIZE];
static uint8_t incoming_payload[MQTT_PAYLOAD_MAX];
static struct mqtt_bridge_snapshot state;
static struct app_config_data active_config;
static struct mqtt_utf8 username;
static struct mqtt_utf8 password;
static atomic_t session_connected;
static atomic_t session_ready;
K_MUTEX_DEFINE(mqtt_state_lock);

static bool topic_equals(const struct mqtt_utf8 *topic, const char *expected)
{
	return topic->size == strlen(expected) &&
	       memcmp(topic->utf8, expected, topic->size) == 0;
}

static int publish_text(const char *topic, const char *payload, bool retained)
{
	struct mqtt_publish_param param = {0};

	param.message.topic.topic.utf8 = (uint8_t *)topic;
	param.message.topic.topic.size = strlen(topic);
	param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	param.message.payload.data = (uint8_t *)payload;
	param.message.payload.len = strlen(payload);
	param.message_id = sys_rand16_get();
	param.retain_flag = retained ? 1U : 0U;
	return mqtt_publish(&client, &param);
}

static bool json_uint(const char *payload_text, const char *key, unsigned int *value)
{
	char needle[20];
	const char *field;
	const char *separator;

	snprintk(needle, sizeof(needle), "\"%s\"", key);
	field = strstr(payload_text, needle);
	if (field == NULL) {
		return false;
	}
	separator = strchr(field + strlen(needle), ':');
	return separator != NULL && sscanf(separator + 1, "%u", value) == 1;
}

static void handle_light_command(const char *payload_text)
{
	unsigned int red = 255;
	unsigned int green = 255;
	unsigned int blue = 255;
	unsigned int brightness = 255;

	if (strstr(payload_text, "\"state\":\"OFF\"") != NULL ||
	    strstr(payload_text, "\"state\": \"OFF\"") != NULL) {
		(void)led_controller_set(LED_MODE_OFF, 0, 0, 0, 0);
		return;
	}
	(void)json_uint(payload_text, "r", &red);
	(void)json_uint(payload_text, "g", &green);
	(void)json_uint(payload_text, "b", &blue);
	(void)json_uint(payload_text, "brightness", &brightness);
	(void)led_controller_set(LED_MODE_SOLID, MIN(red, 255U), MIN(green, 255U),
				 MIN(blue, 255U), MIN((brightness * 100U) / 255U, 100U));
}

static void handle_publish(const struct mqtt_evt *event)
{
	const struct mqtt_publish_param *publish = &event->param.publish;
	int bytes;

	bytes = mqtt_read_publish_payload_blocking(&client, incoming_payload,
						   sizeof(incoming_payload) - 1);
	if (bytes < 0) {
		LOG_WRN("MQTT payload read failed: %d", bytes);
		return;
	}
	incoming_payload[bytes] = '\0';

	if (topic_equals(&publish->message.topic.topic, TOPIC_COVER_COMMAND)) {
		if (strcmp((char *)incoming_payload, "OPEN") == 0 ||
		    strcmp((char *)incoming_payload, "open") == 0) {
			(void)door_controller_command(DOOR_COMMAND_OPEN);
		} else if (strcmp((char *)incoming_payload, "CLOSE") == 0 ||
			   strcmp((char *)incoming_payload, "close") == 0) {
			(void)door_controller_command(DOOR_COMMAND_CLOSE);
		} else if (strcmp((char *)incoming_payload, "STOP") == 0 ||
			   strcmp((char *)incoming_payload, "stop") == 0) {
			(void)door_controller_command(DOOR_COMMAND_STOP);
		}
	} else if (topic_equals(&publish->message.topic.topic, TOPIC_LIGHT_COMMAND)) {
		handle_light_command((char *)incoming_payload);
	}

	if (publish->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
		const struct mqtt_puback_param ack = { .message_id = publish->message_id };

		(void)mqtt_publish_qos1_ack(&client, &ack);
	}
}

static void mqtt_event_handler(struct mqtt_client *const context,
			       const struct mqtt_evt *event)
{
	ARG_UNUSED(context);
	switch (event->type) {
	case MQTT_EVT_CONNACK:
		if (event->result == 0) {
			atomic_set(&session_connected, 1);
			atomic_set(&session_ready, 1);
		} else {
			LOG_WRN("MQTT connection refused: %d", event->result);
		}
		break;
	case MQTT_EVT_DISCONNECT:
		atomic_clear(&session_connected);
		break;
	case MQTT_EVT_PUBLISH:
		handle_publish(event);
		break;
	default:
		break;
	}
}

static int resolve_broker(const char *host, uint16_t port)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *result;
	char service[6];
	int ret;

	snprintk(service, sizeof(service), "%u", port);
	ret = zsock_getaddrinfo(host, service, &hints, &result);
	if (ret != 0 || result == NULL) {
		LOG_WRN("Unable to resolve MQTT broker %s: %d", host, ret);
		return -EHOSTUNREACH;
	}
	memcpy(&broker, result->ai_addr, result->ai_addrlen);
	zsock_freeaddrinfo(result);
	return 0;
}

static void init_client(void)
{
	static const char client_id[] = "maker-esp32-dog-door";
	static const char will_payload[] = "offline";
	static struct mqtt_topic will_topic = {
		.topic = { .utf8 = (uint8_t *)TOPIC_AVAILABILITY,
			   .size = sizeof(TOPIC_AVAILABILITY) - 1 },
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
	};
	static struct mqtt_utf8 will_message = {
		.utf8 = (uint8_t *)will_payload,
		.size = sizeof(will_payload) - 1,
	};

	mqtt_client_init(&client);
	client.broker = &broker;
	client.evt_cb = mqtt_event_handler;
	client.client_id.utf8 = (uint8_t *)client_id;
	client.client_id.size = sizeof(client_id) - 1;
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;
	client.keepalive = 30;
	client.will_topic = &will_topic;
	client.will_message = &will_message;
	if (active_config.mqtt_username[0] != '\0') {
		username.utf8 = (uint8_t *)active_config.mqtt_username;
		username.size = strlen(active_config.mqtt_username);
		client.user_name = &username;
	}
	if (active_config.mqtt_password[0] != '\0') {
		password.utf8 = (uint8_t *)active_config.mqtt_password;
		password.size = strlen(active_config.mqtt_password);
		client.password = &password;
	}
}

static int subscribe_commands(void)
{
	struct mqtt_topic topics[] = {
		{ .topic = { .utf8 = (uint8_t *)TOPIC_COVER_COMMAND,
			     .size = sizeof(TOPIC_COVER_COMMAND) - 1 },
		  .qos = MQTT_QOS_0_AT_MOST_ONCE },
		{ .topic = { .utf8 = (uint8_t *)TOPIC_LIGHT_COMMAND,
			     .size = sizeof(TOPIC_LIGHT_COMMAND) - 1 },
		  .qos = MQTT_QOS_0_AT_MOST_ONCE },
	};
	struct mqtt_subscription_list list = {
		.list = topics,
		.list_count = ARRAY_SIZE(topics),
		.message_id = sys_rand16_get(),
	};

	return mqtt_subscribe(&client, &list);
}

static void publish_discovery(void)
{
	char payload[1500];
	const char *device = "\"device\":{\"identifiers\":[\"maker_esp32_dogdoor\"],"
		"\"manufacturer\":\"NULLLAB\",\"model\":\"Maker ESP32\","
		"\"name\":\"Dog Door\"}";

	snprintk(payload, sizeof(payload),
		 "{\"name\":\"%s\",\"unique_id\":\"maker_esp32_dogdoor_cover\","
		 "\"command_topic\":\"%s\",\"state_topic\":\"%s\","
		 "\"availability_topic\":\"%s\",\"payload_open\":\"OPEN\","
		 "\"payload_close\":\"CLOSE\",\"payload_stop\":\"STOP\","
		 "\"state_open\":\"open\",\"state_closed\":\"closed\","
		 "\"state_opening\":\"opening\",\"state_closing\":\"closing\","
		 "\"state_stopped\":\"stopped\",%s}", active_config.device_name,
		 TOPIC_COVER_COMMAND, TOPIC_COVER_STATE, TOPIC_AVAILABILITY, device);
	(void)publish_text("homeassistant/cover/maker_esp32_dogdoor/config", payload, true);

	snprintk(payload, sizeof(payload),
		 "{\"name\":\"Door LEDs\",\"unique_id\":\"maker_esp32_dogdoor_light\","
		 "\"schema\":\"json\",\"command_topic\":\"%s\",\"state_topic\":\"%s\","
		 "\"availability_topic\":\"%s\",\"brightness\":true,"
		 "\"supported_color_modes\":[\"rgb\"],%s}",
		 TOPIC_LIGHT_COMMAND, TOPIC_LIGHT_STATE, TOPIC_AVAILABILITY, device);
	(void)publish_text("homeassistant/light/maker_esp32_dogdoor/config", payload, true);

	for (int upper = 0; upper < 2; upper++) {
		const char *which = upper ? "upper" : "lower";
		const char *name = upper ? "Upper limit" : "Lower limit";

		snprintk(payload, sizeof(payload),
			 "{\"name\":\"%s\",\"unique_id\":\"maker_esp32_dogdoor_%s_limit\","
			 "\"state_topic\":\"%s\","
			 "\"value_template\":\"{{ 'ON' if value_json.%s else 'OFF' }}\","
			 "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
			 "\"availability_topic\":\"%s\",%s}",
			 name, which, TOPIC_LIMITS, which, TOPIC_AVAILABILITY, device);
		char topic[96];

		snprintk(topic, sizeof(topic), "homeassistant/binary_sensor/maker_esp32_dogdoor_%s/config",
			 which);
		(void)publish_text(topic, payload, true);
	}
}

static void publish_state(void)
{
	struct door_snapshot door;
	struct led_snapshot led;
	char payload[256];
	const char *cover_state;

	door_controller_get(&door);
	led_controller_get(&led);
	cover_state = door_state_name(door.state);
	if (door.state == DOOR_STATE_HOMING) {
		cover_state = "opening";
	} else if (door.state == DOOR_STATE_UNKNOWN || door.state == DOOR_STATE_FAULT) {
		cover_state = "stopped";
	}
	(void)publish_text(TOPIC_COVER_STATE, cover_state, true);
	snprintk(payload, sizeof(payload), "{\"upper\":%s,\"lower\":%s}",
		 door.upper_limit ? "true" : "false", door.lower_limit ? "true" : "false");
	(void)publish_text(TOPIC_LIMITS, payload, true);
	snprintk(payload, sizeof(payload),
		 "{\"state\":\"%s\",\"brightness\":%u,\"color\":{\"r\":%u,\"g\":%u,\"b\":%u}}",
		 led.mode == LED_MODE_OFF ? "OFF" : "ON", (led.brightness * 255U) / 100U,
		 led.red, led.green, led.blue);
	(void)publish_text(TOPIC_LIGHT_STATE, payload, true);
	snprintk(payload, sizeof(payload),
		 "{\"door\":\"%s\",\"fault\":\"%s\",\"actuator_armed\":%s}",
		 door_state_name(door.state), door.fault, door.actuator_armed ? "true" : "false");
	(void)publish_text(TOPIC_DIAGNOSTIC, payload, true);
}

static int connect_session(void)
{
	struct zsock_pollfd descriptor;
	int ret;

	ret = resolve_broker(active_config.mqtt_host, active_config.mqtt_port);
	if (ret != 0) {
		return ret;
	}
	init_client();
	atomic_clear(&session_connected);
	atomic_clear(&session_ready);
	ret = mqtt_connect(&client);
	if (ret != 0) {
		return ret;
	}
	descriptor.fd = client.transport.tcp.sock;
	descriptor.events = ZSOCK_POLLIN;
	ret = zsock_poll(&descriptor, 1, 5000);
	if (ret > 0) {
		ret = mqtt_input(&client);
	}
	if (!atomic_get(&session_connected)) {
		mqtt_abort(&client);
		return ret < 0 ? ret : -ECONNREFUSED;
	}
	return 0;
}

static void mqtt_thread(void)
{
	uint32_t last_door_generation = UINT32_MAX;
	uint32_t last_led_generation = UINT32_MAX;

	while (true) {
		struct door_snapshot door;
		struct led_snapshot led;
		struct zsock_pollfd descriptor;
		int ret;

		app_config_get(&active_config);
		k_mutex_lock(&mqtt_state_lock, K_FOREVER);
		state.enabled = active_config.mqtt_enabled;
		strncpy(state.broker, active_config.mqtt_host, sizeof(state.broker) - 1);
		k_mutex_unlock(&mqtt_state_lock);
		if (!active_config.mqtt_enabled || active_config.mqtt_host[0] == '\0' ||
		    !network_manager_is_connected()) {
			k_sleep(K_SECONDS(2));
			continue;
		}

		ret = connect_session();
		if (ret != 0) {
			LOG_WRN("MQTT connection failed: %d", ret);
			k_sleep(K_SECONDS(MQTT_RECONNECT_SECONDS));
			continue;
		}
		k_mutex_lock(&mqtt_state_lock, K_FOREVER);
		state.connected = true;
		state.generation++;
		k_mutex_unlock(&mqtt_state_lock);
		(void)subscribe_commands();
		(void)publish_text(TOPIC_AVAILABILITY, "online", true);
		publish_discovery();
		publish_state();

		descriptor.fd = client.transport.tcp.sock;
		descriptor.events = ZSOCK_POLLIN;
		while (atomic_get(&session_connected) && network_manager_is_connected()) {
			ret = zsock_poll(&descriptor, 1, 500);
			if (ret > 0) {
				ret = mqtt_input(&client);
				if (ret != 0) {
					break;
				}
			}
			ret = mqtt_live(&client);
			if (ret != 0 && ret != -EAGAIN) {
				break;
			}
			door_controller_get(&door);
			led_controller_get(&led);
			if (door.generation != last_door_generation ||
			    led.generation != last_led_generation) {
				publish_state();
				last_door_generation = door.generation;
				last_led_generation = led.generation;
			}
		}
		mqtt_abort(&client);
		atomic_clear(&session_connected);
		k_mutex_lock(&mqtt_state_lock, K_FOREVER);
		state.connected = false;
		state.generation++;
		k_mutex_unlock(&mqtt_state_lock);
		k_sleep(K_SECONDS(MQTT_RECONNECT_SECONDS));
	}
}

K_THREAD_DEFINE(mqtt_thread_id, 6144, mqtt_thread, NULL, NULL, NULL, 8, 0, 0);

int mqtt_bridge_init(void)
{
	return 0;
}

void mqtt_bridge_get(struct mqtt_bridge_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return;
	}
	k_mutex_lock(&mqtt_state_lock, K_FOREVER);
	*snapshot = state;
	k_mutex_unlock(&mqtt_state_lock);
}
