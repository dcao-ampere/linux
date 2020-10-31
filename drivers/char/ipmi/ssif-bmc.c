// SPDX-License-Identifier: GPL-2.0+
/*
 * The driver for BMC side of SSIF interface
 *
 * Copyright (c) 2020, Ampere Computing LLC
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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ipmi_smi.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/regmap.h>
#include <linux/sched.h>
#include <linux/ssif-bmc.h>
#include <linux/timer.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define PFX		"IPMI SSIF BMC : "

#define DEVICE_NAME	"ipmi-ssif-host"

#define MSG_PAYLOAD_LEN_MAX			252

/* A standard SMBus Transaction is limited to 32 data bytes */
#define MAX_PAYLOAD_PER_TRANSACTION		32

#define MAX_IPMI_DATA_PER_START_TRANSACTION	30
#define MAX_IPMI_DATA_PER_MIDDLE_TRANSACTION	31

#define	SSIF_IPMI_REQUEST			2
#define	SSIF_IPMI_MULTI_PART_REQUEST_START	6
#define	SSIF_IPMI_MULTI_PART_REQUEST_MIDDLE	7
#define	SSIF_IPMI_RESPONSE			3
#define	SSIF_IPMI_MULTI_PART_RESPONSE_MIDDLE	9

struct ssif_msg {
	u8 len;
	u8 netfn_lun;
	u8 cmd;
	u8 payload[MSG_PAYLOAD_LEN_MAX];
} __packed;

static inline u32 ssif_msg_len(struct ssif_msg *ssif_msg)
{
	return ssif_msg->len + 1;
}

struct ssif_bmc {
	struct i2c_client	*client;
	struct aspeed_i2c_bus	*i2c_bus;
	struct miscdevice	miscdev;
	u8			smbus_cmd;
	struct ssif_msg		request;
	bool			request_available;
	struct ssif_msg		response;
	bool			response_in_progress;
	/* Response buffer for Multi-part Read command */
	u8			response_buffer[MAX_PAYLOAD_PER_TRANSACTION];
	/* Flag to identify the response is a multi-part */
	bool			is_multi_part;
	bool			middle_start_response;
	u8			num_bytes_processed;
	u8			remain_data_len;
	/*  Block Number of Multi-part Read Middle command */
	u8			block_num;
	size_t			msg_idx;
	spinlock_t		lock;
	wait_queue_head_t	wait_queue;
	struct mutex		file_mutex;
};

/* ASPEED I2C Register */
#define ASPEED_I2C_FUN_CTRL_REG				0x00
#define ASPEED_I2C_AC_TIMING_REG1			0x04
#define ASPEED_I2C_AC_TIMING_REG2			0x08
#define ASPEED_I2C_INTR_CTRL_REG			0x0c
#define ASPEED_I2C_INTR_STS_REG				0x10
#define ASPEED_I2C_CMD_REG				0x14
#define ASPEED_I2C_DEV_ADDR_REG				0x18
#define ASPEED_I2C_BYTE_BUF_REG				0x20

/* Global Register Definition */
/* 0x00 : I2C Interrupt Status Register  */
/* 0x08 : I2C Interrupt Target Assignment  */

/* Device Register Definition */
/* 0x00 : I2CD Function Control Register  */
#define ASPEED_I2CD_MULTI_MASTER_DIS			BIT(15)
#define ASPEED_I2CD_SDA_DRIVE_1T_EN			BIT(8)
#define ASPEED_I2CD_M_SDA_DRIVE_1T_EN			BIT(7)
#define ASPEED_I2CD_M_HIGH_SPEED_EN			BIT(6)
#define ASPEED_I2CD_SLAVE_EN				BIT(1)
#define ASPEED_I2CD_MASTER_EN				BIT(0)

/* 0x04 : I2CD Clock and AC Timing Control Register #1 */
#define ASPEED_I2CD_TIME_TBUF_MASK			GENMASK(31, 28)
#define ASPEED_I2CD_TIME_THDSTA_MASK			GENMASK(27, 24)
#define ASPEED_I2CD_TIME_TACST_MASK			GENMASK(23, 20)
#define ASPEED_I2CD_TIME_SCL_HIGH_SHIFT			16
#define ASPEED_I2CD_TIME_SCL_HIGH_MASK			GENMASK(19, 16)
#define ASPEED_I2CD_TIME_SCL_LOW_SHIFT			12
#define ASPEED_I2CD_TIME_SCL_LOW_MASK			GENMASK(15, 12)
#define ASPEED_I2CD_TIME_BASE_DIVISOR_MASK		GENMASK(3, 0)
#define ASPEED_I2CD_TIME_SCL_REG_MAX			GENMASK(3, 0)
/* 0x08 : I2CD Clock and AC Timing Control Register #2 */
#define ASPEED_NO_TIMEOUT_CTRL				0

/* 0x0c : I2CD Interrupt Control Register &
 * 0x10 : I2CD Interrupt Status Register
 *
 * These share bit definitions, so use the same values for the enable &
 * status bits.
 */
#define ASPEED_I2CD_INTR_SDA_DL_TIMEOUT			BIT(14)
#define ASPEED_I2CD_INTR_BUS_RECOVER_DONE		BIT(13)
#define ASPEED_I2CD_INTR_SLAVE_MATCH			BIT(7)
#define ASPEED_I2CD_INTR_SCL_TIMEOUT			BIT(6)
#define ASPEED_I2CD_INTR_ABNORMAL			BIT(5)
#define ASPEED_I2CD_INTR_NORMAL_STOP			BIT(4)
#define ASPEED_I2CD_INTR_ARBIT_LOSS			BIT(3)
#define ASPEED_I2CD_INTR_RX_DONE			BIT(2)
#define ASPEED_I2CD_INTR_TX_NAK				BIT(1)
#define ASPEED_I2CD_INTR_TX_ACK				BIT(0)
#define ASPEED_I2CD_INTR_ALL						       \
		(ASPEED_I2CD_INTR_SDA_DL_TIMEOUT |			       \
		 ASPEED_I2CD_INTR_BUS_RECOVER_DONE |			       \
		 ASPEED_I2CD_INTR_SCL_TIMEOUT |				       \
		 ASPEED_I2CD_INTR_ABNORMAL |				       \
		 ASPEED_I2CD_INTR_NORMAL_STOP |				       \
		 ASPEED_I2CD_INTR_ARBIT_LOSS |				       \
		 ASPEED_I2CD_INTR_RX_DONE |				       \
		 ASPEED_I2CD_INTR_TX_NAK |				       \
		 ASPEED_I2CD_INTR_TX_ACK)

/* 0x14 : I2CD Command/Status Register   */
#define ASPEED_I2CD_SCL_LINE_STS			BIT(18)
#define ASPEED_I2CD_SDA_LINE_STS			BIT(17)
#define ASPEED_I2CD_BUS_BUSY_STS			BIT(16)
#define ASPEED_I2CD_BUS_RECOVER_CMD			BIT(11)

/* Command Bit */
#define ASPEED_I2CD_M_STOP_CMD				BIT(5)
#define ASPEED_I2CD_M_S_RX_CMD_LAST			BIT(4)
#define ASPEED_I2CD_M_RX_CMD				BIT(3)
#define ASPEED_I2CD_S_TX_CMD				BIT(2)
#define ASPEED_I2CD_M_TX_CMD				BIT(1)
#define ASPEED_I2CD_M_START_CMD				BIT(0)

/* 0x18 : I2CD Slave Device Address Register   */
#define ASPEED_I2CD_DEV_ADDR_MASK			GENMASK(6, 0)

enum aspeed_i2c_master_state {
	ASPEED_I2C_MASTER_INACTIVE,
	ASPEED_I2C_MASTER_START,
	ASPEED_I2C_MASTER_TX_FIRST,
	ASPEED_I2C_MASTER_TX,
	ASPEED_I2C_MASTER_RX_FIRST,
	ASPEED_I2C_MASTER_RX,
	ASPEED_I2C_MASTER_STOP,
};

enum aspeed_i2c_slave_state {
	ASPEED_I2C_SLAVE_STOP,
	ASPEED_I2C_SLAVE_START,
	ASPEED_I2C_SLAVE_READ_REQUESTED,
	ASPEED_I2C_SLAVE_READ_PROCESSED,
	ASPEED_I2C_SLAVE_WRITE_REQUESTED,
	ASPEED_I2C_SLAVE_WRITE_RECEIVED,
};

struct aspeed_i2c_bus {
	struct i2c_adapter		adap;
	struct device			*dev;
	void __iomem			*base;
	struct reset_control		*rst;
	/* Synchronizes I/O mem access to base. */
	spinlock_t			lock;
	struct completion		cmd_complete;
	u32				(*get_clk_reg_val)(u32 divisor);
	unsigned long			parent_clk_frequency;
	u32				bus_frequency;
	/* Transaction state. */
	enum aspeed_i2c_master_state	master_state;
	struct i2c_msg			*msgs;
	size_t				buf_index;
	size_t				msgs_index;
	size_t				msgs_count;
	bool				send_stop;
	int				cmd_err;
	/* Protected only by i2c_lock_bus */
	int				master_xfer_result;
#if IS_ENABLED(CONFIG_I2C_SLAVE)
	struct i2c_client		*slave;
	enum aspeed_i2c_slave_state	slave_state;
	bool				stop_bus;
#endif /* CONFIG_I2C_SLAVE */
};

void aspeed_i2c_disable_interrupt(struct aspeed_i2c_bus *bus,
		unsigned long mask)
{
	unsigned long current_mask;

	current_mask = readl(bus->base + ASPEED_I2C_INTR_CTRL_REG);
	writel(current_mask & ~mask, bus->base + ASPEED_I2C_INTR_CTRL_REG);
}

void aspeed_i2c_disable_slave(struct aspeed_i2c_bus *bus)
{
	unsigned long flags;

	spin_lock_irqsave(&bus->lock, flags);

	aspeed_i2c_disable_interrupt(bus, ASPEED_I2CD_INTR_RX_DONE);
	aspeed_i2c_disable_interrupt(bus, ASPEED_I2CD_INTR_SLAVE_MATCH);

	spin_unlock_irqrestore(&bus->lock, flags);
}

void aspeed_i2c_enable_interrupt(struct aspeed_i2c_bus *bus,
		unsigned long mask)
{
	unsigned long current_mask;

	current_mask = readl(bus->base + ASPEED_I2C_INTR_CTRL_REG);
	writel(current_mask | mask, bus->base + ASPEED_I2C_INTR_CTRL_REG);
}

void aspeed_i2c_enable_slave(struct aspeed_i2c_bus *bus)
{
	unsigned long flags;

	spin_lock_irqsave(&bus->lock, flags);

	aspeed_i2c_enable_interrupt(bus, ASPEED_I2CD_INTR_RX_DONE);
	aspeed_i2c_enable_interrupt(bus, ASPEED_I2CD_INTR_SLAVE_MATCH);

	spin_unlock_irqrestore(&bus->lock, flags);
}

/*
 * Call in READ context
 */
static int receive_ssif_bmc_request(struct ssif_bmc *ssif_bmc, bool non_blocking,
					struct ssif_msg *request)
{
	int res;
	unsigned long flags;

	if (!non_blocking) {
try_again:
		res = wait_event_interruptible(
				ssif_bmc->wait_queue,
				ssif_bmc->request_available);
		if (res)
			return res;
	}

	spin_lock_irqsave(&ssif_bmc->lock, flags);
	if (!ssif_bmc->request_available) {
		spin_unlock_irqrestore(&ssif_bmc->lock, flags);
		if (non_blocking)
			return -EAGAIN;
		goto try_again;
	}

	memcpy(request, &ssif_bmc->request, sizeof(*request));
	ssif_bmc->request_available = false;
	spin_unlock_irqrestore(&ssif_bmc->lock, flags);

	return 0;
}

/*
 * Call in WRITE context
 */
static int send_ssif_bmc_response(struct ssif_bmc *ssif_bmc, bool non_blocking,
					struct ssif_msg *response)
{
	int res;
	unsigned long flags;

	if (!non_blocking) {
try_again:
		res = wait_event_interruptible(ssif_bmc->wait_queue,
					       !ssif_bmc->response_in_progress);
		if (res)
			return res;
	}

	spin_lock_irqsave(&ssif_bmc->lock, flags);
	if (ssif_bmc->response_in_progress) {
		spin_unlock_irqrestore(&ssif_bmc->lock, flags);
		if (non_blocking)
			return -EAGAIN;
		goto try_again;
	}

	memcpy(&ssif_bmc->response, response, sizeof(*response));
	ssif_bmc->response_in_progress = true;

	/* Check the response length to determine single or multi-part output */
	if (ssif_msg_len(&ssif_bmc->response) >
		(MAX_PAYLOAD_PER_TRANSACTION + 1)) { /* 1: byte of length */
		ssif_bmc->is_multi_part = true;
	} else {
		ssif_bmc->is_multi_part = false;
	}

	spin_unlock_irqrestore(&ssif_bmc->lock, flags);

	return 0;
}

static inline struct ssif_bmc *to_ssif_bmc(struct file *file)
{
	return container_of(file->private_data, struct ssif_bmc, miscdev);
}

/* Handle SSIF message that will be sent to user */
static ssize_t ssif_bmc_read(struct file *file, char __user *buf, size_t count,
					loff_t *ppos)
{
	struct ssif_bmc *ssif_bmc = to_ssif_bmc(file);
	struct ssif_msg msg;
	ssize_t ret;

	mutex_lock(&ssif_bmc->file_mutex);
	ret = receive_ssif_bmc_request(ssif_bmc, file->f_flags & O_NONBLOCK, &msg);
	if (ret < 0)
		goto out;
	count = min_t(ssize_t, count, ssif_msg_len(&msg));
	if (copy_to_user(buf, &msg, count)) {
		ret = -EFAULT;
		goto out;
	}

out:
	mutex_unlock(&ssif_bmc->file_mutex);
	if (ret < 0)
		return ret;
	else
		return count;
}

/* Handle SSIF message that is written by user */
static ssize_t ssif_bmc_write(struct file *file, const char __user *buf, size_t count,
					loff_t *ppos)
{
	struct ssif_bmc *ssif_bmc = to_ssif_bmc(file);
	struct ssif_msg msg;
	ssize_t ret;

	if (count > sizeof(msg))
		return -EINVAL;

	if (copy_from_user(&msg, buf, count) || count < ssif_msg_len(&msg))
		return -EINVAL;

	mutex_lock(&ssif_bmc->file_mutex);
	ret = send_ssif_bmc_response(ssif_bmc, file->f_flags & O_NONBLOCK, &msg);
	mutex_unlock(&ssif_bmc->file_mutex);

	if (ret < 0)
		return ret;

	/* FIXME: Re-enable I2C Slave to accept the incoming interrupt.
	 * When user application is done with the response, master can
	 * start to get the response after this.
	 */
	aspeed_i2c_enable_slave(ssif_bmc->i2c_bus);

	return count;
}

static long ssif_bmc_ioctl(struct file *file, unsigned int cmd, unsigned long param)
{
	return 0;
}

static unsigned int ssif_bmc_poll(struct file *file, poll_table *wait)
{
	struct ssif_bmc *ssif_bmc = to_ssif_bmc(file);
	unsigned int mask = 0;

	mutex_lock(&ssif_bmc->file_mutex);
	poll_wait(file, &ssif_bmc->wait_queue, wait);

	/* Allows user app can start to get the request */
	if (ssif_bmc->request_available)
		mask |= POLLIN;

	mutex_unlock(&ssif_bmc->file_mutex);
	return mask;
}

/*
 * System calls to device interface for user apps
 */
static const struct file_operations ssif_bmc_fops = {
	.owner		= THIS_MODULE,
	.read		= ssif_bmc_read,
	.write		= ssif_bmc_write,
	.poll		= ssif_bmc_poll,
	.unlocked_ioctl	= ssif_bmc_ioctl,
};

/* Called with ssif_bmc->lock held. */
static int handle_request(struct ssif_bmc *ssif_bmc)
{
	/* FIXME: Disable I2C Slave to prevent incoming interrupts
	 * It should be called as soon as possible right after the request
	 * is received.
	 */
	aspeed_i2c_disable_slave(ssif_bmc->i2c_bus);

	/* Data request is available to process */
	ssif_bmc->request_available = true;
	/* This is the new READ request.
	 * Clear the response buffer of previous transfer
	 */
	memset(&ssif_bmc->response, 0, sizeof(struct ssif_msg));
	wake_up_all(&ssif_bmc->wait_queue);
	return 0;
}

/* Called with ssif_bmc->lock held. */
static int complete_response(struct ssif_bmc *ssif_bmc)
{
	/* Invalidate response in buffer to denote it having been sent. */
	ssif_bmc->response.len = 0;
	ssif_bmc->response_in_progress = false;
	ssif_bmc->num_bytes_processed = 0;
	ssif_bmc->remain_data_len = 0;
	memset(&ssif_bmc->response_buffer, 0, MAX_PAYLOAD_PER_TRANSACTION);
	wake_up_all(&ssif_bmc->wait_queue);
	return 0;
}

static void set_response_buffer(struct ssif_bmc *ssif_bmc)
{
	u8 response_data_len = 0;

	switch (ssif_bmc->smbus_cmd) {
	case SSIF_IPMI_RESPONSE:
		/* IPMI READ Start can carry up to 30 bytes IPMI Data
		 * and Start Flag 0x00 0x01.
		 */
		ssif_bmc->response_buffer[0] = 0x00; /* Start Flag */
		ssif_bmc->response_buffer[1] = 0x01; /* Start Flag */
		ssif_bmc->response_buffer[2] = ssif_bmc->response.netfn_lun;
		ssif_bmc->response_buffer[3] = ssif_bmc->response.cmd;
		ssif_bmc->response_buffer[4] = ssif_bmc->response.payload[0];

		response_data_len = MAX_PAYLOAD_PER_TRANSACTION - 5;

		memcpy(ssif_bmc->response_buffer + 5,
				ssif_bmc->response.payload + 1,
				response_data_len);


		break;
	case SSIF_IPMI_MULTI_PART_RESPONSE_MIDDLE:
		/* IPMI READ Middle or Read End can carry up to 31 bytes IPMI
		 * data plus block number byte.
		 */
		ssif_bmc->response_buffer[0] = ssif_bmc->block_num;

		if (ssif_bmc->remain_data_len < MAX_IPMI_DATA_PER_MIDDLE_TRANSACTION)
			response_data_len = ssif_bmc->remain_data_len;
		else
			response_data_len = MAX_IPMI_DATA_PER_MIDDLE_TRANSACTION;

		memcpy(ssif_bmc->response_buffer + 1,
			ssif_bmc->response.payload + 1 + ssif_bmc->num_bytes_processed,
			response_data_len
			);

		break;
	default:
		/* Do not expect to go to this case */
		pr_err("Error: Unexpected SMBus command received 0x%x\n",
				ssif_bmc->smbus_cmd);
		break;
	}

	ssif_bmc->num_bytes_processed += response_data_len;
}

static void event_request_read(struct ssif_bmc *ssif_bmc, u8 *val)
{
	u8 *buf;
	u8 data_len;

	buf = (u8 *) &ssif_bmc->response;
	data_len = ssif_bmc->response.len;

	/* Single part processing */
	if (!ssif_bmc->is_multi_part) {
		/* TODO: slave must do NACK to Master I2C bus to notify that
		 * the response is not ready to read. ASPEED does not support
		 * NACK at slave mode, the only way to NACK is to disable slave
		 * mode, which would also prevent the slave from responding to
		 * incoming messages when it is unable to provide an outgoing
		 * message.
		 */
		if (buf[ssif_bmc->msg_idx] == 0) {
			/* Workaround: Set return length is 1, master will
			 * resend SMBUS READ command.
			 * Return zero len would lead I2C master bus be timeout
			 */
			*val = 0x1;
		} else {
			*val = buf[ssif_bmc->msg_idx];
		}

		return;
	}

	/* Multi-part processing */
	switch (ssif_bmc->smbus_cmd) {
	case SSIF_IPMI_RESPONSE:
		/* Read Start length is 32 bytes
		 * Read Start transfer first 30 bytes of IPMI response
		 *  and 2 special code 0x00, 0x01
		 */
		*val = MAX_PAYLOAD_PER_TRANSACTION;
		ssif_bmc->remain_data_len =
			data_len - MAX_IPMI_DATA_PER_START_TRANSACTION;
		ssif_bmc->block_num = 0;
		if (ssif_bmc->remain_data_len >
				MAX_IPMI_DATA_PER_MIDDLE_TRANSACTION) {
			ssif_bmc->middle_start_response = true;
		}
		break;
	case SSIF_IPMI_MULTI_PART_RESPONSE_MIDDLE:
		/* Read middle part */
		if (ssif_bmc->remain_data_len <=
				MAX_IPMI_DATA_PER_MIDDLE_TRANSACTION) {
			/* This is READ End message
			 * Return length is: remain response plus block number
			 */
			*val = ssif_bmc->remain_data_len + 1;
			ssif_bmc->block_num = 0xFF;
		} else {
			/* This is READ Middle message
			 * Return length is maximum SMBUS transfer length
			 */
			*val = MAX_PAYLOAD_PER_TRANSACTION;
			if (!ssif_bmc->middle_start_response) {
				ssif_bmc->block_num++;
			} else {
				/* Start Read Middle part of response */
				ssif_bmc->block_num = 0;
				ssif_bmc->middle_start_response = false;
			}
			ssif_bmc->remain_data_len -=
				MAX_IPMI_DATA_PER_MIDDLE_TRANSACTION;
		}
		break;
	default:
		/* Do not expect to go to this case */
		pr_err("Error: Unexpected SMBus command received 0x%x\n",
				ssif_bmc->smbus_cmd);
		break;
	}

	/* Prepare the response buffer that ready to be sent */
	set_response_buffer(ssif_bmc);
}

static void event_process_read(struct ssif_bmc *ssif_bmc, u8 *val)
{
	u8 *buf;

	if (!ssif_bmc->is_multi_part) {
		/* Read byte-to-byte if len of response is non-zero */
		buf = (u8 *) &ssif_bmc->response;
		if (ssif_bmc->response.len &&
		    ssif_bmc->msg_idx < ssif_msg_len(&ssif_bmc->response)) {
			*val = buf[++ssif_bmc->msg_idx];
		} else {
			*val = 0;
		}
		/* Invalidate response buffer to denote it is sent */
		if (ssif_bmc->msg_idx + 1 >= ssif_msg_len(&ssif_bmc->response))
			complete_response(ssif_bmc);
	} else {
		/* Multi-part processing */
		switch (ssif_bmc->smbus_cmd) {
		case SSIF_IPMI_RESPONSE:
		case SSIF_IPMI_MULTI_PART_RESPONSE_MIDDLE:
			buf = (u8 *) &ssif_bmc->response_buffer;
			*val = buf[ssif_bmc->msg_idx++];
			break;
		default:
			/* Do not expect to go to this case */
			pr_err("Error: Unexpected SMBus command received 0x%x\n",
					ssif_bmc->smbus_cmd);
			break;
		}

		/* Invalidate response buffer to denote last response is sent */
		if ((ssif_bmc->block_num == 0xFF)
			&& (ssif_bmc->msg_idx > ssif_bmc->remain_data_len)) {
			complete_response(ssif_bmc);
		}
	}
}

/*
 * Callback function to handle I2C slave events
 */
static int ssif_bmc_cb(struct i2c_client *client,
				enum i2c_slave_event event, u8 *val)
{
	struct ssif_bmc *ssif_bmc = i2c_get_clientdata(client);
	u8 *buf;

	spin_lock(&ssif_bmc->lock);

	/* I2C Event Handler:
	 *   I2C_SLAVE_READ_REQUESTED	0x0
	 *   I2C_SLAVE_WRITE_REQUESTED	0x1
	 *   I2C_SLAVE_READ_PROCESSED	0x2
	 *   I2C_SLAVE_WRITE_RECEIVED	0x3
	 *   I2C_SLAVE_STOP		0x4
	 */
	switch (event) {
	case I2C_SLAVE_READ_REQUESTED:
		ssif_bmc->msg_idx = 0;
		event_request_read(ssif_bmc, val);

		/*
		 * Do not increment buffer_idx here, because we don't know if
		 * this byte will be actually used. Read Linux I2C slave docs
		 * for details.
		 */
		break;

	case I2C_SLAVE_WRITE_REQUESTED:
		ssif_bmc->msg_idx = 0;
		break;

	case I2C_SLAVE_READ_PROCESSED:
		event_process_read(ssif_bmc, val);
		break;

	case I2C_SLAVE_WRITE_RECEIVED:
		/* First byte is SMBUS command, not a part of SSIF message */
		if (ssif_bmc->msg_idx == 0) {
			/* SMBUS read command can vary (single or multi-part) */
			ssif_bmc->smbus_cmd = *val;
			ssif_bmc->msg_idx++;
		} else {
			buf = (u8 *) &ssif_bmc->request;
			if (ssif_bmc->msg_idx >= sizeof(struct ssif_msg))
				break;

			/* Write byte-to-byte to buffer */
			buf[ssif_bmc->msg_idx - 1] = *val;
			ssif_bmc->msg_idx++;
			if ((ssif_bmc->msg_idx - 1) >= ssif_msg_len(&ssif_bmc->request))
				handle_request(ssif_bmc);

			/* TODO: support SSIF Multi-part Write*/
		}
		break;

	case I2C_SLAVE_STOP:
		/* Reset msg index */
		ssif_bmc->msg_idx = 0;
		break;

	default:
		break;
	}

	spin_unlock(&ssif_bmc->lock);

	return 0;
}

static int ssif_bmc_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct ssif_bmc *ssif_bmc;
	int ret;

	ssif_bmc = devm_kzalloc(&client->dev, sizeof(*ssif_bmc), GFP_KERNEL);
	if (!ssif_bmc)
		return -ENOMEM;

	spin_lock_init(&ssif_bmc->lock);

	init_waitqueue_head(&ssif_bmc->wait_queue);
	ssif_bmc->request_available = false;
	ssif_bmc->response_in_progress = false;

	mutex_init(&ssif_bmc->file_mutex);

	/* Register misc device interface */
	ssif_bmc->miscdev.minor = MISC_DYNAMIC_MINOR;
	ssif_bmc->miscdev.name = DEVICE_NAME;
	ssif_bmc->miscdev.fops = &ssif_bmc_fops;
	ssif_bmc->miscdev.parent = &client->dev;
	ret = misc_register(&ssif_bmc->miscdev);
	if (ret)
		return ret;

	ssif_bmc->client = client;
	ssif_bmc->client->flags |= I2C_CLIENT_SLAVE;

	/* Register I2C slave */
	i2c_set_clientdata(client, ssif_bmc);
	ret = i2c_slave_register(client, ssif_bmc_cb);
	if (ret) {
		misc_deregister(&ssif_bmc->miscdev);
		return ret;
	}

	ssif_bmc->i2c_bus = i2c_get_adapdata(client->adapter);

	return 0;
}

static int ssif_bmc_remove(struct i2c_client *client)
{
	struct ssif_bmc *ssif_bmc = i2c_get_clientdata(client);

	i2c_slave_unregister(client);
	misc_deregister(&ssif_bmc->miscdev);

	return 0;
}

static const struct of_device_id ssif_bmc_match[] = {
	{ .compatible = "aspeed,ast2500-ssif-bmc" },
	{ },
};

static const struct i2c_device_id ssif_bmc_id[] = {
	{ DEVICE_NAME, 0 },
	{ },
};

MODULE_DEVICE_TABLE(i2c, ssif_bmc_id);

static struct i2c_driver ssif_bmc_driver = {
	.driver		= {
		.name		= DEVICE_NAME,
		.of_match_table = ssif_bmc_match,
	},
	.probe		= ssif_bmc_probe,
	.remove		= ssif_bmc_remove,
	.id_table	= ssif_bmc_id,
};

module_i2c_driver(ssif_bmc_driver);

MODULE_AUTHOR("Chuong Tran <chuong.tran@amperecomputing.com>");
MODULE_AUTHOR("Thang Q. Nguyen <thangqn@amperecomputing.com>");
MODULE_DESCRIPTION("Linux device driver of the BMC IPMI SSIF interface.");
MODULE_LICENSE("GPL");
