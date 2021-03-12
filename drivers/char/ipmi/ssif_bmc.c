// SPDX-License-Identifier: GPL-2.0+
/*
 * The driver for BMC side of SSIF interface
 *
 * Copyright (c) 2021, Ampere Computing LLC
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

#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

#include "ssif_bmc.h"

#define POLY    (0x1070U << 3)
static u8 crc8(u16 data)
{
	int i;

	for (i = 0; i < 8; i++) {
		if (data & 0x8000)
			data = data ^ POLY;
		data = data << 1;
	}
	return (u8)(data >> 8);
}

/* Incremental CRC8 over count bytes in the array pointed to by p */
static u8 i2c_calculate_pec(u8 crc, u8 *p, size_t count)
{
	int i;

	for (i = 0; i < count; i++)
		crc = crc8((crc ^ p[i]) << 8);
	return crc;
}

static u8 i2c_8bit_addr(u8 addr_7bit)
{
	return (addr_7bit << 1);
}

/*
 * Call in WRITE context
 */
static int send_ssif_bmc_response(struct ssif_bmc_ctx *ssif_bmc, bool non_blocking)
{
	unsigned long flags;
	int ret;

	if (!non_blocking) {
retry:
		ret = wait_event_interruptible(ssif_bmc->wait_queue,
				!ssif_bmc->response_in_progress);
		if (ret)
			return ret;
	}

	spin_lock_irqsave(&ssif_bmc->lock, flags);
	if (ssif_bmc->response_in_progress) {
		spin_unlock_irqrestore(&ssif_bmc->lock, flags);
		if (non_blocking)
			return -EAGAIN;

		goto retry;
	}

	/*
	 * Check the response data length from userspace to determine the type
	 * of the response message whether it is single-part or multi-part.
	 */
	ssif_bmc->is_multi_part_read =
		(ssif_msg_len(&ssif_bmc->response) >
		 (MAX_PAYLOAD_PER_TRANSACTION + 1)) ?
		true : false; /* 1: byte of length */

	ssif_bmc->response_in_progress = true;
	spin_unlock_irqrestore(&ssif_bmc->lock, flags);

	return 0;
}

/*
 * Call in READ context
 */
static int receive_ssif_bmc_request(struct ssif_bmc_ctx *ssif_bmc, bool non_blocking)
{
	unsigned long flags;
	int ret;

	if (!non_blocking) {
retry:
		ret = wait_event_interruptible(
				ssif_bmc->wait_queue,
				ssif_bmc->request_available);
		if (ret)
			return ret;
	}

	spin_lock_irqsave(&ssif_bmc->lock, flags);
	if (!ssif_bmc->request_available) {
		spin_unlock_irqrestore(&ssif_bmc->lock, flags);
		if (non_blocking)
			return -EAGAIN;
		goto retry;
	}
	spin_unlock_irqrestore(&ssif_bmc->lock, flags);

	return 0;
}

/* Handle SSIF message that will be sent to user */
static ssize_t ssif_bmc_read(struct file *file, char __user *buf, size_t count,
					loff_t *ppos)
{
	struct ssif_bmc_ctx *ssif_bmc = to_ssif_bmc(file);
	unsigned long flags;
	ssize_t ret;

	mutex_lock(&ssif_bmc->file_mutex);

	ret = receive_ssif_bmc_request(ssif_bmc, file->f_flags & O_NONBLOCK);
	if (ret < 0)
		goto out;

	spin_lock_irqsave(&ssif_bmc->lock, flags);
	count = min_t(ssize_t, count, ssif_msg_len(&ssif_bmc->request));
	ret = copy_to_user(buf, &ssif_bmc->request, count);
	if (!ret)
		ssif_bmc->request_available = false;
	spin_unlock_irqrestore(&ssif_bmc->lock, flags);
out:
	mutex_unlock(&ssif_bmc->file_mutex);

	return (ret < 0) ? ret : count;
}

/* Handle SSIF message that is written by user */
static ssize_t ssif_bmc_write(struct file *file, const char __user *buf, size_t count,
					loff_t *ppos)
{
	struct ssif_bmc_ctx *ssif_bmc = to_ssif_bmc(file);
	unsigned long flags;
	ssize_t ret;

	if (count > sizeof(struct ssif_msg))
		return -EINVAL;

	mutex_lock(&ssif_bmc->file_mutex);

	spin_lock_irqsave(&ssif_bmc->lock, flags);
	ret = copy_from_user(&ssif_bmc->response, buf, count);
	if ( ret || count < ssif_msg_len(&ssif_bmc->response)) {
		spin_unlock_irqrestore(&ssif_bmc->lock, flags);
		ret = -EINVAL;
		goto out;
	}
	spin_unlock_irqrestore(&ssif_bmc->lock, flags);

	ret = send_ssif_bmc_response(ssif_bmc, file->f_flags & O_NONBLOCK);
	if (!ret) {
		if (ssif_bmc->set_ssif_bmc_status)
			ssif_bmc->set_ssif_bmc_status(ssif_bmc, SSIF_BMC_READY);
	}
out:
	mutex_unlock(&ssif_bmc->file_mutex);

	return (ret < 0) ? ret : count;
}

static long ssif_bmc_ioctl(struct file *file, unsigned int cmd, unsigned long param)
{
	return 0;
}

static unsigned int ssif_bmc_poll(struct file *file, poll_table *wait)
{
	struct ssif_bmc_ctx *ssif_bmc = to_ssif_bmc(file);
	unsigned int mask = 0;

	mutex_lock(&ssif_bmc->file_mutex);
	poll_wait(file, &ssif_bmc->wait_queue, wait);

	/*
	 * The request message is now available so userspace application can
	 * get the request
	 */
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
static int handle_request(struct ssif_bmc_ctx *ssif_bmc)
{
	if (ssif_bmc->set_ssif_bmc_status)
		ssif_bmc->set_ssif_bmc_status(ssif_bmc, SSIF_BMC_BUSY);

	/* Request message is available to process */
	ssif_bmc->request_available = true;
	/*
	 * This is the new READ request.
	 * Clear the response buffer of the previous transaction
	 */
	memset(&ssif_bmc->response, 0, sizeof(struct ssif_msg));
	wake_up_all(&ssif_bmc->wait_queue);
	return 0;
}

/* Called with ssif_bmc->lock held. */
static int complete_response(struct ssif_bmc_ctx *ssif_bmc)
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

static void set_response_buffer(struct ssif_bmc_ctx *ssif_bmc, u8 *val)
{
	u8 response_data_len = 0;
	int idx = 0;
	u8 data_len;

	data_len = ssif_bmc->response.len;
	switch (ssif_bmc->smbus_cmd) {
	case SSIF_IPMI_RESPONSE:
		/*
		 * Read Start length is 32 bytes.
		 * Read Start transfer first 30 bytes of IPMI response
		 * and 2 special code 0x00, 0x01.
		 */
		*val = MAX_PAYLOAD_PER_TRANSACTION;
		ssif_bmc->remain_data_len =
			data_len - MAX_IPMI_DATA_PER_START_TRANSACTION;
		ssif_bmc->block_num = 0;

		ssif_bmc->response_buffer[idx++] = 0x00; /* Start Flag */
		ssif_bmc->response_buffer[idx++] = 0x01; /* Start Flag */
		ssif_bmc->response_buffer[idx++] = ssif_bmc->response.netfn_lun;
		ssif_bmc->response_buffer[idx++] = ssif_bmc->response.cmd;
		ssif_bmc->response_buffer[idx++] = ssif_bmc->response.payload[0];

		response_data_len = MAX_PAYLOAD_PER_TRANSACTION - idx;

		memcpy(&ssif_bmc->response_buffer[idx],
				&ssif_bmc->response.payload[1],
				response_data_len);
		break;

	case SSIF_IPMI_MULTI_PART_RESPONSE_MIDDLE:
		/*
		 * IPMI READ Middle or READ End messages can carry up to 31 bytes
		 * IPMI data plus block number byte.
		 */
		if (ssif_bmc->remain_data_len <=
				MAX_IPMI_DATA_PER_MIDDLE_TRANSACTION) {
			/*
			 * This is READ End message
			 *  Return length is the remaining response data length
			 *  plus block number
			 *  Block number 0xFF is to indicate this is last message
			 *
			 * Return length is: remain response plus block number
			 */
			*val = ssif_bmc->remain_data_len + 1;
			ssif_bmc->block_num = 0xFF;
			ssif_bmc->response_buffer[idx++] = ssif_bmc->block_num;
			response_data_len = ssif_bmc->remain_data_len;
		} else {
			/*
			 * This is READ Middle message
			 *  Response length is the maximum SMBUS transfer length
			 *  Block number byte is incremented
			 * Return length is maximum SMBUS transfer length
			 */
			*val = MAX_PAYLOAD_PER_TRANSACTION;
			ssif_bmc->remain_data_len -= MAX_IPMI_DATA_PER_MIDDLE_TRANSACTION;
			response_data_len = MAX_IPMI_DATA_PER_MIDDLE_TRANSACTION;
			ssif_bmc->response_buffer[idx++] = ssif_bmc->block_num;
			ssif_bmc->block_num++;
		}

		memcpy(&ssif_bmc->response_buffer[idx],
			ssif_bmc->response.payload + 1 + ssif_bmc->num_bytes_processed,
			response_data_len);
		break;

	default:
		/* Do not expect to go to this case */
		pr_err("Error: Unexpected SMBus command received 0x%x\n",
				ssif_bmc->smbus_cmd);
		break;
	}

	ssif_bmc->num_bytes_processed += response_data_len;

	return;
}

/* Return the length of IPMI response that is going to be read by master */
static void event_request_read(struct ssif_bmc_ctx *ssif_bmc, u8 *val)
{
	u8 *buf;

	buf = (u8 *) &ssif_bmc->response;
	/* Single-part processing */
	if (!ssif_bmc->is_multi_part_read) {
		/*
		 * Do not expect the IPMI response has data length 0.
		 * With some I2C SMBus controllers (Aspeed I2C), return 0 for
		 * the SMBus Read Request callback might cause bad state for
		 * the bus. So return 1 byte length so that master will
		 * resend the Read Request because the length of response is
		 * less than a normal IPMI response.
		 *
		 * Otherwise, return the length of IPMI response
		 */
		if (buf[ssif_bmc->msg_idx] == 0)
			*val = 0x1;
		else
			*val = buf[ssif_bmc->msg_idx];

		return;
	}
	/* Multi-part processing */
	/* Prepare the response buffer that ready to be sent */
	set_response_buffer(ssif_bmc, val);

	return;
}

/* Process the IPMI response that will be read by master */
static void event_process_read(struct ssif_bmc_ctx *ssif_bmc, u8 *val)
{
	u8 *buf;
	u8 pec_len, addr, len;
	u8 pec_base = 0;

	pec_len = ssif_bmc->pec_support ? 1 : 0;
	/* PEC - Start Read Address */
	addr = i2c_8bit_addr(ssif_bmc->client->addr);
	pec_base = i2c_calculate_pec(pec_base, &addr, 1);
	/* PEC - SSIF Command */
	pec_base = i2c_calculate_pec(pec_base, &ssif_bmc->smbus_cmd, 1);
	/* PEC - Restart Write Address */
	addr = addr | 0x01;
	pec_base = i2c_calculate_pec(pec_base, &addr, 1);
	if (!ssif_bmc->is_multi_part_read) {
		/* Single-part Read processing */
		buf = (u8 *) &ssif_bmc->response;

		if (ssif_bmc->response.len &&
		    ssif_bmc->msg_idx < ssif_bmc->response.len) {
			ssif_bmc->msg_idx++;
			*val = buf[ssif_bmc->msg_idx];
		} else if (ssif_bmc->response.len &&
			   (ssif_bmc->msg_idx == ssif_bmc->response.len)){
			ssif_bmc->msg_idx++;
			*val = i2c_calculate_pec(pec_base, buf,
					ssif_msg_len(&ssif_bmc->response));
		} else
			*val = 0;
		/* Invalidate response buffer to denote it is sent */
		if (ssif_bmc->msg_idx + 1 >=
		   (ssif_msg_len(&ssif_bmc->response) + pec_len))
			complete_response(ssif_bmc);
	} else {
		/* Multi-part Read processing */
		switch (ssif_bmc->smbus_cmd) {
		case SSIF_IPMI_RESPONSE:
		case SSIF_IPMI_MULTI_PART_RESPONSE_MIDDLE:
			buf = (u8 *) &ssif_bmc->response_buffer;
			*val = buf[ssif_bmc->msg_idx];
			ssif_bmc->msg_idx++;
			break;
		default:
			/* Do not expect to go to this case */
			pr_err("Error: Unexpected SMBus command received 0x%x\n",
					ssif_bmc->smbus_cmd);
			break;
		}
		len = (ssif_bmc->block_num == 0xFF) ?
		       ssif_bmc->remain_data_len + 1 : MAX_PAYLOAD_PER_TRANSACTION;
		if (ssif_bmc->msg_idx == (len + 1)) {
			pec_base = i2c_calculate_pec(pec_base, &len, 1);
			*val = i2c_calculate_pec(pec_base,
					ssif_bmc->response_buffer, len);
		}
		/* Invalidate response buffer to denote last response is sent */
		if ((ssif_bmc->block_num == 0xFF) &&
		    (ssif_bmc->msg_idx > (ssif_bmc->remain_data_len) + pec_len)) {
			complete_response(ssif_bmc);
		}
	}
}

static void event_received_write(struct ssif_bmc_ctx *ssif_bmc, u8 *val)
{
	u8 *buf;
	u8 index;
	u8 smbus_cmd;

	buf = (u8 *) &ssif_bmc->request;
	if (ssif_bmc->msg_idx >= sizeof(struct ssif_msg))
		return;

	smbus_cmd = ssif_bmc->smbus_cmd;
	switch (smbus_cmd) {
	case SSIF_IPMI_REQUEST:
		/* Single-part write */
		buf[ssif_bmc->msg_idx - 1] = *val;
		ssif_bmc->msg_idx++;
		break;
	case SSIF_IPMI_MULTI_PART_REQUEST_START:
	case SSIF_IPMI_MULTI_PART_REQUEST_MIDDLE:
	case SSIF_IPMI_MULTI_PART_REQUEST_END:
		/* Multi-part write */
		if (ssif_bmc->msg_idx == 1) {
			/* 2nd byte received is length */
			if (smbus_cmd == SSIF_IPMI_MULTI_PART_REQUEST_START) {
				/* Reset length to zero */
				ssif_bmc->request.len = 0;
			}
			ssif_bmc->request.len += *val;
			ssif_bmc->recv_data_len = *val;
			ssif_bmc->msg_idx++;
			break;
		}
		index = ssif_bmc->request.len - ssif_bmc->recv_data_len;
		buf[ssif_bmc->msg_idx - 1 + index] = *val;
		ssif_bmc->msg_idx++;
		break;
	default:
		/* Do not expect to go to this case */
		pr_err("Error: Unexpected SMBus command received 0x%x\n",
				ssif_bmc->smbus_cmd);
		break;
	}
}

static bool validate_pec(struct ssif_bmc_ctx *ssif_bmc)
{
	u8 rpec = 0,cpec = 0;
	bool ret = true;
	u8 addr, index;
	u8 *buf;

	buf = (u8 *) &ssif_bmc->request;
	switch (ssif_bmc->smbus_cmd) {
	case SSIF_IPMI_REQUEST:
		if ((ssif_bmc->msg_idx - 1) ==
		    ssif_msg_len(&ssif_bmc->request)) {
			/* PEC is not included */
			ssif_bmc->pec_support = false;
		} else if ((ssif_bmc->msg_idx - 1) ==
			   (ssif_msg_len(&ssif_bmc->request) + 1)) {
			/* PEC is included */
			ssif_bmc->pec_support = true;
			rpec = buf[ssif_bmc->msg_idx - 2];
			addr = i2c_8bit_addr(ssif_bmc->client->addr);
			cpec = i2c_calculate_pec(cpec, &addr, 1);
			cpec = i2c_calculate_pec(cpec, &ssif_bmc->smbus_cmd, 1);
			cpec = i2c_calculate_pec(cpec, buf,
					ssif_msg_len(&ssif_bmc->request));
			if (rpec != cpec) {
				pr_err("Bad PEC 0x%02x vs. 0x%02x\n", rpec, cpec);
				ret = false;
			}
		} else
			goto error;
		break;
	case SSIF_IPMI_MULTI_PART_REQUEST_START:
	case SSIF_IPMI_MULTI_PART_REQUEST_MIDDLE:
	case SSIF_IPMI_MULTI_PART_REQUEST_END:
		index = ssif_bmc->request.len - ssif_bmc->recv_data_len;
		if ((ssif_bmc->msg_idx - 1 + index) ==
		    ssif_msg_len(&ssif_bmc->request)) {
			/* PEC is not included */
			ssif_bmc->pec_support = false;
		} else if ((ssif_bmc->msg_idx - 1 + index) ==
			   (ssif_msg_len(&ssif_bmc->request) + 1)) {
			/* PEC is included */
			ssif_bmc->pec_support = true;
			rpec = buf[ssif_bmc->msg_idx - 2 + index];
			addr = i2c_8bit_addr(ssif_bmc->client->addr);
			cpec = i2c_calculate_pec(cpec, &addr, 1);
			cpec = i2c_calculate_pec(cpec, &ssif_bmc->smbus_cmd, 1);
			cpec = i2c_calculate_pec(cpec, &ssif_bmc->recv_data_len, 1);
			ssif_bmc->msg_idx++;
			/* As SMBus specification does not allow the length
			 * (byte count) in the Write-Block protocol to be zero.
			 * Therefore, it is illegal to have the last Middle
			 * transaction in the sequence carry 32-bytes and have
			 * a length of ‘0’ in the End transaction.
			 * But some users may try to use this way and we should
			 * prevent ssif_bmc driver broken in this case.
			 */
			if (ssif_bmc->recv_data_len != 0) {
				cpec = i2c_calculate_pec(cpec, buf + 1 + index,
						ssif_bmc->recv_data_len);
			}
			if (rpec != cpec) {
				pr_err("Bad PEC 0x%02x vs. 0x%02x\n", rpec, cpec);
				ret = false;
			}
		} else
			goto error;
		break;
	default:
		break;
	}

	return ret;
error:
	/* Do not expect to go to this case */
	pr_err("Error: Unexpected length received %d\n",
	       ssif_msg_len(&ssif_bmc->request));

	return false;
}

static void complete_received(struct ssif_bmc_ctx *ssif_bmc)
{
	u8 cmd = ssif_bmc->smbus_cmd;

	/* A BMC that receives an invalid PEC shall drop the data for the write
	 * transaction and any further transactions (read or write) until
	 * the next valid read or write Start transaction is received
	 */
	if (!validate_pec(ssif_bmc)) {
		pr_err("Received invalid PEC\n");
		return;
	}

	if ((cmd == SSIF_IPMI_REQUEST) ||
	    (cmd == SSIF_IPMI_MULTI_PART_REQUEST_END))
		handle_request(ssif_bmc);

	return;
}

/*
 * Callback function to handle I2C slave events
 */
static int ssif_bmc_cb(struct i2c_client *client,
				enum i2c_slave_event event, u8 *val)
{
	struct ssif_bmc_ctx *ssif_bmc = i2c_get_clientdata(client);

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
		ssif_bmc->prev_event = I2C_SLAVE_READ_REQUESTED;
		ssif_bmc->msg_idx = 0;
		event_request_read(ssif_bmc, val);
		break;

	case I2C_SLAVE_WRITE_REQUESTED:
		ssif_bmc->prev_event = I2C_SLAVE_WRITE_REQUESTED;
		ssif_bmc->msg_idx = 0;
		break;

	case I2C_SLAVE_READ_PROCESSED:
		ssif_bmc->prev_event = I2C_SLAVE_READ_PROCESSED;
		event_process_read(ssif_bmc, val);
		break;

	case I2C_SLAVE_WRITE_RECEIVED:
		ssif_bmc->prev_event = I2C_SLAVE_WRITE_RECEIVED;
		/*
		 * First byte is SMBUS command, not a part of SSIF message.
		 * SSIF request buffer starts with msg_idx 1 for the first
		 *  buffer byte.
		 */
		if (ssif_bmc->msg_idx == 0) {
			/* SMBUS command can vary (single or multi-part) */
			ssif_bmc->smbus_cmd = *val;
			ssif_bmc->msg_idx++;
		} else {
			event_received_write(ssif_bmc, val);
		}
		break;

	case I2C_SLAVE_STOP:
		/*
		 * PEC byte is appended at the end of each transaction.
		 * Detect PEC is support or not after receiving write request
		 * completely.
		 */
		if (ssif_bmc->prev_event == I2C_SLAVE_WRITE_RECEIVED)
			complete_received(ssif_bmc);
		/* Reset message index */
		ssif_bmc->msg_idx = 0;
		break;

	default:
		break;
	}

	spin_unlock(&ssif_bmc->lock);

	return 0;
}

struct ssif_bmc_ctx *ssif_bmc_alloc(struct i2c_client *client, int sizeof_priv)
{
	struct ssif_bmc_ctx *ssif_bmc;
	int ret;

	ssif_bmc = devm_kzalloc(&client->dev, sizeof(*ssif_bmc) + sizeof_priv, GFP_KERNEL);
	if (!ssif_bmc)
		return ERR_PTR(-ENOMEM);

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
		goto out;

	ssif_bmc->client = client;
	ssif_bmc->client->flags |= I2C_CLIENT_SLAVE;

	/* Register I2C slave */
	i2c_set_clientdata(client, ssif_bmc);
	ret = i2c_slave_register(client, ssif_bmc_cb);
	if (ret) {
		misc_deregister(&ssif_bmc->miscdev);
		goto out;
	}

	return ssif_bmc;

out:
	devm_kfree(&client->dev, ssif_bmc);
	return ERR_PTR(ret);;
}
EXPORT_SYMBOL(ssif_bmc_alloc);

MODULE_AUTHOR("Chuong Tran <chuong@os.amperecomputing.com>");
MODULE_AUTHOR("Quan Nguyen <quan@os.amperecomputing.com>");
MODULE_DESCRIPTION("Linux device driver of the BMC IPMI SSIF interface.");
MODULE_LICENSE("GPL");
