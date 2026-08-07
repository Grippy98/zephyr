/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DOG_DOOR_MQTT_BRIDGE_H_
#define DOG_DOOR_MQTT_BRIDGE_H_

#include <stdbool.h>
#include <stdint.h>

struct mqtt_bridge_snapshot {
	bool enabled;
	bool connected;
	char broker[65];
	uint32_t generation;
};

int mqtt_bridge_init(void);
void mqtt_bridge_get(struct mqtt_bridge_snapshot *snapshot);

#endif /* DOG_DOOR_MQTT_BRIDGE_H_ */
