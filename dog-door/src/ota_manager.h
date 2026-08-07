/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DOG_DOOR_OTA_MANAGER_H_
#define DOG_DOOR_OTA_MANAGER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DOG_DOOR_OTA_VERSION_MAX 25

struct ota_snapshot {
	bool image_confirmed;
	bool upload_in_progress;
	bool reboot_pending;
	size_t bytes_received;
	char running_version[DOG_DOOR_OTA_VERSION_MAX];
	char update_version[DOG_DOOR_OTA_VERSION_MAX];
};

int ota_manager_init(void);
void ota_manager_mark_healthy(void);
int ota_manager_begin(void);
int ota_manager_write(const uint8_t *data, size_t length, bool final);
void ota_manager_abort(void);
void ota_manager_response_sent(void);
void ota_manager_get(struct ota_snapshot *snapshot);

#endif /* DOG_DOOR_OTA_MANAGER_H_ */
