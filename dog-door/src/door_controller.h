/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DOG_DOOR_CONTROLLER_H_
#define DOG_DOOR_CONTROLLER_H_

#include <stdbool.h>
#include <stdint.h>

enum door_state {
	DOOR_STATE_UNKNOWN,
	DOOR_STATE_HOMING,
	DOOR_STATE_OPENING,
	DOOR_STATE_CLOSING,
	DOOR_STATE_OPEN,
	DOOR_STATE_CLOSED,
	DOOR_STATE_STOPPED,
	DOOR_STATE_FAULT,
};

enum door_command {
	DOOR_COMMAND_OPEN,
	DOOR_COMMAND_CLOSE,
	DOOR_COMMAND_STOP,
	DOOR_COMMAND_HOME,
};

struct door_snapshot {
	enum door_state state;
	bool upper_limit;
	bool lower_limit;
	bool actuator_armed;
	bool motor_ready;
	char fault[64];
	uint32_t generation;
};

int door_controller_init(void);
int door_controller_command(enum door_command command);
void door_controller_get(struct door_snapshot *snapshot);
const char *door_state_name(enum door_state state);

#endif /* DOG_DOOR_CONTROLLER_H_ */
