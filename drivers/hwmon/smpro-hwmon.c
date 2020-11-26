// SPDX-License-Identifier: GPL-2.0+
/*
 * Ampere Computing SoC's SMpro Hardware Monitoring Driver
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

#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>

/* Identification Registers */
#define REG_SPEC_VERSION_REG		0x00
#define VERSION_REG			0x01
#define MANUFACTURER_ID_REG		0x02
#define DEVICE_ID_REG			0x03
#define BUILD_ID_LO_REG			0x09
#define BUILD_ID_HI_REG			0x0A

/* Capability Registers  */
#define TEMP_SENSOR_SUPPORT_REG		0x05
#define PWR_SENSOR_SUPPORT_REG		0x06
#define VOLT_SENSOR_SUPPORT_REG		0x07
#define OTHER_CAP_REG			0x08
#define CORE_CLUSTER_CNT_REG		0x0B
#define SYS_CACHE_PCIE_CNT_REG		0x0C
#define SOCKET_INFO_REG			0x0D
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
#define SOC_VRD_PWR_REG			0x21
#define DIMM_VRD1_PWR_REG		0x22
#define DIMM_VRD2_PWR_REG		0x23
#define CORE_VRD_PWR_MW_REG		0x26
#define SOC_VRD_PWR_MW_REG		0x27
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

/* Add for DIMM group */
#define DIMM_GROUP_DUMMY_REG		0xFF

/* GPI Control set  Registers */
#define GPI_CTRL0_REG			0x50
#define GPI_CTRL1_REG			0x51
#define GPI_CTRL2_REG			0x52
#define GPI_CTRL3_REG			0x53
#define GPI_CE_UE_MASK_REG		0x54

/* GPI data set Registers */
#define GPI_DATA_SET_REG		0x60
#define GPI_DATA_SET0_REG		0x61
#define GPI_DATA_SET1_REG		0x62
#define GPI_DATA_SET2_REG		0x63
#define GPI_DATA_SET3_REG		0x64

/* GPI Status Registers */
#define GPI_CLUSTER_ERR_SET0_REG	0x70
#define GPI_VRD_FAULT_ERR_REG		0x78
#define GPI_VRD_HOT_ERR_REG		0x79
#define GPI_DIMM_HOT_ERR_REG		0x7A
#define GPI_BOOT_ERR1_REG		0x7B
#define GPI_BOOT_ERR2_REG		0x7C

/* GPI RAS Error Registers */
#define GPI_WDT_STS_REG			0x7D
#define GPI_RAS_ERR_REG			0x7E

/*  SPI NOR Authentication Failure */
#define GPI_SPI_NOR_AUTHFAIL_REG	0x7F

/* Core and L2C Error Registers */
#define CORE_CE_ERR_CNT_REG		0x80
#define CORE_CE_ERR_LEN_REG		0x81
#define CORE_CE_ERR_DATA_REG		0x82
#define CORE_UE_ERR_CNT_REG		0x83
#define CORE_UE_ERR_LEN_REG		0x84
#define CORE_UE_ERR_DATA_REG		0x85

/* Memory Error Registers */
#define MEM_CE_ERR_CNT_REG		0x90
#define MEM_CE_ERR_LEN_REG		0x91
#define MEM_CE_ERR_DATA_REG		0x92
#define MEM_UE_ERR_CNT_REG		0x93
#define MEM_UE_ERR_LEN_REG		0x94
#define MEM_UE_ERR_DATA_REG		0x95

/* RAS Error/Warning Registers */
#define ERR_SMPRO_TYPE_REG		0xA0
#define ERR_PMPRO_TYPE_REG		0xA1
#define ERR_SMPRO_INFO_LO_REG		0xA2
#define ERR_SMPRO_INFO_HI_REG		0xA3
#define ERR_SMPRO_DATA_LO_REG		0xA4
#define ERR_SMPRO_DATA_HI_REG		0xA5
#define WARN_SMPRO_INFO_LO_REG		0xAA
#define WARN_SMPRO_INFO_HI_REG		0xAB
#define ERR_PMPRO_INFO_LO_REG		0xA6
#define ERR_PMPRO_INFO_HI_REG		0xA7
#define ERR_PMPRO_DATA_LO_REG		0xA8
#define ERR_PMPRO_DATA_HI_REG		0xA9
#define WARN_PMPRO_INFO_LO_REG		0xAC
#define WARN_PMPRO_INFO_HI_REG		0xAD

/* Boot Stage/Progress Registers */
#define BOOT_STAGE_SELECT_REG		0xB0
#define BOOT_STAGE_STATUS_LO_REG	0xB1
#define BOOT_STAGE_CUR_STAGE_REG	0xB2
#define BOOT_STAGE_STATUS_HI_REG	0xB2

/* PCIE Error Registers */
#define PCIE_CE_ERR_CNT_REG		0xC0
#define PCIE_CE_ERR_LEN_REG		0xC1
#define PCIE_CE_ERR_DATA_REG		0xC2
#define PCIE_UE_ERR_CNT_REG		0xC3
#define PCIE_UE_ERR_LEN_REG		0xC4
#define PCIE_UE_ERR_DATA_REG		0xC5

/* Other Error Registers */
#define OTHER_CE_ERR_CNT_REG		0xD0
#define OTHER_CE_ERR_LEN_REG		0xD1
#define OTHER_CE_ERR_DATA_REG		0xD2
#define OTHER_UE_ERR_CNT_REG		0xD8
#define OTHER_UE_ERR_LEN_REG		0xD9
#define OTHER_UE_ERR_DATA_REG		0xDA

/* ACPI State Registers */
#define ACPI_SYSTEM_STATE_REG		0xE0
#define ACPI_CPPC_CLUSTER_SEL_REG	0xE3
#define ACPI_CPPC_CLUSTER_DATA_REG	0xE4
#define ACPI_POWER_LIMIT_REG		0xE5

/* I2C read block data constant */
#define MAX_READ_BLOCK_LENGTH		48
#define NUM_I2C_MESSAGES		2
#define MAX_READ_ERROR			35
#define MAX_MSG_LEN			128

#define RAS_SMPRO_ERRS			0
#define RAS_PMPRO_ERRS			1

/* Bit masks */
#define BIT_0				0x0001
#define BIT_1				0x0002
#define BIT_2				0x0004
#define BIT_8				0x0100

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
	{CORE_CE_ERR_CNT_REG, CORE_CE_ERR_LEN_REG,
		CORE_CE_ERR_DATA_REG},
	{CORE_UE_ERR_CNT_REG, CORE_UE_ERR_LEN_REG,
		CORE_UE_ERR_DATA_REG},
	{MEM_CE_ERR_CNT_REG, MEM_CE_ERR_LEN_REG,
		MEM_CE_ERR_DATA_REG},
	{MEM_UE_ERR_CNT_REG, MEM_UE_ERR_LEN_REG,
		MEM_UE_ERR_DATA_REG},
	{PCIE_CE_ERR_CNT_REG, PCIE_CE_ERR_LEN_REG,
		PCIE_CE_ERR_DATA_REG},
	{PCIE_UE_ERR_CNT_REG, PCIE_UE_ERR_LEN_REG,
		PCIE_UE_ERR_DATA_REG},
	{OTHER_CE_ERR_CNT_REG, OTHER_CE_ERR_LEN_REG,
		OTHER_CE_ERR_DATA_REG},
	{OTHER_UE_ERR_CNT_REG, OTHER_UE_ERR_LEN_REG,
		OTHER_UE_ERR_DATA_REG},
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
	{ERR_SMPRO_TYPE_REG,
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

struct smpro_data {
	struct i2c_client *client;

	u16 temp_support_regs;
	u16 pwr_support_regs;
	u16 volt_support_regs;
	u16 other_caps;
	u16 core_cluster_cnt_reg;
	u16 sys_cache_pcie_cnt_reg;
	u16 socket_info_reg;
};

static const u8 temp_regs[] = {
	SOC_TEMP_REG,
	SOC_VRD_TEMP_REG,
	DIMM_VRD_TEMP_REG,
	CORE_VRD_TEMP_REG,
	/* reserved for DIMM G0 */
	DIMM_GROUP_DUMMY_REG,
	CH0_DIMM_TEMP_REG,
	CH1_DIMM_TEMP_REG,
	CH2_DIMM_TEMP_REG,
	CH3_DIMM_TEMP_REG,
	/* reserved for DIMM G1 */
	DIMM_GROUP_DUMMY_REG,
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
	/* VRD1 has higher priority than VRD2. Use VRD1 as output for ddr */
	DIMM_VRD1_VOLT_REG,
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
	PMD_VRD_PWR,
	SOC_VRD_PWR,
	DIMM_VRD1_PWR,
	DIMM_VRD2_PWR,
	CPU_VRD_PWR,
	DIMM_VRD_PWR,
	RCA_VRD_PWR,
	SOC_TDP_PWR,
};
static const char * const label[] = {
	"SoC",
	"SoC VRD",
	"DIMM VRD",
	"DIMM VRD1",
	"DIMM VRD2",
	"PMD VRD",
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
	"DIMM G0",
	"DIMM G1",
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
				 struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct smpro_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int channel = attr->index;
	unsigned char err_data[MAX_READ_BLOCK_LENGTH + 2];
	unsigned char msg[MAX_MSG_LEN] = {'\0'};
	struct smpro_error_hdr errInfo;
	s32 errCount = 1, errLength = 0;
	u8 i = 0;
	s32 ret;

	*buf = 0;
	if (channel >= NUM_48BYTES_ERR_TYPE)
		goto done;

	errInfo = smpro_error_table[channel];

	memset(err_data, 0xff, MAX_READ_BLOCK_LENGTH + 2);

	errCount = i2c_smbus_read_word_swapped(client, errInfo.err_count);
	if (errCount <= 0)
		goto done;

	if (errCount > MAX_READ_ERROR)
		errCount = MAX_READ_ERROR;

	for (i = 0; i < errCount; i++) {
		errLength = i2c_smbus_read_word_swapped(client,
			errInfo.err_len);

		if (errLength <= 0)
			break;

		if (errLength > MAX_READ_BLOCK_LENGTH)
			errLength = MAX_READ_BLOCK_LENGTH;

		ret = read_i2c_block_data(client, errInfo.err_data,
			errLength, err_data);
		if (ret < 0)
			break;

		snprintf(msg, MAX_MSG_LEN, "%s", "");
		format_error_output(err_data, MAX_READ_BLOCK_LENGTH + 2,
			msg, MAX_MSG_LEN);
		strcat(msg, "\n");

		/* go to next error */
		ret = i2c_smbus_write_word_swapped(client,
			errInfo.err_count, 0x100);
		if (ret < 0)
			break;

		/* add error message to buffer */
		strncat(buf, msg, strlen(msg));
	}
done:
	return strlen(buf);
}

static s32 smpro_err_int_get_info(struct i2c_client *client, u8 addr,
	u8 addr1, u8 addr2, u8 addr3, u8 subtype, char *buf)
{
	s32 retHi = 0, retLo = 0, dataLo = 0, dataHi = 0;

	snprintf(buf, MAX_MSG_LEN, "%s", "");
	retLo = i2c_smbus_read_word_swapped(client, addr);
	if (retLo < 0)
		return -1;
	retHi = i2c_smbus_read_word_swapped(client, addr1);
	if (retHi < 0)
		return -1;

	if (addr2 != 0xff) {
		dataLo = i2c_smbus_read_word_swapped(client, addr2);
		if (dataLo < 0)
			return -1;
		dataHi = i2c_smbus_read_word_swapped(client, addr3);
		if (dataHi < 0)
			return -1;
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

static ssize_t smpro_err_internal_read(struct device *dev,
				struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct smpro_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int channel = attr->index;
	struct smpro_int_error_hdr err_info;
	unsigned char msg[MAX_MSG_LEN] = {'\0'};
	s32 ret = 0, curType = 0, ret1 = 0;

	if (!(channel == RAS_SMPRO_ERRS || channel == RAS_PMPRO_ERRS))
		return scnprintf(buf, PAGE_SIZE, "0\n");

	/* read error status */
	ret = i2c_smbus_read_word_swapped(client, GPI_RAS_ERR_REG);
	if (ret < 0)
		return scnprintf(buf, PAGE_SIZE, "0\n");

	if (!((channel == RAS_SMPRO_ERRS && (ret & BIT_0)) ||
		(channel == RAS_PMPRO_ERRS && (ret & BIT_1))))
		return scnprintf(buf, PAGE_SIZE, "0\n");

	err_info = list_smpro_int_error_hdr[channel];
	ret = i2c_smbus_read_word_swapped(client, err_info.err_type);
	curType = ret;
	if (ret < 0)
		return scnprintf(buf, PAGE_SIZE, "0\n");

	/* Warning type */
	if (ret & BIT_0) {
		ret1 = smpro_err_int_get_info(client, err_info.warn_info_low,
					      err_info.warn_info_high,
					      0xff, 0xff, 1, msg);
		if (ret1 < 0)
			return scnprintf(buf, PAGE_SIZE, "0\n");
		strncat(buf, msg, strlen(msg));
	}
	/* Error type */
	if (ret & BIT_1) {
		ret1 = smpro_err_int_get_info(client, err_info.err_info_low,
					      err_info.err_info_high,
					      0xff, 0xff, 2, msg);
		if (ret1 < 0)
			return scnprintf(buf, PAGE_SIZE, "0\n");
		strncat(buf, msg, strlen(msg));
	}
	/* Error with data type */
	if (ret & BIT_2) {
		ret1 = smpro_err_int_get_info(client,
					      err_info.err_info_low,
					      err_info.err_info_high,
					      err_info.err_data_low,
					      err_info.err_data_high, 4, msg);
		if (ret1 < 0)
			return scnprintf(buf, PAGE_SIZE, "0\n");
		strncat(buf, msg, strlen(msg));
	}
	/* clear the read errors */
	ret = i2c_smbus_write_word_swapped(client, err_info.err_type, curType);

	return strlen(buf);
}

static void smpro_init_device(struct i2c_client *client,
				struct smpro_data *data)
{
	s32 ret;

	ret = i2c_smbus_read_word_swapped(client, TEMP_SENSOR_SUPPORT_REG);
	if (ret < 0)
		return;
	data->temp_support_regs = ret;

	ret = i2c_smbus_read_word_swapped(client, PWR_SENSOR_SUPPORT_REG);
	if (ret < 0)
		return;
	data->pwr_support_regs = ret;

	ret = i2c_smbus_read_word_swapped(client, VOLT_SENSOR_SUPPORT_REG);
	if (ret < 0)
		return;
	data->volt_support_regs = ret;

	ret = i2c_smbus_read_word_swapped(client, OTHER_CAP_REG);
	if (ret < 0)
		return;
	data->other_caps = ret;

	ret = i2c_smbus_read_word_swapped(client, CORE_CLUSTER_CNT_REG);
	if (ret < 0)
		return;
	data->core_cluster_cnt_reg = ret;

	ret = i2c_smbus_read_word_swapped(client, SYS_CACHE_PCIE_CNT_REG);
	if (ret < 0)
		return;
	data->sys_cache_pcie_cnt_reg = ret;

	ret = i2c_smbus_read_word_swapped(client, SOCKET_INFO_REG);
	if (ret < 0)
		return;

	data->socket_info_reg = ret;
}

static int smpro_read_temp(struct device *dev, u32 attr, int channel,
			   long *val)
{
	struct smpro_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	s32 highest_temp = -1;
	s32 i = 0;
	s32 ret = -1;

	switch (attr) {
	case hwmon_temp_input:
		if (temp_regs[channel] == DIMM_GROUP_DUMMY_REG) {
			for (i = 1; i <= 4; i++) {
				ret = i2c_smbus_read_word_swapped(client,
					temp_regs[channel + i]);
				if ((ret < 0xff) && (ret > highest_temp))
					highest_temp = ret;
			}
			if (highest_temp <= 0)
				return -ERANGE;
			*val = (highest_temp & 0x1ff) * 1000;
		} else {
			ret = i2c_smbus_read_word_swapped(client,
				temp_regs[channel]);
			if (ret < 0)
				return ret;
			*val = (ret & 0x1ff) * 1000;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int smpro_read_in(struct device *dev, u32 attr, int channel,
			 long *val)
{
	struct smpro_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	s32 ret;

	switch (attr) {
	case hwmon_in_input:
		ret = i2c_smbus_read_word_swapped(client, volt_regs[channel]);
		if (ret < 0)
			return ret;
		*val = ret & 0x7fff;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int smpro_read_curr(struct device *dev, u32 attr, int channel,
			   long *val)
{
	struct smpro_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	s32 ret;

	switch (attr) {
	case hwmon_curr_input:
		ret = i2c_smbus_read_word_swapped(client, curr_regs[channel]);
		if (ret < 0)
			return ret;
		*val = ret & 0x7fff;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int smpro_read_power(struct device *dev, u32 attr, int channel,
			    long *val)
{
	struct smpro_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	s32 ret = 0, ret_mw = 0;
	s32 ret2 = 0, ret2_mw = 0;

	switch (attr) {
	case hwmon_power_input:
		switch (channel) {
		case PMD_VRD_PWR:
			ret = i2c_smbus_read_word_swapped(client,
							  CORE_VRD_PWR_REG);
			ret_mw = i2c_smbus_read_word_swapped(client,
							     CORE_VRD_PWR_MW_REG);
			break;
		case SOC_VRD_PWR:
			ret = i2c_smbus_read_word_swapped(client,
							  SOC_VRD_PWR_REG);
			ret_mw = i2c_smbus_read_word_swapped(client,
							     SOC_VRD_PWR_MW_REG);
			break;
		case DIMM_VRD1_PWR:
			ret = i2c_smbus_read_word_swapped(client,
			DIMM_VRD1_PWR_REG);
			ret_mw = i2c_smbus_read_word_swapped(client,
							     DIMM_VRD1_PWR_MW_REG);
			break;
		case DIMM_VRD2_PWR:
			ret = i2c_smbus_read_word_swapped(client,
							  DIMM_VRD2_PWR_REG);
			ret_mw = i2c_smbus_read_word_swapped(client,
							     DIMM_VRD2_PWR_MW_REG);
			break;
		case RCA_VRD_PWR:
			ret = i2c_smbus_read_word_swapped(client,
							  RCA_VRD_PWR_REG);
			ret_mw = i2c_smbus_read_word_swapped(client,
							     RCA_VRD_PWR_MW_REG);
			break;
		case SOC_TDP_PWR:
			ret = i2c_smbus_read_word_swapped(client,
							  SOC_TDP_REG);
			break;
		case CPU_VRD_PWR:
			ret = i2c_smbus_read_word_swapped(client,
							  CORE_VRD_PWR_REG);
			ret_mw = i2c_smbus_read_word_swapped(client,
							     CORE_VRD_PWR_MW_REG);
			ret2 = i2c_smbus_read_word_swapped(client,
							   SOC_VRD_PWR_REG);
			ret2_mw = i2c_smbus_read_word_swapped(client,
							      SOC_VRD_PWR_MW_REG);
			break;
		case DIMM_VRD_PWR:
			ret = i2c_smbus_read_word_swapped(client,
							  DIMM_VRD1_PWR_REG);
			ret_mw = i2c_smbus_read_word_swapped(client,
							     DIMM_VRD1_PWR_MW_REG);
			ret2 = i2c_smbus_read_word_swapped(client,
							   DIMM_VRD2_PWR_REG);
			ret2_mw = i2c_smbus_read_word_swapped(client,
							      DIMM_VRD2_PWR_MW_REG);
			break;
		default:
			return -EOPNOTSUPP;
		}

		if (ret_mw == 0xffff)
			ret_mw = 0;
		if (ret2_mw == 0xffff)
			ret2_mw = 0;
		if (ret < 0 || ret_mw < 0 || ret2 < 0 || ret2_mw < 0)
			return ret < 0 ? ret : ret_mw < 0 ?
				ret_mw : ret2 < 0 ? ret2 : ret2_mw;

		*val = (ret + ret2)*1000000 + (ret_mw + ret2_mw)*1000;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static ssize_t smpro_read_generic(struct device *dev,
			  struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct smpro_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	s32 ret;

	ret = i2c_smbus_read_word_swapped(client, attr->index);

	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", ret);
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

static ssize_t smpro_write_generic(struct device *dev,
			   struct device_attribute *da,
			   const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct smpro_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	unsigned long val;
	s32 ret;

	ret = kstrtoul(buf, 16, &val);

	ret = i2c_smbus_write_word_swapped(client, attr->index, val);
	if (ret < 0)
		return -EPROTO;

	return count;
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
static SENSOR_DEVICE_ATTR(temp5_label, 0444, show_label, NULL, 19);
static SENSOR_DEVICE_ATTR(temp6_label, 0444, show_label, NULL, 6);
static SENSOR_DEVICE_ATTR(temp7_label, 0444, show_label, NULL, 7);
static SENSOR_DEVICE_ATTR(temp8_label, 0444, show_label, NULL, 8);
static SENSOR_DEVICE_ATTR(temp9_label, 0444, show_label, NULL, 9);
static SENSOR_DEVICE_ATTR(temp10_label, 0444, show_label, NULL, 20);
static SENSOR_DEVICE_ATTR(temp11_label, 0444, show_label, NULL, 10);
static SENSOR_DEVICE_ATTR(temp12_label, 0444, show_label, NULL, 11);
static SENSOR_DEVICE_ATTR(temp13_label, 0444, show_label, NULL, 12);
static SENSOR_DEVICE_ATTR(temp14_label, 0444, show_label, NULL, 13);
static SENSOR_DEVICE_ATTR(temp15_label, 0444, show_label, NULL, 14);
static SENSOR_DEVICE_ATTR(temp16_label, 0444, show_label, NULL, 15);
static SENSOR_DEVICE_ATTR(temp17_label, 0444, show_label, NULL, 17);

static SENSOR_DEVICE_ATTR(in0_label, 0444, show_label, NULL, 5);
static SENSOR_DEVICE_ATTR(in1_label, 0444, show_label, NULL, 1);
static SENSOR_DEVICE_ATTR(in2_label, 0444, show_label, NULL, 3);
static SENSOR_DEVICE_ATTR(in3_label, 0444, show_label, NULL, 4);
static SENSOR_DEVICE_ATTR(in4_label, 0444, show_label, NULL, 2);
static SENSOR_DEVICE_ATTR(in5_label, 0444, show_label, NULL, 17);

static SENSOR_DEVICE_ATTR(power1_label, 0444, show_label, NULL, 5);
static SENSOR_DEVICE_ATTR(power2_label, 0444, show_label, NULL, 1);
static SENSOR_DEVICE_ATTR(power3_label, 0444, show_label, NULL, 3);
static SENSOR_DEVICE_ATTR(power4_label, 0444, show_label, NULL, 4);
static SENSOR_DEVICE_ATTR(power5_label, 0444, show_label, NULL, 16);
static SENSOR_DEVICE_ATTR(power6_label, 0444, show_label, NULL, 2);
static SENSOR_DEVICE_ATTR(power7_label, 0444, show_label, NULL, 17);
static SENSOR_DEVICE_ATTR(power8_label, 0444, show_label, NULL, 18);

static SENSOR_DEVICE_ATTR(gpi_boot_stage_select, 0644,
	smpro_read_generic, smpro_write_generic, BOOT_STAGE_SELECT_REG);
static SENSOR_DEVICE_ATTR(gpi_boot_stage_status_lo, 0444,
	smpro_read_generic, NULL, BOOT_STAGE_STATUS_LO_REG);
static SENSOR_DEVICE_ATTR(gpi_boot_stage_cur_stage, 0444,
	smpro_read_generic, NULL, BOOT_STAGE_CUR_STAGE_REG);
static SENSOR_DEVICE_ATTR(acpi_power_limit, 0644,
	smpro_read_generic, smpro_write_generic, ACPI_POWER_LIMIT_REG);

static SENSOR_DEVICE_ATTR(curr1_label, 0444, show_label, NULL, 5);
static SENSOR_DEVICE_ATTR(curr2_label, 0444, show_label, NULL, 1);
static SENSOR_DEVICE_ATTR(curr3_label, 0444, show_label, NULL, 3);
static SENSOR_DEVICE_ATTR(curr4_label, 0444, show_label, NULL, 4);
static SENSOR_DEVICE_ATTR(curr5_label, 0444, show_label, NULL, 17);

static SENSOR_DEVICE_ATTR(errors_core_ce, 0444, smpro_error_data_read, NULL,
			CORE_CE_ERRS);
static SENSOR_DEVICE_ATTR(errors_core_ue, 0444, smpro_error_data_read, NULL,
			CORE_UE_ERRS);
static SENSOR_DEVICE_ATTR(errors_mem_ce, 0444, smpro_error_data_read, NULL,
			MEM_CE_ERRS);
static SENSOR_DEVICE_ATTR(errors_mem_ue, 0444, smpro_error_data_read, NULL,
			MEM_UE_ERRS);
static SENSOR_DEVICE_ATTR(errors_pcie_ce, 0444, smpro_error_data_read, NULL,
			PCIE_CE_ERRS);
static SENSOR_DEVICE_ATTR(errors_pcie_ue, 0444, smpro_error_data_read, NULL,
			PCIE_UE_ERRS);
static SENSOR_DEVICE_ATTR(errors_other_ce, 0444, smpro_error_data_read, NULL,
			OTHER_CE_ERRS);
static SENSOR_DEVICE_ATTR(errors_other_ue, 0444, smpro_error_data_read, NULL,
			OTHER_UE_ERRS);
static SENSOR_DEVICE_ATTR(errors_smpro, 0444, smpro_err_internal_read, NULL,
			RAS_SMPRO_ERRS);
static SENSOR_DEVICE_ATTR(errors_pmpro, 0444, smpro_err_internal_read, NULL,
			RAS_PMPRO_ERRS);

static struct attribute *smpro_attrs[] = {
	&sensor_dev_attr_gpi_boot_stage_select.dev_attr.attr,
	&sensor_dev_attr_gpi_boot_stage_status_lo.dev_attr.attr,
	&sensor_dev_attr_gpi_boot_stage_cur_stage.dev_attr.attr,
	&sensor_dev_attr_acpi_power_limit.dev_attr.attr,
	&sensor_dev_attr_errors_core_ce.dev_attr.attr,
	&sensor_dev_attr_errors_core_ue.dev_attr.attr,
	&sensor_dev_attr_errors_mem_ce.dev_attr.attr,
	&sensor_dev_attr_errors_mem_ue.dev_attr.attr,
	&sensor_dev_attr_errors_pcie_ce.dev_attr.attr,
	&sensor_dev_attr_errors_pcie_ue.dev_attr.attr,
	&sensor_dev_attr_errors_other_ce.dev_attr.attr,
	&sensor_dev_attr_errors_other_ue.dev_attr.attr,
	&sensor_dev_attr_errors_smpro.dev_attr.attr,
	&sensor_dev_attr_errors_pmpro.dev_attr.attr,

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
	&sensor_dev_attr_temp16_label.dev_attr.attr,
	&sensor_dev_attr_temp17_label.dev_attr.attr,

	&sensor_dev_attr_in0_label.dev_attr.attr,
	&sensor_dev_attr_in1_label.dev_attr.attr,
	&sensor_dev_attr_in2_label.dev_attr.attr,
	&sensor_dev_attr_in3_label.dev_attr.attr,
	&sensor_dev_attr_in4_label.dev_attr.attr,
	&sensor_dev_attr_in5_label.dev_attr.attr,

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
	&sensor_dev_attr_power7_label.dev_attr.attr,
	&sensor_dev_attr_power8_label.dev_attr.attr,

	NULL
};
ATTRIBUTE_GROUPS(smpro);

static int smpro_i2c_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = client->adapter;
	struct device *dev = &client->dev;
	struct smpro_data *data;
	struct device *hwmon_dev;

	if (!i2c_check_functionality(adapter,
			I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA))
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(struct smpro_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;

	/* Initialize the SMpro chip */
	smpro_init_device(client, data);

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name,
							 data,
							 &smpro_chip_info,
							 smpro_groups);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct of_device_id smpro_match[] = {
	{ .compatible = "ampere,ac01-smpro" },
	{}
};

static const struct i2c_device_id smpro_id[] = {
	{ "smpro", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, smpro_id);

static struct i2c_driver smpro_driver = {
	.class		= I2C_CLASS_HWMON,
	.probe		= smpro_i2c_probe,
	.driver = {
		.name	= "smpro",
		.of_match_table = smpro_match,
	},
	.id_table	= smpro_id,
};

module_i2c_driver(smpro_driver);

MODULE_AUTHOR("Thinh Pham <thinh.pham@amperecomputing.com>");
MODULE_AUTHOR("Hoang Nguyen <hnguyen@amperecomputing.com>");
MODULE_AUTHOR("Thu Nguyen <tbnguyen@amperecomputing.com>");
MODULE_DESCRIPTION("Ampere Altra SMpro driver");
MODULE_LICENSE("GPL");
