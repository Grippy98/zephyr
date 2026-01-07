/*
 * Copyright (c) 2024 Your Name
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_PWM_PWM_EHRPWM_H_
#define ZEPHYR_DRIVERS_PWM_PWM_EHRPWM_H_

#include <zephyr/types.h>

/* Time base module registers */
#define TI_EHRPWM_TBCTL				0x00
#define TI_EHRPWM_TBSTS				0x02
#define TI_EHRPWM_TBPHSH			0x04
#define TI_EHRPWM_TBPHS				0x06
#define TI_EHRPWM_TBCNT				0x08
#define TI_EHRPWM_TBPRD				0x0A

/* Counter compare module registers */
#define TI_EHRPWM_CMPA				0x12
#define TI_EHRPWM_CMPB				0x14

/* Action qualifier module registers */
#define TI_EHRPWM_AQCTLA			0x16
#define TI_EHRPWM_AQCTLB			0x18
#define TI_EHRPWM_AQSFRC			0x1A
#define TI_EHRPWM_AQCSFRC			0x1C

/* Dead band module registers */
#define TI_EHRPWM_DBCTL				0x1E
#define TI_EHRPWM_DBRED				0x20
#define TI_EHRPWM_DBFED				0x22

/* Trip zone module registers */
#define TI_EHRPWM_TZSEL				0x24
#define TI_EHRPWM_TZCTL				0x28
#define TI_EHRPWM_TZEINT			0x2A
#define TI_EHRPWM_TZFLG				0x2C
#define TI_EHRPWM_TZCLR				0x2E
#define TI_EHRPWM_TZFRC				0x30

/* Event trigger module registers */
#define TI_EHRPWM_ETSEL				0x32
#define TI_EHRPWM_ETPS				0x34
#define TI_EHRPWM_ETFLG				0x36
#define TI_EHRPWM_ETCLR				0x38
#define TI_EHRPWM_ETFRC				0x3A

/* PC control register */
#define TI_EHRPWM_PCCTL				0x3C

/* HR control register */
#define TI_EHRPWM_HRCTL				0x40

/* Bit Field Definitions */

/* TBCTL (Time Base Control) */
#define TI_EHRPWM_TBCTL_RUN_MASK		    BIT(15) /* Free/Soft emulation mode */
#define TI_EHRPWM_TBCTL_PRDLD_MASK		    BIT(3)  /* Shadow select */
#define TI_EHRPWM_TBCTL_PRDLD_SHDW		    0
#define TI_EHRPWM_TBCTL_PRDLD_IMDT		    BIT(3)

#define TI_EHRPWM_TBCTL_CLKDIV_MASK		    GENMASK(12, 7)
#define TI_EHRPWM_TBCTL_CTRMODE_MASK	    GENMASK(1, 0)
#define TI_EHRPWM_TBCTL_CTRMODE_UP		    0
#define TI_EHRPWM_TBCTL_CTRMODE_DOWN		BIT(0)
#define TI_EHRPWM_TBCTL_CTRMODE_UPDOWN		BIT(1)
#define TI_EHRPWM_TBCTL_CTRMODE_FREEZE		GENMASK(1, 0)

#define TI_EHRPWM_TBCTL_HSPCLKDIV_SHIFT		7
#define TI_EHRPWM_TBCTL_CLKDIV_SHIFT		10

#define TI_EHRPWM_CLKDIV_MAX				7
#define TI_EHRPWM_HSPCLKDIV_MAX			    7
#define TI_EHRPWM_PERIOD_MAX				0xFFFF

/* AQCTL (Action Qualifier Control) */
#define TI_EHRPWM_AQCTL_CBU_MASK		    GENMASK(9, 8)
#define TI_EHRPWM_AQCTL_CBU_FRCLOW		    BIT(8)
#define TI_EHRPWM_AQCTL_CBU_FRCHIGH		    BIT(9)
#define TI_EHRPWM_AQCTL_CBU_FRCTOGGLE	    GENMASK(9, 8)

#define TI_EHRPWM_AQCTL_CAU_MASK		    GENMASK(5, 4)
#define TI_EHRPWM_AQCTL_CAU_FRCLOW		    BIT(4)
#define TI_EHRPWM_AQCTL_CAU_FRCHIGH		    BIT(5)
#define TI_EHRPWM_AQCTL_CAU_FRCTOGGLE	    GENMASK(5, 4)

#define TI_EHRPWM_AQCTL_PRD_MASK		    GENMASK(3, 2)
#define TI_EHRPWM_AQCTL_PRD_FRCLOW		    BIT(2)
#define TI_EHRPWM_AQCTL_PRD_FRCHIGH		    BIT(3)
#define TI_EHRPWM_AQCTL_PRD_FRCTOGGLE	    GENMASK(3, 2)

#define TI_EHRPWM_AQCTL_ZRO_MASK		    GENMASK(1, 0)
#define TI_EHRPWM_AQCTL_ZRO_FRCLOW		    BIT(0)
#define TI_EHRPWM_AQCTL_ZRO_FRCHIGH		    BIT(1)
#define TI_EHRPWM_AQCTL_ZRO_FRCTOGGLE	    GENMASK(1, 0)

/* Polarity macros helper */
#define TI_EHRPWM_AQCTL_CHANA_POLNORMAL		(TI_EHRPWM_AQCTL_CAU_FRCLOW | \
						 TI_EHRPWM_AQCTL_PRD_FRCHIGH | \
						 TI_EHRPWM_AQCTL_ZRO_FRCHIGH)
#define TI_EHRPWM_AQCTL_CHANA_POLINVERSED	(TI_EHRPWM_AQCTL_CAU_FRCHIGH | \
						 TI_EHRPWM_AQCTL_PRD_FRCLOW | \
						 TI_EHRPWM_AQCTL_ZRO_FRCLOW)
#define TI_EHRPWM_AQCTL_CHANB_POLNORMAL		(TI_EHRPWM_AQCTL_CBU_FRCLOW | \
						 TI_EHRPWM_AQCTL_PRD_FRCHIGH | \
						 TI_EHRPWM_AQCTL_ZRO_FRCHIGH)
#define TI_EHRPWM_AQCTL_CHANB_POLINVERSED	(TI_EHRPWM_AQCTL_CBU_FRCHIGH | \
						 TI_EHRPWM_AQCTL_PRD_FRCLOW | \
						 TI_EHRPWM_AQCTL_ZRO_FRCLOW)


/* AQSFRC (Action Qualifier Software Force) */
#define TI_EHRPWM_AQSFRC_RLDCSF_MASK		GENMASK(7, 6)
#define TI_EHRPWM_AQSFRC_RLDCSF_ZRO		    0
#define TI_EHRPWM_AQSFRC_RLDCSF_PRD		    BIT(6)
#define TI_EHRPWM_AQSFRC_RLDCSF_ZROPRD		BIT(7)
#define TI_EHRPWM_AQSFRC_RLDCSF_IMDT		GENMASK(7, 6)

/* AQCSFRC (Action Qualifier Continuous Software Force) */
#define TI_EHRPWM_AQCSFRC_CSFB_MASK		    GENMASK(3, 2)
#define TI_EHRPWM_AQCSFRC_CSFB_FRCDIS		0
#define TI_EHRPWM_AQCSFRC_CSFB_FRCLOW		BIT(2)
#define TI_EHRPWM_AQCSFRC_CSFB_FRCHIGH		BIT(3)
#define TI_EHRPWM_AQCSFRC_CSFB_DISSWFRC		GENMASK(3, 2)

#define TI_EHRPWM_AQCSFRC_CSFA_MASK		    GENMASK(1, 0)
#define TI_EHRPWM_AQCSFRC_CSFA_FRCDIS		0
#define TI_EHRPWM_AQCSFRC_CSFA_FRCLOW		BIT(0)
#define TI_EHRPWM_AQCSFRC_CSFA_FRCHIGH		BIT(1)
#define TI_EHRPWM_AQCSFRC_CSFA_DISSWFRC		GENMASK(1, 0)

#define TI_EHRPWM_NUM_CHANNELS              2

/* Zephyr Driver Data/Config */

struct ehrpwm_ti_config {
	long base;
	uint32_t clk_freq;
	const struct pinctrl_dev_config *pcfg;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
};

struct ehrpwm_ti_data {
	/* Store period cycles for each channel to check for conflicts */
	uint32_t period_cycles[TI_EHRPWM_NUM_CHANNELS];
	bool polarity_reversed[TI_EHRPWM_NUM_CHANNELS];
};

#endif /* ZEPHYR_DRIVERS_PWM_PWM_EHRPWM_H_ */
