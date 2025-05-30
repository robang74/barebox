// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2007 Sascha Hauer, Pengutronix
 *               2009 Marc Kleine-Budde <mkl@pengutronix.de>
 * Copyright (C) 2010 Baruch Siach <baruch@tkos.co.il>
 */

#include <common.h>
#include <driver.h>
#include <xfuncs.h>
#include <errno.h>

#include <i2c/i2c.h>
#include <mfd/mc34704.h>

#define DRIVERNAME		"mc34704"

#define to_mc34704(a)		container_of(a, struct mc34704, cdev)

static struct mc34704 *mc34704_dev;

struct mc34704 *mc34704_get(void)
{
	if (!mc34704_dev)
		return NULL;

	return mc34704_dev;
}
EXPORT_SYMBOL(mc34704_get);

int mc34704_reg_read(struct mc34704 *mc34704, u8 reg, u8 *val)
{
	int ret;

	ret = i2c_read_reg(mc34704->client, 1 << 7 | reg, val, 1);

	return ret == 1 ? 0 : ret;
}
EXPORT_SYMBOL(mc34704_reg_read);

int mc34704_reg_write(struct mc34704 *mc34704, u8 reg, u8 val)
{
	int ret;

	ret = i2c_write_reg(mc34704->client, reg, &val, 1);

	return ret == 1 ? 0 : ret;
}
EXPORT_SYMBOL(mc34704_reg_write);

static ssize_t mc34704_read(struct cdev *cdev, void *_buf, size_t count,
		loff_t offset, ulong flags)
{
	struct mc34704 *priv = to_mc34704(cdev);
	u8 *buf = _buf;
	size_t i = count;
	int err;

	while (i) {
		err = mc34704_reg_read(priv, offset, buf);
		if (err)
			return (ssize_t)err;
		buf++;
		i--;
		offset++;
	}

	return count;
}

static ssize_t mc34704_write(struct cdev *cdev, const void *_buf, size_t count,
		loff_t offset, ulong flags)
{
	struct mc34704 *mc34704 = to_mc34704(cdev);
	const u8 *buf = _buf;
	size_t i = count;
	int err;

	while (i) {
		err = mc34704_reg_write(mc34704, offset, *buf);
		if (err)
			return (ssize_t)err;
		buf++;
		i--;
		offset++;
	}

	return count;
}

static struct cdev_operations mc34704_fops = {
	.read	= mc34704_read,
	.write	= mc34704_write,
};

static int mc34704_probe(struct device *dev)
{
	if (mc34704_dev)
		return -EBUSY;

	mc34704_dev = xzalloc(sizeof(struct mc34704));
	mc34704_dev->cdev.name = DRIVERNAME;
	mc34704_dev->client = to_i2c_client(dev);
	mc34704_dev->cdev.size = 128;
	mc34704_dev->cdev.dev = dev;
	mc34704_dev->cdev.ops = &mc34704_fops;

	devfs_create(&mc34704_dev->cdev);

	return 0;
}

static __maybe_unused struct of_device_id mc34704_dt_ids[] = {
	{ .compatible = "fsl,mc34704", },
	{ }
};

static struct driver mc34704_driver = {
	.name  = DRIVERNAME,
	.probe = mc34704_probe,
	.of_compatible = DRV_OF_COMPAT(mc34704_dt_ids),
};
device_i2c_driver(mc34704_driver);
