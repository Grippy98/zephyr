/*
 * Copyright (c) 2024 Your Name
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ti_ehrpwm

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/kernel.h>

#include "pwm_ehrpwm.h"

LOG_MODULE_REGISTER(pwm_ehrpwm, CONFIG_PWM_LOG_LEVEL);

/* Helper to read/write registers */
static inline uint16_t ehrpwm_read(const struct device *dev, uint32_t offset)
{
	const struct ehrpwm_ti_config *cfg = dev->config;
	return sys_read16(cfg->base + offset);
}

static inline void ehrpwm_write(const struct device *dev, uint32_t offset, uint16_t val)
{
	const struct ehrpwm_ti_config *cfg = dev->config;
	sys_write16(val, cfg->base + offset);
}

static inline void ehrpwm_modify(const struct device *dev, uint32_t offset,
				 uint16_t mask, uint16_t val)
{
	uint16_t reg_val;

	reg_val = ehrpwm_read(dev, offset);
	reg_val &= ~mask;
	reg_val |= (val & mask);
	ehrpwm_write(dev, offset, reg_val);
}

static int set_prescale_div(uint32_t rqst_prescaler, uint16_t *prescale_div,
			    uint16_t *tb_clk_div)
{
	uint16_t clkdiv, hspclkdiv;

	for (clkdiv = 0; clkdiv <= TI_EHRPWM_CLKDIV_MAX; clkdiv++) {
		for (hspclkdiv = 0; hspclkdiv <= TI_EHRPWM_HSPCLKDIV_MAX;
		     hspclkdiv++) {
			/*
			 * calculations for prescaler value :
			 * prescale_div = HSPCLKDIVIDER * CLKDIVIDER.
			 * HSPCLKDIVIDER =  2 ** hspclkdiv
			 * CLKDIVIDER = (1),            if clkdiv == 0 *OR*
			 *              (2 * clkdiv),   if clkdiv != 0
			 *
			 * Configure prescale_div value such that period
			 * register value is less than 65535.
			 */

			*prescale_div = (1 << clkdiv) *
				(hspclkdiv ? (hspclkdiv * 2) : 1);
			if (*prescale_div > rqst_prescaler) {
				*tb_clk_div =
				    (clkdiv << TI_EHRPWM_TBCTL_CLKDIV_SHIFT) |
				    (hspclkdiv <<
				     TI_EHRPWM_TBCTL_HSPCLKDIV_SHIFT);
				return 0;
			}
		}
	}

	return 1;
}

static void ti_ehrpwm_configure_polarity(const struct device *dev, uint32_t channel)
{
	struct ehrpwm_ti_data *data = dev->data;
	uint16_t aqctl_val, aqctl_mask;
	uint32_t aqctl_reg;

	/*
	 * Configure PWM output to HIGH/LOW level on counter
	 * reaches compare register value and LOW/HIGH level
	 * on counter value reaches period register value and
	 * zero value on counter
	 */
	if (channel == 1) {
		aqctl_reg = TI_EHRPWM_AQCTLB;
		aqctl_mask = TI_EHRPWM_AQCTL_CBU_MASK;

		if (data->polarity_reversed[channel]) {
			aqctl_val = TI_EHRPWM_AQCTL_CHANB_POLINVERSED;
		} else {
			aqctl_val = TI_EHRPWM_AQCTL_CHANB_POLNORMAL;
		}
	} else {
		aqctl_reg = TI_EHRPWM_AQCTLA;
		aqctl_mask = TI_EHRPWM_AQCTL_CAU_MASK;

		if (data->polarity_reversed[channel]) {
			aqctl_val = TI_EHRPWM_AQCTL_CHANA_POLINVERSED;
		} else {
			aqctl_val = TI_EHRPWM_AQCTL_CHANA_POLNORMAL;
		}
	}

	aqctl_mask |= TI_EHRPWM_AQCTL_PRD_MASK | TI_EHRPWM_AQCTL_ZRO_MASK;
	ehrpwm_modify(dev, aqctl_reg, aqctl_mask, aqctl_val);
}

static int ehrpwm_ti_set_cycles(const struct device *dev, uint32_t channel,
				uint32_t period_cycles, uint32_t pulse_cycles,
				pwm_flags_t flags)
{
	const struct ehrpwm_ti_config *cfg = dev->config;
	struct ehrpwm_ti_data *data = dev->data;
	uint16_t ps_divval, tb_divval;
	int i;
	uint32_t cmp_reg;
	bool polarity_inverted = (flags & PWM_POLARITY_INVERTED) != 0;

	if (channel >= TI_EHRPWM_NUM_CHANNELS) {
		return -EINVAL;
	}

	/* Store polarity request */
	data->polarity_reversed[channel] = polarity_inverted;

	if (period_cycles == 0) {
		return -EINVAL;
	}
	
	/* 
	 * Period values should be same for multiple PWM channels as IP uses
	 * same period register for multiple channels.
	 */
	for (i = 0; i < TI_EHRPWM_NUM_CHANNELS; i++) {
		if (data->period_cycles[i] &&
		    data->period_cycles[i] != period_cycles) {
			/*
			 * Allow channel to reconfigure period if no other
			 * channels being configured (checked by checking if stored period is 0 or match).
			 * HOWEVER, Zephyr PWM API assumes statelessness somewhat, but hardware is shared.
			 * If another channel IS active (non-zero), we check conflict.
			 */
			 if (i == channel) continue;
			 
			 /* If the other channel is effectively disabled/zero, we might override, 
			  * but here we trust data->period_cycles reflects active config.
			  * If the other channel is active with different period, error.
			  */
			 LOG_ERR("Period value conflicts with channel %d", i);
			 return -EINVAL;
		}
	}

	data->period_cycles[channel] = period_cycles;

	/* Configure clock prescaler to support Low frequency PWM wave */
	if (set_prescale_div(period_cycles / TI_EHRPWM_PERIOD_MAX, &ps_divval,
			     &tb_divval)) {
		LOG_ERR("Unsupported period value");
		return -EINVAL;
	}

	/* Update clock prescaler values */
	ehrpwm_modify(dev, TI_EHRPWM_TBCTL, TI_EHRPWM_TBCTL_CLKDIV_MASK, tb_divval);

	/* Update period & duty cycle with prescaler division */
	uint32_t final_period = period_cycles / ps_divval;
	uint32_t final_duty = pulse_cycles / ps_divval;

	/* Configure shadow loading on Period register */
	ehrpwm_modify(dev, TI_EHRPWM_TBCTL, TI_EHRPWM_TBCTL_PRDLD_MASK,
		      TI_EHRPWM_TBCTL_PRDLD_SHDW);

	ehrpwm_write(dev, TI_EHRPWM_TBPRD, (uint16_t)final_period);

	/* Configure ehrpwm counter for up-count mode */
	ehrpwm_modify(dev, TI_EHRPWM_TBCTL, TI_EHRPWM_TBCTL_CTRMODE_MASK,
		      TI_EHRPWM_TBCTL_CTRMODE_UP);

	if (channel == 1) {
		cmp_reg = TI_EHRPWM_CMPB;
	} else {
		cmp_reg = TI_EHRPWM_CMPA;
	}

	ehrpwm_write(dev, cmp_reg, (uint16_t)final_duty);
	
	/* Apply actions */
	
	if (pulse_cycles == 0) {
		/* Force low (Disable) */
		uint16_t aqcsfrc_val, aqcsfrc_mask;

		if (channel) {
			aqcsfrc_val = TI_EHRPWM_AQCSFRC_CSFB_FRCLOW;
			aqcsfrc_mask = TI_EHRPWM_AQCSFRC_CSFB_MASK;
		} else {
			aqcsfrc_val = TI_EHRPWM_AQCSFRC_CSFA_FRCLOW;
			aqcsfrc_mask = TI_EHRPWM_AQCSFRC_CSFA_MASK;
		}
		
		/* Update shadow register first before modifying active register */
		ehrpwm_modify(dev, TI_EHRPWM_AQSFRC, TI_EHRPWM_AQSFRC_RLDCSF_MASK,
				TI_EHRPWM_AQSFRC_RLDCSF_ZRO);
				
		ehrpwm_modify(dev, TI_EHRPWM_AQCSFRC, aqcsfrc_mask, aqcsfrc_val);
		
		/* 
		 * Changes to immediate action on Action Qualifier. This puts
		 * Action Qualifier control on PWM output from next TBCLK
		 */
		ehrpwm_modify(dev, TI_EHRPWM_AQSFRC, TI_EHRPWM_AQSFRC_RLDCSF_MASK,
				TI_EHRPWM_AQSFRC_RLDCSF_IMDT);
				
		ehrpwm_modify(dev, TI_EHRPWM_AQCSFRC, aqcsfrc_mask, aqcsfrc_val);
		
		return 0;
	}

	/* 
	 * The original driver had a specific verify/enable step. 
	 * Zephyr set_cycles usually implies enable.
	 * We need to ensure Action Qualifier is set up.
	 */
	
	/* Enable TBCLK (Time Base Clock) - in original driver it's a clock gate. 
	 * Here we assume the device clock is managed via power/clock APIs if needed.
	 * But we do need to enable the output in AQCSFRC.
	 */
	
	/* 
	 * Force logic: The original driver forced low when disabled.
	 * We will do the standard polarity config here.
	 */
	ti_ehrpwm_configure_polarity(dev, channel);
	
	/* Enable output (Unforce) */
	uint16_t aqcsfrc_val, aqcsfrc_mask;

	if (channel) {
		aqcsfrc_val = TI_EHRPWM_AQCSFRC_CSFB_FRCDIS;
		aqcsfrc_mask = TI_EHRPWM_AQCSFRC_CSFB_MASK;
	} else {
		aqcsfrc_val = TI_EHRPWM_AQCSFRC_CSFA_FRCDIS;
		aqcsfrc_mask = TI_EHRPWM_AQCSFRC_CSFA_MASK;
	}
	
	/* Changes to shadow mode */
	ehrpwm_modify(dev, TI_EHRPWM_AQSFRC, TI_EHRPWM_AQSFRC_RLDCSF_MASK,
		      TI_EHRPWM_AQSFRC_RLDCSF_ZRO);

	ehrpwm_modify(dev, TI_EHRPWM_AQCSFRC, aqcsfrc_mask, aqcsfrc_val);
	
	return 0;
}

static int ehrpwm_ti_get_cycles_per_sec(const struct device *dev,
					uint32_t channel, uint64_t *cycles)
{
	const struct ehrpwm_ti_config *cfg = dev->config;
	
	*cycles = cfg->clk_freq;
	return 0;
}

static const struct pwm_driver_api ehrpwm_ti_driver_api = {
	.set_cycles = ehrpwm_ti_set_cycles,
	.get_cycles_per_sec = ehrpwm_ti_get_cycles_per_sec,
};

static int ehrpwm_ti_init(const struct device *dev)
{
	const struct ehrpwm_ti_config *cfg = dev->config;
	int ret;

	/* Pinctrl */
	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("Failed to apply pinctrl state");
		return ret;
	}

	/* Clock */
	if (cfg->clock_dev) {
		if (!device_is_ready(cfg->clock_dev)) {
			LOG_ERR("Clock device not ready");
			return -ENODEV;
		}

		ret = clock_control_on(cfg->clock_dev, cfg->clock_subsys);
		if (ret < 0) {
			LOG_ERR("Could not enable clock");
			return ret;
		}
	}
	
	/* 
	 * Hardware initialization if needed.
	 * Most logic is postponed to set_cycles in this IP.
	 */
	return 0;
}

#define EHRPWM_TI_INIT(inst)						\
	PINCTRL_DT_INST_DEFINE(inst);					\
	static struct ehrpwm_ti_data ehrpwm_ti_data_##inst;		\
	static const struct ehrpwm_ti_config ehrpwm_ti_config_##inst = { \
		.base = DT_INST_REG_ADDR(inst),				\
		.clk_freq = DT_INST_PROP(inst, clock_frequency),	\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),		\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(inst)),	\
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(inst, id), \
	};								\
									\
	DEVICE_DT_INST_DEFINE(inst,					\
			    ehrpwm_ti_init,				\
			    NULL,					\
			    &ehrpwm_ti_data_##inst,			\
			    &ehrpwm_ti_config_##inst,			\
			    POST_KERNEL,				\
			    CONFIG_PWM_INIT_PRIORITY,			\
			    &ehrpwm_ti_driver_api);

DT_INST_FOREACH_STATUS_OKAY(EHRPWM_TI_INIT)
