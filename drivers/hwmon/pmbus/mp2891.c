// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hardware monitoring driver for MPS Multi-phase Digital VR Controllers(MP2891)
 *
 * Copyright (C) 2023 Nvidia
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pmbus.h>
#include "pmbus.h"

/*
 * Vendor specific registers.
 * Note: command PMBUS_READ_DUTY_CYCLE (0x94) is re-purposed for reading input power.
 *	 command PMBUS_READ_FREQUENCY (0x95) is re-purposed for reading input current.
 */
#define MP2891_PMBUS_VOUT_MIN		0x2b
#define MP2891_VOUT_OV_FAULT_LIMIT_R1	0x40
#define MP2891_VOUT_UV_FAULT_LIMIT_R1	0x44
#define MP2891_MFR_IIN_RPT_EST		0x53
#define MP2891_MFR_IIN_TUNE_GAIN_EST	0x54
#define MP2891_MFR_SVI3_IOUT_PRT	0x65
#define MP2891_MFR_READ_PIN_EST		0x94
#define MP2891_MFR_READ_IIN_EST		0x95
#define MP2891_MFR_VOUT_LOOP_CTRL	0xbd

#define MP2891_MFR_OVUV_DIV2_R1		BIT(13)
#define MP2891_MFR_OVP_REF_SEL_R1	BIT(12)
#define MP2891_MFR_OVP_DELTA_R1		GENAMSK(11, 8)
#define MP2891_MFR_OVP_ABS_LIMIT_R1	GENMASK(8, 0)
#define MP2891_MFR_OVP_DELTA_DEFAULT	500
#define MP2891_MFR_UVP_REF_SEL_R1	BIT(12)
#define MP2891_MFR_UVP_DELTA_R1		GENAMSK(11, 8)
#define MP2891_MFR_UVP_ABS_LIMIT_R1	GENMASK(8, 0)
#define MP2891_MFR_UVP_OFFSET_DEFAULT	(-5000)
#define MP2891_MFR_OVP_OFFSET_DEFAULT	5000

#define MP2891_MFR_OVP_UVP_OFFSET_GET(ret, off) \
	(((((ret) & GENMASK(11, 8)) >> 8) + 1) * (off))

#define MP2891_VID_STEP_POS		14
#define MP2891_VID_STEP_MASK		GENMASK(MP2891_VID_STEP_POS + 1, MP2891_VID_STEP_POS)
#define MP2891_DAC_2P5MV_MASK		BIT(13)
#define MP2891_IOUT_SCALE_MASK		GENMASK(2, 0)

#define MP2975_IIN_OC_WARN_LIMIT_UNIT	2
#define MP2975_IOUT_OC_LIMIT_UNIT	4
#define MP2975_PIN_LIMIT_UNIT		2
#define MP2975_VIN_UNIT			32
#define MP2975_IOUT_UC_LIMIT_SCALE	124
#define MP2975_IOUT_UC_LIMIT_UNIT	25600
#define MP2975_TEMP_LIMIT_OFFSET	40

#define MP2891_PAGE_NUM			2

#define MP2891_RAIL1_FUNC		(PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_IOUT | \
					 PMBUS_HAVE_IIN | PMBUS_HAVE_TEMP | PMBUS_HAVE_POUT | \
					 PMBUS_HAVE_PIN)

#define MP2891_RAIL2_FUNC		(PMBUS_HAVE_VOUT | PMBUS_HAVE_IOUT | PMBUS_HAVE_IIN | \
					 PMBUS_HAVE_TEMP | PMBUS_HAVE_POUT)

struct mp2891_data {
	struct pmbus_driver_info info;
	int vid_step[MP2891_PAGE_NUM];
	int vid_ref[MP2891_PAGE_NUM];
	int iout_scale[MP2891_PAGE_NUM];
};

#define to_mp2891_data(x) container_of(x, struct mp2891_data, info)

static int mp2891_read_vout(struct i2c_client *client, int page, int phase, int reg)
{
	const struct pmbus_driver_info *info = pmbus_get_driver_info(client);
	struct mp2891_data *data = to_mp2891_data(info);
	int ret;

	ret = pmbus_read_word_data(client, page, phase, reg);

	return ret < 0 ? ret : ret * data->vid_step[page] / 100;
}

static int mp2891_read_byte_data(struct i2c_client *client, int page, int reg)
{
	int ret;

	switch (reg) {
	case PMBUS_VOUT_MODE:
		/*
		 * Enforce VOUT direct format, since device allows to set the
		 * different formats for the different rails. Conversion from
		 * VID to direct provided by driver internally, in case it is
		 * necessary.
		 */
		ret = PB_VOUT_MODE_DIRECT;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int mp2891_read_word_data(struct i2c_client *client, int page, int phase, int reg)
{
	const struct pmbus_driver_info *info = pmbus_get_driver_info(client);
	struct mp2891_data *data = to_mp2891_data(info);
	int ret;

	switch (reg) {
	case PMBUS_READ_VOUT:
		return mp2891_read_vout(client, page, phase, reg);
	case PMBUS_READ_VIN:
		ret = pmbus_read_word_data(client, page, phase, reg);
		if (ret <= 0)
			return ret;

		/*
		 * READ_VIN register contains bits 15:11 set with the fixed value 11011b, bit 10
		 * is set with 0. Bits 9:0 provides input voltage in linear11 format, scaled as
		 * 1/32V/LSB.
		 */
		return DIV_ROUND_CLOSEST(((ret & GENMASK(9, 0)) * 1000), MP2975_VIN_UNIT);
	case PMBUS_OT_WARN_LIMIT:
	case PMBUS_OT_FAULT_LIMIT:
		return (pmbus_read_word_data(client, page, phase, reg) & GENMASK(7, 0)) -
			MP2975_TEMP_LIMIT_OFFSET;
	case PMBUS_VIN_OV_FAULT_LIMIT:
		ret = pmbus_read_word_data(client, page, phase, reg);
		if (ret <= 0)
			return ret;
		return DIV_ROUND_CLOSEST(ret & GENMASK(7, 0), 8) * 1000;
	case PMBUS_VOUT_UV_FAULT_LIMIT:
		ret = pmbus_read_word_data(client, page, phase, MP2891_PMBUS_VOUT_MIN);
		if (ret <= 0)
			return ret;
		ret = (ret & GENMASK(10, 0)) * data->vid_step[page] * 105;
		return DIV_ROUND_CLOSEST(ret, 10000);
	case PMBUS_VOUT_OV_FAULT_LIMIT:
		ret = pmbus_read_word_data(client, page, phase, PMBUS_VOUT_MAX);
		if (ret <= 0)
			return ret;
		ret = (ret & GENMASK(10, 0)) * data->vid_step[page] * 95;
		return DIV_ROUND_CLOSEST(ret, 10000);
	case PMBUS_IOUT_UC_FAULT_LIMIT:
		ret = pmbus_read_word_data(client, page, phase, reg);
		return ret <= 0 ? ret : DIV_ROUND_CLOSEST((ret & GENMASK(7, 0)) *
							   MP2975_IOUT_UC_LIMIT_SCALE,
							  MP2975_IOUT_UC_LIMIT_UNIT);
			return ret;
	case PMBUS_READ_PIN:
		/*
		 * From some unknown reason the vendor decide to re-purpose command
		 * PMBUS_READ_DUTY_CYCLE (0x94) for reading input and output power.
		 */
		return pmbus_read_word_data(client, page, phase, MP2891_MFR_READ_PIN_EST);
	case PMBUS_READ_POUT:
		return pmbus_read_word_data(client, page, phase, reg);
	case PMBUS_READ_IIN:
		/*
		 * From some unknown reason the vendor decide to re-purpose command
		 * PMBUS_READ_FREQUENCY (0x95) for reading input current.
		 */
		return pmbus_read_word_data(client, page, phase, MP2891_MFR_READ_IIN_EST);
	case PMBUS_IIN_OC_WARN_LIMIT:
		/* Read only from page 0. */
		ret = pmbus_read_word_data(client, 0, phase, reg);
		return ret <= 0 ? ret : DIV_ROUND_CLOSEST(ret, MP2975_IIN_OC_WARN_LIMIT_UNIT);
	case PMBUS_PIN_OP_WARN_LIMIT:
		ret = pmbus_read_word_data(client, page, phase, reg);
		return ret <= 0 ? ret : ret * MP2975_PIN_LIMIT_UNIT;
	case PMBUS_IOUT_OC_WARN_LIMIT:
	case PMBUS_IOUT_OC_FAULT_LIMIT:
		ret = pmbus_read_word_data(client, page, phase, reg);
		return ret <= 0 ? ret : DIV_ROUND_CLOSEST(ret * data->iout_scale[page],
							  MP2975_IOUT_OC_LIMIT_UNIT);
	case PMBUS_UT_WARN_LIMIT:
	case PMBUS_UT_FAULT_LIMIT:
	case PMBUS_VIN_OV_WARN_LIMIT:
	case PMBUS_VIN_UV_WARN_LIMIT:
	case PMBUS_VOUT_OV_WARN_LIMIT:
	case PMBUS_VOUT_UV_WARN_LIMIT:
	case PMBUS_POUT_OP_WARN_LIMIT:
	case PMBUS_IIN_OC_FAULT_LIMIT:
	case PMBUS_POUT_MAX:
	case PMBUS_POUT_OP_FAULT_LIMIT:
	case PMBUS_MFR_VIN_MIN:
	case PMBUS_MFR_VOUT_MIN:
	case PMBUS_MFR_VIN_MAX:
	case PMBUS_MFR_VOUT_MAX:
	case PMBUS_MFR_IIN_MAX:
	case PMBUS_MFR_IOUT_MAX:
	case PMBUS_MFR_PIN_MAX:
	case PMBUS_MFR_POUT_MAX:
	case PMBUS_MFR_MAX_TEMP_1:
		return -ENXIO;
	default:
		return -ENODATA;
	}
}

static int mp2891_write_word_data(struct i2c_client *client, int page, int reg, u16 word)
{
	switch (reg) {
	case PMBUS_OT_FAULT_LIMIT:
	case PMBUS_OT_WARN_LIMIT:
		/* Drop unused bits 15:8. */
		word = clamp_val(word, 0, GENMASK(7, 0));
		break;
	case PMBUS_IOUT_OC_WARN_LIMIT:
	case PMBUS_POUT_OP_WARN_LIMIT:
	case PMBUS_IIN_OC_WARN_LIMIT:
		/* Drop unused bits 15:10. */
		word = clamp_val(word, 0, GENMASK(9, 0));
		break;
	default:
		return -ENODATA;
	}
	return pmbus_write_word_data(client, page, reg, word);
}

static int
mp2891_git_vid_volt_ref(struct i2c_client *client, struct mp2891_data *data, int page)
{
	int ret;

	ret = i2c_smbus_read_word_data(client, PMBUS_VOUT_COMMAND);
	if (ret < 0)
		return ret;

	data->vid_ref[page] = (ret & GENMASK(10, 0)) * data->vid_step[page];

	return 0;
}

static int mp2891_identify_vid(struct i2c_client *client, struct mp2891_data *data, u32 reg,
			       int page)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, PMBUS_PAGE, page);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_read_word_data(client, reg);
	if (ret < 0)
		return ret;

	/*
	 * Obtain vid_step from MP2891_MFR_VOUT_LOOP_CTRL register:
	 * bit 13 = 1, the vid_step is below 2.5mV/LSB;
	 * bit 13 = 0, the vid_step is defined by bits 15:14:
	 * 00b - 6.25mV/LSB, 01b - 5mV/LSB, 10b - 2mV/LSB, 11b - 1mV
	 */
	if (ret & MP2891_DAC_2P5MV_MASK) {
		data->vid_step[page] = 250;
		return mp2891_git_vid_volt_ref(client, data, page);
	}

	switch ((ret & MP2891_VID_STEP_MASK) >> MP2891_VID_STEP_POS) {
	case 1:
		data->vid_step[page] = 500;
			break;
	case 2:
		data->vid_step[page] = 200;
		break;
	case 3:
		data->vid_step[page] = 100;
		break;
	default:
		data->vid_step[page] = 250;
		break;
	}

	return mp2891_git_vid_volt_ref(client, data, page);
}

static int mp2891_identify_rails_vid(struct i2c_client *client, struct mp2891_data *data)
{
	int ret;

	/* Identify vid_step for rail 1. */
	ret = mp2891_identify_vid(client, data, MP2891_MFR_VOUT_LOOP_CTRL, 0);
	if (ret < 0)
		return ret;

	/* Identify vid_step for rail 2. */
	return mp2891_identify_vid(client, data, MP2891_MFR_VOUT_LOOP_CTRL, 1);
}

static int
mp2891_iout_scale_get(struct i2c_client *client, struct mp2891_data *data, u32 reg, int page)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, PMBUS_PAGE, page);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_read_word_data(client, reg);
	if (ret < 0)
		return ret;

	/*
	 * Obtain iout_scale from the register MP2891_MFR_SVI3_IOUT_PRT, bits 2-0.
	 * The value is selected as below:
	 * 000b - 1A/LSB, 001b - (1/32)A/LSB, 010b - (1/16)A/LSB,
	 * 011b - (1/8)A/LSB, 100b - (1/4)A/LSB, 101b - (1/2)A/LSB
	 * 110b - 1A/LSB, 111b - 2A/LSB
	 */
	switch (ret & MP2891_IOUT_SCALE_MASK) {
	case 0:
	case 6:
		data->iout_scale[page] = 32;
		return 0;
	case 1:
		data->iout_scale[page] = 1;
		return 0;
	case 2:
		data->iout_scale[page] = 2;
		return 0;
	case 3:
		data->iout_scale[page] = 4;
		return 0;
	case 4:
		data->iout_scale[page] = 8;
		return 0;
	case 5:
		data->iout_scale[page] = 16;
		return 0;
	default:
		data->iout_scale[page] = 64;
		return 0;
	}
}

static int mp2891_rails_iout_scale_get(struct i2c_client *client, struct mp2891_data *data)
{
	int ret;

	/* Get iout_scale for rail 1. */
	ret = mp2891_iout_scale_get(client, data, MP2891_MFR_SVI3_IOUT_PRT, 0);
	/* Get iout_scale for rail 2. */
	return ret < 0 ? ret : mp2891_iout_scale_get(client, data, MP2891_MFR_SVI3_IOUT_PRT, 1);
}

static struct pmbus_driver_info mp2891_info = {
	.pages = MP2891_PAGE_NUM,
	.format[PSC_VOLTAGE_IN] = direct,
	.format[PSC_VOLTAGE_OUT] = direct,
	.format[PSC_CURRENT_OUT] = linear,
	.format[PSC_TEMPERATURE] = direct,
	.format[PSC_POWER] = linear,
	.m[PSC_VOLTAGE_IN] = 1,
	.m[PSC_VOLTAGE_OUT] = 1,
	.m[PSC_CURRENT_OUT] = 1,
	.m[PSC_TEMPERATURE] = 1,
	.R[PSC_VOLTAGE_IN] = 3,
	.R[PSC_VOLTAGE_OUT] = 3,
	.R[PSC_CURRENT_OUT] = 1,
	.R[PSC_TEMPERATURE] = 0,
	.func[0] = MP2891_RAIL1_FUNC,
	.func[1] = MP2891_RAIL2_FUNC,
	.read_byte_data = mp2891_read_byte_data,
	.read_word_data = mp2891_read_word_data,
	.write_word_data = mp2891_write_word_data,
};

static struct pmbus_platform_data mp2891_pdata = {
	.flags = PMBUS_SKIP_STATUS_CHECK,
};

static int mp2891_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct pmbus_driver_info *info;
	struct mp2891_data *data;
	int ret;

	data = devm_kzalloc(&client->dev, sizeof(struct mp2891_data), GFP_KERNEL);

	if (!data)
		return -ENOMEM;

	client->dev.platform_data = &mp2891_pdata;
	memcpy(&data->info, &mp2891_info, sizeof(*info));
	info = &data->info;

	/* Identify VID setting per rail - obtain the vid_step of output voltage. */
	ret = mp2891_identify_rails_vid(client, data);
	if (ret < 0)
		return ret;

	/* Get iout scale per rail - obtain current scale. */
	ret = mp2891_rails_iout_scale_get(client, data);
	if (ret < 0)
		return ret;

	return pmbus_do_probe(client, info);
}

static const struct i2c_device_id mp2891_id[] = {
	{"mp2891", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, mp2891_id);

static const struct of_device_id __maybe_unused mp2891_of_match[] = {
	{.compatible = "mps,mp2891"},
	{}
};
MODULE_DEVICE_TABLE(of, mp2891_of_match);

static struct i2c_driver mp2891_driver = {
	.driver = {
		.name = "mp2891",
		.of_match_table = mp2891_of_match,
	},
	.probe = mp2891_probe,
	.id_table = mp2891_id,
};

module_i2c_driver(mp2891_driver);

MODULE_DESCRIPTION("PMBus driver for MPS MP2891 device");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(PMBUS);
