/* SPDX-License-Identifier: Apache-2.0 */
#include "network_manager.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_credentials.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(network, CONFIG_DOG_DOOR_LOG_LEVEL);

#define WIFI_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT | \
		     NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE | \
		     NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_DISABLE_RESULT)

#define RECONNECT_INITIAL_DELAY_SECONDS 2U
#define RECONNECT_MAX_DELAY_SECONDS 30U
#define CONNECT_ATTEMPT_TIMEOUT_SECONDS 35U
#define FALLBACK_AP_DELAY K_SECONDS(30)

static struct network_snapshot state;
static struct net_if *sta_iface;
static struct net_if *ap_iface;
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static struct k_work_delayable startup_work;
static struct k_work_delayable fallback_ap_work;
static struct k_work_delayable reconnect_work;
static struct wifi_connect_req_params station_params;
static struct wifi_connect_req_params access_point_params;
static char connect_ssid[WIFI_SSID_MAX_LEN + 1];
static char connect_password[WIFI_CREDENTIALS_MAX_PASSWORD_LEN + 1];
static enum wifi_security_type connect_security = WIFI_SECURITY_TYPE_NONE;
static uint32_t reconnect_delay_seconds = RECONNECT_INITIAL_DELAY_SECONDS;
static bool restart_requested;
K_MUTEX_DEFINE(network_lock);

static void bump_generation(void)
{
	state.generation++;
}

static int enable_setup_ap(void)
{
	static struct net_in_addr ap_addr;
	static struct net_in_addr netmask;
	static bool address_configured;
	int ret;

	if (ap_iface == NULL) {
		return -ENODEV;
	}
	if (!address_configured) {
		ret = net_addr_pton(AF_INET, "192.168.4.1", &ap_addr);
		if (ret != 0) {
			return ret;
		}
		ret = net_addr_pton(AF_INET, "255.255.255.0", &netmask);
		if (ret != 0) {
			return ret;
		}
		net_if_ipv4_set_gw(ap_iface, &ap_addr);
		if (net_if_ipv4_addr_add(ap_iface, &ap_addr, NET_ADDR_MANUAL, 0) == NULL) {
			return -EIO;
		}
		(void)net_if_ipv4_set_netmask_by_addr(ap_iface, &ap_addr, &netmask);
		address_configured = true;
	}

	memset(&access_point_params, 0, sizeof(access_point_params));
	access_point_params.ssid = (const uint8_t *)CONFIG_DOG_DOOR_SETUP_AP_SSID;
	access_point_params.ssid_length = strlen(CONFIG_DOG_DOOR_SETUP_AP_SSID);
	access_point_params.psk = (const uint8_t *)CONFIG_DOG_DOOR_SETUP_AP_PASSWORD;
	access_point_params.psk_length = strlen(CONFIG_DOG_DOOR_SETUP_AP_PASSWORD);
	access_point_params.security = access_point_params.psk_length > 0 ?
		WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE;
	access_point_params.channel = WIFI_CHANNEL_ANY;
	access_point_params.band = WIFI_FREQ_BAND_2_4_GHZ;

	ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, ap_iface, &access_point_params,
		       sizeof(access_point_params));
	if (ret == 0) {
		struct net_in_addr pool = ap_addr;

		pool.s4_addr[3] += 10;
		ret = net_dhcpv4_server_start(ap_iface, &pool);
		if (ret != 0 && ret != -EALREADY) {
			LOG_WRN("DHCP server start failed: %d", ret);
		}
		LOG_INF("Setup network: %s at http://192.168.4.1/",
			CONFIG_DOG_DOOR_SETUP_AP_SSID);
	}
	return ret;
}

static int connect_current(void)
{
	int ret;

	if (sta_iface == NULL || connect_ssid[0] == '\0') {
		return -EINVAL;
	}
	memset(&station_params, 0, sizeof(station_params));
	station_params.ssid = (const uint8_t *)connect_ssid;
	station_params.ssid_length = strlen(connect_ssid);
	station_params.psk = (const uint8_t *)connect_password;
	station_params.psk_length = strlen(connect_password);
	station_params.security = connect_security;
	station_params.channel = WIFI_CHANNEL_ANY;
	station_params.band = WIFI_FREQ_BAND_2_4_GHZ;
	station_params.timeout = 30;

	k_mutex_lock(&network_lock, K_FOREVER);
	state.connecting = true;
	strncpy(state.ssid, connect_ssid, sizeof(state.ssid) - 1);
	bump_generation();
	k_mutex_unlock(&network_lock);
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &station_params,
		       sizeof(station_params));
	if (ret != 0) {
		k_mutex_lock(&network_lock, K_FOREVER);
		state.connecting = false;
		bump_generation();
		k_mutex_unlock(&network_lock);
	} else {
		/* Recover if the driver never delivers a connection result. */
		k_work_reschedule(&reconnect_work,
				  K_SECONDS(CONNECT_ATTEMPT_TIMEOUT_SECONDS));
	}
	return ret;
}

struct stored_ssid {
	char value[WIFI_SSID_MAX_LEN + 1];
	size_t length;
};

static void first_ssid(void *cb_arg, const char *ssid, size_t ssid_len)
{
	struct stored_ssid *found = cb_arg;

	if (found->length != 0 || ssid_len == 0 || ssid_len > WIFI_SSID_MAX_LEN) {
		return;
	}
	memcpy(found->value, ssid, ssid_len);
	found->value[ssid_len] = '\0';
	found->length = ssid_len;
}

static int connect_stored(void)
{
	struct stored_ssid stored = {0};
	struct wifi_credentials_personal credentials;
	int ret;

	wifi_credentials_for_each_ssid(first_ssid, &stored);
	if (stored.length == 0) {
		return -ENOENT;
	}
	ret = wifi_credentials_get_by_ssid_personal_struct(stored.value, stored.length,
						   &credentials);
	if (ret != 0) {
		return ret;
	}
	memcpy(connect_ssid, stored.value, stored.length + 1);
	memset(connect_password, 0, sizeof(connect_password));
	memcpy(connect_password, credentials.password,
	       MIN(credentials.password_len, sizeof(connect_password) - 1));
	connect_security = credentials.header.type;
	return connect_current();
}

static void fallback_ap_handler(struct k_work *work)
{
	bool ap_active;
	bool connected;

	ARG_UNUSED(work);
	k_mutex_lock(&network_lock, K_FOREVER);
	ap_active = state.ap_active;
	connected = state.connected;
	k_mutex_unlock(&network_lock);
	if (!ap_active && !connected) {
		(void)enable_setup_ap();
	}
}

static void schedule_reconnect(void)
{
	uint32_t delay_seconds;
	bool retry;

	k_mutex_lock(&network_lock, K_FOREVER);
	retry = connect_ssid[0] != '\0' && !state.connected;
	delay_seconds = reconnect_delay_seconds;
	if (retry) {
		reconnect_delay_seconds = MIN(reconnect_delay_seconds * 2U,
					      RECONNECT_MAX_DELAY_SECONDS);
	}
	k_mutex_unlock(&network_lock);

	if (retry) {
		LOG_INF("Wi-Fi reconnect scheduled in %u seconds", delay_seconds);
		k_work_reschedule(&reconnect_work, K_SECONDS(delay_seconds));
	}
}

static void reconnect_handler(struct k_work *work)
{
	bool connected;
	bool connecting;
	bool have_credentials;
	bool restart;
	int ret;

	ARG_UNUSED(work);
	k_mutex_lock(&network_lock, K_FOREVER);
	restart = restart_requested;
	restart_requested = false;
	connected = state.connected;
	connecting = state.connecting;
	have_credentials = connect_ssid[0] != '\0';
	if (restart) {
		state.connected = false;
		state.connecting = false;
		state.ip_address[0] = '\0';
		bump_generation();
	}
	k_mutex_unlock(&network_lock);

	if (restart) {
		LOG_INF("Restarting Wi-Fi station with updated configuration");
		ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta_iface, NULL, 0);
		if (ret != 0) {
			LOG_WRN("Wi-Fi disconnect before reconfiguration failed: %d", ret);
		}
		k_work_reschedule(&reconnect_work, K_SECONDS(1));
		return;
	}

	if (connected || !have_credentials) {
		return;
	}

	if (connecting) {
		LOG_WRN("Wi-Fi connection attempt timed out; restarting station");
		(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta_iface, NULL, 0);
		k_mutex_lock(&network_lock, K_FOREVER);
		state.connecting = false;
		bump_generation();
		k_mutex_unlock(&network_lock);
		schedule_reconnect();
		return;
	}

	LOG_INF("Retrying Wi-Fi connection to %s", connect_ssid);
	ret = connect_current();
	if (ret != 0) {
		LOG_WRN("Wi-Fi reconnect request failed: %d", ret);
		if (ret == -EALREADY) {
			(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta_iface, NULL, 0);
		}
		schedule_reconnect();
	}
}

static void insert_scan_result(const struct wifi_scan_result *result)
{
	size_t index;
	struct network_scan_entry entry = {0};

	if (result->ssid_length == 0 || result->ssid_length > WIFI_SSID_MAX_LEN) {
		return;
	}
	memcpy(entry.ssid, result->ssid, result->ssid_length);
	entry.ssid[result->ssid_length] = '\0';
	entry.rssi = result->rssi;
	entry.channel = result->channel;
	entry.security = result->security;

	for (index = 0; index < state.scan_count; index++) {
		if (strcmp(state.scan[index].ssid, entry.ssid) == 0) {
			if (entry.rssi > state.scan[index].rssi) {
				state.scan[index] = entry;
			}
			return;
		}
	}
	if (state.scan_count < ARRAY_SIZE(state.scan)) {
		state.scan[state.scan_count++] = entry;
	}
}

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t event, struct net_if *iface)
{
	const struct wifi_status *status = cb->info;
	struct wifi_ps_params power_save = {
		.enabled = WIFI_PS_DISABLED,
	};
	bool request_fallback = false;
	bool request_reconnect = false;
	bool disable_power_save = false;
	bool start_dhcp = false;
	bool stop_http_server = false;
	int ret;

	ARG_UNUSED(iface);
	k_mutex_lock(&network_lock, K_FOREVER);
	switch (event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		state.connecting = false;
		if (status != NULL && status->status == 0) {
			LOG_INF("Wi-Fi link connected to %s", state.ssid);
			disable_power_save = true;
			/*
			 * IPv4 can be reported before this event. Do not restart DHCP or
			 * mark an already-addressed station as connecting again.
			 */
			if (!state.connected) {
				state.connecting = true;
				start_dhcp = true;
			}
		} else {
			state.connected = false;
			state.ip_address[0] = '\0';
			request_fallback = true;
			request_reconnect = true;
			LOG_WRN("Wi-Fi connection failed: %d", status ? status->status : -1);
		}
		bump_generation();
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		state.connected = false;
		state.connecting = false;
		state.ip_address[0] = '\0';
		request_fallback = true;
		request_reconnect = true;
		stop_http_server = true;
		bump_generation();
		LOG_WRN("Wi-Fi disconnected");
		break;
	case NET_EVENT_WIFI_SCAN_RESULT:
		if (cb->info != NULL) {
			insert_scan_result((const struct wifi_scan_result *)cb->info);
		}
		break;
	case NET_EVENT_WIFI_SCAN_DONE:
		state.scanning = false;
		bump_generation();
		break;
	case NET_EVENT_WIFI_AP_ENABLE_RESULT:
		state.ap_active = status == NULL || status->status == 0;
		bump_generation();
		break;
	case NET_EVENT_WIFI_AP_DISABLE_RESULT:
		state.ap_active = false;
		bump_generation();
		break;
	default:
		break;
	}
	k_mutex_unlock(&network_lock);
	if (stop_http_server) {
		ret = http_server_stop();
		if (ret != 0 && ret != -EALREADY) {
			LOG_WRN("Could not stop HTTP server after Wi-Fi loss: %d", ret);
		}
	}
	if (disable_power_save) {
		ret = net_mgmt(NET_REQUEST_WIFI_PS, sta_iface, &power_save,
			       sizeof(power_save));
		if (ret != 0) {
			LOG_WRN("Could not disable Wi-Fi power save: %d", ret);
		}
	}
	if (start_dhcp) {
		net_dhcpv4_start(sta_iface);
	}
	if (request_reconnect) {
		schedule_reconnect();
	}
	if (request_fallback) {
		/* Do not switch into AP+STA mode for a brief station outage. */
		k_work_schedule(&fallback_ap_work, FALLBACK_AP_DELAY);
	}
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t event, struct net_if *iface)
{
	struct net_in_addr *address;
	bool ap_active;
	int ret;

	ARG_UNUSED(cb);
	if (event != NET_EVENT_IPV4_ADDR_ADD || iface != sta_iface) {
		return;
	}
	address = net_if_ipv4_get_global_addr(sta_iface, NET_ADDR_PREFERRED);
	if (address == NULL) {
		return;
	}
	k_mutex_lock(&network_lock, K_FOREVER);
	(void)net_addr_ntop(AF_INET, address, state.ip_address, sizeof(state.ip_address));
	state.connected = true;
	state.connecting = false;
	reconnect_delay_seconds = RECONNECT_INITIAL_DELAY_SECONDS;
	ap_active = state.ap_active;
	bump_generation();
	LOG_INF("Dashboard available at http://%s/", state.ip_address);
	k_mutex_unlock(&network_lock);
	ret = http_server_start();
	if (ret != 0 && ret != -EALREADY) {
		LOG_WRN("Could not start HTTP server after Wi-Fi recovery: %d", ret);
	}
	k_work_cancel_delayable(&reconnect_work);
	k_work_cancel_delayable(&fallback_ap_work);
	if (ap_active) {
		(void)net_dhcpv4_server_stop(ap_iface);
		(void)net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, ap_iface, NULL, 0);
	}
}

static void startup_handler(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);
	ret = connect_stored();
	if (ret == -ENOENT) {
		LOG_INF("No Wi-Fi credentials stored; entering setup mode");
	} else if (ret != 0) {
		LOG_WRN("Stored Wi-Fi connection failed to start: %d", ret);
	}
	if (ret != 0) {
		if (ret == -ENOENT) {
			(void)enable_setup_ap();
		} else {
			schedule_reconnect();
			k_work_schedule(&fallback_ap_work, FALLBACK_AP_DELAY);
		}
	}
}

int network_manager_init(void)
{
	sta_iface = net_if_get_wifi_sta();
	ap_iface = net_if_get_wifi_sap();
	if (sta_iface == NULL || ap_iface == NULL) {
		LOG_ERR("AP+STA Wi-Fi interfaces are not available");
		return -ENODEV;
	}
	k_work_init_delayable(&startup_work, startup_handler);
	k_work_init_delayable(&fallback_ap_work, fallback_ap_handler);
	k_work_init_delayable(&reconnect_work, reconnect_handler);
	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler, WIFI_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);
	k_work_schedule(&startup_work, K_SECONDS(2));
	return 0;
}

int network_manager_scan(void)
{
	int ret;

	k_mutex_lock(&network_lock, K_FOREVER);
	if (state.scanning) {
		k_mutex_unlock(&network_lock);
		return -EALREADY;
	}
	state.scan_count = 0;
	state.scanning = true;
	bump_generation();
	k_mutex_unlock(&network_lock);
	ret = net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface, NULL, 0);
	if (ret != 0) {
		k_mutex_lock(&network_lock, K_FOREVER);
		state.scanning = false;
		bump_generation();
		k_mutex_unlock(&network_lock);
	}
	return ret;
}

int network_manager_set_wifi(const char *ssid, const char *password,
			     enum wifi_security_type security)
{
	size_t ssid_len;
	size_t password_len;
	int ret;

	if (ssid == NULL || password == NULL) {
		return -EINVAL;
	}
	ssid_len = strnlen(ssid, WIFI_SSID_MAX_LEN + 1);
	password_len = strnlen(password, WIFI_CREDENTIALS_MAX_PASSWORD_LEN + 1);
	if (ssid_len == 0 || ssid_len > WIFI_SSID_MAX_LEN ||
	    password_len > WIFI_CREDENTIALS_MAX_PASSWORD_LEN ||
	    (security != WIFI_SECURITY_TYPE_NONE && password_len < 8)) {
		return -EINVAL;
	}
	if (security == WIFI_SECURITY_TYPE_UNKNOWN) {
		security = password_len == 0 ? WIFI_SECURITY_TYPE_NONE : WIFI_SECURITY_TYPE_PSK;
	}

	(void)wifi_credentials_delete_all();
	ret = wifi_credentials_set_personal(ssid, ssid_len, security, NULL, 0,
					    password, password_len,
					    WIFI_CREDENTIALS_FLAG_2_4GHz | WIFI_CREDENTIALS_FLAG_FAVORITE,
					    WIFI_CHANNEL_ANY, 30);
	if (ret != 0) {
		return ret;
	}
	k_work_cancel_delayable(&fallback_ap_work);
	k_mutex_lock(&network_lock, K_FOREVER);
	memcpy(connect_ssid, ssid, ssid_len + 1);
	memset(connect_password, 0, sizeof(connect_password));
	memcpy(connect_password, password, password_len);
	connect_security = security;
	reconnect_delay_seconds = RECONNECT_INITIAL_DELAY_SECONDS;
	restart_requested = state.connected || state.connecting;
	k_mutex_unlock(&network_lock);

	/* Let the HTTP response leave before a live station is disconnected. */
	ret = k_work_reschedule(&reconnect_work, K_MSEC(500));
	if (ret < 0) {
		LOG_ERR("Could not schedule Wi-Fi reconfiguration: %d", ret);
		return ret;
	}
	LOG_INF("Wi-Fi reconfiguration scheduled");
	return 0;
}

void network_manager_get(struct network_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return;
	}
	k_mutex_lock(&network_lock, K_FOREVER);
	*snapshot = state;
	k_mutex_unlock(&network_lock);
}

bool network_manager_is_connected(void)
{
	bool connected;

	k_mutex_lock(&network_lock, K_FOREVER);
	connected = state.connected;
	k_mutex_unlock(&network_lock);
	return connected;
}
