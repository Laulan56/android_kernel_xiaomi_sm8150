/* Copyright (c) 2015-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __MAXIM_32560_H
#define __MAXIM_32560_H

#include <linux/spi/spi.h>
#include <linux/types.h>
#include <linux/version.h>

#include <linux/semaphore.h>
#include <linux/completion.h>

#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/nfcinfo.h>

#define DEV_COUNT	1
#define DEVICE_NAME	"nq-nci"
#define CLASS_NAME	"maxim"
#define MAX_BUFFER_SIZE			(320)
#define WAKEUP_SRC_TIMEOUT		(2000)

#define NFC_LOGI(...)	dev_info(__VA_ARGS__)
#define NFC_LOGE(...)	dev_err(__VA_ARGS__)

#ifdef NFC_DEBUG_LOG
#define NFC_LOGD(...)	dev_err(__VA_ARGS__)
#else
#define NFC_LOGD(...)
#endif

struct maxim_dev {
	struct spi_device *client;

	uint8_t *send_xbuf;
	uint8_t *send_rbuf;

	uint8_t *recv_xbuf;
	uint8_t *recv_rbuf;

	size_t kbuflen;

	wait_queue_head_t	read_wq;
	wait_queue_head_t	write_wq;
	struct	mutex		read_mutex;
	struct	mutex		dev_ref_mutex;
	struct	mutex		spi_mutex;

	/* NFC GPIO variables */
	unsigned int		irq_gpio;
	unsigned int		enable_gpio;
	unsigned int		reset_gpio;

	dev_t				devno;
	struct class		*maxim_class;
	struct device		*maxim_device;
	struct cdev			c_dev;

	/* NFC_IRQ state */
	bool				irq_enabled;

	/* NFC_IRQ wake-up state */
	bool				irq_wake_up;
	spinlock_t			irq_enabled_lock;
	bool				rsp_data;
	//bool				write_cmd;

	/* NFC_IRQ Count */
	unsigned int		dev_ref_count;

	/* ERROR Check */
	bool				read_check;

};

int32_t print_command(uint8_t *tmp, uint16_t len, uint16_t rw);

#endif

