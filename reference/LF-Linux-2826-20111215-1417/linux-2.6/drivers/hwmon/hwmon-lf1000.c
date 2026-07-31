/* hwmon-lf1000.c
 *
 * Power and Battery monitoring.  This driver provides battery and external
 * power information, as well as an input device for the Power button.
 * 
 * Andrey Yurovsky <andrey@cozybit.com>
 * Scott Esters <sesters@leapfrog.com>
 *
 * Copyright 2008 LeapFrog Enterprises Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/sysfs.h>
#include <linux/fs.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>

#include <mach/adc.h>
#include <mach/gpio.h>
#include <mach/gpio_hal.h>
#include <mach/common.h>
#include <mach/clkpwr.h>
#include <mach/power.h>
#include <mach/lfp100.h>

#include <asm/io.h>
#include <asm/uaccess.h>

/*
 * power hardware
 */

enum lf1000_power_chip {
	UNDEFINED = 0,
	LF1000 = 1,	/* uses LF1000 power toggle input pin */
	LFP100 = 2,	/* using LFP100 power management chip */
};


/*
 * pins
 */

#define POWER_PORT	GPIO_PORT_C	/* power button */
#define POWER_PIN	GPIO_PIN20

/*
 * configuration
 */

#define POWER_BUTTON_SAMPLING_J	(HZ/4)
#define POWER_SAMPLING_J	(HZ/4)

#define SHUTDOWN_SECS	4
/* power down system after SHUTDOWN_SECS */
#define POWER_BUTTON_COUNT  ((HZ * SHUTDOWN_SECS) / POWER_BUTTON_SAMPLING_J)

/* platform device data */
struct lf1000_hwmon {
	enum lf1000_power_status status;	/* board status	*/
	unsigned int max_battery_mv;		/* max battery, above is Ext */
	unsigned int normal_battery_mv;		/* hysterisis low -> normal */
	unsigned int low_battery_mv;		/* low battery level */
	unsigned int low_battery_repeat_mv;	/* low battery repeat delta */
	unsigned int critical_battery_mv;	/* critical battery level */

	unsigned int adc_slope_256;		/* mx part of line	*/
	unsigned int adc_constant;		/* constant part of line */

	int supply_mv;				/* power supply, in mV */
	int low_battery_reported_mv;		/* low battery reported,in mV */
	unsigned on_battery :1;			/* running on battery */
	unsigned shutdown : 1;			/* shutdown requested */
	unsigned request_battery_report;	/* report battery status changes */

	struct platform_device *pdev;

	struct work_struct battery_work;	/* monitor power */
	struct workqueue_struct *battery_tasks;
	struct timer_list battery_timer;

	struct work_struct power_button_work;	/* monitor power button*/
	struct workqueue_struct *power_button_tasks;
	struct timer_list power_button_timer;

	int	power_button_count;		/* count power button calls */

	enum	lf1000_power_chip chip;		/* support LF1000 or LFP100 */

	/* input device interface */
	unsigned char buttons[5];		/* one slot for each state */
	struct input_dev *input;
};

static struct lf1000_hwmon *hwmon_dev = NULL;

/* return battery reading in millivolts */
static int lf1000_get_battery_mv(struct lf1000_hwmon *hwmon_dev)
{
	int reading;

	/* ignore battery check on older Didj TouchScreen prototypes */
	if (gpio_have_touchscreen() &&
	    (gpio_have_gpio_dev() || gpio_have_gpio_didj()))
		return (987654321);

	reading = adc_GetReading(LF1000_ADC_VBATSENSE);
	if(reading < 0) /* pass the error code down */
		return reading;
	return ((hwmon_dev->adc_slope_256 * reading / 256) +
		 hwmon_dev->adc_constant);
}
EXPORT_SYMBOL_GPL(lf1000_get_battery_mv);

/*
 * Battery pack thermister value below 1000 mv (1 volts) means not in
 * charger ADC reference is 3.3 volts, so each count is 3300/1024 ~ 3.2 mv
 */

#define BATTERY_PACK_LOW	(900 * 1024) / 3300 	// aprox .9 volts
#define BATTERY_PACK_MEDIUM	(2250 * 1024) / 3300	// aprox 2.25 volts

/*
 * Have NiMH returns if NiMH pack is present and also if in charger.
 *
 * Take two battery pack ADC readings, GPIO low for first and high for second.
 * ADC reading is LOW (<350), MEDIUM(350-700), or HIGH(>700).
 * Return result based on this table:
 *
 * ADC    ADC 
 * FIRST  SECOND  battery pack
 * low    low     BATTERY_NIMH
 * medium medium  BATTERY_NIMH_CHARGER
 * low    high    BATTERY_OTHER
 */

static enum lf1000_power_source lf1000_get_power_source(unsigned int on_battery)
{
	int adc_val_0 = 0;
	int adc_val_1 = 0;

	// Madrid does not support charger
	if (!gpio_have_gpio_madrid()) {
		gpio_set_val(lf1000_l2p_port(BATTERY_PACK),	/* set pin low */
				lf1000_l2p_pin(BATTERY_PACK), 0);

		udelay(10);		/* 10 us settling time */
		adc_val_0 = adc_GetReading(LF1000_ADC_BATT_TEMP);

		gpio_set_val(lf1000_l2p_port(BATTERY_PACK),	/* set pin high */
				lf1000_l2p_pin(BATTERY_PACK), 1);

		udelay(10);		/* 10 us settling time */
		adc_val_1 = adc_GetReading(LF1000_ADC_BATT_TEMP);

		if (adc_val_0 < BATTERY_PACK_LOW && 
				adc_val_1 < BATTERY_PACK_LOW)
			return (on_battery ? POWER_NIMH : POWER_NIMH_EXTERNAL);

		if (BATTERY_PACK_LOW     < adc_val_0 &&
			adc_val_0        < BATTERY_PACK_MEDIUM &&
			BATTERY_PACK_LOW < adc_val_1 &&
			adc_val_1        < BATTERY_PACK_MEDIUM)

			return (POWER_NIMH_CHARGER);
	}	/* if (!gpio_have_gpio_madrid()) */

	return(on_battery ? POWER_BATTERY : POWER_EXTERNAL);
}

/*
 * Convert battery reading to power status based on current status 
 */
static enum lf1000_power_status power_to_status(
		struct lf1000_hwmon *hwmon_dev, int mv)
{
	enum lf1000_power_source power_source;

	if(gpio_have_gpio_k2())	     /* K2 hack: always external power */
		return EXTERNAL;

	// Look for NiMH charger
	power_source = lf1000_get_power_source(hwmon_dev->on_battery);

	/* see if we're on the NiMH Charger */
	switch (power_source) {
	case POWER_UNKNOWN:		break;
	case POWER_OTHER:		break;
	case POWER_NIMH:		break;
	case POWER_NIMH_CHARGER:	return(NIMH_CHARGER);
	case POWER_NIMH_EXTERNAL:	break;
	case POWER_BATTERY:		break;
	case POWER_EXTERNAL:		break;
	default:			break;
	}


	if(!hwmon_dev->on_battery) /* no need to make measurements */
		return EXTERNAL;

	if(mv < 0)
		return UNKNOWN;

	if(mv < hwmon_dev->critical_battery_mv)
		return CRITICAL_BATTERY;
	
	/* hysterisis between 'low' and 'normal' battery */
	if(mv < hwmon_dev->low_battery_mv || 
		(mv < hwmon_dev->normal_battery_mv &&
		 hwmon_dev->status == LOW_BATTERY)) {
		return LOW_BATTERY;
	}
	if(mv < hwmon_dev->max_battery_mv)
		return (power_source == POWER_BATTERY ? BATTERY : NIMH);
	
	return EXTERNAL;
}

/*
 * Determine and report current power status
 */
static enum lf1000_power_status lf1000_get_battery_status(void)
{
	if(!hwmon_dev)
		return UNKNOWN;
	return power_to_status(hwmon_dev, lf1000_get_battery_mv(hwmon_dev));
}
EXPORT_SYMBOL_GPL(lf1000_get_battery_status);

/*
 * sysfs Interface
 */

/* report whether shutdown was requested */
static ssize_t show_shutdown(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)dev->driver_data;

	return sprintf(buf, "%d\n", priv->shutdown);
}
static DEVICE_ATTR(shutdown, S_IRUSR|S_IRGRP|S_IROTH, show_shutdown, NULL);

/* report current battery voltage, in mV */
static ssize_t show_voltage(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)dev->driver_data;

	return sprintf(buf, "%d\n", priv->supply_mv);
}
static DEVICE_ATTR(voltage, S_IRUSR|S_IRGRP|S_IROTH, show_voltage, NULL);

/* low battery reported voltage, in mV */
static ssize_t show_low_battery_reported_voltage(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)dev->driver_data;

	return sprintf(buf, "%d\n", priv->low_battery_reported_mv);
}
static DEVICE_ATTR(low_battery_reported_voltage, S_IRUSR|S_IRGRP|S_IROTH,
			show_low_battery_reported_voltage, NULL);

/* report state of external power (1 = on external, 0 = on battery) */
static ssize_t show_external(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)dev->driver_data;

	return sprintf(buf, "%d\n", priv->on_battery ? 0 : 1);
}
static DEVICE_ATTR(external, S_IRUSR|S_IRGRP|S_IROTH, show_external, NULL);

/* report power status as a number */
static ssize_t show_status(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)dev->driver_data;

	return sprintf(buf, "%d\n", priv->status);
}
static DEVICE_ATTR(status, S_IRUSR|S_IRGRP|S_IROTH, show_status, NULL);

static ssize_t show_max_battery_mv(struct device *pdev,
				struct device_attribute *attr, char *buf)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	return sprintf(buf, "%d\n", priv->max_battery_mv);
}

static ssize_t set_max_battery_mv(struct device *pdev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int temp;
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	if (sscanf(buf, "%d", &temp) != 1)
		return -EINVAL;
	if (temp < 0 || temp > 16384)
		return -EINVAL;
	priv->max_battery_mv = temp;
	return(count);
}

static DEVICE_ATTR(max_battery_mv, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH,
		show_max_battery_mv, set_max_battery_mv);

static ssize_t show_normal_battery_mv(struct device *pdev,
				struct device_attribute *attr, char *buf)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	return sprintf(buf, "%d\n", priv->normal_battery_mv);
}

static ssize_t set_normal_battery_mv(struct device *pdev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int temp;
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	if (sscanf(buf, "%d", &temp) != 1)
		return -EINVAL;
	if (temp < 0 || temp > 16384)
		return -EINVAL;
	priv->normal_battery_mv = temp;
	return(count);
}

static DEVICE_ATTR(normal_battery_mv, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH,
		show_normal_battery_mv, set_normal_battery_mv);

static ssize_t show_low_battery_mv(struct device *pdev,
				struct device_attribute *attr, char *buf)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	return sprintf(buf, "%d\n", priv->low_battery_mv);
}

static ssize_t set_low_battery_mv(struct device *pdev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int temp;
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	if (sscanf(buf, "%d", &temp) != 1)
		return -EINVAL;
	if (temp < 0 || temp > 16384)
		return -EINVAL;
	priv->low_battery_mv = temp;
	return(count);
}

static DEVICE_ATTR(low_battery_mv, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH,
		show_low_battery_mv, set_low_battery_mv);


static ssize_t show_low_battery_repeat_mv(struct device *pdev,
				struct device_attribute *attr, char *buf)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	return sprintf(buf, "%d\n", priv->low_battery_repeat_mv);
}

static ssize_t set_low_battery_repeat_mv(struct device *pdev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int temp;
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	if (sscanf(buf, "%d", &temp) != 1)
		return -EINVAL;
	if (temp < 0 || temp > 16384)
		return -EINVAL;
	priv->low_battery_repeat_mv = temp;
	return(count);
}

static DEVICE_ATTR(low_battery_repeat_mv, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH,
		show_low_battery_repeat_mv, set_low_battery_repeat_mv);

static ssize_t show_critical_battery_mv(struct device *pdev,
				struct device_attribute *attr, char *buf)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	return sprintf(buf, "%d\n", priv->critical_battery_mv);
}

static ssize_t set_critical_battery_mv(struct device *pdev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int temp;
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	if (sscanf(buf, "%d", &temp) != 1)
		return -EINVAL;
	if (temp < 0 || temp > 16384)
		return -EINVAL;
	priv->critical_battery_mv = temp;
	return(count);
}

static DEVICE_ATTR(critical_battery_mv,
		S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH,
		show_critical_battery_mv, set_critical_battery_mv);

static ssize_t show_adc_slope_256(struct device *pdev,
			      struct device_attribute *attr, char *buf)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	return sprintf(buf, "%d\n", priv->adc_slope_256);
}

static ssize_t set_adc_slope_256(struct device *pdev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int temp;
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	if (sscanf(buf, "%d", &temp) != 1)
		return -EINVAL;
	if (temp < -65536 || temp > 65535)
		return -EINVAL;
	priv->adc_slope_256 = temp;
	priv->request_battery_report = 1;	/* report battery status */
	return(count);
}

static DEVICE_ATTR(adc_slope_256, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH,
		show_adc_slope_256, set_adc_slope_256);

static ssize_t show_adc_constant(struct device *pdev,
				struct device_attribute *attr, char *buf)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	return sprintf(buf, "%d\n", priv->adc_constant);
}

static ssize_t set_adc_constant(struct device *pdev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int temp;
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;

	if (sscanf(buf, "%d", &temp) != 1)
		return -EINVAL;
	if (temp < -4096 || temp > 4095)
		return -EINVAL;
	priv->adc_constant = temp;
	priv->request_battery_report = 1;	/* report battery status */
	return(count);
}

static DEVICE_ATTR(adc_constant, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH,
		show_adc_constant, set_adc_constant);

static ssize_t show_power_source(struct device *pdev,
				struct device_attribute *attr, char *buf)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)pdev->driver_data;
	enum lf1000_power_source power_source;

	power_source = lf1000_get_power_source(priv->on_battery);

	switch (power_source) {
	case POWER_UNKNOWN:	  return sprintf(buf, "UNKNOWN\n");
	case POWER_OTHER:	  return sprintf(buf, "OTHER\n");
	case POWER_NIMH:	  return sprintf(buf, "NIMH\n");
	case POWER_NIMH_CHARGER:  return sprintf(buf, "NIMH_CHARGER\n");
	case POWER_NIMH_EXTERNAL: return sprintf(buf, "NIMH_EXTERNAL\n");
	case POWER_BATTERY:	  return sprintf(buf, "BATTERY\n");
	case POWER_EXTERNAL:	  return sprintf(buf, "EXTERNAL\n");
	default:	          return sprintf(buf, "default(%d)\n",
						power_source);
	}
}

static DEVICE_ATTR(power_source, S_IRUSR|S_IRGRP|S_IROTH,
		show_power_source, NULL);

static struct attribute *power_attributes[] = {
	&dev_attr_shutdown.attr,
	&dev_attr_voltage.attr,
	&dev_attr_low_battery_reported_voltage.attr,
	&dev_attr_external.attr,
	&dev_attr_status.attr,
	&dev_attr_max_battery_mv.attr,
	&dev_attr_normal_battery_mv.attr,
	&dev_attr_low_battery_mv.attr,
	&dev_attr_low_battery_repeat_mv.attr,
	&dev_attr_critical_battery_mv.attr,
	&dev_attr_adc_slope_256.attr,
	&dev_attr_adc_constant.attr,
	&dev_attr_power_source.attr,
	NULL
};

static struct attribute_group power_attr_group = {
	.attrs = power_attributes
};

static void battery_monitoring_task(unsigned long data)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)data;

	queue_work(priv->battery_tasks, &priv->battery_work);
	priv->battery_timer.expires += POWER_SAMPLING_J;
	priv->battery_timer.function = battery_monitoring_task;
	priv->battery_timer.data = data;
	if (!timer_pending(&priv->battery_timer))
		add_timer(&priv->battery_timer);
}

static void power_button_monitoring_task(unsigned long data)
{
	struct lf1000_hwmon *priv = (struct lf1000_hwmon *)data;

	queue_work(priv->power_button_tasks, &priv->power_button_work);
}

/*
 * read the system power button status
 */
static int get_power_button(int chip)
{
	int ret = 0;
	
	switch(chip)
	{
		case LF1000:
			ret = gpio_get_val(POWER_PORT, POWER_PIN);
			break;
		case LFP100:
			ret = lfp100_get_power_button();
			break;
		default:
			dev_alert(&hwmon_dev->pdev->dev,
				"%s.%d unknown chip %d\n", __FUNCTION__,
				__LINE__, chip);
			break;
	}
	return ret;
}


/*
 * handle power button, shutdown if pressed for SHUTDOWN_SECS
 */

static void lf1000_power_button(struct work_struct *work)
{
	int power_button = get_power_button(hwmon_dev->chip);

	/* report event if power button just pressed */
	if (!hwmon_dev->power_button_count && power_button) {	
		input_report_key(hwmon_dev->input, KEY_POWER, 1);
		dev_info(&hwmon_dev->pdev->dev,
			"Power button pressed\n");
	}

	/* report if power button is released */
	if (hwmon_dev->power_button_count < POWER_BUTTON_COUNT &&
		!power_button) {

		/* report event if power button just released */
		if (hwmon_dev->power_button_count) {
			input_report_key(hwmon_dev->input, KEY_POWER, 0);
			dev_info(&hwmon_dev->pdev->dev,
				"Power button released\n");
		}

		hwmon_dev->power_button_count = 0; /* reset count */ 
		/* enable the power button IRQ */
		switch (hwmon_dev->chip)
		{
		case LF1000:
			gpio_set_int(POWER_PORT, POWER_PIN, 1);
			break;
		case LFP100:
			break;
		default:
			dev_alert(&hwmon_dev->pdev->dev,
				"%s.%d unknown chip %d\n", __FUNCTION__,
				__LINE__, hwmon_dev->chip);
			break;
		}
		return;
	}

	/* note shutdown was requested, set shutdown bit but never clear it */
	hwmon_dev->shutdown = 1;

	/* button still pressed, run task up to POWER_BUTTON_COUNT times */
	if (hwmon_dev->power_button_count++ <= POWER_BUTTON_COUNT) {
		hwmon_dev->power_button_timer.expires +=
			 POWER_BUTTON_SAMPLING_J;
		hwmon_dev->power_button_timer.function =
			power_button_monitoring_task;
		hwmon_dev->power_button_timer.data = (unsigned long)hwmon_dev;

		if (!timer_pending(&hwmon_dev->power_button_timer))
			add_timer(&hwmon_dev->power_button_timer);

		/*
		 * Use second to last time through to show some status,
		 * as pm_power_off() is too late.
		 */
		if (hwmon_dev->power_button_count == POWER_BUTTON_COUNT)
		       dev_info(&hwmon_dev->pdev->dev,
			"Power button held, shutdown with pm_power_off()\n");
	} else {
		/* ran timer long enough, shutdown */
		pm_power_off();
	}
}

/*
 * Start background process
 */

static void lf1000_start_power_button_monitor(void)
{
	if (!hwmon_dev->power_button_count) {/* run background task */
		hwmon_dev->power_button_timer.expires = get_jiffies_64();
		hwmon_dev->power_button_timer.function =
			power_button_monitoring_task;
		hwmon_dev->power_button_timer.data = (unsigned long)hwmon_dev;
		if (!timer_pending(&hwmon_dev->power_button_timer))
			add_timer(&hwmon_dev->power_button_timer);
	}
}


/*
 * Got an interrupt.  Let background task process it
 */
static irqreturn_t power_button_irq(enum gpio_port port, enum gpio_pin pin,
					void *data) 
{
	if (gpio_get_pend(POWER_PORT, POWER_PIN)) {
		gpio_set_int(POWER_PORT, POWER_PIN, 0);
		lf1000_start_power_button_monitor();
		gpio_clear_pend(POWER_PORT, POWER_PIN);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

/* Running on battery?  If so return non-zero */
static int lf1000_is_battery(int chip)
{
	int ret = 0;

	switch(chip)
	{
	case LF1000:
		ret = gpio_get_val(lf1000_l2p_port(EXT_POWER),
				   lf1000_l2p_pin(EXT_POWER)) ? 0 : 1;
		break;
	case LFP100:
		ret = lfp100_is_battery();
		break;
	default:
		dev_alert(&hwmon_dev->pdev->dev,
			"%s.%d unknown chip %d\n", __FUNCTION__,
			__LINE__, chip);
	}
	return ret;
}

static void lf1000_set_battery(struct work_struct *work)
{
	/* last reported battery status */
	enum lf1000_power_status last_status = hwmon_dev->status;
	int supply_drop;
#ifdef CONFIG_MACH_LF_LF1000
	/* get external power status */

	if(gpio_have_gpio_k2())	     /* K2 hack: always external power */
		hwmon_dev->on_battery = 0;
	else
		hwmon_dev->on_battery = lf1000_is_battery(hwmon_dev->chip);
#endif
	/* read the current battery voltage */
	hwmon_dev->supply_mv = lf1000_get_battery_mv(hwmon_dev);
	/* determine the current power status */
	hwmon_dev->status = power_to_status(hwmon_dev, hwmon_dev->supply_mv);

	switch(hwmon_dev->status)
	{
	case CRITICAL_BATTERY:
	    /* first report of critical battery */
	    if(last_status != CRITICAL_BATTERY) {
		input_report_key(hwmon_dev->input, KEY_BATTERY, 0); /* up */
		input_report_key(hwmon_dev->input, KEY_BATTERY, 1); /* down */
		dev_alert(&hwmon_dev->pdev->dev,
				"Report Critical Battery (%d mv)\n",
				hwmon_dev->supply_mv);
		hwmon_dev->low_battery_reported_mv = hwmon_dev->supply_mv;
	    }
	    break;

	case LOW_BATTERY:
	    /* first report of LOW_BATTERY, key down */
	    if ((last_status != LOW_BATTERY &&
		 last_status != CRITICAL_BATTERY) ||
		(last_status != LOW_BATTERY &&
		 hwmon_dev->request_battery_report)) {
		input_report_key(hwmon_dev->input, KEY_MINUS, 0); /* up */
		input_report_key(hwmon_dev->input, KEY_MINUS, 1); /* down */
	    	dev_alert(&hwmon_dev->pdev->dev,
				"Report Low Battery (%d mv)\n",
				hwmon_dev->supply_mv);
		hwmon_dev->low_battery_reported_mv = hwmon_dev->supply_mv;
	    } else {
		/* repeat LOW_BATTERY when voltage drops below threshold */
		supply_drop = hwmon_dev->low_battery_reported_mv -
		hwmon_dev->supply_mv;
		if (0 < supply_drop &&
		    hwmon_dev->low_battery_repeat_mv < supply_drop) {
		    input_report_key(hwmon_dev->input, KEY_MINUS, 0); /* up */
		    input_report_key(hwmon_dev->input, KEY_MINUS, 1); /* down */
		    dev_alert(&hwmon_dev->pdev->dev,
				    "Report Low Battery (%d mv)\n",
				    hwmon_dev->supply_mv);
		    hwmon_dev->low_battery_reported_mv = hwmon_dev->supply_mv;
		}
	    }
	    break;

	case BATTERY:
	    /* first report of BATTERY, key down */
	    if ((last_status != BATTERY && last_status != CRITICAL_BATTERY) ||
		(last_status != BATTERY && hwmon_dev->request_battery_report)) {
		input_report_key(hwmon_dev->input, KEY_EQUAL, 0); /* up */
		input_report_key(hwmon_dev->input, KEY_EQUAL, 1); /* down */
		dev_alert(&hwmon_dev->pdev->dev, "Report Battery (%d mv)\n",
				hwmon_dev->supply_mv);
	    }
	    break;

	case EXTERNAL:
	    /* first report of EXTERNAL, key down */
	    if ((last_status != EXTERNAL && last_status != CRITICAL_BATTERY) ||
	       (last_status != EXTERNAL && hwmon_dev->request_battery_report)) {
		input_report_key(hwmon_dev->input, KEY_UP, 0); /* up */
		input_report_key(hwmon_dev->input, KEY_UP, 1); /* down */
		dev_alert(&hwmon_dev->pdev->dev, "Report External (%d mv)\n",
				hwmon_dev->supply_mv);
	    }
	    break;

	case NIMH:
	    /* first report of NIMH, key down */
	    if ((last_status != NIMH && last_status != CRITICAL_BATTERY) ||
		(last_status != NIMH && hwmon_dev->request_battery_report)) {
		input_report_key(hwmon_dev->input, KEY_KPEQUAL, 0); /* up */
		input_report_key(hwmon_dev->input, KEY_KPEQUAL, 1); /* down */
		dev_alert(&hwmon_dev->pdev->dev, "Report NIMH (%d mv)\n",
				hwmon_dev->supply_mv);
	    }
	    break;


	case NIMH_CHARGER:
	    /* first report of NIMH_CHARGER, key down */
	    if ((last_status != NIMH_CHARGER &&
					last_status != CRITICAL_BATTERY) ||
		(last_status != NIMH_CHARGER &&
					hwmon_dev->request_battery_report)) {
		input_report_key(hwmon_dev->input, KEY_PAGEUP, 0); /* up */
		input_report_key(hwmon_dev->input, KEY_PAGEUP, 1); /* down */
		dev_alert(&hwmon_dev->pdev->dev, "Report NIMH_CHARGER (%d mv)\n",
				hwmon_dev->supply_mv);
	    }
	    break;

	default:
	    break;
	}
	hwmon_dev->request_battery_report = 0;	/* finished any reporting */
}

/*
 * set up input device for power button and critical battery
 */
static int setup_power_button(struct platform_device *pdev)
{
	struct lf1000_hwmon *data = platform_get_drvdata(pdev);
	struct input_dev *input_dev;
	int ret;

	input_dev = input_allocate_device();
	if(!input_dev)
		return -ENOMEM;

	input_dev->name = "Power Button";
	input_dev->phys = "lf1000/power_button";
	input_dev->id.bustype = BUS_HOST;
	input_dev->id.vendor = 0x0001;
	input_dev->id.product = 0x0001;
	input_dev->id.version = 0x0001;
	
	input_dev->evbit[0] = BIT(EV_KEY);
	input_dev->keycode = data->buttons;
	input_dev->keycodesize = sizeof(unsigned char);
	input_dev->keycodemax = 7;

	data->input = input_dev;
	data->buttons[0] = KEY_POWER;	/* we only support power button */
	data->buttons[1] = KEY_BATTERY;	/* and critical battery warning */
	data->buttons[2] = KEY_MINUS;	/* low battery warning */
	data->buttons[3] = KEY_EQUAL;	/* normal battery */
	data->buttons[4] = KEY_UP;	/* external power */
	data->buttons[5] = KEY_KPEQUAL;	/* NiMH battery */
	data->buttons[6] = KEY_PAGEUP;	/* NiMH Charger */
	set_bit(data->buttons[0], input_dev->keybit);
	set_bit(data->buttons[1], input_dev->keybit);
	set_bit(data->buttons[2], input_dev->keybit);
	set_bit(data->buttons[3], input_dev->keybit);
	set_bit(data->buttons[4], input_dev->keybit);
	set_bit(data->buttons[5], input_dev->keybit);
	set_bit(data->buttons[6], input_dev->keybit);

	ret = input_register_device(data->input);
	if(ret)
		goto fail_register;

	switch (data->chip)
	{
		case LF1000:
			ret = gpio_request_irq(POWER_PORT, POWER_PIN,
					power_button_irq, data);
			if(ret) {
				dev_err(&pdev->dev,
					"failed to get button IRQ\n");
				goto fail_irq;
			}
			break;
		case LFP100:
			lfp100_set_power_button_callback(
				lf1000_start_power_button_monitor);
			break;
		default:
			dev_alert(&hwmon_dev->pdev->dev,
				"%s.%d unknown chip %d\n", __FUNCTION__,
				__LINE__, data->chip);
			break;
	}
	
	/* configure GPIO pin for interrupts */
	switch (data->chip)
	{
		case LF1000:
			gpio_set_fn(POWER_PORT, POWER_PIN, GPIO_GPIOFN);
			gpio_set_int_mode(POWER_PORT, POWER_PIN,
						GPIO_IMODE_RISING_EDGE);
			gpio_set_int(POWER_PORT, POWER_PIN, 0);
			gpio_clear_pend(POWER_PORT, POWER_PIN);
			break;
		case LFP100:
			break;
		default:
			dev_alert(&hwmon_dev->pdev->dev,
				"%s.%d unknown chip %d\n", __FUNCTION__,
				__LINE__, data->chip);
			break;
	}

	/* enable the power button IRQ */
	switch (data->chip)
	{
		case LF1000:
			gpio_set_int(POWER_PORT, POWER_PIN, 1);
			break;
		case LFP100:
			break;
		default:
			dev_alert(&hwmon_dev->pdev->dev,
				"%s.%d unknown chip %d\n", __FUNCTION__,
				__LINE__, data->chip);
			break;
	}

	return 0;

fail_irq:
	switch (data->chip)
	{
		case LF1000:
			gpio_set_int(POWER_PORT, POWER_PIN, 0);
			gpio_clear_pend(POWER_PORT, POWER_PIN);
			gpio_free_irq(POWER_PORT, POWER_PIN, power_button_irq);
			break;
		case LFP100:
			lfp100_set_power_button_callback(NULL);
			break;
		default:
			dev_alert(&hwmon_dev->pdev->dev,
				"%s.%d unknown chip %d\n", __FUNCTION__,
				__LINE__, data->chip);
			break;
	}

fail_register:
	input_free_device(input_dev);
	return ret;
}

static int lf1000_power_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct lf1000_hwmon *priv;

	priv = kzalloc(sizeof(struct lf1000_hwmon), GFP_KERNEL);
	if(!priv) {
		ret = -ENOMEM;
		goto fail_alloc;
	}

	priv->pdev = pdev;

	platform_set_drvdata(pdev, priv);

	hwmon_dev = priv;

	/* sniff for LFP100 power chip */
	if (lfp100_have_lfp100()) {
		priv->chip = LFP100;
		dev_info(&pdev->dev, "%s.%d use LFP100 power control\n",
			__FUNCTION__, __LINE__);
	} else {
		priv->chip = LF1000;
		dev_info(&pdev->dev, "%s.%d use LF1000 power control\n",
			__FUNCTION__, __LINE__);
	}

	/* set initial battery thresholds */
	priv->max_battery_mv      = MAX_BATTERY_MV;
	priv->normal_battery_mv   = NORMAL_BATTERY_MV;
	priv->low_battery_mv      = LOW_BATTERY_MV;
	priv->low_battery_repeat_mv = LOW_BATTERY_REPEAT_MV;
	priv->low_battery_mv      = LOW_BATTERY_MV;
	priv->critical_battery_mv = CRITICAL_BATTERY_MV;

	if (gpio_have_gpio_dev()) {
		priv->adc_slope_256 = ADC_SLOPE_256_ME_LF1000;
		priv->adc_constant  = ADC_CONSTANT_ME_LF1000;
	} else {
		priv->adc_slope_256 = ADC_SLOPE_256_LF_LF1000;
		priv->adc_constant  = ADC_CONSTANT_LF_LF1000;
	}

#ifdef CONFIG_MACH_LF_LF1000
	if(gpio_have_gpio_k2())	     /* K2 hack: never on battery */
		hwmon_dev->on_battery = 0;
	else
		hwmon_dev->on_battery = lf1000_is_battery(hwmon_dev->chip);
#else /* CONFIG_MACH_ME_LF1000 */
	hwmon_dev->on_battery = 0; /* never on battery */
#endif

	/* grab initial battery setting */
	hwmon_dev->supply_mv = lf1000_get_battery_mv(priv);
	priv->low_battery_reported_mv = LOW_BATTERY_MV;
	if(priv->supply_mv < 0) {
		dev_err(&pdev->dev, "%s.%d: initial battery read failed\n",
			__FUNCTION__, __LINE__);
		goto fail_adc;
	}

	/* set initial power state */
	priv->status = power_to_status(priv, priv->supply_mv);

	dev_info(&pdev->dev, "%s.%d status is: %d\n",
			__FUNCTION__, __LINE__, priv->status);

	dev_info(&pdev->dev, "%s.%d have_external is: %d\n",
			__FUNCTION__, __LINE__, priv->on_battery ? 0 : 1);

	dev_info(&pdev->dev, "%s.%d initial battery reading is: %d\n",
			__FUNCTION__, __LINE__, priv->supply_mv);

	/* no initial battery report */
	priv->request_battery_report = 0;

	/*
	 * setup to BATTERY_PACK GPIO as output
	 * to sniff for NiMH battery pack
	 */
	gpio_configure_pin(lf1000_l2p_port(BATTERY_PACK),
		lf1000_l2p_pin(BATTERY_PACK), GPIO_GPIOFN, 1, 0, 0);

	/* set up work queue to monitor the battery */
	priv->battery_tasks =
		create_singlethread_workqueue("power tasks");
	INIT_WORK(&priv->battery_work, lf1000_set_battery);

	/* set up periodic sampling of the battery */
	setup_timer(&priv->battery_timer, battery_monitoring_task,
		       (unsigned long)priv);
	priv->battery_timer.expires = get_jiffies_64() + POWER_SAMPLING_J;
	add_timer(&priv->battery_timer);

	/* set up work queue to handle the power button */
	priv->power_button_tasks =
		create_singlethread_workqueue("power button tasks");
	INIT_WORK(&priv->power_button_work, lf1000_power_button);

	/* init button power timer */
	setup_timer(&priv->power_button_timer, power_button_monitoring_task,
		       (unsigned long)priv);

	/* setup power button monitoring after initializing timer structure */
	ret = setup_power_button(pdev);
	if(ret)
		goto fail_button;
	sysfs_create_group(&pdev->dev.kobj, &power_attr_group);
	return 0;

fail_adc:
fail_button:
fail_alloc:
	kfree(priv);
	return ret;
}

static int lf1000_power_remove(struct platform_device *pdev)
{
	struct lf1000_hwmon *priv = platform_get_drvdata(pdev);

	switch (priv->chip)
	{
		case LF1000:
			gpio_set_int(POWER_PORT, POWER_PIN, 0);
			gpio_clear_pend(POWER_PORT, POWER_PIN);
			gpio_free_irq(POWER_PORT, POWER_PIN, power_button_irq);
			break;
		case LFP100:
			lfp100_set_power_button_callback(NULL);
			break;
		default:
			dev_alert(&hwmon_dev->pdev->dev,
				"%s.%d unknown chip %d\n", __FUNCTION__,
				__LINE__, priv->chip);
			break;
	}

	destroy_workqueue(priv->battery_tasks);
	destroy_workqueue(priv->power_button_tasks);

	sysfs_remove_group(&pdev->dev.kobj, &power_attr_group);

	input_unregister_device(priv->input);

	kfree(priv);

	return 0;
}

static struct platform_driver lf1000_power_driver = {
	.probe      = lf1000_power_probe,
	.remove     = lf1000_power_remove,
	.driver     = {
		.name	= "lf1000-power",
		.owner	= THIS_MODULE,
	},
};

/*
 * module stuff
 */
 
static int __init init_lf1000_power(void)
{
	return platform_driver_register(&lf1000_power_driver);
}

static void cleanup_lf1000_power(void)
{
	platform_driver_unregister(&lf1000_power_driver);
}

module_init(init_lf1000_power);
module_exit(cleanup_lf1000_power);

MODULE_AUTHOR("Scott Esters, Andrey Yurovsky");
MODULE_DESCRIPTION("LF1000 hardware monitoring");
MODULE_LICENSE("GPL");
