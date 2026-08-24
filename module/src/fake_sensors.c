/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Skeleton fake sensors (ticket 0003): one shared sensor-API implementation
 * behind the three devicetree nodes the firmware grabs unconditionally —
 *
 *   vbat  (DT_NODELABEL, battery_manager.c:361, lua_system.c:416)
 *   accel (DT_CHOSEN zephyr,accel, lua_imu.c:26)
 *   magn  (DT_CHOSEN zephyr,magn,  lua_imu.c:27)
 *
 * Fixed, plausible readings so init paths succeed and nothing dereferences
 * a missing device. Tickets 0011 (battery) and 0012 (IMU + tap injection)
 * replace the fixed values with control-plane-settable state and fire the
 * triggers registered here; until then triggers are accepted and never fire,
 * and attr_set/attr_get swallow everything (lua_imu.c issues BMA580-private
 * attrs at init that a fake must tolerate).
 */

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

struct fake_sensor_data {
	sensor_trigger_handler_t trig_handler;
	const struct sensor_trigger *trig;
};

static int fake_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	return 0;
}

static int fake_channel_get(const struct device *dev, enum sensor_channel chan,
			    struct sensor_value *val)
{
	ARG_UNUSED(dev);

	val->val1 = 0;
	val->val2 = 0;

	switch (chan) {
	case SENSOR_CHAN_GAUGE_VOLTAGE:
		/* battery_manager reads val1 as millivolts */
		val->val1 = 4000;
		break;
	case SENSOR_CHAN_GAUGE_STATE_OF_CHARGE:
		val->val1 = 80;
		break;
	case SENSOR_CHAN_GAUGE_STDBY_CURRENT:
		/* != 0 means "charging" to battery_manager — emu: on battery */
		val->val1 = 0;
		break;
	case SENSOR_CHAN_ACCEL_Z:
		/* production mounting, device level: gravity on +Z (m/s^2) */
		val->val1 = 9;
		val->val2 = 806650;
		break;
	default:
		/* accel X/Y, magn X/Y/Z, anything else: zero */
		break;
	}

	return 0;
}

static int fake_attr_set(const struct device *dev, enum sensor_channel chan,
			 enum sensor_attribute attr, const struct sensor_value *val)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	ARG_UNUSED(attr);
	ARG_UNUSED(val);
	/* Accept everything, including BMA580-private attributes */
	return 0;
}

static int fake_attr_get(const struct device *dev, enum sensor_channel chan,
			 enum sensor_attribute attr, struct sensor_value *val)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	ARG_UNUSED(attr);
	val->val1 = 0;
	val->val2 = 0;
	return 0;
}

static int fake_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
			    sensor_trigger_handler_t handler)
{
	struct fake_sensor_data *data = dev->data;

	/* Remembered for ticket 0011/0012 injection; never fires until then.
	 * One slot suffices today (per device the firmware registers one
	 * handler: battery charge-delta, or the shared IMU tap handler). */
	data->trig = trig;
	data->trig_handler = handler;
	return 0;
}

static const struct sensor_driver_api fake_sensor_api = {
	.sample_fetch = fake_sample_fetch,
	.channel_get = fake_channel_get,
	.attr_set = fake_attr_set,
	.attr_get = fake_attr_get,
	.trigger_set = fake_trigger_set,
};

static int fake_sensor_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

/* Data structs are named by DT dependency ordinal (UTIL_CAT expands its
 * arguments): unique across the three compatible blocks below, which all
 * reuse instance number 0. */
#define FAKE_SENSOR_DEFINE(inst)                                                                   \
	static struct fake_sensor_data UTIL_CAT(fake_sensor_data_, DT_INST_DEP_ORD(inst));         \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, fake_sensor_init, NULL,                                 \
				     &UTIL_CAT(fake_sensor_data_, DT_INST_DEP_ORD(inst)), NULL,    \
				     POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,                     \
				     &fake_sensor_api);

#define DT_DRV_COMPAT halo_emu_vbat
DT_INST_FOREACH_STATUS_OKAY(FAKE_SENSOR_DEFINE)
#undef DT_DRV_COMPAT

#define DT_DRV_COMPAT halo_emu_bma580
DT_INST_FOREACH_STATUS_OKAY(FAKE_SENSOR_DEFINE)
#undef DT_DRV_COMPAT

#define DT_DRV_COMPAT halo_emu_qmc6308
DT_INST_FOREACH_STATUS_OKAY(FAKE_SENSOR_DEFINE)
#undef DT_DRV_COMPAT
