// SPDX-License-Identifier: GPL-2.0+
/*
 * Ampere Computing SoC's SMpro Misc Driver
 *
 * Copyright (c) 2019-2020, Ampere Computing LLC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* Identification Registers */
#define MANUFACTURER_ID_REG	0x02
#define AMPERE_MANUFACTURER_ID	0xCD3A

/* Boot Stage/Progress Registers */
#define BOOT_STAGE_SELECT_REG		0xB0
#define BOOT_STAGE_STATUS_LO_REG	0xB1
#define BOOT_STAGE_CUR_STAGE_REG	0xB2
#define BOOT_STAGE_STATUS_HI_REG	0xB3

/* ACPI State Registers */
#define ACPI_POWER_LIMIT_REG	0xE5

/* Boot stages */
enum {
	BOOT_STAGE_SMPRO = 0,
	BOOT_STAGE_PMPRO,
	BOOT_STAGE_ATF_BL1,
	BOOT_STAGE_DDR_INIT,
	BOOT_STAGE_DDR_INIT_PROGRESS,
	BOOT_STAGE_ATF_BL2,
	BOOT_STAGE_ATF_BL31,
	BOOT_STAGE_ATF_BL32,
	BOOT_STAGE_UEFI,
	BOOT_STAGE_OS,
	BOOT_STAGE_MAX
};

struct smpro_misc {
	struct regmap *regmap;
};

static int boot_progress_show(struct device *dev,
		struct device_attribute *da, char *buf)
{
	struct smpro_misc *misc = dev_get_drvdata(dev);
	u32 boot_stage_high_reg;
	u32 boot_stage_low_reg;
	u32 current_boot_stage;
	u32 boot_stage_reg;
	u32 boot_progress;
	u8 boot_status;
	u8 boot_stage;
	int stage_cnt;
	int ret;


	/* Read current boot stage */
	ret = regmap_read(misc->regmap,
			BOOT_STAGE_CUR_STAGE_REG, &current_boot_stage);
	if (ret)
		return ret;

	current_boot_stage &= 0xff;
	stage_cnt = 0;
	do {
		/* Read the boot progress */
		ret = regmap_read(misc->regmap,
				BOOT_STAGE_SELECT_REG, &boot_stage_reg);
		if (ret)
			return ret;

		boot_stage = (boot_stage_reg & 0xff00) >> 8;
		boot_status = boot_stage_reg & 0xff;

		if (boot_stage == current_boot_stage)
			break;

		stage_cnt++;
		ret = regmap_write(misc->regmap, BOOT_STAGE_SELECT_REG,
				((boot_stage_reg & 0xff00) | 0x1));
		if (ret)
			return ret;
	/* Never exceeded max number of stages */
	} while (stage_cnt < BOOT_STAGE_MAX);

	if (boot_stage != current_boot_stage)
		goto error;

	switch (boot_stage) {
	case BOOT_STAGE_UEFI:
	case BOOT_STAGE_OS:
		/*
		 * The progress is 32 bits:
		 * B3.byte[0] B3.byte[1] B1.byte[0] B1.byte[1]
		 */
		ret = regmap_read(misc->regmap,	BOOT_STAGE_STATUS_LO_REG,
				&boot_stage_low_reg);
		if (!ret)
			ret = regmap_read(misc->regmap,
					BOOT_STAGE_STATUS_HI_REG,
					&boot_stage_high_reg);
		if (ret)
			return ret;

		boot_progress = swab16(boot_stage_low_reg) |
			swab16(boot_stage_high_reg) << 16;
		goto done;
	default:
		goto done;
	}

error:
	boot_stage = 0xff;
	boot_status = 0xff;
	boot_progress = 0xffffffff;
done:
	return snprintf(buf, PAGE_SIZE, "0x%02x 0x%02x 0x%08X\n",
			boot_stage, boot_status, boot_progress);
}

static DEVICE_ATTR_RO(boot_progress);

static int acpi_power_limit_show(struct device *dev,
				struct device_attribute *da, char *buf)
{
	struct smpro_misc *misc = dev_get_drvdata(dev);
	int ret;
	unsigned int value;

	ret = regmap_read(misc->regmap, ACPI_POWER_LIMIT_REG, &value);
	if (ret)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", value);
}

static int acpi_power_limit_store(struct device *dev,
				struct device_attribute *da,
				const char *buf, size_t count)
{
	struct smpro_misc *misc = dev_get_drvdata(dev);
	unsigned long val;
	s32 ret;

	ret = kstrtoul(buf, 16, &val);

	ret = regmap_write(misc->regmap, ACPI_POWER_LIMIT_REG,
			(unsigned int)val);
	if (ret)
		return -EPROTO;

	return count;
}

static DEVICE_ATTR_RW(acpi_power_limit);

static struct attribute *smpro_misc_attrs[] = {
	&dev_attr_boot_progress.attr,
	&dev_attr_acpi_power_limit.attr,
	NULL
};

static const struct attribute_group smpro_misc_attr_group = {
	.attrs = smpro_misc_attrs
};

static int check_valid_id(struct regmap *regmap)
{
	unsigned int val;
	int ret;

	ret = regmap_read(regmap, MANUFACTURER_ID_REG, &val);
	if (ret)
		return ret;
	return  (val == AMPERE_MANUFACTURER_ID) ? 0 : 1;
}

static int smpro_misc_probe(struct platform_device *pdev)
{
	struct smpro_misc *misc;
	int ret;

	misc = devm_kzalloc(&pdev->dev, sizeof(struct smpro_misc),
			GFP_KERNEL);
	if (!misc)
		return -ENOMEM;

	platform_set_drvdata(pdev, misc);

	misc->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!misc->regmap)
		return -ENODEV;

	/* Check for valid ID */
	ret = check_valid_id(misc->regmap);
	if (ret)
		dev_warn(&pdev->dev, "Hmmh, SMPro not ready yet\n");

	ret = sysfs_create_group(&pdev->dev.kobj, &smpro_misc_attr_group);
	if (ret)
		dev_err(&pdev->dev, "SMPro misc sysfs registration failed\n");

	return 0;
}

static int smpro_misc_remove(struct platform_device *pdev)
{
	sysfs_remove_group(&pdev->dev.kobj, &smpro_misc_attr_group);
	pr_info("SMPro misc sysfs entries removed");

	return 0;
}

static const struct of_device_id smpro_misc_of_match[] = {
	{ .compatible = "ampere,ac01-misc" },
	{}
};
MODULE_DEVICE_TABLE(of, smpro_misc_of_match);

static struct platform_driver smpro_misc_driver = {
	.probe		= smpro_misc_probe,
	.remove		= smpro_misc_remove,
	.driver = {
		.name	= "smpro-misc",
		.of_match_table = smpro_misc_of_match,
	},
};

module_platform_driver(smpro_misc_driver);

MODULE_AUTHOR("Tung Nguyen <tung.nguyen@amperecomputing.com>");
MODULE_AUTHOR("Quan Nguyen <quan@os.amperecomputing.com>");
MODULE_DESCRIPTION("Ampere Altra SMpro Misc driver");
MODULE_LICENSE("GPL");
