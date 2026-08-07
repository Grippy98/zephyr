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
#define STEPPER_NODE DT_NODELABEL(stepper)
#define SAMPLE_INTERVAL_MS 10
#define STEPPER_OUTPUT_COUNT 4
#define STEPPER_PHASE_COUNT 8
#define STEPPER_THREAD_STACK_SIZE 2048
#define STEPPER_THREAD_PRIORITY 5

BUILD_ASSERT(DT_NODE_HAS_STATUS(UPPER_LIMIT_NODE, okay), "upper-limit-switch alias is required");
BUILD_ASSERT(DT_NODE_HAS_STATUS(LOWER_LIMIT_NODE, okay), "lower-limit-switch alias is required");
BUILD_ASSERT(DT_NODE_HAS_STATUS(STEPPER_NODE, okay), "door stepper node is required");
BUILD_ASSERT(DT_PROP_LEN(STEPPER_NODE, pwms) == STEPPER_OUTPUT_COUNT,
	     "door stepper requires A+, A-, B+, and B- PWM outputs");

static const struct gpio_dt_spec upper_limit = GPIO_DT_SPEC_GET(UPPER_LIMIT_NODE, gpios);
static const struct gpio_dt_spec lower_limit = GPIO_DT_SPEC_GET(LOWER_LIMIT_NODE, gpios);
static const struct pwm_dt_spec stepper_outputs[STEPPER_OUTPUT_COUNT] = {
	PWM_DT_SPEC_GET_BY_IDX(STEPPER_NODE, 0),
	PWM_DT_SPEC_GET_BY_IDX(STEPPER_NODE, 1),
	PWM_DT_SPEC_GET_BY_IDX(STEPPER_NODE, 2),
	PWM_DT_SPEC_GET_BY_IDX(STEPPER_NODE, 3),
};

/* A+, A-, B+, B-. One or two windings are energized per half-step. */
static const uint8_t half_step_phases[STEPPER_PHASE_COUNT] = {
	BIT(0), BIT(0) | BIT(2), BIT(2), BIT(1) | BIT(2),
	BIT(1), BIT(1) | BIT(3), BIT(3), BIT(0) | BIT(3),
};

static struct door_snapshot state;
static bool upper_raw;
static bool lower_raw;
static int64_t upper_changed_at;
static int64_t lower_changed_at;
static int64_t motion_started_at;
static struct k_work_delayable monitor_work;
static struct k_thread stepper_thread_data;
static struct k_sem stepper_wake;
static atomic_t stepper_running;
static atomic_t stepper_direction;
static atomic_t stepper_error;
static uint8_t stepper_phase;
K_THREAD_STACK_DEFINE(stepper_stack, STEPPER_THREAD_STACK_SIZE);
K_MUTEX_DEFINE(door_lock);
K_MUTEX_DEFINE(stepper_output_lock);

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

static int stepper_outputs_off(void)
{
	int first_error = 0;

	for (size_t i = 0; i < ARRAY_SIZE(stepper_outputs); i++) {
		int ret = pwm_set_dt(&stepper_outputs[i], stepper_outputs[i].period, 0);

		if (ret != 0 && first_error == 0) {
			first_error = ret;
		}
	}
	return first_error;
}

static int set_stepper_phase(uint8_t phase)
{
	uint8_t active = half_step_phases[phase % STEPPER_PHASE_COUNT];
	int first_error;

	if (!IS_ENABLED(CONFIG_DOG_DOOR_ACTUATOR_ARMED)) {
		return stepper_outputs_off();
	}

	first_error = stepper_outputs_off();
	for (size_t i = 0; i < ARRAY_SIZE(stepper_outputs); i++) {
		uint32_t pulse;
		int ret;

		if ((active & BIT(i)) == 0U) {
			continue;
		}
		pulse = ((uint64_t)stepper_outputs[i].period *
			 CONFIG_DOG_DOOR_STEPPER_DRIVE_PERCENT) / 100U;
		ret = pwm_set_dt(&stepper_outputs[i], stepper_outputs[i].period, pulse);
		if (ret != 0 && first_error == 0) {
			first_error = ret;
		}
	}
	return first_error;
}

static void stepper_thread(void *first, void *second, void *third)
{
	ARG_UNUSED(first);
	ARG_UNUSED(second);
	ARG_UNUSED(third);

	while (true) {
		k_sem_take(&stepper_wake, K_FOREVER);
		while (atomic_get(&stepper_running)) {
			int direction = atomic_get(&stepper_direction);
			int ret;

			k_mutex_lock(&stepper_output_lock, K_FOREVER);
			if (!atomic_get(&stepper_running)) {
				k_mutex_unlock(&stepper_output_lock);
				break;
			}
			stepper_phase = (stepper_phase + direction + STEPPER_PHASE_COUNT) %
				STEPPER_PHASE_COUNT;
			ret = set_stepper_phase(stepper_phase);
			if (ret != 0) {
				(void)stepper_outputs_off();
			}
			k_mutex_unlock(&stepper_output_lock);
			if (ret != 0) {
				atomic_set(&stepper_error, ret);
				atomic_clear(&stepper_running);
				break;
			}
			k_usleep(CONFIG_DOG_DOOR_STEPPER_STEP_INTERVAL_US);
		}
	}
}

static int start_stepper(int direction)
{
	int ret;

	if (!state.motor_ready) {
		return -ENODEV;
	}
	if (!IS_ENABLED(CONFIG_DOG_DOOR_ACTUATOR_ARMED)) {
		return -EACCES;
	}
	if (IS_ENABLED(CONFIG_DOG_DOOR_STEPPER_INVERT_DIRECTION)) {
		direction = -direction;
	}
	atomic_set(&stepper_direction, direction);
	atomic_clear(&stepper_error);
	k_mutex_lock(&stepper_output_lock, K_FOREVER);
	ret = set_stepper_phase(stepper_phase);
	if (ret == 0) {
		atomic_set(&stepper_running, 1);
	}
	k_mutex_unlock(&stepper_output_lock);
	if (ret == 0) {
		k_sem_give(&stepper_wake);
	}
	return ret;
}

static int stop_stepper(void)
{
	int ret;

	atomic_clear(&stepper_running);
	k_mutex_lock(&stepper_output_lock, K_FOREVER);
	ret = stepper_outputs_off();
	k_mutex_unlock(&stepper_output_lock);
	return ret;
}

static void bump_generation(void)
{
	state.generation++;
}

static void stop_locked(enum door_state final_state)
{
	int ret = stop_stepper();

	if (ret != 0) {
		LOG_ERR("Unable to stop stepper: %d", ret);
	}
	state.state = final_state;
	motion_started_at = 0;
	bump_generation();
}

static void fault_locked(const char *reason)
{
	(void)stop_stepper();
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
	if (atomic_get(&stepper_error) != 0) {
		fault_locked("Stepper PWM output failed");
		atomic_clear(&stepper_error);
		goto out;
	}
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
	state.motor_ready = true;
	for (size_t i = 0; i < ARRAY_SIZE(stepper_outputs); i++) {
		state.motor_ready = state.motor_ready && pwm_is_ready_dt(&stepper_outputs[i]);
	}
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

	k_sem_init(&stepper_wake, 0, 1);
	if (state.motor_ready) {
		(void)stepper_outputs_off();
		k_thread_create(&stepper_thread_data, stepper_stack,
				K_THREAD_STACK_SIZEOF(stepper_stack), stepper_thread,
				NULL, NULL, NULL, STEPPER_THREAD_PRIORITY, 0, K_NO_WAIT);
		k_thread_name_set(&stepper_thread_data, "dog-door-stepper");
	} else {
		LOG_ERR("Stepper PWM outputs are not ready");
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
	int direction = 0;
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
	if (!state.motor_ready) {
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
		direction = 1;
		next_state = DOOR_STATE_OPENING;
		break;
	case DOOR_COMMAND_CLOSE:
		if (state.lower_limit) {
			state.state = DOOR_STATE_CLOSED;
			bump_generation();
			goto out;
		}
		direction = -1;
		next_state = DOOR_STATE_CLOSING;
		break;
	case DOOR_COMMAND_HOME:
		if (state.upper_limit) {
			state.state = DOOR_STATE_OPEN;
			bump_generation();
			goto out;
		}
		direction = 1;
		next_state = DOOR_STATE_HOMING;
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	ret = start_stepper(direction);
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
