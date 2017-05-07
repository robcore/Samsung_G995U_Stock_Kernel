/* Copyright (c) 2016 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/power_supply.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <linux/qpnp/qpnp-revid.h>
#include "smb-reg.h"
#include "smb-lib.h"
#include "storm-watch.h"
#include "pmic-voter.h"

#define SMB138X_DEFAULT_FCC_UA 1000000
#define SMB138X_DEFAULT_ICL_UA 1500000

/* Registers that are not common to be mentioned in smb-reg.h */
#define SMB2CHG_MISC_ENG_SDCDC_CFG2	(MISC_BASE + 0xC1)
#define ENG_SDCDC_SEL_OOB_VTH_BIT	BIT(0)

#define SMB2CHG_MISC_ENG_SDCDC_CFG6	(MISC_BASE + 0xC5)
#define DEAD_TIME_MASK			GENMASK(7, 4)
#define HIGH_DEAD_TIME_MASK		GENMASK(7, 4)

enum {
	OOB_COMP_WA_BIT = BIT(0),
};

static struct smb_params v1_params = {
	.fcc		= {
		.name	= "fast charge current",
		.reg	= FAST_CHARGE_CURRENT_CFG_REG,
		.min_u	= 0,
		.max_u	= 6000000,
		.step_u	= 25000,
	},
	.fv		= {
		.name	= "float voltage",
		.reg	= FLOAT_VOLTAGE_CFG_REG,
		.min_u	= 2450000,
		.max_u	= 4950000,
		.step_u	= 10000,
	},
	.usb_icl	= {
		.name	= "usb input current limit",
		.reg	= USBIN_CURRENT_LIMIT_CFG_REG,
		.min_u	= 0,
		.max_u	= 6000000,
		.step_u	= 25000,
	},
	.dc_icl		= {
		.name	= "dc input current limit",
		.reg	= DCIN_CURRENT_LIMIT_CFG_REG,
		.min_u	= 0,
		.max_u	= 6000000,
		.step_u	= 25000,
	},
	.freq_buck	= {
		.name	= "buck switching frequency",
		.reg	= CFG_BUCKBOOST_FREQ_SELECT_BUCK_REG,
		.min_u	= 500,
		.max_u	= 2000,
		.step_u	= 100,
	},
};

struct smb_dt_props {
	bool	suspend_input;
	int	fcc_ua;
	int	usb_icl_ua;
	int	dc_icl_ua;
};

struct smb138x {
	struct smb_charger	chg;
	struct smb_dt_props	dt;
	struct power_supply	*parallel_psy;
	u32			wa_flags;
};

static int __debug_mask;
module_param_named(
	debug_mask, __debug_mask, int, S_IRUSR | S_IWUSR
);

static int smb138x_parse_dt(struct smb138x *chip)
{
	struct smb_charger *chg = &chip->chg;
	struct device_node *node = chg->dev->of_node;
	int rc;

	if (!node) {
		pr_err("device tree node missing\n");
		return -EINVAL;
	}

	chip->dt.suspend_input = of_property_read_bool(node,
				"qcom,suspend-input");

	rc = of_property_read_u32(node,
				"qcom,fcc-max-ua", &chip->dt.fcc_ua);
	if (rc < 0)
		chip->dt.fcc_ua = SMB138X_DEFAULT_FCC_UA;

	rc = of_property_read_u32(node,
				"qcom,usb-icl-ua", &chip->dt.usb_icl_ua);
	if (rc < 0)
		chip->dt.usb_icl_ua = SMB138X_DEFAULT_ICL_UA;

	rc = of_property_read_u32(node,
				"qcom,dc-icl-ua", &chip->dt.dc_icl_ua);
	if (rc < 0)
		chip->dt.dc_icl_ua = SMB138X_DEFAULT_ICL_UA;

	return 0;
}

/************************
 * USB PSY REGISTRATION *
 ************************/

static enum power_supply_property smb138x_usb_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_MIN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_TYPEC_MODE,
	POWER_SUPPLY_PROP_TYPEC_POWER_ROLE,
	POWER_SUPPLY_PROP_TYPEC_CC_ORIENTATION,
};

static int smb138x_usb_get_prop(struct power_supply *psy,
				enum power_supply_property prop,
				union power_supply_propval *val)
{
	struct smb138x *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	switch (prop) {
	case POWER_SUPPLY_PROP_PRESENT:
		rc = smblib_get_prop_usb_present(chg, val);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		rc = smblib_get_prop_usb_online(chg, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN:
		val->intval = chg->voltage_min_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = chg->voltage_max_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		rc = smblib_get_prop_usb_voltage_now(chg, val);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		rc = smblib_get_prop_usb_current_max(chg, val);
		break;
	case POWER_SUPPLY_PROP_TYPE:
		val->intval = chg->usb_psy_desc.type;
		break;
	case POWER_SUPPLY_PROP_TYPEC_MODE:
		rc = smblib_get_prop_typec_mode(chg, val);
		break;
	case POWER_SUPPLY_PROP_TYPEC_POWER_ROLE:
		rc = smblib_get_prop_typec_power_role(chg, val);
		break;
	case POWER_SUPPLY_PROP_TYPEC_CC_ORIENTATION:
		rc = smblib_get_prop_typec_cc_orientation(chg, val);
		break;
	default:
		pr_err("get prop %d is not supported\n", prop);
		return -EINVAL;
	}
	if (rc < 0) {
		pr_debug("Couldn't get prop %d rc = %d\n", prop, rc);
		return -ENODATA;
	}
	return 0;
}

static int smb138x_usb_set_prop(struct power_supply *psy,
				enum power_supply_property prop,
				const union power_supply_propval *val)
{
	struct smb138x *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	switch (prop) {
	case POWER_SUPPLY_PROP_VOLTAGE_MIN:
		rc = smblib_set_prop_usb_voltage_min(chg, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		rc = smblib_set_prop_usb_voltage_max(chg, val);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		rc = smblib_set_prop_usb_current_max(chg, val);
		break;
	case POWER_SUPPLY_PROP_TYPEC_POWER_ROLE:
		rc = smblib_set_prop_typec_power_role(chg, val);
		break;
	default:
		pr_err("set prop %d is not supported\n", prop);
		return -EINVAL;
	}

	return rc;
}

static int smb138x_usb_prop_is_writeable(struct power_supply *psy,
					 enum power_supply_property prop)
{
	switch (prop) {
	case POWER_SUPPLY_PROP_TYPEC_POWER_ROLE:
		return 1;
	default:
		break;
	}

	return 0;
}

static int smb138x_init_usb_psy(struct smb138x *chip)
{
	struct power_supply_config usb_cfg = {};
	struct smb_charger *chg = &chip->chg;

	chg->usb_psy_desc.name = "usb";
	chg->usb_psy_desc.type = POWER_SUPPLY_TYPE_UNKNOWN;
	chg->usb_psy_desc.properties = smb138x_usb_props;
	chg->usb_psy_desc.num_properties = ARRAY_SIZE(smb138x_usb_props);
	chg->usb_psy_desc.get_property = smb138x_usb_get_prop;
	chg->usb_psy_desc.set_property = smb138x_usb_set_prop;
	chg->usb_psy_desc.property_is_writeable = smb138x_usb_prop_is_writeable;

	usb_cfg.drv_data = chip;
	usb_cfg.of_node = chg->dev->of_node;
	chg->usb_psy = devm_power_supply_register(chg->dev,
						  &chg->usb_psy_desc,
						  &usb_cfg);
	if (IS_ERR(chg->usb_psy)) {
		pr_err("Couldn't register USB power supply\n");
		return PTR_ERR(chg->usb_psy);
	}

	return 0;
}

/*************************
 * BATT PSY REGISTRATION *
 *************************/

static enum power_supply_property smb138x_batt_props[] = {
	POWER_SUPPLY_PROP_INPUT_SUSPEND,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGER_TEMP,
	POWER_SUPPLY_PROP_CHARGER_TEMP_MAX,
};

static int smb138x_batt_get_prop(struct power_supply *psy,
				 enum power_supply_property prop,
				 union power_supply_propval *val)
{
	struct smb138x *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	switch (prop) {
	case POWER_SUPPLY_PROP_STATUS:
		rc = smblib_get_prop_batt_status(chg, val);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		rc = smblib_get_prop_batt_health(chg, val);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		rc = smblib_get_prop_batt_present(chg, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
		rc = smblib_get_prop_input_suspend(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		rc = smblib_get_prop_batt_charge_type(chg, val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		rc = smblib_get_prop_batt_capacity(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHARGER_TEMP:
		rc = smblib_get_prop_charger_temp(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHARGER_TEMP_MAX:
		rc = smblib_get_prop_charger_temp_max(chg, val);
		break;
	default:
		pr_err("batt power supply get prop %d not supported\n", prop);
		return -EINVAL;
	}
	if (rc < 0) {
		pr_debug("Couldn't get prop %d rc = %d\n", prop, rc);
		return -ENODATA;
	}
	return 0;
}

static int smb138x_batt_set_prop(struct power_supply *psy,
				 enum power_supply_property prop,
				 const union power_supply_propval *val)
{
	struct smb138x *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	switch (prop) {
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
		rc = smblib_set_prop_input_suspend(chg, val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		rc = smblib_set_prop_batt_capacity(chg, val);
		break;
	default:
		pr_err("batt power supply set prop %d not supported\n", prop);
		return -EINVAL;
	}

	return rc;
}

static int smb138x_batt_prop_is_writeable(struct power_supply *psy,
					  enum power_supply_property prop)
{
	switch (prop) {
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
	case POWER_SUPPLY_PROP_CAPACITY:
		return 1;
	default:
		break;
	}

	return 0;
}

static const struct power_supply_desc batt_psy_desc = {
	.name			= "battery",
	.type			= POWER_SUPPLY_TYPE_BATTERY,
	.properties		= smb138x_batt_props,
	.num_properties		= ARRAY_SIZE(smb138x_batt_props),
	.get_property		= smb138x_batt_get_prop,
	.set_property		= smb138x_batt_set_prop,
	.property_is_writeable	= smb138x_batt_prop_is_writeable,
};

static int smb138x_init_batt_psy(struct smb138x *chip)
{
	struct power_supply_config batt_cfg = {};
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	batt_cfg.drv_data = chip;
	batt_cfg.of_node = chg->dev->of_node;
	chg->batt_psy = devm_power_supply_register(chg->dev,
						   &batt_psy_desc,
						   &batt_cfg);
	if (IS_ERR(chg->batt_psy)) {
		pr_err("Couldn't register battery power supply\n");
		return PTR_ERR(chg->batt_psy);
	}

	return rc;
}

/*****************************
 * PARALLEL PSY REGISTRATION *
 *****************************/

static enum power_supply_property smb138x_parallel_props[] = {
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CHARGING_ENABLED,
	POWER_SUPPLY_PROP_PIN_ENABLED,
	POWER_SUPPLY_PROP_INPUT_SUSPEND,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGER_TEMP,
	POWER_SUPPLY_PROP_CHARGER_TEMP_MAX,
};

static int smb138x_parallel_get_prop(struct power_supply *psy,
				     enum power_supply_property prop,
				     union power_supply_propval *val)
{
	struct smb138x *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	int rc = 0;
	u8 temp;

	switch (prop) {
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		rc = smblib_get_prop_batt_charge_type(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		rc = smblib_read(chg, BATTERY_CHARGER_STATUS_5_REG,
				 &temp);
		if (rc >= 0)
			val->intval = (bool)(temp & CHARGING_ENABLE_BIT);
		break;
	case POWER_SUPPLY_PROP_PIN_ENABLED:
		rc = smblib_read(chg, BATTERY_CHARGER_STATUS_5_REG,
				 &temp);
		if (rc >= 0)
			val->intval = !(temp & DISABLE_CHARGING_BIT);
		break;
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
		rc = smblib_get_usb_suspend(chg, &val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		rc = smblib_get_charge_param(chg, &chg->param.fv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		rc = smblib_get_charge_param(chg, &chg->param.fcc,
					     &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		rc = smblib_get_prop_slave_current_now(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHARGER_TEMP:
		rc = smblib_get_prop_charger_temp(chg, val);
		break;
	case POWER_SUPPLY_PROP_CHARGER_TEMP_MAX:
		rc = smblib_get_prop_charger_temp_max(chg, val);
		break;
	default:
		pr_err("parallel power supply get prop %d not supported\n",
			prop);
		return -EINVAL;
	}
	if (rc < 0) {
		pr_debug("Couldn't get prop %d rc = %d\n", prop, rc);
		return -ENODATA;
	}
	return 0;
}

static int smb138x_parallel_set_prop(struct power_supply *psy,
				     enum power_supply_property prop,
				     const union power_supply_propval *val)
{
	struct smb138x *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	switch (prop) {
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
		rc = smblib_set_usb_suspend(chg, val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		rc = smblib_set_charge_param(chg, &chg->param.fv, val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		rc = smblib_set_charge_param(chg, &chg->param.fcc, val->intval);
		break;
	case POWER_SUPPLY_PROP_BUCK_FREQ:
		rc = smblib_set_charge_param(chg, &chg->param.freq_buck,
					     val->intval);
		break;
	default:
		pr_err("parallel power supply set prop %d not supported\n",
			prop);
		return -EINVAL;
	}

	return rc;
}

static int smb138x_parallel_prop_is_writeable(struct power_supply *psy,
					      enum power_supply_property prop)
{
	return 0;
}

static const struct power_supply_desc parallel_psy_desc = {
	.name			= "parallel",
	.type			= POWER_SUPPLY_TYPE_USB_PARALLEL,
	.properties		= smb138x_parallel_props,
	.num_properties		= ARRAY_SIZE(smb138x_parallel_props),
	.get_property		= smb138x_parallel_get_prop,
	.set_property		= smb138x_parallel_set_prop,
	.property_is_writeable	= smb138x_parallel_prop_is_writeable,
};

static int smb138x_init_parallel_psy(struct smb138x *chip)
{
	struct power_supply_config parallel_cfg = {};
	struct smb_charger *chg = &chip->chg;

	parallel_cfg.drv_data = chip;
	parallel_cfg.of_node = chg->dev->of_node;
	chip->parallel_psy = devm_power_supply_register(chg->dev,
						   &parallel_psy_desc,
						   &parallel_cfg);
	if (IS_ERR(chip->parallel_psy)) {
		pr_err("Couldn't register parallel power supply\n");
		return PTR_ERR(chip->parallel_psy);
	}

	return 0;
}

/******************************
 * VBUS REGULATOR REGISTRATION *
 ******************************/

struct regulator_ops smb138x_vbus_reg_ops = {
	.enable		= smblib_vbus_regulator_enable,
	.disable	= smblib_vbus_regulator_disable,
	.is_enabled	= smblib_vbus_regulator_is_enabled,
};

static int smb138x_init_vbus_regulator(struct smb138x *chip)
{
	struct smb_charger *chg = &chip->chg;
	struct regulator_config cfg = {};
	int rc = 0;

	chg->vbus_vreg = devm_kzalloc(chg->dev, sizeof(*chg->vbus_vreg),
				      GFP_KERNEL);
	if (!chg->vbus_vreg)
		return -ENOMEM;

	cfg.dev = chg->dev;
	cfg.driver_data = chip;

	chg->vbus_vreg->rdesc.owner = THIS_MODULE;
	chg->vbus_vreg->rdesc.type = REGULATOR_VOLTAGE;
	chg->vbus_vreg->rdesc.ops = &smb138x_vbus_reg_ops;
	chg->vbus_vreg->rdesc.of_match = "qcom,smb138x-vbus";
	chg->vbus_vreg->rdesc.name = "qcom,smb138x-vbus";

	chg->vbus_vreg->rdev = devm_regulator_register(chg->dev,
						&chg->vbus_vreg->rdesc, &cfg);
	if (IS_ERR(chg->vbus_vreg->rdev)) {
		rc = PTR_ERR(chg->vbus_vreg->rdev);
		chg->vbus_vreg->rdev = NULL;
		if (rc != -EPROBE_DEFER)
			pr_err("Couldn't register VBUS regualtor rc=%d\n", rc);
	}

	return rc;
}

/******************************
 * VCONN REGULATOR REGISTRATION *
 ******************************/

struct regulator_ops smb138x_vconn_reg_ops = {
	.enable		= smblib_vconn_regulator_enable,
	.disable	= smblib_vconn_regulator_disable,
	.is_enabled	= smblib_vconn_regulator_is_enabled,
};

static int smb138x_init_vconn_regulator(struct smb138x *chip)
{
	struct smb_charger *chg = &chip->chg;
	struct regulator_config cfg = {};
	int rc = 0;

	chg->vconn_vreg = devm_kzalloc(chg->dev, sizeof(*chg->vconn_vreg),
				      GFP_KERNEL);
	if (!chg->vconn_vreg)
		return -ENOMEM;

	cfg.dev = chg->dev;
	cfg.driver_data = chip;

	chg->vconn_vreg->rdesc.owner = THIS_MODULE;
	chg->vconn_vreg->rdesc.type = REGULATOR_VOLTAGE;
	chg->vconn_vreg->rdesc.ops = &smb138x_vconn_reg_ops;
	chg->vconn_vreg->rdesc.of_match = "qcom,smb138x-vconn";
	chg->vconn_vreg->rdesc.name = "qcom,smb138x-vconn";

	chg->vconn_vreg->rdev = devm_regulator_register(chg->dev,
						&chg->vconn_vreg->rdesc, &cfg);
	if (IS_ERR(chg->vconn_vreg->rdev)) {
		rc = PTR_ERR(chg->vconn_vreg->rdev);
		chg->vconn_vreg->rdev = NULL;
		if (rc != -EPROBE_DEFER)
			pr_err("Couldn't register VCONN regualtor rc=%d\n", rc);
	}

	return rc;
}

/***************************
 * HARDWARE INITIALIZATION *
 ***************************/

static int smb138x_init_hw(struct smb138x *chip)
{
	struct smb_charger *chg = &chip->chg;
	int rc;

	/* votes must be cast before configuring software control */
	vote(chg->usb_suspend_votable,
		DEFAULT_VOTER, chip->dt.suspend_input, 0);
	vote(chg->dc_suspend_votable,
		DEFAULT_VOTER, chip->dt.suspend_input, 0);
	vote(chg->fcc_votable,
		DEFAULT_VOTER, true, chip->dt.fcc_ua);
	vote(chg->usb_icl_votable,
		DCP_VOTER, true, chip->dt.usb_icl_ua);
	vote(chg->dc_icl_votable,
		DEFAULT_VOTER, true, chip->dt.dc_icl_ua);

	chg->dcp_icl_ua = chip->dt.usb_icl_ua;

	/* configure charge enable for software control; active high */
	rc = smblib_masked_write(chg, CHGR_CFG2_REG,
				 CHG_EN_POLARITY_BIT | CHG_EN_SRC_BIT, 0);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure charge enable source rc=%d\n", rc);
		return rc;
	}

	/* enable the charging path */
	rc = vote(chg->chg_disable_votable, DEFAULT_VOTER, false, 0);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't enable charging rc=%d\n", rc);
		return rc;
	}

	/*
	 * trigger the usb-typec-change interrupt only when the CC state
	 * changes, or there was a VBUS error
	 */
	rc = smblib_write(chg, TYPE_C_INTRPT_ENB_REG,
			    TYPEC_CCSTATE_CHANGE_INT_EN_BIT
			  | TYPEC_VBUS_ERROR_INT_EN_BIT);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure Type-C interrupts rc=%d\n", rc);
		return rc;
	}

	/* configure VCONN for software control */
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 VCONN_EN_SRC_BIT | VCONN_EN_VALUE_BIT,
				 VCONN_EN_SRC_BIT);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure VCONN for SW control rc=%d\n", rc);
		return rc;
	}

	/* configure VBUS for software control */
	rc = smblib_masked_write(chg, OTG_CFG_REG, OTG_EN_SRC_CFG_BIT, 0);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure VBUS for SW control rc=%d\n", rc);
		return rc;
	}

	/* configure power role for dual-role */
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 TYPEC_POWER_ROLE_CMD_MASK, 0);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure power role for DRP rc=%d\n", rc);
		return rc;
	}

	if (chip->wa_flags & OOB_COMP_WA_BIT) {
		rc = smblib_masked_write(chg, SMB2CHG_MISC_ENG_SDCDC_CFG2,
					ENG_SDCDC_SEL_OOB_VTH_BIT,
					ENG_SDCDC_SEL_OOB_VTH_BIT);
		if (rc < 0) {
			dev_err(chg->dev,
			"Couldn't configure the oob comp threh rc = %d\n", rc);
			return rc;
		}

		rc = smblib_masked_write(chg, SMB2CHG_MISC_ENG_SDCDC_CFG6,
				DEAD_TIME_MASK, HIGH_DEAD_TIME_MASK);
		if (rc < 0) {
			dev_err(chg->dev,
			"Couldn't configure the sdcdc cfg 6 reg rc = %d\n", rc);
			return rc;
		}
	}

	return rc;
}

static int smb138x_setup_wa_flags(struct smb138x *chip)
{
	struct pmic_revid_data *pmic_rev_id;
	struct device_node *revid_dev_node;

	revid_dev_node = of_parse_phandle(chip->chg.dev->of_node,
					"qcom,pmic-revid", 0);
	if (!revid_dev_node) {
		pr_err("Missing qcom,pmic-revid property\n");
		return -EINVAL;
	}

	pmic_rev_id = get_revid_data(revid_dev_node);
	if (IS_ERR_OR_NULL(pmic_rev_id)) {
		/*
		 * the revid peripheral must be registered, any failure
		 * here only indicates that the rev-id module has not
		 * probed yet.
		 */
		return -EPROBE_DEFER;
	}

	switch (pmic_rev_id->pmic_subtype) {
	case SMB1381_SUBTYPE:
		if (pmic_rev_id->rev4 < 2) /* SMB1381 rev 1.0 */
			chip->wa_flags |= OOB_COMP_WA_BIT;
		break;
	default:
		pr_err("PMIC subtype %d not supported\n",
				pmic_rev_id->pmic_subtype);
		return -EINVAL;
	}

	return 0;
}

/****************************
 * DETERMINE INITIAL STATUS *
 ****************************/

static int smb138x_determine_initial_status(struct smb138x *chip)
{
	struct smb_irq_data irq_data = {chip, "determine-initial-status"};

	smblib_handle_usb_plugin(0, &irq_data);
	smblib_handle_usb_typec_change(0, &irq_data);
	smblib_handle_usb_source_change(0, &irq_data);

	return 0;
}

/**************************
 * INTERRUPT REGISTRATION *
 **************************/

struct smb138x_irq_info {
	const char			*name;
	const irq_handler_t		handler;
	const struct storm_watch	storm_data;
};

static const struct smb138x_irq_info smb138x_irqs[] = {
/* CHARGER IRQs */
	{
		.name		= "chg-error",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "chg-state-change",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "step-chg-state-change",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "step-chg-soc-update-fail",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "step-chg-soc-update-request",
		.handler	= smblib_handle_debug,
	},
/* OTG IRQs */
	{
		.name		= "otg-fail",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "otg-overcurrent",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "otg-oc-dis-sw-sts",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "testmode-change-detect",
		.handler	= smblib_handle_debug,
	},
/* BATTERY IRQs */
	{
		.name		= "bat-temp",
		.handler	= smblib_handle_batt_psy_changed,
	},
	{
		.name		= "bat-ocp",
		.handler	= smblib_handle_batt_psy_changed,
	},
	{
		.name		= "bat-ov",
		.handler	= smblib_handle_batt_psy_changed,
	},
	{
		.name		= "bat-low",
		.handler	= smblib_handle_batt_psy_changed,
	},
	{
		.name		= "bat-therm-or-id-missing",
		.handler	= smblib_handle_batt_psy_changed,
	},
	{
		.name		= "bat-terminal-missing",
		.handler	= smblib_handle_batt_psy_changed,
	},
/* USB INPUT IRQs */
	{
		.name		= "usbin-collapse",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "usbin-lt-3p6v",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "usbin-uv",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "usbin-ov",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "usbin-plugin",
		.handler	= smblib_handle_usb_plugin,
	},
	{
		.name		= "usbin-src-change",
		.handler	= smblib_handle_usb_source_change,
	},
	{
		.name		= "usbin-icl-change",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "type-c-change",
		.handler	= smblib_handle_usb_typec_change,
	},
/* DC INPUT IRQs */
	{
		.name		= "dcin-collapse",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "dcin-lt-3p6v",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "dcin-uv",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "dcin-ov",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "dcin-plugin",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "div2-en-dg",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "dcin-icl-change",
		.handler	= smblib_handle_debug,
	},
/* MISCELLANEOUS IRQs */
	{
		.name		= "wdog-snarl",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "wdog-bark",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "aicl-fail",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "aicl-done",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "high-duty-cycle",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "input-current-limiting",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "temperature-change",
		.handler	= smblib_handle_debug,
	},
	{
		.name		= "switcher-power-ok",
		.handler	= smblib_handle_debug,
	},
};

static int smb138x_get_irq_index_byname(const char *irq_name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(smb138x_irqs); i++) {
		if (strcmp(smb138x_irqs[i].name, irq_name) == 0)
			return i;
	}

	return -ENOENT;
}

static int smb138x_request_interrupt(struct smb138x *chip,
				     struct device_node *node,
				     const char *irq_name)
{
	struct smb_charger *chg = &chip->chg;
	int rc, irq, irq_index;
	struct smb_irq_data *irq_data;

	irq = of_irq_get_byname(node, irq_name);
	if (irq < 0) {
		pr_err("Couldn't get irq %s byname\n", irq_name);
		return irq;
	}

	irq_index = smb138x_get_irq_index_byname(irq_name);
	if (irq_index < 0) {
		pr_err("%s is not a defined irq\n", irq_name);
		return irq_index;
	}

	irq_data = devm_kzalloc(chg->dev, sizeof(*irq_data), GFP_KERNEL);
	if (!irq_data)
		return -ENOMEM;

	irq_data->parent_data = chip;
	irq_data->name = irq_name;
	irq_data->storm_data = smb138x_irqs[irq_index].storm_data;

	rc = devm_request_threaded_irq(chg->dev, irq, NULL,
					smb138x_irqs[irq_index].handler,
					IRQF_ONESHOT, irq_name, irq_data);
	if (rc < 0) {
		pr_err("Couldn't request irq %d\n", irq);
		return rc;
	}

	return rc;
}

static int smb138x_request_interrupts(struct smb138x *chip)
{
	struct smb_charger *chg = &chip->chg;
	struct device_node *node = chg->dev->of_node;
	struct device_node *child;
	int rc = 0;
	const char *name;
	struct property *prop;

	for_each_available_child_of_node(node, child) {
		of_property_for_each_string(child, "interrupt-names",
					    prop, name) {
			rc = smb138x_request_interrupt(chip, child, name);
			if (rc < 0) {
				pr_err("Coudn't request interrupt %s rc=%d\n",
				       name, rc);
				return rc;
			}
		}
	}

	return rc;
}

/*********
 * PROBE *
 *********/

static int smb138x_master_probe(struct smb138x *chip)
{
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	chg->param = v1_params;

	rc = smblib_init(chg);
	if (rc < 0) {
		pr_err("Couldn't initialize smblib rc=%d\n", rc);
		return rc;
	}

	rc = smb138x_parse_dt(chip);
	if (rc < 0) {
		pr_err("Couldn't parse device tree rc=%d\n", rc);
		return rc;
	}

	rc = smb138x_init_vbus_regulator(chip);
	if (rc < 0) {
		pr_err("Couldn't initialize vbus regulator rc=%d\n",
			rc);
		return rc;
	}

	rc = smb138x_init_vconn_regulator(chip);
	if (rc < 0) {
		pr_err("Couldn't initialize vconn regulator rc=%d\n",
			rc);
		return rc;
	}

	rc = smb138x_init_usb_psy(chip);
	if (rc < 0) {
		pr_err("Couldn't initialize usb psy rc=%d\n", rc);
		return rc;
	}

	rc = smb138x_init_batt_psy(chip);
	if (rc < 0) {
		pr_err("Couldn't initialize batt psy rc=%d\n", rc);
		return rc;
	}

	rc = smb138x_init_hw(chip);
	if (rc < 0) {
		pr_err("Couldn't initialize hardware rc=%d\n", rc);
		return rc;
	}

	rc = smb138x_determine_initial_status(chip);
	if (rc < 0) {
		pr_err("Couldn't determine initial status rc=%d\n",
			rc);
		return rc;
	}

	rc = smb138x_request_interrupts(chip);
	if (rc < 0) {
		pr_err("Couldn't request interrupts rc=%d\n", rc);
		return rc;
	}

	return rc;
}

static int smb138x_slave_probe(struct smb138x *chip)
{
	struct smb_charger *chg = &chip->chg;
	int rc = 0;

	chg->param = v1_params;

	rc = smblib_init(chg);
	if (rc < 0) {
		pr_err("Couldn't initialize smblib rc=%d\n", rc);
		return rc;
	}

	if (chip->wa_flags & OOB_COMP_WA_BIT) {
		rc = smblib_masked_write(chg, SMB2CHG_MISC_ENG_SDCDC_CFG2,
					ENG_SDCDC_SEL_OOB_VTH_BIT,
					ENG_SDCDC_SEL_OOB_VTH_BIT);
		if (rc < 0) {
			dev_err(chg->dev,
			"Couldn't configure the oob comp threh rc = %d\n", rc);
			return rc;
		}

		rc = smblib_masked_write(chg, SMB2CHG_MISC_ENG_SDCDC_CFG6,
				DEAD_TIME_MASK, HIGH_DEAD_TIME_MASK);
		if (rc < 0) {
			dev_err(chg->dev,
			"Couldn't configure the sdcdc cfg 6 reg rc = %d\n", rc);
			return rc;
		}
	}

	/* suspend usb input */
	rc = smblib_set_usb_suspend(chg, true);
	if (rc < 0) {
		pr_err("Couldn't suspend USB input rc=%d\n", rc);
		return rc;
	}

	/* initialize FCC to 0 */
	rc = smblib_set_charge_param(chg, &chg->param.fcc, 0);
	if (rc < 0) {
		pr_err("Couldn't set 0 FCC rc=%d\n", rc);
		return rc;
	}

	/* enable the charging path */
	rc = smblib_masked_write(chg, CHARGING_ENABLE_CMD_REG,
				 CHARGING_ENABLE_CMD_BIT,
				 CHARGING_ENABLE_CMD_BIT);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't enable charging rc=%d\n", rc);
		return rc;
	}

	/* configure charge enable for software control; active high */
	rc = smblib_masked_write(chg, CHGR_CFG2_REG,
				 CHG_EN_POLARITY_BIT | CHG_EN_SRC_BIT, 0);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure charge enable source rc=%d\n",
			rc);
		return rc;
	}

	/* enable parallel current sensing */
	rc = smblib_masked_write(chg, CFG_REG,
				 VCHG_EN_CFG_BIT, VCHG_EN_CFG_BIT);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't enable parallel current sensing rc=%d\n",
			rc);
		return rc;
	}

	/* keep at the end of probe, ready to serve before notifying others */
	rc = smb138x_init_parallel_psy(chip);
	if (rc < 0) {
		pr_err("Couldn't initialize parallel psy rc=%d\n", rc);
		return rc;
	}

	return rc;
}

static const struct of_device_id match_table[] = {
	{
		.compatible = "qcom,smb138x-charger",
		.data = (void *) PARALLEL_MASTER
	},
	{
		.compatible = "qcom,smb138x-parallel-slave",
		.data = (void *) PARALLEL_SLAVE
	},
	{ },
};

static int smb138x_probe(struct platform_device *pdev)
{
	struct smb138x *chip;
	const struct of_device_id *id;
	int rc = 0;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->chg.dev = &pdev->dev;
	chip->chg.debug_mask = &__debug_mask;
	chip->chg.name = "SMB";

	chip->chg.regmap = dev_get_regmap(chip->chg.dev->parent, NULL);
	if (!chip->chg.regmap) {
		pr_err("parent regmap is missing\n");
		return -EINVAL;
	}

	id = of_match_device(of_match_ptr(match_table), chip->chg.dev);
	if (!id) {
		pr_err("Couldn't find a matching device\n");
		return -ENODEV;
	}

	platform_set_drvdata(pdev, chip);

	rc = smb138x_setup_wa_flags(chip);
	if (rc < 0) {
		if (rc != -EPROBE_DEFER)
			pr_err("Couldn't setup wa flags rc = %d\n", rc);
		return rc;
	}

	chip->chg.mode = (enum smb_mode) id->data;
	switch (chip->chg.mode) {
	case PARALLEL_MASTER:
		rc = smb138x_master_probe(chip);
		break;
	case PARALLEL_SLAVE:
		rc = smb138x_slave_probe(chip);
		break;
	default:
		pr_err("Couldn't find a matching mode %d\n", chip->chg.mode);
		rc = -EINVAL;
		goto cleanup;
	}

	pr_info("SMB138X probed successfully mode=%d\n", chip->chg.mode);
	return rc;

cleanup:
	platform_set_drvdata(pdev, NULL);
	return rc;
}

static int smb138x_remove(struct platform_device *pdev)
{
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static struct platform_driver smb138x_driver = {
	.driver	= {
		.name		= "qcom,smb138x-charger",
		.owner		= THIS_MODULE,
		.of_match_table	= match_table,
	},
	.probe	= smb138x_probe,
	.remove	= smb138x_remove,
};
module_platform_driver(smb138x_driver);

MODULE_DESCRIPTION("QPNP SMB138X Charger Driver");
MODULE_LICENSE("GPL v2");