/* Copyright (c) 2015-2019, The Linux Foundation. All rights reserved.
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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/spinlock.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/uaccess.h>
#include <linux/clk.h>
#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif

#include "max32560.h"

static struct maxim_dev *maxim_dev;

MODULE_DEVICE_TABLE(of, msm_match_table);

static void maxim_disable_irq(struct maxim_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_enabled_lock, flags);
	if (dev->irq_enabled) {
		disable_irq_nosync(dev->client->irq);
		dev->irq_enabled = false;
		NFC_LOGD(&dev->client->dev, "%s\n", __func__);
	}
	spin_unlock_irqrestore(&dev->irq_enabled_lock, flags);
}

static void maxim_enable_irq(struct maxim_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_enabled_lock, flags);
	if (!dev->irq_enabled) {
		dev->irq_enabled = true;
		enable_irq(dev->client->irq);
		NFC_LOGD(&dev->client->dev, "%s\n", __func__);
	}
	spin_unlock_irqrestore(&dev->irq_enabled_lock, flags);
}

static int32_t nfc_spi_write(struct maxim_dev *dev,
		uint8_t *buf_write, uint16_t len)
{
	int32_t ret = 0;
	struct spi_message m;
	struct spi_transfer t = {
		.len    = len,
	};

	/*
	 * Wait for the end of the general response. Check 4 times in a
	 * row to avoid continuous responses.
	 */
	NFC_LOGD(&dev->client->dev, "%s ---> [%02X %02X %02X]\n",
			__func__,
			buf_write[0], buf_write[1], buf_write[2]);
	mutex_lock(&dev->spi_mutex);
	NFC_LOGD(&dev->client->dev, "nfc_spi_write_mutex ---> lock\n");
	memset(maxim_dev->send_xbuf, 0xFF, len+3);
	memset(maxim_dev->send_rbuf, 0, len+3);

	memcpy(maxim_dev->send_xbuf, buf_write, len);

	t.len = len+3;
	t.bits_per_word = 8;
	t.speed_hz = maxim_dev->client->max_speed_hz;
	t.tx_buf = maxim_dev->send_xbuf;
	t.rx_buf = maxim_dev->send_rbuf;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	spi_sync(maxim_dev->client, &m);
	mutex_unlock(&maxim_dev->spi_mutex);
	NFC_LOGD(&dev->client->dev, "nfc_spi_write_mutex ---> lock end\n");

	return ret;
}

static bool nfc_rsp_error_check(uint8_t *rbuf)
{
	if (rbuf[2] != 0x00) {
		if (rbuf[0] == 0x40) {
			if (rbuf[1] >= 0x00 && rbuf[1] <= 0x05)
				return true;
			return false;
		} else if (rbuf[0] == 0x41) {
			if ((rbuf[1] >= 0x00 && rbuf[1] <= 0x06) ||
				(rbuf[1] == 0x08) || (rbuf[1] == 0x0B))
				return true;
			return false;
		} else if (rbuf[0] == 0x60) {
			if ((rbuf[1] == 0x00) || (rbuf[1] == 0x06) ||
					(rbuf[1] == 0x07) || (rbuf[1] == 0x08))
				return true;
			return false;
		} else if (rbuf[0] == 0x61) {
			if ((rbuf[1] == 0x02) || (rbuf[1] == 0x03) ||
				(rbuf[1] >= 0x05 && rbuf[1] <= 0x0A))
				return true;
			return false;
		} else if (rbuf[0] == 0x62) {
			if (rbuf[1] == 0x01)
				return true;
			return false;
		} else if (rbuf[0] == 0x00) {
			if (rbuf[1] == 0x00)
				return true;
			return false;
		} else
			return false;
	} else
		return false;
}

static int32_t nfc_spi_read(struct maxim_dev *dev,
							uint8_t *buf_read,
							uint16_t len)
{
	int32_t ret = 0;

	struct spi_message m;
	struct spi_transfer t = {
		.len    = len,
	};

	NFC_LOGD(&dev->client->dev, "%s <--- wait lock count:%d\n",
								__func__, len);
	mutex_lock(&dev->spi_mutex);
	NFC_LOGD(&dev->client->dev, "%s <--- lock\n", __func__);

	if (dev->read_check == false)
		memset(dev->recv_xbuf, 0xFF, len);
	else
		memset(dev->recv_xbuf, 0xFE, len);

	memset(dev->recv_rbuf, 0, len);

	t.bits_per_word = 8;
	t.speed_hz = dev->client->max_speed_hz;
	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	t.tx_buf = dev->recv_xbuf;
	t.rx_buf = dev->recv_rbuf;
	t.len = len;

	ret = spi_sync(dev->client, &m);
	NFC_LOGD(&dev->client->dev,
			"%s <--- [%02X %02X %02X]\n",
			__func__,
			dev->recv_rbuf[0],
			dev->recv_rbuf[1],
			dev->recv_rbuf[2]);
	memcpy(buf_read, dev->recv_rbuf, len);

	if (dev->rsp_data == false) {
		/* header error check */
		if (nfc_rsp_error_check(dev->recv_rbuf)) {
			NFC_LOGD(&dev->client->dev,
					"%s <--- set rsp_data = true\n",
					__func__);
			dev->rsp_data = true;
			goto success;
		} else
			goto header_error;
	} else {
		/* The response is complete. */
		NFC_LOGD(&dev->client->dev,
				"%s <--- set rsp_data = false\n",
				__func__);
		dev->rsp_data = false;
	}
success:
	mutex_unlock(&dev->spi_mutex);
	NFC_LOGD(&dev->client->dev, "%s <--- lock end\n", __func__);
	return ret;

header_error:
	mutex_unlock(&dev->spi_mutex);
	dev->read_check = !dev->read_check;
	ret = -1;
	NFC_LOGD(&dev->client->dev,
			"%s <--- header error read_check=%d\n", __func__,
			dev->read_check);
	return ret;
}

static irqreturn_t maxim_dev_irq_handler(int irq, void *dev_id)
{
	NFC_LOGD(&maxim_dev->client->dev, "%s : --- enter\n", __func__);

	if (device_may_wakeup(&maxim_dev->client->dev))
		pm_wakeup_event(&maxim_dev->client->dev, WAKEUP_SRC_TIMEOUT);

	maxim_disable_irq(maxim_dev);
	wake_up(&maxim_dev->read_wq);
	NFC_LOGD(&maxim_dev->client->dev, "%s : --- exit\n", __func__);

	return IRQ_HANDLED;
}

static int nfc_waiting_to_read(struct maxim_dev *dev)
{
	int ret = 0;

	while (1) {
		if (!dev->irq_enabled) {
			dev->irq_enabled = true;
			wake_up(&dev->write_wq);
			NFC_LOGD(&dev->client->dev,
					"%s <--- wake_up write_wq and set irq_enabled=%d\n",
					__func__,
					dev->irq_enabled);
			enable_irq(dev->client->irq);
		}
		if (!gpio_get_value(dev->irq_gpio)) {
			NFC_LOGD(&dev->client->dev,
				"%s <--- Start : wait_event_interruptible irq_enabled=%d\n",
				__func__,
				dev->irq_enabled);
			ret = wait_event_interruptible(dev->read_wq,
				!dev->irq_enabled);
		}
		if (ret)
			return ret;

		maxim_disable_irq(dev);

		if (gpio_get_value(dev->irq_gpio))
			break;

	}
	return ret;
}

static ssize_t nfc_read(struct file *filp, char __user *buf,
	size_t count, loff_t *offset)
{

	unsigned char *tmp = NULL;
	int ret;
	int irq_gpio_val = 0;

	NFC_LOGD(&maxim_dev->client->dev, "%s <--- enter\n", __func__);

	if (!maxim_dev) {
		ret = -ENODEV;
		goto out;
	}

	if (count > maxim_dev->kbuflen)
		count = maxim_dev->kbuflen;

	mutex_lock(&maxim_dev->read_mutex);

reload:
	ret = 0;
	if (maxim_dev->rsp_data == false) {
		/* wait for maxim irq reset */
		NFC_LOGD(&maxim_dev->client->dev,
				"%s <--- wait for next\n",
				__func__);
		msleep(20);
	}

	if (maxim_dev->rsp_data == false) {
		irq_gpio_val = gpio_get_value(maxim_dev->irq_gpio);
		if (irq_gpio_val == 0) {
			ret = nfc_waiting_to_read(maxim_dev);
			if (ret)
				goto err;
		}
	}
	NFC_LOGD(&maxim_dev->client->dev,
			"%s <--- End : wait_event_interruptible\n", __func__);
	tmp = maxim_dev->recv_rbuf;
	memset(tmp, 0x00, count);

	ret = 0;

	ret = nfc_spi_read(maxim_dev, tmp, count);

	if (ret == -1) {
		NFC_LOGD(&maxim_dev->client->dev,
				"%s <--- read error goto reload\n", __func__);
		goto reload;
	}

	ret = count;

	if (copy_to_user(buf, tmp, ret)) {
		dev_warn(&maxim_dev->client->dev,
				"%s :failed to copy to user space\n", __func__);
		ret = -EFAULT;
		goto err;
	}

err:
	mutex_unlock(&maxim_dev->read_mutex);
out:
	NFC_LOGD(&maxim_dev->client->dev, "%s <--- exit\n", __func__);
	return ret;
}

static ssize_t nfc_write(struct file *filp, const char __user *buf,
	size_t count, loff_t *offset)
{
	int ret = 0;
	char *tmp = NULL;

	NFC_LOGD(&maxim_dev->client->dev, "%s ---> enter\n", __func__);

	if (!maxim_dev) {
		ret = -ENODEV;
		goto out;
	}

	if (count > maxim_dev->kbuflen) {
		NFC_LOGE(&maxim_dev->client->dev,
				"%s: out of memory\n",
				__func__);
		ret = -ENOMEM;
		goto out;
	}

	tmp = memdup_user(buf, count);

	if (maxim_dev->irq_enabled == 0 ||
			gpio_get_value(maxim_dev->irq_gpio)) {
		NFC_LOGD(&maxim_dev->client->dev,
			"%s --->wait_event_interruptible start irq_enabled=%d\n",
			__func__,
			maxim_dev->irq_enabled);
		ret = wait_event_interruptible(maxim_dev->write_wq,
				maxim_dev->irq_enabled);

	}
	NFC_LOGD(&maxim_dev->client->dev,
			"%s ---> wait_event_interruptible end\n", __func__);

	ret = count;

	nfc_spi_write(maxim_dev, tmp, count);
	kfree(tmp);
out:
	NFC_LOGD(&maxim_dev->client->dev, "%s <--- exit\n", __func__);

	return ret;
}

static int nfc_open(struct inode *inode, struct file *filp)
{
	maxim_dev->rsp_data = false;
	maxim_dev->read_check = false;

	mutex_lock(&maxim_dev->dev_ref_mutex);

	if (maxim_dev->dev_ref_count == 0)
		maxim_enable_irq(maxim_dev);


	maxim_dev->dev_ref_count = maxim_dev->dev_ref_count + 1;

	mutex_unlock(&maxim_dev->dev_ref_mutex);

	dev_dbg(&maxim_dev->client->dev,
			"%s: %d,%d\n", __func__, imajor(inode), iminor(inode));
	return 0;
}

static int nfc_close(struct inode *inode, struct file *filp)
{
	mutex_lock(&maxim_dev->dev_ref_mutex);

	if (maxim_dev->dev_ref_count == 1)
		maxim_disable_irq(maxim_dev);

	if (maxim_dev->dev_ref_count > 0)
		maxim_dev->dev_ref_count = maxim_dev->dev_ref_count - 1;

	mutex_unlock(&maxim_dev->dev_ref_mutex);

	filp->private_data = NULL;

	return 0;
}

#ifdef CONFIG_COMPAT
static long nfc_compat_ioctl(struct file *pfile, unsigned int cmd,
	unsigned long arg)
{
	/* RFU */
	return 0;
}
#endif


static long nfc_ioctl(struct file *pfile, unsigned int cmd,
		unsigned long arg)
{
	/* RFU */
	return 0;
}

static const struct file_operations nfc_dev_fops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.read  = nfc_read,
	.write = nfc_write,
	.open = nfc_open,
	.release = nfc_close,
	.unlocked_ioctl = nfc_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = nfc_compat_ioctl
#endif
};

int32_t print_command(uint8_t *tmp, uint16_t len, uint16_t rw)
{
	char dmesgline[300] = {0};
	int n;

	for (n = 0 ; n < len ; n++)
		snprintf(dmesgline, sizeof(dmesgline), "%s  %02X",
				dmesgline, tmp[n]);
	if (rw == 1) {
		NFC_LOGE(&maxim_dev->client->dev,
				"Read TX command=[%s] count=[%d]\n",
				dmesgline, len);
	} else if (rw == 2) {
		NFC_LOGE(&maxim_dev->client->dev,
				"Read RX command=[%s] count=[%d]\n",
				dmesgline, len);
	} else if (rw == 3) {
		NFC_LOGE(&maxim_dev->client->dev,
				"Write TX command=[%s] count=[%d]\n",
				dmesgline, len);
	} else if (rw == 4) {
		NFC_LOGE(&maxim_dev->client->dev,
				"Write RX command=[%s] count=[%d]\n",
				dmesgline, len);
	}

	return 0;
}

static ssize_t maxim_dev_reset(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	gpio_set_value(maxim_dev->enable_gpio, 0);
	msleep(100);
	gpio_set_value(maxim_dev->enable_gpio, 1);
	return count;
}

static DEVICE_ATTR(maxim_reset, 0664, NULL, maxim_dev_reset);

static int nfc_parse_dt(struct device *dev)
{
	int r = 0;
	struct device_node *np = dev->of_node;

	maxim_dev->irq_gpio = of_get_named_gpio(np, "maxim,irq-gpio", 0);
	if ((!gpio_is_valid(maxim_dev->irq_gpio)))
		return -EINVAL;

	maxim_dev->enable_gpio = of_get_named_gpio(np, "maxim,en-gpio", 0);
	if ((!gpio_is_valid(maxim_dev->enable_gpio)))
		return -EINVAL;

	maxim_dev->reset_gpio = of_get_named_gpio(np, "maxim,reset-gpio", 0);
	if ((!gpio_is_valid(maxim_dev->reset_gpio)))
		return -EINVAL;

	return r;
}

static int maxim_probe(struct spi_device *client)
{
	int r = 0;
	int irqn = 0;
	int irq;
	int cs;
	int cpha, cpol, cs_high;
	u32 max_speed;

	dev_err(&client->dev, "max32560 %s: enter\n", __func__);

	maxim_dev = kzalloc(sizeof(*maxim_dev), GFP_KERNEL);

	if (maxim_dev == NULL)
		return -ENOMEM;

	maxim_dev->client = client;

	/* init SPI configuration */
	maxim_dev->send_xbuf = kzalloc(MAX_BUFFER_SIZE, GFP_KERNEL);
	if (maxim_dev->send_xbuf == NULL) {
		r = -ENOMEM;
		goto err_free_dev;
	}

	maxim_dev->send_rbuf = kzalloc(MAX_BUFFER_SIZE, GFP_KERNEL);
	if (maxim_dev->send_rbuf == NULL) {
		r = -ENOMEM;
		goto err_free_send_xbuf;
	}

	maxim_dev->recv_xbuf = kzalloc(MAX_BUFFER_SIZE, GFP_KERNEL);
	if (maxim_dev->recv_xbuf == NULL) {
		r = -ENOMEM;
		goto err_free_send_rbuf;
	}

	maxim_dev->recv_rbuf = kzalloc(MAX_BUFFER_SIZE, GFP_KERNEL);
	if (maxim_dev->recv_rbuf == NULL) {
		r = -ENOMEM;
		goto err_free_recv_xbuf;
	}

	maxim_dev->kbuflen = MAX_BUFFER_SIZE;
	maxim_dev->client->bits_per_word = 8;

	/* ---Check SPI configuration--- */
	irq = maxim_dev->client->irq;
	cs = maxim_dev->client->chip_select;
	cpha = (maxim_dev->client->mode & SPI_CPHA) ? 1:0;
	cpol = (maxim_dev->client->mode & SPI_CPOL) ? 1:0;
	cs_high = (maxim_dev->client->mode & SPI_CS_HIGH) ? 1:0;
	max_speed = maxim_dev->client->max_speed_hz;
	NFC_LOGE(&maxim_dev->client->dev,
		"irq [%d] cs [%x] CPHA [%x] CPOL [%x] CS_HIGH [%x] Max Speed [%d]\n",
		irq, cs, cpha, cpol, cs_high, max_speed);

	/* init GPIO */
	r = nfc_parse_dt(&maxim_dev->client->dev);
	if (r)
		goto err_spi_setup;

	if (gpio_is_valid(maxim_dev->irq_gpio)) {
		r = gpio_request(maxim_dev->irq_gpio, "nfc_irq_gpio");
		if (r) {
			NFC_LOGE(&maxim_dev->client->dev,
					"%s: unable to request nfc irq gpio [%d]\n",
					__func__, maxim_dev->irq_gpio);
			goto err_free_recv_rbuf;
		}
		r = gpio_direction_input(maxim_dev->irq_gpio);
		if (r) {
			NFC_LOGE(&maxim_dev->client->dev,
			"%s: unable to set direction for nfc irq gpio [%d]\n",
			  __func__,
			  maxim_dev->irq_gpio);
			goto err_irq_gpio;
		}
		irqn = gpio_to_irq(maxim_dev->irq_gpio);
		if (irqn < 0) {
			r = irqn;
			goto err_irq_gpio;
		}
		maxim_dev->client->irq = irqn;
	} else {
		NFC_LOGE(&maxim_dev->client->dev, "%s: irq gpio not provided\n",
				__func__);
		goto err_free_recv_rbuf;
	}

	/* Reset pin is low trigger. */
	if (gpio_is_valid(maxim_dev->reset_gpio)) {
		r = gpio_request(maxim_dev->reset_gpio, "nfc_reset_gpio");
		if (r) {
			NFC_LOGE(&maxim_dev->client->dev,
					"%s: unable to request nfc reset gpio [%d]\n",
					__func__, maxim_dev->reset_gpio);
			goto err_irq_gpio;

		}

		r = gpio_direction_output(maxim_dev->reset_gpio, 1);
		if (r) {
			NFC_LOGE(&maxim_dev->client->dev,
			"%s: unable to set direction for nfc reset gpio [%d]\n",
				__func__,
				maxim_dev->reset_gpio);
			goto err_enable_gpio;
		}
	} else {
		NFC_LOGE(&maxim_dev->client->dev,
				"%s: reset gpio not provided\n",
				__func__);
		goto err_irq_gpio;
	}

	if (gpio_is_valid(maxim_dev->enable_gpio)) {
		r = gpio_request(maxim_dev->enable_gpio, "nfc_enable_gpio");
		if (r) {
			NFC_LOGE(&maxim_dev->client->dev,
					"%s: unable to request nfc enable gpio [%d]\n",
					__func__, maxim_dev->enable_gpio);
			goto err_enable_gpio;
		}
		/* Enable 0 = off / 1 = on */
		r = gpio_direction_output(maxim_dev->enable_gpio, 1);
		if (r) {
			NFC_LOGE(&maxim_dev->client->dev,
			"%s: unable to set direction for nfc enable gpio [%d]\n",
				__func__, maxim_dev->enable_gpio);
			goto err_reset_gpio;
		}
	} else {
		NFC_LOGE(&maxim_dev->client->dev,
				"%s: enable gpio not provided\n",
				__func__);
		goto err_enable_gpio;
	}

	/* Power */
	if (gpio_is_valid(maxim_dev->enable_gpio))
		gpio_set_value(maxim_dev->enable_gpio, 1);

	/* init mutex and queues */
	init_waitqueue_head(&maxim_dev->read_wq);
	init_waitqueue_head(&maxim_dev->write_wq);
	mutex_init(&maxim_dev->read_mutex);
	mutex_init(&maxim_dev->spi_mutex);
	mutex_init(&maxim_dev->dev_ref_mutex);

	spin_lock_init(&maxim_dev->irq_enabled_lock);

	r = alloc_chrdev_region(&maxim_dev->devno, 0, DEV_COUNT, DEVICE_NAME);
	if (r < 0) {
		NFC_LOGE(&maxim_dev->client->dev,
			"%s: failed to alloc chrdev region\n", __func__);
		goto err_char_dev_register;
	}

	maxim_dev->maxim_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(maxim_dev->maxim_class)) {
		NFC_LOGE(&maxim_dev->client->dev,
			"%s: failed to register device class\n", __func__);
		goto err_class_create;
	}

	cdev_init(&maxim_dev->c_dev, &nfc_dev_fops);
	r = cdev_add(&maxim_dev->c_dev, maxim_dev->devno, DEV_COUNT);
	if (r < 0) {
		NFC_LOGE(&maxim_dev->client->dev, "%s: failed to add cdev\n",
				__func__);
		goto err_cdev_add;
	}

	maxim_dev->maxim_device = device_create(maxim_dev->maxim_class, NULL,
			maxim_dev->devno, maxim_dev, DEVICE_NAME);
	if (IS_ERR(maxim_dev->maxim_device)) {
		NFC_LOGE(&client->dev,
			"%s: failed to create the device\n", __func__);
		goto err_device_create;
	}

	/* NFC_INT IRQ */
	maxim_dev->irq_enabled = true;
	r = devm_request_irq(&maxim_dev->client->dev,
						maxim_dev->client->irq,
						maxim_dev_irq_handler,
						IRQF_TRIGGER_HIGH,
						"MAXIM-nfc",
						maxim_dev);

	if (r) {
		NFC_LOGE(&maxim_dev->client->dev,
				"%s: request_irq failed\n", __func__);
		goto err_request_irq_failed;
	}

	maxim_disable_irq(maxim_dev);

	spi_set_drvdata(client, maxim_dev);

	NFC_LOGI(&maxim_dev->client->dev,
			"max32560 %s: probing exited successfully\n",
			__func__);

	/* Firmware download
	 * /sys/bus/spi/drivers/nq-nci/spi0.0/maxim_reset
	 */
	device_create_file(&maxim_dev->client->dev, &dev_attr_maxim_reset);

	return 0;

err_request_irq_failed:
	device_destroy(maxim_dev->maxim_class, maxim_dev->devno);
err_device_create:
	cdev_del(&maxim_dev->c_dev);
err_cdev_add:
	class_destroy(maxim_dev->maxim_class);
err_class_create:
	unregister_chrdev_region(maxim_dev->devno, DEV_COUNT);
err_char_dev_register:
	mutex_destroy(&maxim_dev->read_mutex);
	mutex_destroy(&maxim_dev->spi_mutex);
err_enable_gpio:
	gpio_free(maxim_dev->enable_gpio);
err_reset_gpio:
	gpio_free(maxim_dev->reset_gpio);
err_irq_gpio:
	gpio_free(maxim_dev->irq_gpio);
err_spi_setup:
err_free_recv_rbuf:
	kfree(maxim_dev->recv_rbuf);
	maxim_dev->recv_rbuf = NULL;
err_free_recv_xbuf:
	kfree(maxim_dev->send_xbuf);
	maxim_dev->send_xbuf = NULL;
err_free_send_rbuf:
	kfree(maxim_dev->send_rbuf);
	maxim_dev->send_rbuf = NULL;
err_free_send_xbuf:
	kfree(maxim_dev->send_xbuf);
	maxim_dev->send_xbuf = NULL;
err_free_dev:
	kfree(maxim_dev);
	maxim_dev = NULL;

	return r;
}

static int maxim_remove(struct spi_device *client)
{
	int ret = 0;

	if (!maxim_dev) {
		NFC_LOGE(&maxim_dev->client->dev,
		"%s: device doesn't exist anymore\n", __func__);
		ret = -ENODEV;
		goto err;
	}

	cdev_del(&maxim_dev->c_dev);
	device_destroy(maxim_dev->maxim_class, maxim_dev->devno);
	class_destroy(maxim_dev->maxim_class);
	unregister_chrdev_region(maxim_dev->devno, DEV_COUNT);

	mutex_destroy(&maxim_dev->read_mutex);
	mutex_destroy(&maxim_dev->dev_ref_mutex);
	mutex_destroy(&maxim_dev->spi_mutex);

	gpio_free(maxim_dev->irq_gpio);
	gpio_free(maxim_dev->enable_gpio);
	gpio_free(maxim_dev->reset_gpio);

	kfree(maxim_dev->send_xbuf);
	maxim_dev->send_xbuf = NULL;
	kfree(maxim_dev->send_rbuf);
	maxim_dev->send_rbuf = NULL;
	kfree(maxim_dev->recv_xbuf);
	maxim_dev->recv_xbuf = NULL;
	kfree(maxim_dev->recv_rbuf);
	maxim_dev->recv_rbuf = NULL;

	kfree(maxim_dev);
	maxim_dev = NULL;
err:
	return ret;
}

static int maxim_suspend(struct device *device)
{
	gpio_set_value(maxim_dev->enable_gpio, 0);
	return 0;
}

static int maxim_resume(struct device *device)
{
	gpio_set_value(maxim_dev->enable_gpio, 1);
	return 0;
}

static const struct dev_pm_ops nfc_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(maxim_suspend, maxim_resume)
};

static const struct of_device_id msm_match_table[] = {
	{.compatible = "maxim,max32560"},
	{}
};

static struct spi_driver maxim = {
	.probe = maxim_probe,
	.remove = maxim_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "nq-nci",
		.of_match_table = msm_match_table,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.pm = &nfc_pm_ops,
	},
};

static int __init maxim_dev_init(void)
{
	return spi_register_driver(&maxim);
}
module_init(maxim_dev_init);

static void __exit maxim_dev_exit(void)
{
	spi_unregister_driver(&maxim);
}
module_exit(maxim_dev_exit);

MODULE_DESCRIPTION("NFC MAX32560");
MODULE_LICENSE("GPL v2");

