// SPDX-License-Identifier: GPL-2.0+
/*
 * Ampere Computing SoC's SMpro Hardware Monitoring Driver
 */
#include <linux/bitfield.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

/* Identification Registers */
#define MANUFACTURER_ID_REG		0x02
#define AMPERE_MANUFACTURER_ID		0xCD3A

/* Capability Registers  */
#define SOC_TDP_REG			0x0E

/* Logical Power Sensor Registers */
#define SOC_TEMP_REG			0x10
#define SOC_VRD_TEMP_REG		0x11
#define DIMM_VRD_TEMP_REG		0x12
#define CORE_VRD_TEMP_REG		0x13
#define CH0_DIMM_TEMP_REG		0x14
#define CH1_DIMM_TEMP_REG		0x15
#define CH2_DIMM_TEMP_REG		0x16
#define CH3_DIMM_TEMP_REG		0x17
#define CH4_DIMM_TEMP_REG		0x18
#define CH5_DIMM_TEMP_REG		0x19
#define CH6_DIMM_TEMP_REG		0x1A
#define CH7_DIMM_TEMP_REG		0x1B
#define RCA_VRD_TEMP_REG		0x1C

#define CORE_VRD_PWR_REG		0x20
#define SOC_PWR_REG			0x21
#define DIMM_VRD1_PWR_REG		0x22
#define DIMM_VRD2_PWR_REG		0x23
#define CORE_VRD_PWR_MW_REG		0x26
#define SOC_PWR_MW_REG			0x27
#define DIMM_VRD1_PWR_MW_REG		0x28
#define DIMM_VRD2_PWR_MW_REG		0x29
#define RCA_VRD_PWR_REG			0x2A
#define RCA_VRD_PWR_MW_REG		0x2B

#define MEM_HOT_THRESHOLD_REG		0x32
#define SOC_VR_HOT_THRESHOLD_REG	0x33
#define CORE_VRD_VOLT_REG		0x34
#define SOC_VRD_VOLT_REG		0x35
#define DIMM_VRD1_VOLT_REG		0x36
#define DIMM_VRD2_VOLT_REG		0x37
#define RCA_VRD_VOLT_REG		0x38

#define CORE_VRD_CURR_REG		0x39
#define SOC_VRD_CURR_REG		0x3A
#define DIMM_VRD1_CURR_REG		0x3B
#define DIMM_VRD2_CURR_REG		0x3C
#define RCA_VRD_CURR_REG		0x3D

struct smpro_hwmon {
	struct regmap *regmap;
};

static const u8 temp_regs[] = {
	SOC_TEMP_REG,
	SOC_VRD_TEMP_REG,
	DIMM_VRD_TEMP_REG,
	CORE_VRD_TEMP_REG,
	CH0_DIMM_TEMP_REG,
	CH1_DIMM_TEMP_REG,
	CH2_DIMM_TEMP_REG,
	CH3_DIMM_TEMP_REG,
	CH4_DIMM_TEMP_REG,
	CH5_DIMM_TEMP_REG,
	CH6_DIMM_TEMP_REG,
	CH7_DIMM_TEMP_REG,
	MEM_HOT_THRESHOLD_REG,
	SOC_VR_HOT_THRESHOLD_REG,
	RCA_VRD_TEMP_REG,
};

static const u8 volt_regs[] = {
	CORE_VRD_VOLT_REG,
	SOC_VRD_VOLT_REG,
	DIMM_VRD1_VOLT_REG,
	DIMM_VRD2_VOLT_REG,
	RCA_VRD_VOLT_REG,
};

static const u8 curr_regs[] = {
	CORE_VRD_CURR_REG,
	SOC_VRD_CURR_REG,
	DIMM_VRD1_CURR_REG,
	DIMM_VRD2_CURR_REG,
	RCA_VRD_CURR_REG,
};

enum pwr_regs {
	CORE_VRD_PWR,
	SOC_PWR,
	DIMM_VRD1_PWR,
	DIMM_VRD2_PWR,
	RCA_VRD_PWR,
	SOC_TDP_PWR,
};
static const char * const label[] = {
	"SoC",
	"SoC VRD",
	"DIMM VRD",
	"DIMM VRD1",
	"DIMM VRD2",
	"CORE VRD",
	"CH0 DIMM",
	"CH1 DIMM",
	"CH2 DIMM",
	"CH3 DIMM",
	"CH4 DIMM",
	"CH5 DIMM",
	"CH6 DIMM",
	"CH7 DIMM",
	"MEM HOT",
	"SoC VR HOT",
	"CPU VRD",
	"RCA VRD",
	"SOC TDP",
};

static int smpro_read_temp(struct device *dev, u32 attr, int channel,
				long *val)
{
	struct smpro_hwmon *hwmon = dev_get_drvdata(dev);
	unsigned int value;
	int ret = -1;

	switch (attr) {
	case hwmon_temp_input:
		ret = regmap_read(hwmon->regmap,
				temp_regs[channel], &value);
		if (ret)
			return ret;
		*val = (value & 0x1ff) * 1000;
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int smpro_read_in(struct device *dev, u32 attr, int channel,
				long *val)
{
	struct smpro_hwmon *hwmon = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	switch (attr) {
	case hwmon_in_input:
		ret = regmap_read(hwmon->regmap, volt_regs[channel], &value);
		if (ret < 0)
			return ret;
		*val = value & 0x7fff;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int smpro_read_curr(struct device *dev, u32 attr, int channel,
				long *val)
{
	struct smpro_hwmon *hwmon = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	switch (attr) {
	case hwmon_curr_input:
		ret = regmap_read(hwmon->regmap, curr_regs[channel], &value);
		if (ret < 0)
			return ret;
		*val = value & 0x7fff;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int smpro_read_power(struct device *dev, u32 attr, int channel,
				long *val_pwr)
{
	struct smpro_hwmon *hwmon = dev_get_drvdata(dev);
	unsigned int val = 0, val_mw = 0;
	int ret = 0;

	switch (attr) {
	case hwmon_power_input:
		switch (channel) {
		case CORE_VRD_PWR:
			if (!ret)
				ret = regmap_read(hwmon->regmap,
						CORE_VRD_PWR_REG, &val);
			if (!ret)
				ret = regmap_read(hwmon->regmap,
						CORE_VRD_PWR_MW_REG, &val_mw);
			if (ret)
				return ret;
			break;
		case SOC_PWR:
			if (!ret)
				ret = regmap_read(hwmon->regmap,
						SOC_PWR_REG, &val);
			if (!ret)
				ret = regmap_read(hwmon->regmap,
						SOC_PWR_MW_REG, &val_mw);
			if (ret)
				return ret;
			break;
		case DIMM_VRD1_PWR:
			if (!ret)
				ret = regmap_read(hwmon->regmap,
						DIMM_VRD1_PWR_REG, &val);
			if (!ret)
				ret = regmap_read(hwmon->regmap,
						DIMM_VRD1_PWR_MW_REG, &val_mw);
			if (ret)
				return ret;
			break;
		case DIMM_VRD2_PWR:
			if (!ret)
				ret = regmap_read(hwmon->regmap,
						DIMM_VRD2_PWR_REG, &val);
			if (!ret)
				ret = regmap_read(hwmon->regmap,
						DIMM_VRD2_PWR_MW_REG, &val_mw);
			if (ret)
				return ret;
			break;
		case RCA_VRD_PWR:
			if (!ret)
				ret = regmap_read(hwmon->regmap,
						RCA_VRD_PWR_REG, &val);
			if (!ret)
				ret = regmap_read(hwmon->regmap,
						RCA_VRD_PWR_MW_REG, &val_mw);
			if (ret)
				return ret;
			break;
		case SOC_TDP_PWR:
			if (!ret)
				ret = regmap_read(hwmon->regmap,
						SOC_TDP_REG, &val);
			if (ret)
				return ret;
			break;
		default:
			return -EOPNOTSUPP;
		}

		if (val_mw == 0xffff)
			val_mw = 0;

		*val_pwr = val*1000000 + val_mw*1000;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int smpro_read(struct device *dev, enum hwmon_sensor_types type,
				u32 attr, int channel, long *val)
{
	switch (type) {
	case hwmon_temp:
		return smpro_read_temp(dev, attr, channel, val);
	case hwmon_in:
		return smpro_read_in(dev, attr, channel, val);
	case hwmon_power:
		return smpro_read_power(dev, attr, channel, val);
	case hwmon_curr:
		return smpro_read_curr(dev, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int smpro_write(struct device *dev, enum hwmon_sensor_types type,
				u32 attr, int channel, long val)
{
	return -EOPNOTSUPP;
}

static umode_t smpro_is_visible(const void *data,
				enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	return 0444;
}

static ssize_t show_label(struct device *dev, struct device_attribute *devattr,
				char *buf)
{
	int index = to_sensor_dev_attr(devattr)->index;

	return sprintf(buf, "%s\n", label[index]);
}

static const u32 smpro_temp_config[] = {
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	0
};

static const struct hwmon_channel_info smpro_temp = {
	.type = hwmon_temp,
	.config = smpro_temp_config,
};

static const u32 smpro_in_config[] = {
	HWMON_I_INPUT,
	HWMON_I_INPUT,
	HWMON_I_INPUT,
	HWMON_I_INPUT,
	HWMON_I_INPUT,
	0
};

static const struct hwmon_channel_info smpro_in = {
	.type = hwmon_in,
	.config = smpro_in_config,
};

static const u32 smpro_curr_config[] = {
	HWMON_C_INPUT,
	HWMON_C_INPUT,
	HWMON_C_INPUT,
	HWMON_C_INPUT,
	HWMON_C_INPUT,
	0
};

static const struct hwmon_channel_info smpro_curr = {
	.type = hwmon_curr,
	.config = smpro_curr_config,
};

static const u32 smpro_power_config[] = {
	HWMON_P_INPUT,
	HWMON_P_INPUT,
	HWMON_P_INPUT,
	HWMON_P_INPUT,
	HWMON_P_INPUT,
	HWMON_P_INPUT,
	0
};

static const struct hwmon_channel_info smpro_power = {
	.type = hwmon_power,
	.config = smpro_power_config,
};

static const struct hwmon_channel_info *smpro_info[] = {
	&smpro_temp,
	&smpro_in,
	&smpro_power,
	&smpro_curr,
	NULL
};

static const struct hwmon_ops smpro_hwmon_ops = {
	.is_visible = smpro_is_visible,
	.read = smpro_read,
	.write = smpro_write,
};

static const struct hwmon_chip_info smpro_chip_info = {
	.ops = &smpro_hwmon_ops,
	.info = smpro_info,
};

static SENSOR_DEVICE_ATTR(temp1_label, 0444, show_label, NULL, 0);
static SENSOR_DEVICE_ATTR(temp2_label, 0444, show_label, NULL, 1);
static SENSOR_DEVICE_ATTR(temp3_label, 0444, show_label, NULL, 2);
static SENSOR_DEVICE_ATTR(temp4_label, 0444, show_label, NULL, 5);
static SENSOR_DEVICE_ATTR(temp5_label, 0444, show_label, NULL, 6);
static SENSOR_DEVICE_ATTR(temp6_label, 0444, show_label, NULL, 7);
static SENSOR_DEVICE_ATTR(temp7_label, 0444, show_label, NULL, 8);
static SENSOR_DEVICE_ATTR(temp8_label, 0444, show_label, NULL, 9);
static SENSOR_DEVICE_ATTR(temp9_label, 0444, show_label, NULL, 10);
static SENSOR_DEVICE_ATTR(temp10_label, 0444, show_label, NULL, 11);
static SENSOR_DEVICE_ATTR(temp11_label, 0444, show_label, NULL, 12);
static SENSOR_DEVICE_ATTR(temp12_label, 0444, show_label, NULL, 13);
static SENSOR_DEVICE_ATTR(temp13_label, 0444, show_label, NULL, 14);
static SENSOR_DEVICE_ATTR(temp14_label, 0444, show_label, NULL, 15);
static SENSOR_DEVICE_ATTR(temp15_label, 0444, show_label, NULL, 17);

static SENSOR_DEVICE_ATTR(in0_label, 0444, show_label, NULL, 5);
static SENSOR_DEVICE_ATTR(in1_label, 0444, show_label, NULL, 1);
static SENSOR_DEVICE_ATTR(in2_label, 0444, show_label, NULL, 3);
static SENSOR_DEVICE_ATTR(in3_label, 0444, show_label, NULL, 4);
static SENSOR_DEVICE_ATTR(in4_label, 0444, show_label, NULL, 17);

static SENSOR_DEVICE_ATTR(power1_label, 0444, show_label, NULL, 5);
static SENSOR_DEVICE_ATTR(power2_label, 0444, show_label, NULL, 0);
static SENSOR_DEVICE_ATTR(power3_label, 0444, show_label, NULL, 3);
static SENSOR_DEVICE_ATTR(power4_label, 0444, show_label, NULL, 4);
static SENSOR_DEVICE_ATTR(power5_label, 0444, show_label, NULL, 2);
static SENSOR_DEVICE_ATTR(power6_label, 0444, show_label, NULL, 17);

static SENSOR_DEVICE_ATTR(curr1_label, 0444, show_label, NULL, 5);
static SENSOR_DEVICE_ATTR(curr2_label, 0444, show_label, NULL, 1);
static SENSOR_DEVICE_ATTR(curr3_label, 0444, show_label, NULL, 3);
static SENSOR_DEVICE_ATTR(curr4_label, 0444, show_label, NULL, 4);
static SENSOR_DEVICE_ATTR(curr5_label, 0444, show_label, NULL, 17);

static struct attribute *smpro_attrs[] = {
	&sensor_dev_attr_temp1_label.dev_attr.attr,
	&sensor_dev_attr_temp2_label.dev_attr.attr,
	&sensor_dev_attr_temp3_label.dev_attr.attr,
	&sensor_dev_attr_temp4_label.dev_attr.attr,
	&sensor_dev_attr_temp5_label.dev_attr.attr,
	&sensor_dev_attr_temp6_label.dev_attr.attr,
	&sensor_dev_attr_temp7_label.dev_attr.attr,
	&sensor_dev_attr_temp8_label.dev_attr.attr,
	&sensor_dev_attr_temp9_label.dev_attr.attr,
	&sensor_dev_attr_temp10_label.dev_attr.attr,
	&sensor_dev_attr_temp11_label.dev_attr.attr,
	&sensor_dev_attr_temp12_label.dev_attr.attr,
	&sensor_dev_attr_temp13_label.dev_attr.attr,
	&sensor_dev_attr_temp14_label.dev_attr.attr,
	&sensor_dev_attr_temp15_label.dev_attr.attr,

	&sensor_dev_attr_in0_label.dev_attr.attr,
	&sensor_dev_attr_in1_label.dev_attr.attr,
	&sensor_dev_attr_in2_label.dev_attr.attr,
	&sensor_dev_attr_in3_label.dev_attr.attr,
	&sensor_dev_attr_in4_label.dev_attr.attr,

	&sensor_dev_attr_curr1_label.dev_attr.attr,
	&sensor_dev_attr_curr2_label.dev_attr.attr,
	&sensor_dev_attr_curr3_label.dev_attr.attr,
	&sensor_dev_attr_curr4_label.dev_attr.attr,
	&sensor_dev_attr_curr5_label.dev_attr.attr,

	&sensor_dev_attr_power1_label.dev_attr.attr,
	&sensor_dev_attr_power2_label.dev_attr.attr,
	&sensor_dev_attr_power3_label.dev_attr.attr,
	&sensor_dev_attr_power4_label.dev_attr.attr,
	&sensor_dev_attr_power5_label.dev_attr.attr,
	&sensor_dev_attr_power6_label.dev_attr.attr,

	NULL
};
ATTRIBUTE_GROUPS(smpro);

static int check_valid_id (struct regmap *regmap)
{
	unsigned int val;
	int ret;

	ret = regmap_read(regmap, MANUFACTURER_ID_REG, &val);
	if (ret)
		return ret;

	return  (val == AMPERE_MANUFACTURER_ID) ? 0 : 1;
}

static int smpro_hwmon_probe(struct platform_device *pdev)
{
	struct smpro_hwmon *hwmon;
	struct device *hwmon_dev;
	int ret;

	hwmon = devm_kzalloc(&pdev->dev, sizeof(struct smpro_hwmon),
			GFP_KERNEL);
	if (!hwmon)
		return -ENOMEM;

	hwmon->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!hwmon->regmap)
		return -ENODEV;

	/* Check for valid ID */
	ret = check_valid_id(hwmon->regmap);
	if (ret)
		dev_warn(&pdev->dev, "Hmmh, SMPro not ready yet\n");

	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev,
			"smpro_hwmon", hwmon,
			&smpro_chip_info, smpro_groups);
	if (IS_ERR(hwmon_dev))
		dev_err(&pdev->dev, "failed to register as hwmon device");

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct of_device_id smpro_hwmon_of_match[] = {
	{ .compatible = "ampere,ac01-hwmon" },
	{}
};
MODULE_DEVICE_TABLE(of, smpro_hwmon_of_match);

static struct platform_driver smpro_hwmon_driver = {
	.probe		= smpro_hwmon_probe,
	.driver = {
		.name	= "smpro-hwmon",
		.of_match_table = smpro_hwmon_of_match,
	},
};

module_platform_driver(smpro_hwmon_driver);

MODULE_AUTHOR("Thinh Pham <thinh.pham@amperecomputing.com>");
MODULE_AUTHOR("Hoang Nguyen <hnguyen@amperecomputing.com>");
MODULE_AUTHOR("Thu Nguyen <tbnguyen@amperecomputing.com>");
MODULE_DESCRIPTION("Ampere Altra SMpro hwmon driver");
MODULE_LICENSE("GPL");
