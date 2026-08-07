/* SPDX-License-Identifier: Apache-2.0 */
#include "ota_manager.h"

#include "door_controller.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/app_version.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(ota, CONFIG_DOG_DOOR_LOG_LEVEL);

#define IMAGE_CONFIRM_DELAY K_SECONDS(30)
#define IMAGE_CONFIRM_RETRY K_SECONDS(10)
#define REBOOT_DELAY K_MSEC(750)
#define MINIMUM_IMAGE_SIZE 4096U

static struct flash_img_context flash_context;
static struct ota_snapshot state;
static struct k_work_delayable confirm_work;
static struct k_work_delayable reboot_work;
K_MUTEX_DEFINE(ota_lock);

static void confirm_handler(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);
	if (boot_is_img_confirmed()) {
		return;
	}

	ret = boot_write_img_confirmed();
	if (ret == 0) {
		LOG_INF("Firmware image confirmed after healthy startup");
	} else {
		LOG_ERR("Unable to confirm firmware image: %d", ret);
		k_work_reschedule(&confirm_work, IMAGE_CONFIRM_RETRY);
	}
}

static void reboot_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_INF("Rebooting into the uploaded firmware image");
	sys_reboot(SYS_REBOOT_COLD);
}

int ota_manager_init(void)
{
	memset(&state, 0, sizeof(state));
	strncpy(state.running_version, APP_VERSION_STRING,
		sizeof(state.running_version) - 1);
	k_work_init_delayable(&confirm_work, confirm_handler);
	k_work_init_delayable(&reboot_work, reboot_handler);

	if (!boot_is_img_confirmed()) {
		LOG_WRN("Running an unconfirmed firmware image");
	}
	return 0;
}

void ota_manager_mark_healthy(void)
{
	if (!boot_is_img_confirmed()) {
		LOG_INF("Startup healthy; firmware confirmation scheduled in 30 seconds");
		k_work_reschedule(&confirm_work, IMAGE_CONFIRM_DELAY);
	}
}

int ota_manager_begin(void)
{
	int ret;

	k_mutex_lock(&ota_lock, K_FOREVER);
	if (state.upload_in_progress || state.reboot_pending) {
		k_mutex_unlock(&ota_lock);
		return -EBUSY;
	}
	if (!boot_is_img_confirmed()) {
		k_mutex_unlock(&ota_lock);
		return -EAGAIN;
	}

	(void)door_controller_command(DOOR_COMMAND_STOP);
	memset(&flash_context, 0, sizeof(flash_context));
	ret = flash_img_init(&flash_context);
	if (ret == 0) {
		state.upload_in_progress = true;
		state.bytes_received = 0;
		state.update_version[0] = '\0';
		LOG_INF("Firmware upload started; door motion stopped");
	}
	k_mutex_unlock(&ota_lock);
	return ret;
}

int ota_manager_write(const uint8_t *data, size_t length, bool final)
{
	struct mcuboot_img_header header;
	size_t bytes_written;
	int ret;

	k_mutex_lock(&ota_lock, K_FOREVER);
	if (!state.upload_in_progress) {
		k_mutex_unlock(&ota_lock);
		return -EPIPE;
	}

	ret = flash_img_buffered_write(&flash_context, data, length, final);
	bytes_written = flash_img_bytes_written(&flash_context);
	state.bytes_received = bytes_written;
	if (ret != 0 || !final) {
		k_mutex_unlock(&ota_lock);
		return ret;
	}

	ret = boot_read_bank_header(flash_img_get_upload_slot(), &header, sizeof(header));
	if (ret != 0 || header.mcuboot_version != 1U ||
	    header.h.v1.image_size < MINIMUM_IMAGE_SIZE ||
	    header.h.v1.image_size > bytes_written) {
		LOG_ERR("Uploaded file is not a valid MCUboot image");
		state.upload_in_progress = false;
		k_mutex_unlock(&ota_lock);
		return -EBADMSG;
	}

	snprintk(state.update_version, sizeof(state.update_version), "%u.%u.%u+%u",
		header.h.v1.sem_ver.major, header.h.v1.sem_ver.minor,
		header.h.v1.sem_ver.revision, header.h.v1.sem_ver.build_num);
	ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (ret == 0) {
		state.reboot_pending = true;
		LOG_INF("Firmware %s staged for test boot (%zu bytes)",
			state.update_version, bytes_written);
	}
	state.upload_in_progress = false;
	k_mutex_unlock(&ota_lock);
	return ret;
}

void ota_manager_abort(void)
{
	k_mutex_lock(&ota_lock, K_FOREVER);
	if (flash_context.flash_area != NULL) {
		flash_area_close(flash_context.flash_area);
		flash_context.flash_area = NULL;
	}
	state.upload_in_progress = false;
	state.bytes_received = 0;
	k_mutex_unlock(&ota_lock);
	LOG_WRN("Firmware upload aborted");
}

void ota_manager_response_sent(void)
{
	k_mutex_lock(&ota_lock, K_FOREVER);
	if (state.reboot_pending) {
		k_work_reschedule(&reboot_work, REBOOT_DELAY);
	}
	k_mutex_unlock(&ota_lock);
}

void ota_manager_get(struct ota_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return;
	}
	k_mutex_lock(&ota_lock, K_FOREVER);
	*snapshot = state;
	snapshot->image_confirmed = boot_is_img_confirmed();
	k_mutex_unlock(&ota_lock);
}
