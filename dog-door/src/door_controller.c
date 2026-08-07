/* SPDX-License-Identifier: Apache-2.0 */
#include "door_controller.h"

#include <errno.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(door, CONFIG_DOG_DOOR_LOG_LEVEL);

#define UPPER_LIMIT_NODE DT_ALIAS(upper_limit_switch)
#define LOWER_LIMIT_NODE DT_ALIAS(lower_limit_switch)
#define SERVO_NODE DT_NODELABEL(servo)
#define SAMPLE_INTERVAL_MS 10

BUILD_ASSERT(DT_NODE_HAS_STATUS(UPPER_LIMIT_NODE, okay), "upper-limit-switch alias is required");
BUILD_ASSERT(DT_NODE_HAS_STATUS(LOWER_LIMIT_NODE, okay), "lower-limit-switch alias is required");
BUILD_ASSERT(DT_NODE_HAS_STATUS(SERVO_NODE, okay), "door servo node is required");

static const struct gpio_dt_spec upper_limit = GPIO_DT_SPEC_GET(UPPER_LIMIT_NODE, gpios);
static const struct gpio_dt_spec lower_limit = GPIO_DT_SPEC_GET(LOWER_LIMIT_NODE, gpios);
static const struct pwm_dt_spec servo = PWM_DT_SPEC_GET(SERVO_NODE);

static struct door_snapshot state;
static bool upper_raw;
static bool lower_raw;
static int64_t upper_changed_at;
static int64_t lower_changed_at;
static int64_t motion_started_at;
static struct k_work_delayable monitor_work;
K_MUTEX_DEFINE(door_lock);

static bool is_moving(enum door_state value)
{
	return value == DOOR_STATE_OPENING || value == DOOR_STATE_CLOSING ||
	       value == DOOR_STATE_HOMING;
}

const char *door_state_name(enum door_state value)
{
	switch (value) {
	case DOOR_STATE_UNKNOWN: return "unknown";
	case DOOR_STATE_HOMING: return "homing";
	case DOOR_STATE_OPENING: return "opening";
	case DOOR_STATE_CLOSING: return "closing";
	case DOOR_STATE_OPEN: return "open";
	case DOOR_STATE_CLOSED: return "closed";
	case DOOR_STATE_STOPPED: return "stopped";
	case DOOR_STATE_FAULT: return "fault";
	default: return "unknown";
	}
}

static int set_servo_pulse(uint32_t pulse_us)
{
	if (!state.servo_ready) {
		return -ENODEV;
	}

	if (!IS_ENABLED(CONFIG_DOG_DOOR_ACTUATOR_ARMED)) {
		pulse_us = 0;
	}

	return pwm_set_dt(&servo, PWM_USEC(CONFIG_DOG_DOOR_SERVO_PERIOD_US),
			  PWM_USEC(pulse_us));
}

static void bump_generation(void)
{
	state.generation++;
}

static void stop_locked(enum door_state final_state)
{
	int ret = set_servo_pulse(CONFIG_DOG_DOOR_SERVO_STOP_US);

	if (ret != 0) {
		LOG_ERR("Unable to stop servo: %d", ret);
	}
	state.state = final_state;
	motion_started_at = 0;
	bump_generation();
}

static void fault_locked(const char *reason)
{
	(void)set_servo_pulse(CONFIG_DOG_DOOR_SERVO_STOP_US);
	state.state = DOOR_STATE_FAULT;
	strncpy(state.fault, reason, sizeof(state.fault) - 1);
	state.fault[sizeof(state.fault) - 1] = '\0';
	motion_started_at = 0;
	bump_generation();
	LOG_ERR("Door fault: %s", state.fault);
}

static void update_debounced_limit(bool sample, bool *raw, int64_t *changed_at,
				   bool *stable, int64_t now)
{
	if (sample != *raw) {
		*raw = sample;
		*changed_at = now;
	}

	if (*stable != *raw && now - *changed_at >= CONFIG_DOG_DOOR_LIMIT_DEBOUNCE_MS) {
		*stable = *raw;
		bump_generation();
	}
}

static void monitor_handler(struct k_work *work)
{
	int upper_sample;
	int lower_sample;
	int64_t now = k_uptime_get();

	ARG_UNUSED(work);
	upper_sample = gpio_pin_get_dt(&upper_limit);
	lower_sample = gpio_pin_get_dt(&lower_limit);

	k_mutex_lock(&door_lock, K_FOREVER);
	if (upper_sample < 0 || lower_sample < 0) {
		fault_locked("Unable to read a limit switch");
		goto out;
	}

	update_debounced_limit(upper_sample != 0, &upper_raw, &upper_changed_at,
				 &state.upper_limit, now);
	update_debounced_limit(lower_sample != 0, &lower_raw, &lower_changed_at,
				 &state.lower_limit, now);

	if (state.upper_limit && state.lower_limit) {
		if (state.state != DOOR_STATE_FAULT ||
		    strcmp(state.fault, "Both limit switches are active") != 0) {
			fault_locked("Both limit switches are active");
		}
		goto out;
	}

	if ((state.state == DOOR_STATE_OPENING && state.upper_limit) ||
	    (state.state == DOOR_STATE_HOMING && state.upper_limit)) {
		stop_locked(DOOR_STATE_OPEN);
		state.fault[0] = '\0';
	} else if (state.state == DOOR_STATE_CLOSING && state.lower_limit) {
		stop_locked(DOOR_STATE_CLOSED);
		state.fault[0] = '\0';
	} else if (is_moving(state.state) && motion_started_at > 0 &&
		   now - motion_started_at > CONFIG_DOG_DOOR_TRAVEL_TIMEOUT_MS) {
		fault_locked("Travel timed out before reaching the limit switch");
	}

out:
	k_mutex_unlock(&door_lock);
	k_work_reschedule(&monitor_work, K_MSEC(SAMPLE_INTERVAL_MS));
}

int door_controller_init(void)
{
	int ret;

	memset(&state, 0, sizeof(state));
	state.actuator_armed = IS_ENABLED(CONFIG_DOG_DOOR_ACTUATOR_ARMED);
	state.servo_ready = pwm_is_ready_dt(&servo);
	state.state = DOOR_STATE_UNKNOWN;

	if (!gpio_is_ready_dt(&upper_limit) || !gpio_is_ready_dt(&lower_limit)) {
		LOG_ERR("Limit switch GPIO device is not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&upper_limit, GPIO_INPUT);
	if (ret != 0) {
		return ret;
	}
	ret = gpio_pin_configure_dt(&lower_limit, GPIO_INPUT);
	if (ret != 0) {
		return ret;
	}

	upper_raw = gpio_pin_get_dt(&upper_limit) > 0;
	lower_raw = gpio_pin_get_dt(&lower_limit) > 0;
	state.upper_limit = upper_raw;
	state.lower_limit = lower_raw;
	upper_changed_at = lower_changed_at = k_uptime_get();
	if (upper_raw && lower_raw) {
		fault_locked("Both limit switches are active");
	} else if (upper_raw) {
		state.state = DOOR_STATE_OPEN;
	} else if (lower_raw) {
		state.state = DOOR_STATE_CLOSED;
	}

	if (state.servo_ready) {
		(void)set_servo_pulse(CONFIG_DOG_DOOR_SERVO_STOP_US);
	} else {
		LOG_ERR("Servo PWM is not ready");
	}

	k_work_init_delayable(&monitor_work, monitor_handler);
	k_work_schedule(&monitor_work, K_MSEC(SAMPLE_INTERVAL_MS));
	LOG_INF("Controller ready: state=%s, actuator=%s", door_state_name(state.state),
		state.actuator_armed ? "ARMED" : "DISARMED");
	return 0;
}

int door_controller_command(enum door_command command)
{
	int ret = 0;
	uint32_t pulse = 0;
	enum door_state next_state = DOOR_STATE_STOPPED;

	k_mutex_lock(&door_lock, K_FOREVER);
	if (command == DOOR_COMMAND_STOP) {
		stop_locked(DOOR_STATE_STOPPED);
		goto out;
	}
	if (!state.actuator_armed) {
		ret = -EACCES;
		goto out;
	}
	if (!state.servo_ready) {
		ret = -ENODEV;
		goto out;
	}
	if (state.upper_limit && state.lower_limit) {
		ret = -EIO;
		goto out;
	}

	switch (command) {
	case DOOR_COMMAND_OPEN:
		if (state.upper_limit) {
			state.state = DOOR_STATE_OPEN;
			bump_generation();
			goto out;
		}
		pulse = CONFIG_DOG_DOOR_SERVO_OPEN_US;
		next_state = DOOR_STATE_OPENING;
		break;
	case DOOR_COMMAND_CLOSE:
		if (state.lower_limit) {
			state.state = DOOR_STATE_CLOSED;
			bump_generation();
			goto out;
		}
		pulse = CONFIG_DOG_DOOR_SERVO_CLOSE_US;
		next_state = DOOR_STATE_CLOSING;
		break;
	case DOOR_COMMAND_HOME:
		if (state.upper_limit) {
			state.state = DOOR_STATE_OPEN;
			bump_generation();
			goto out;
		}
		pulse = CONFIG_DOG_DOOR_SERVO_OPEN_US;
		next_state = DOOR_STATE_HOMING;
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	ret = set_servo_pulse(pulse);
	if (ret == 0) {
		state.state = next_state;
		state.fault[0] = '\0';
		motion_started_at = k_uptime_get();
		bump_generation();
		LOG_INF("Command accepted: %s", door_state_name(next_state));
	}

out:
	k_mutex_unlock(&door_lock);
	return ret;
}

void door_controller_get(struct door_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return;
	}
	k_mutex_lock(&door_lock, K_FOREVER);
	*snapshot = state;
	k_mutex_unlock(&door_lock);
}
