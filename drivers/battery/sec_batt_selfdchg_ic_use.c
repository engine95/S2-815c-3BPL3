/*
 * Samsung Mobile VE Group.
 *
 * drivers/battery/sec_batt_selfdchg_ic_use.c
 *
 * Drivers for samsung batter self discharging with IC.
 *
 * Copyright (C) 2015, Samsung Electronics.
 *
 * This program is free software. You can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation
 */
 
 /******************************************/
 
#include <linux/sec_batt_selfdchg_common.h>
 
 /******************************************/
 // Samsung Custom Header
#ifdef CONFIG_BATTERY_SAMSUNG
#include <linux/battery/sec_battery.h>
#endif
/******************************************/

static struct sdchg_info_t *sdchg_info;
static char sdchg_type[] = "sdchg_ic";

static void sdchg_ic_use_parse_dt(struct device *dev)
{
	struct device_node *np = dev->of_node;

	sdchg_info->chip->factory_discharging = 
		of_get_named_gpio(np, "battery,factory_discharging", 0);
	if (sdchg_info->chip->factory_discharging < 0) {
		pr_info("%s : Force Discharging pin is Empty(%d)\n", 
			__func__, sdchg_info->chip->factory_discharging);
		sdchg_info->chip->factory_discharging = 0;
	}
	/**************************************/
	sdchg_info->chip->sdchg_en = of_property_read_bool(np,
							"battery,self_discharging_en");
	/**************************************/
	if (of_property_read_u32(np, "battery,force_discharging_limit",
				   &sdchg_info->temp_start))
		pr_info("%s : Force Discharging limit is Empty", __func__);

	if (of_property_read_u32(np, "battery,force_discharging_recov",
				   &sdchg_info->temp_end))
		pr_info("%s : Force Discharging recov is Empty", __func__);

	if (of_property_read_u32(np, "battery,discharging_adc_min",
				   &sdchg_info->chip->adc_min))
		pr_info("%s : Discharging ADC Min is Empty", __func__);

	if (of_property_read_u32(np, "battery,discharging_adc_max",
				   &sdchg_info->chip->adc_max))
		pr_info("%s : Discharging ADC Max is Empty", __func__);

	if (of_property_read_u32(np, "battery,self_discharging_voltage_limit",
				   &sdchg_info->voltage_start))
		pr_info("%s : Force Discharging recov is Empty", __func__);

	// voltage_end : using swelling_drop_float_voltage value
	if (of_property_read_u32(np, "battery,swelling_drop_float_voltage",
				   &sdchg_info->voltage_end))
		pr_err("%s : Force Discharging voltage recov is Empty", __func__);

#ifndef CONFIG_MACH_UNIVERSAL5433
	if (of_property_read_u32(np, "battery,discharging_ntc_limit",
				&sdchg_info->chip->ntc_limit))
		pr_info("%s : Discharging NTC LIMIT is Empty", __func__);
#endif

	pr_info("[SDCHG][%s] factory_discharging = %d\n", __func__, sdchg_info->chip->factory_discharging);
	pr_info("[SDCHG][%s] sdchg_en = %d\n", __func__, sdchg_info->chip->sdchg_en);
	pr_info("[SDCHG][%s] temp_start = %u\n", __func__, sdchg_info->temp_start);
	pr_info("[SDCHG][%s] temp_end = %u\n", __func__, sdchg_info->temp_end);
	pr_info("[SDCHG][%s] adc_min = %u\n", __func__, sdchg_info->chip->adc_min);
	pr_info("[SDCHG][%s] adc_max = %u\n", __func__, sdchg_info->chip->adc_max);
	pr_info("[SDCHG][%s] voltage_start = %u\n", __func__, sdchg_info->voltage_start);
	pr_info("[SDCHG][%s] voltage_end = %u\n", __func__, sdchg_info->voltage_end);
#ifndef CONFIG_MACH_UNIVERSAL5433
	pr_info("[SDCHG][%s] ntc_limit = %u\n", __func__, sdchg_info->chip->ntc_limit);
#endif

	return; 
}

// prev func : sec_bat_self_discharging_check
extern int sec_bat_get_adc_value_using_sdchg
	(struct sec_battery_info *battery, int channel);
static void sdchg_ic_use_adc_check(void *arg)
{
	struct sec_battery_info *battery = (struct sec_battery_info *)arg;
#ifdef CONFIG_MACH_UNIVERSAL5433
	int dis_adc;
#else
	unsigned int dis_adc;
#endif

	dis_adc = sec_bat_get_adc_value_using_sdchg
				(battery, SEC_BAT_ADC_CHANNEL_DISCHARGING_CHECK);

#ifdef CONFIG_MACH_UNIVERSAL5433
	if (dis_adc < 0)
		battery->self_discharging_adc = 0;
	else
		battery->self_discharging_adc = dis_adc;

	if (dis_adc < (int)sdchg_info->chip->adc_min)
		battery->self_discharging_adc = 0;
#else
	if (dis_adc)
		battery->self_discharging_adc = dis_adc;
	else
		battery->self_discharging_adc = 0;
#endif

	if ((dis_adc >= (int)sdchg_info->chip->adc_min) &&
		(dis_adc <= (int)sdchg_info->chip->adc_max))
		battery->self_discharging = true;
	else
		battery->self_discharging = false;

#ifdef CONFIG_MACH_UNIVERSAL5433
	pr_info("%s : SELF_DISCHARGING(%d) SELF_DISCHARGING_ADC(%d:%d)\n",
		__func__, battery->self_discharging, dis_adc, battery->self_discharging_adc);
#else
	pr_info("%s : SELF_DISCHARGING(%d) SELF_DISCHARGING_ADC(%d)\n",
		__func__, battery->self_discharging, battery->self_discharging_adc);
#endif

	return;
}


#ifndef CONFIG_MACH_UNIVERSAL5433
// prev func : sec_bat_self_discharging_ntc_check
static void sdchg_ic_use_ntc_check(void *arg)
{
	struct sec_battery_info *battery = (struct sec_battery_info *)arg;
	int ntc_adc;

	ntc_adc = sec_bat_get_adc_value_using_sdchg
			(battery, SEC_BAT_ADC_CHANNEL_DISCHARGING_NTC);
	if (ntc_adc)
		battery->discharging_ntc_adc = ntc_adc;
	else
		battery->discharging_ntc_adc = 0;

	if (ntc_adc > (int)sdchg_info->chip->ntc_limit)
		battery->discharging_ntc = true;
	else
		battery->discharging_ntc = false;

	pr_info("%s : DISCHARGING_NTC(%d) DISCHARGING_NTC_ADC(%d)\n",
		__func__,battery->discharging_ntc, battery->discharging_ntc_adc);

	return;
}
#endif

// prev func : sec_bat_self_discharging_control
static void sdchg_ic_use_force_control(void *arg, bool dis_en)
{
	struct sec_battery_info *battery = (struct sec_battery_info *)arg;

	if (!sdchg_info->chip->factory_discharging) {
		pr_info("Can't control Self Discharging IC (No Factory Discharging Pin).\n");
		return;
	}

	if (dis_en) {
		dev_info(battery->dev,
			 "%s : Self Discharging IC doesn't act until (%d) degree & (%d) voltage. "
			 "Auto Discharging IC ENABLE\n", __func__,
			 battery->temperature, battery->voltage_now);
		gpio_direction_output(sdchg_info->chip->factory_discharging, 1);
		battery->force_discharging = true;
	} else {
		dev_info(battery->dev, "%s : Self Discharging IC disable.\n", __func__);
		gpio_direction_output(sdchg_info->chip->factory_discharging, 0);
		battery->force_discharging = false;
	}

	return;
}


// prev func : sec_bat_discharging_check
static void sdchg_ic_use_discharging_check(void *arg)
{
	struct sec_battery_info *battery = (struct sec_battery_info *)arg;

	if (!sdchg_info->chip->sdchg_en) {
		pr_info("[SDCHG][%s] --(sdchg_en false)\n", __func__);
		return;
	}

	sdchg_ic_use_adc_check(battery);

	if(battery->factory_self_discharging_mode_on) {
		dev_info(battery->dev,
		 "%s: It is Factory mode by self discharging mode, Auto_DIS(%d), Force_DIS(%d)\n",
		 __func__, battery->self_discharging, battery->force_discharging);
		return;
	}

	if (!battery->self_discharging &&
		(battery->temperature >=  (int)sdchg_info->temp_start) &&
#if defined(CONFIG_SEC_FACTORY) && defined(CONFIG_MACH_A8_CHN_OPEN)
		battery->temperature < 800 &&
#endif
		(battery->voltage_now >= (int)sdchg_info->voltage_start)) {
	    	sdchg_ic_use_force_control((void *)battery, true);
	} else if (battery->force_discharging &&
		((battery->temperature <= (int)sdchg_info->temp_end) ||
		(battery->voltage_now <= (int)sdchg_info->voltage_end))) {
		sdchg_ic_use_force_control((void *)battery, false);
	}
	dev_info(battery->dev,
		 "%s: Auto_DIS(%d), Force_DIS(%d)\n",
		 __func__, battery->self_discharging, battery->force_discharging);

	return;
}


static int sdchg_ic_use_force_check(void *arg)
{
	int ret = 0;

	if (!sdchg_info->chip->factory_discharging) {
		pr_info("Can't control Self Discharging IC (No Factory Discharging Pin).\n");
		goto out;
	}
	
	ret = gpio_get_value(sdchg_info->chip->factory_discharging);
out:
	return ret;
}

static int sdchg_ic_use_probe(void *battery)
{
	int ret = 0;

	sdchg_info->battery = battery;

	if (sdchg_info->chip->factory_discharging) {
		ret = gpio_request((unsigned)sdchg_info->chip->factory_discharging, 
			"FACTORY_DISCHARGING");
		if (ret) {
			pr_err("[SDCHG][%s] failed to request GPIO %d\n", 
				__func__, sdchg_info->chip->factory_discharging);
			goto err_gpio;
		}
	}

err_gpio:
	return ret;
}

static int sdchg_ic_use_remove(void)
{
	if (sdchg_info->chip->factory_discharging)
		gpio_free(sdchg_info->chip->factory_discharging);

	return 0;
}

static int __init sdchg_ic_use_init(void)
{
	int ret;

	pr_info("[SDCHG][%s] ++\n", __func__);

	sdchg_info = kzalloc(sizeof(struct sdchg_info_t), GFP_KERNEL);
	if (!sdchg_info) {
		pr_err("Failed to allocate memory for self discharging\n");
		ret = -ENOMEM;
		goto fail_out;
	}
	sdchg_info->type = sdchg_type;

	/*****************************************/
	sdchg_info->chip = kzalloc(sizeof(struct sdchg_info_chip_t), GFP_KERNEL);
	if (!sdchg_info) {
		pr_err("Failed to allocate nochip memory for self discharging\n");
		ret = -ENOMEM;
		goto after_alloc_info;
	}

	sdchg_info->chip->pinfo = sdchg_info;
	/*****************************************/
	sdchg_info->sdchg_probe = sdchg_ic_use_probe;
	sdchg_info->sdchg_remove = sdchg_ic_use_remove;
	sdchg_info->sdchg_parse_dt = sdchg_ic_use_parse_dt;

	sdchg_info->sdchg_adc_check = sdchg_ic_use_adc_check;
#ifndef CONFIG_MACH_UNIVERSAL5433
	sdchg_info->sdchg_ntc_check = sdchg_ic_use_ntc_check;
#endif
	sdchg_info->sdchg_force_control = sdchg_ic_use_force_control;
	sdchg_info->sdchg_discharging_check = sdchg_ic_use_discharging_check;

	sdchg_info->sdchg_force_check = sdchg_ic_use_force_check;
	/*****************************************/

	list_add(&sdchg_info->info_list, &sdchg_info_head);

	pr_info("[SDCHG][%s] --\n", __func__);

	return 0;

after_alloc_info:
	if (sdchg_info) {
		kfree(sdchg_info);
		sdchg_info = NULL;
	}
fail_out:
	pr_info("[SDCHG][%s] --(%d)\n", __func__, ret);

	return ret;
 }


static void __exit sdchg_ic_use_exit(void)
{
	pr_info("[SDCHG][%s] ++\n", __func__);

	if (sdchg_info) {
		if (sdchg_info->chip) {
			kfree(sdchg_info->chip);
			sdchg_info->chip = NULL;
		}
		kfree(sdchg_info);
		sdchg_info = NULL;
	}

	pr_info("[SDCHG][%s] --\n", __func__);

	return;
}

arch_initcall(sdchg_ic_use_init);
module_exit(sdchg_ic_use_exit);

MODULE_AUTHOR("jeeon.park@samsung.com");
MODULE_DESCRIPTION("Samsung Electronics Co. Battery Self Discharging \
	for Preventing Swelling with Self Discharging IC");
MODULE_LICENSE("GPL");


