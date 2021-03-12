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
#ifndef __SSIF_BMC_H__
#define __SSIF_BMC_H__

#define DEVICE_NAME				"ipmi-ssif-host"

#define MSG_PAYLOAD_LEN_MAX			252

/* A standard SMBus Transaction is limited to 32 data bytes */
#define MAX_PAYLOAD_PER_TRANSACTION		32

#define MAX_IPMI_DATA_PER_START_TRANSACTION	30
#define MAX_IPMI_DATA_PER_MIDDLE_TRANSACTION	31

#define	SSIF_IPMI_REQUEST			0x2
#define	SSIF_IPMI_MULTI_PART_REQUEST_START	0x6
#define	SSIF_IPMI_MULTI_PART_REQUEST_MIDDLE	0x7
#define	SSIF_IPMI_RESPONSE			0x3
#define	SSIF_IPMI_MULTI_PART_RESPONSE_MIDDLE	0x9

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

#define SSIF_BMC_BUSY   0x01
#define SSIF_BMC_READY  0x02

struct ssif_bmc_ctx {
	struct i2c_client	*client;
	struct miscdevice	miscdev;
	u8			smbus_cmd;
	struct ssif_msg		request;
	bool			request_available;
	struct ssif_msg		response;
	bool			response_in_progress;
	/* Response buffer for Multi-part Read Transaction */
	u8			response_buffer[MAX_PAYLOAD_PER_TRANSACTION];
	/* Flag to identify a Multi-part Read Transaction */
	bool			is_multi_part_read;
	u8			num_bytes_processed;
	u8			remain_data_len;
	/* Block Number of a Multi-part Read Transaction */
	u8			block_num;
	size_t			msg_idx;
	spinlock_t		lock;
	wait_queue_head_t	wait_queue;
	struct mutex		file_mutex;
	void (*set_ssif_bmc_status)(struct ssif_bmc_ctx *, unsigned int );
	void			*priv;
};

static inline struct ssif_bmc_ctx *to_ssif_bmc(struct file *file)
{
	return container_of(file->private_data, struct ssif_bmc_ctx, miscdev);
}

struct ssif_bmc_ctx *ssif_bmc_alloc(struct i2c_client *client, int sizeof_priv);

#endif /* __SSIF_BMC_H__ */
