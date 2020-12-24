// SPDX-License-Identifier: GPL-2.0+
/*
 * Ampere Computing SoC's SMpro Error Monitoring Driver
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

#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* Identification Registers */
#define MANUFACTURER_ID_REG	0x02
#define AMPERE_MANUFACTURER_ID	0xCD3A

/* GPI RAS Error Registers */
#define GPI_RAS_ERR_REG		0x7E

/* Core and L2C Error Registers */
#define CORE_CE_ERR_CNT_REG	0x80
#define CORE_CE_ERR_LEN_REG	0x81
#define CORE_CE_ERR_DATA_REG	0x82
#define CORE_UE_ERR_CNT_REG	0x83
#define CORE_UE_ERR_LEN_REG	0x84
#define CORE_UE_ERR_DATA_REG	0x85

/* Memory Error Registers */
#define MEM_CE_ERR_CNT_REG	0x90
#define MEM_CE_ERR_LEN_REG	0x91
#define MEM_CE_ERR_DATA_REG	0x92
#define MEM_UE_ERR_CNT_REG	0x93
#define MEM_UE_ERR_LEN_REG	0x94
#define MEM_UE_ERR_DATA_REG	0x95

/* RAS Error/Warning Registers */
#define ERR_SMPRO_TYPE_REG	0xA0
#define ERR_PMPRO_TYPE_REG	0xA1
#define ERR_SMPRO_INFO_LO_REG	0xA2
#define ERR_SMPRO_INFO_HI_REG	0xA3
#define ERR_SMPRO_DATA_LO_REG	0xA4
#define ERR_SMPRO_DATA_HI_REG	0xA5
#define WARN_SMPRO_INFO_LO_REG	0xAA
#define WARN_SMPRO_INFO_HI_REG	0xAB
#define ERR_PMPRO_INFO_LO_REG	0xA6
#define ERR_PMPRO_INFO_HI_REG	0xA7
#define ERR_PMPRO_DATA_LO_REG	0xA8
#define ERR_PMPRO_DATA_HI_REG	0xA9
#define WARN_PMPRO_INFO_LO_REG	0xAC
#define WARN_PMPRO_INFO_HI_REG	0xAD

/* PCIE Error Registers */
#define PCIE_CE_ERR_CNT_REG	0xC0
#define PCIE_CE_ERR_LEN_REG	0xC1
#define PCIE_CE_ERR_DATA_REG	0xC2
#define PCIE_UE_ERR_CNT_REG	0xC3
#define PCIE_UE_ERR_LEN_REG	0xC4
#define PCIE_UE_ERR_DATA_REG	0xC5

/* Other Error Registers */
#define OTHER_CE_ERR_CNT_REG	0xD0
#define OTHER_CE_ERR_LEN_REG	0xD1
#define OTHER_CE_ERR_DATA_REG	0xD2
#define OTHER_UE_ERR_CNT_REG	0xD8
#define OTHER_UE_ERR_LEN_REG	0xD9
#define OTHER_UE_ERR_DATA_REG	0xDA

/* I2C read block data constant */
#define MAX_READ_BLOCK_LENGTH	48
#define NUM_I2C_MESSAGES	2
#define MAX_READ_ERROR		35
#define MAX_MSG_LEN		128

#define RAS_SMPRO_ERRS		0
#define RAS_PMPRO_ERRS		1

/* Bit masks */
#define BIT_0			0x0001
#define BIT_1			0x0002
#define BIT_2			0x0004
#define BIT_8			0x0100

enum RAS_48BYTES_ERR_TYPES {
	CORE_CE_ERRS,
	CORE_UE_ERRS,
	MEM_CE_ERRS,
	MEM_UE_ERRS,
	PCIE_CE_ERRS,
	PCIE_UE_ERRS,
	OTHER_CE_ERRS,
	OTHER_UE_ERRS,
	NUM_48BYTES_ERR_TYPE,
};

/*
 * The output of Core/Memory/PCIe/Others UE/CE errors follows below format:
 * <Error Type>  <Error SubType>  <Instance>  <Error Status> \
 * <Error Address>  <Error Misc 0> <Error Misc 1> <Error Misc2> <Error Misc 3>
 * Where:
 *  + Error Type: The hardwares cause the errors. (1 byte)
 *  + SubType: Sub type of error in the specified hardware error. (1 byte)
 *  + Instance: Combination of the socket, channel,
 *    slot cause the error. (2 bytes)
 *  + Error Status: Encode of error status. (4 bytes)
 *  + Error Address: The address in device causes the errors. (8 bytes)
 *  + Error Misc 0/1/2/3: Addition info about the errors. (8 bytes for each)
 * Reference Altra SOC BMC Interface specification.
 */
u_int8_t field_size[9] = {1, 1, 2, 4, 8, 8, 8, 8, 8};

struct smpro_error_hdr {
	u8 err_count;	/* Number of the RAS errors */
	u8 err_len;	/* Number of data bytes */
	u8 err_data;	/* Start of 48-byte data */
};
/*
 * Included Address of registers to get Count, Length of data and Data
 * of the 48 bytes error data
 */
struct smpro_error_hdr smpro_error_table[NUM_48BYTES_ERR_TYPE] = {
	{CORE_CE_ERR_CNT_REG, CORE_CE_ERR_LEN_REG, CORE_CE_ERR_DATA_REG},
	{CORE_UE_ERR_CNT_REG, CORE_UE_ERR_LEN_REG, CORE_UE_ERR_DATA_REG},
	{MEM_CE_ERR_CNT_REG, MEM_CE_ERR_LEN_REG, MEM_CE_ERR_DATA_REG},
	{MEM_UE_ERR_CNT_REG, MEM_UE_ERR_LEN_REG, MEM_UE_ERR_DATA_REG},
	{PCIE_CE_ERR_CNT_REG, PCIE_CE_ERR_LEN_REG, PCIE_CE_ERR_DATA_REG},
	{PCIE_UE_ERR_CNT_REG, PCIE_UE_ERR_LEN_REG, PCIE_UE_ERR_DATA_REG},
	{OTHER_CE_ERR_CNT_REG, OTHER_CE_ERR_LEN_REG, OTHER_CE_ERR_DATA_REG},
	{OTHER_UE_ERR_CNT_REG, OTHER_UE_ERR_LEN_REG, OTHER_UE_ERR_DATA_REG},
};

/*
 * List of SCP registers which are used to get
 * one type of RAS Internal errors.
 */
struct smpro_int_error_hdr {
	u8 err_type;
	u8 err_info_low;
	u8 err_info_high;
	u8 err_data_high;
	u8 err_data_low;
	u8 warn_info_low;
	u8 warn_info_high;
};

struct smpro_int_error_hdr list_smpro_int_error_hdr[2] = {
	{
	 ERR_SMPRO_TYPE_REG,
	 ERR_SMPRO_INFO_LO_REG, ERR_SMPRO_INFO_HI_REG,
	 ERR_SMPRO_DATA_LO_REG, ERR_SMPRO_DATA_HI_REG,
	 WARN_SMPRO_INFO_LO_REG, WARN_SMPRO_INFO_HI_REG
	},
	{
	 ERR_PMPRO_TYPE_REG,
	 ERR_PMPRO_INFO_LO_REG, ERR_PMPRO_INFO_HI_REG,
	 ERR_PMPRO_DATA_LO_REG, ERR_PMPRO_DATA_HI_REG,
	 WARN_PMPRO_INFO_LO_REG, WARN_PMPRO_INFO_HI_REG
	},
};

struct smpro_errmon {
	struct regmap *regmap;
};

static int read_i2c_block_data(struct i2c_client *client,
				u16 address, u16 length, u8 *data)
{
	struct i2c_msg msgs[2];
	unsigned char msgbuf0[2];
	unsigned char msgbuf1[MAX_READ_BLOCK_LENGTH + 2];
	ssize_t ret;
	u8 i = 0;

	if (length > MAX_READ_BLOCK_LENGTH)
		return -EINVAL;

	msgbuf0[0] = (address & 0xff);
	msgbuf0[1] = length;

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags & I2C_M_TEN;
	msgs[0].len = 2;
	msgs[0].buf = msgbuf0;

	msgs[1].addr = client->addr;
	msgs[1].flags = (client->flags  & I2C_M_TEN) | I2C_M_RD;
	msgs[1].len = length;
	msgs[1].buf = msgbuf1;

	ret = i2c_transfer(client->adapter, msgs, NUM_I2C_MESSAGES);
	if (ret < 0)
		return ret;

	if (ret != NUM_I2C_MESSAGES)
		return -EIO;

	for (i = 0; i < length; i++)
		data[i] = msgbuf1[i];

	return length;
}

static int errmon_read_block(struct regmap *map, u16 address, u16 length, u8 *data)
{
	struct i2c_client *client = to_i2c_client(regmap_get_device(map));
	int ret;

	regmap_acquire_lock(map);

	ret = read_i2c_block_data(client, address, length, data);

	regmap_release_lock(map);

	return ret;
}

static int format_error_output(unsigned char datas[], size_t data_len,
			       char *buf, size_t buf_len)
{
	unsigned char str[3] = {'\0'};
	u8 x = 0, y = 0, curPos = 0;

	if (data_len < MAX_READ_BLOCK_LENGTH + 2)
		return 0;
	if (buf_len < MAX_MSG_LEN)
		return 0;

	for (x = 0; x < sizeof(field_size); x++) {
		for (y = 0; y < field_size[x]; y++) {
			snprintf(str, 3, "%02x",
				datas[curPos + field_size[x] - y - 1]);
			strncat(buf, str, strlen(str));
		}
		strncat(buf, " ", strlen(" "));
		curPos = curPos + field_size[x];
	}
	return 1;
}

static ssize_t smpro_error_data_read(struct device *dev,
				 struct device_attribute *da, char *buf, int channel)
{
	struct smpro_errmon *errmon = dev_get_drvdata(dev);
	unsigned char err_data[MAX_READ_BLOCK_LENGTH + 2];
	unsigned char msg[MAX_MSG_LEN] = {'\0'};
	struct smpro_error_hdr err_info;
	s32 err_count = 1, err_length = 0;
	u8 i = 0;
	int ret;

	*buf = 0;
	if (channel >= NUM_48BYTES_ERR_TYPE)
		goto done;

	err_info = smpro_error_table[channel];

	memset(err_data, 0xff, MAX_READ_BLOCK_LENGTH + 2);

	ret = regmap_read(errmon->regmap, err_info.err_count, &err_count);
	if (ret || err_count <= 0)
		goto done;

	if (err_count > MAX_READ_ERROR)
		err_count = MAX_READ_ERROR;

	for (i = 0; i < err_count; i++) {
		ret = regmap_read(errmon->regmap, err_info.err_len, &err_length);
		if (ret || err_length <= 0)
			break;

		if (err_length > MAX_READ_BLOCK_LENGTH)
			err_length = MAX_READ_BLOCK_LENGTH;

		ret = errmon_read_block(errmon->regmap, err_info.err_data,
			err_length, err_data);
		if (ret < 0)
			break;

		snprintf(msg, MAX_MSG_LEN, "%s", "");
		format_error_output(err_data, MAX_READ_BLOCK_LENGTH + 2,
			msg, MAX_MSG_LEN);
		strcat(msg, "\n");

		/* go to next error */
		ret = regmap_write(errmon->regmap, err_info.err_count, 0x100);
		if (ret)
			break;

		/* add error message to buffer */
		strncat(buf, msg, strlen(msg));
	}
done:
	return strlen(buf);
}

static s32 smpro_internal_err_get_info(struct regmap *regmap, u8 addr,
	u8 addr1, u8 addr2, u8 addr3, u8 subtype, char *buf)
{
	unsigned int retHi = 0, retLo = 0, dataLo = 0, dataHi = 0;
	int ret;

	snprintf(buf, MAX_MSG_LEN, "%s", "");

	ret = regmap_read(regmap, addr, &retLo);
	if (ret)
		return ret;

	ret = regmap_read(regmap, addr1, &retHi);
	if (ret)
		return ret;

	if (addr2 != 0xff) {
		ret = regmap_read(regmap, addr2, &dataLo);
		if (ret)
			return ret;
		ret = regmap_read(regmap, addr3, &dataHi);
		if (ret)
			return ret;
	}
	/*
	 * Output format:
	 * <errType> <image> <dir> <Location> <errorCode> <data>
	 * Where:
	 *   + errType: SCP Error Type (3 bits)
	 *      1: Warning
	 *      2: Error
	 *      4: Error with data
	 *   + image: SCP Image Code (8 bits)
	 *   + dir: Direction (1 bit)
	 *      0: Enter
	 *      1: Exit
	 *   + location: SCP Module Location Code (8 bits)
	 *   + errorCode: SCP Error Code (16 bits)
	 *   + data : Extensive data (32 bits)
	 *      All bits are 0 when errType is warning or error.
	 */
	scnprintf(buf, MAX_MSG_LEN, "%01x %02x %01x %02x %04x %04x%04x\n",
			subtype, (retHi & 0xf000) >> 12,
			(retHi & 0x0800) >> 11, retHi & 0xff, retLo,
			dataHi, dataLo);

	return strlen(buf);
}

static ssize_t smpro_internal_err_read(struct device *dev,
				struct device_attribute *da, char *buf, int channel)
{
	struct smpro_errmon *errmon = dev_get_drvdata(dev);
	struct smpro_int_error_hdr err_info;
	unsigned char msg[MAX_MSG_LEN] = {'\0'};
	unsigned int err_type;
	unsigned int value;
	int ret = 0;

	*buf = 0;

	if (!(channel == RAS_SMPRO_ERRS || channel == RAS_PMPRO_ERRS))
		goto done;

	/* read error status */
	ret = regmap_read(errmon->regmap, GPI_RAS_ERR_REG, &value);
	if (ret)
		goto done;

	if (!((channel == RAS_SMPRO_ERRS && (value & BIT_0)) ||
		(channel == RAS_PMPRO_ERRS && (value & BIT_1))))
		goto done;

	err_info = list_smpro_int_error_hdr[channel];
	ret = regmap_read(errmon->regmap, err_info.err_type, &err_type);
	if (ret)
		goto done;

	/* Warning type */
	if (err_type & BIT_0) {
		ret = smpro_internal_err_get_info(errmon->regmap,
						err_info.warn_info_low,
						err_info.warn_info_high,
						0xff, 0xff, 1, msg);
		if (ret < 0)
			goto done;

		strncat(buf, msg, strlen(msg));
	}

	/* Error with data type */
	if (err_type & BIT_2) {
		ret = smpro_internal_err_get_info(errmon->regmap,
					      err_info.err_info_low,
					      err_info.err_info_high,
					      err_info.err_data_low,
					      err_info.err_data_high, 4, msg);
		if (ret < 0)
			goto done;

		strncat(buf, msg, strlen(msg));
	}
	/* Error type */
	else if (err_type & BIT_1) {
		ret = smpro_internal_err_get_info(errmon->regmap,
						err_info.err_info_low,
						err_info.err_info_high,
						0xff, 0xff, 2, msg);
		if (ret < 0)
			goto done;

		strncat(buf, msg, strlen(msg));
	}

	/* clear the read errors */
	regmap_write(errmon->regmap, err_info.err_type, err_type);

done:
	return strlen(buf);
}

static int errors_core_ce_show(struct device *dev,
		struct device_attribute *da, char *buf)
{
	return smpro_error_data_read(dev, da, buf, CORE_CE_ERRS);
}
static DEVICE_ATTR_RO(errors_core_ce);

static int errors_core_ue_show(struct device *dev,
		struct device_attribute *da, char *buf)
{
	return smpro_error_data_read(dev, da, buf, CORE_UE_ERRS);
}
static DEVICE_ATTR_RO(errors_core_ue);

static int errors_mem_ce_show(struct device *dev,
		struct device_attribute *da, char *buf)
{
	return smpro_error_data_read(dev, da, buf, MEM_CE_ERRS);
}
static DEVICE_ATTR_RO(errors_mem_ce);

static int errors_mem_ue_show(struct device *dev,
		struct device_attribute *da, char *buf)
{
	return smpro_error_data_read(dev, da, buf, MEM_UE_ERRS);
}
static DEVICE_ATTR_RO(errors_mem_ue);

static int errors_pcie_ce_show(struct device *dev,
		struct device_attribute *da, char *buf)
{
	return smpro_error_data_read(dev, da, buf, PCIE_CE_ERRS);
}
static DEVICE_ATTR_RO(errors_pcie_ce);

static int errors_pcie_ue_show(struct device *dev,
		struct device_attribute *da, char *buf)
{
	return smpro_error_data_read(dev, da, buf, PCIE_UE_ERRS);
}
static DEVICE_ATTR_RO(errors_pcie_ue);

static int errors_other_ce_show(struct device *dev,
		struct device_attribute *da, char *buf)
{
	return smpro_error_data_read(dev, da, buf, OTHER_CE_ERRS);
}
static DEVICE_ATTR_RO(errors_other_ce);

static int errors_other_ue_show(struct device *dev,
		struct device_attribute *da, char *buf)
{
	return smpro_error_data_read(dev, da, buf, OTHER_UE_ERRS);
}
static DEVICE_ATTR_RO(errors_other_ue);

static int errors_smpro_show(struct device *dev,
		struct device_attribute *da, char *buf)
{
	return smpro_internal_err_read(dev, da, buf, RAS_SMPRO_ERRS);
}
static DEVICE_ATTR_RO(errors_smpro);

static int errors_pmpro_show(struct device *dev,
		struct device_attribute *da, char *buf)
{
	return smpro_internal_err_read(dev, da, buf, RAS_PMPRO_ERRS);
}
static DEVICE_ATTR_RO(errors_pmpro);

static struct attribute *smpro_errmon_attrs[] = {
	&dev_attr_errors_core_ce.attr,
	&dev_attr_errors_core_ue.attr,
	&dev_attr_errors_mem_ce.attr,
	&dev_attr_errors_mem_ue.attr,
	&dev_attr_errors_pcie_ce.attr,
	&dev_attr_errors_pcie_ue.attr,
	&dev_attr_errors_other_ce.attr,
	&dev_attr_errors_other_ue.attr,
	&dev_attr_errors_smpro.attr,
	&dev_attr_errors_pmpro.attr,
	NULL
};

static const struct attribute_group smpro_errmon_attr_group = {
	.attrs = smpro_errmon_attrs
};

static int check_valid_id (struct regmap *regmap)
{
	unsigned int val;
	int ret;

	ret = regmap_read(regmap, MANUFACTURER_ID_REG, &val);
	if (ret)
		return ret;
	return (val == AMPERE_MANUFACTURER_ID) ? 0 : 1;
}

static int smpro_errmon_probe(struct platform_device *pdev)
{
	struct smpro_errmon *errmon;
	int ret;

	errmon = devm_kzalloc(&pdev->dev, sizeof(struct smpro_errmon),
			GFP_KERNEL);
	if (!errmon)
		return -ENOMEM;

	platform_set_drvdata(pdev, errmon);

	errmon->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!errmon->regmap)
		return -ENODEV;

	/* Check for valid ID */
	ret = check_valid_id(errmon->regmap);
	if (ret)
		dev_warn(&pdev->dev, "Hmmh, SMPro not ready yet\n");

	ret = sysfs_create_group(&pdev->dev.kobj, &smpro_errmon_attr_group);
	if (ret)
		dev_err(&pdev->dev, "SMPro errmon sysfs registration failed\n");

	return 0;
}

static int smpro_errmon_remove(struct platform_device *pdev)
{
	sysfs_remove_group(&pdev->dev.kobj, &smpro_errmon_attr_group);
	pr_info("SMPro errmon sysfs entries removed");

	return 0;
}

static const struct of_device_id smpro_errmon_of_match[] = {
	{ .compatible = "ampere,ac01-errmon" },
	{}
};
MODULE_DEVICE_TABLE(of, smpro_errmon_of_match);

static struct platform_driver smpro_errmon_driver = {
	.probe          = smpro_errmon_probe,
	.remove         = smpro_errmon_remove,
	.driver = {
		.name   = "smpro-errmon",
		.of_match_table = smpro_errmon_of_match,
	},
};

module_platform_driver(smpro_errmon_driver);

MODULE_AUTHOR("Tung Nguyen <tung.nguyen@amperecomputing.com>");
MODULE_AUTHOR("Thinh Pham <thinh.pham@amperecomputing.com>");
MODULE_AUTHOR("Hoang Nguyen <hnguyen@amperecomputing.com>");
MODULE_AUTHOR("Thu Nguyen <tbnguyen@amperecomputing.com>");
MODULE_DESCRIPTION("Ampere Altra SMpro driver");
MODULE_LICENSE("GPL");
