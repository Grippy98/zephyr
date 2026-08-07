/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DOG_DOOR_NETWORK_MANAGER_H_
#define DOG_DOOR_NETWORK_MANAGER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/net/wifi_mgmt.h>

#define DOG_DOOR_WIFI_SCAN_MAX 10

struct network_scan_entry {
	char ssid[WIFI_SSID_MAX_LEN + 1];
	int8_t rssi;
	uint8_t channel;
	enum wifi_security_type security;
};

struct network_snapshot {
	bool ap_active;
	bool connected;
	bool connecting;
	bool scanning;
	char ssid[WIFI_SSID_MAX_LEN + 1];
	char ip_address[NET_IPV4_ADDR_LEN];
	int8_t rssi;
	struct network_scan_entry scan[DOG_DOOR_WIFI_SCAN_MAX];
	size_t scan_count;
	uint32_t generation;
};

int network_manager_init(void);
int network_manager_scan(void);
int network_manager_set_wifi(const char *ssid, const char *password,
			     enum wifi_security_type security);
void network_manager_get(struct network_snapshot *snapshot);
bool network_manager_is_connected(void);

#endif /* DOG_DOOR_NETWORK_MANAGER_H_ */
