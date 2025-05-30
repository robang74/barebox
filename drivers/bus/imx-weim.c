// SPDX-License-Identifier: GPL-2.0-only
/*
 * EIM driver for Freescale's i.MX chips
 *
 * Copyright (C) 2013 Freescale Semiconductor, Inc.
 */
#include <common.h>
#include <driver.h>
#include <init.h>
#include <io.h>
#include <of.h>
#include <linux/err.h>

struct imx_weim_devtype {
	unsigned int	cs_count;
	unsigned int	cs_regs_count;
	unsigned int	cs_stride;
};

static const struct imx_weim_devtype imx1_weim_devtype = {
	.cs_count	= 6,
	.cs_regs_count	= 2,
	.cs_stride	= 0x08,
};

static const struct imx_weim_devtype imx27_weim_devtype = {
	.cs_count	= 6,
	.cs_regs_count	= 3,
	.cs_stride	= 0x10,
};

static const struct imx_weim_devtype imx50_weim_devtype = {
	.cs_count	= 4,
	.cs_regs_count	= 6,
	.cs_stride	= 0x18,
};

static const struct imx_weim_devtype imx51_weim_devtype = {
	.cs_count	= 6,
	.cs_regs_count	= 6,
	.cs_stride	= 0x18,
};

static struct of_device_id weim_id_table[] = {
	{
		/* i.MX1/21 */
		.compatible = "fsl,imx1-weim",
		.data = &imx1_weim_devtype,
	}, {
		/* i.MX25/27/31/35 */
		.compatible = "fsl,imx27-weim",
		.data = &imx27_weim_devtype,
	}, {
		/* i.MX50/53/6Q */
		.compatible = "fsl,imx50-weim",
		.data = &imx50_weim_devtype,
	}, {
		/* i.MX51 */
		.compatible = "fsl,imx51-weim",
		.data = &imx51_weim_devtype,
	}, {
		.compatible = "fsl,imx6q-weim",
		.data = &imx50_weim_devtype,
	}, {
	}
};

struct imx_weim {
	struct device *dev;
	void __iomem *base;
	struct imx_weim_devtype *devtype;
};

/* Parse and set the timing for this device. */
static int
weim_timing_setup(struct imx_weim *weim, struct device_node *np)
{
	struct imx_weim_devtype *devtype = weim->devtype;
	u32 cs_idx, value[devtype->cs_regs_count];
	int i, ret;

	/* get the CS index from this child node's "reg" property. */
	ret = of_property_read_u32(np, "reg", &cs_idx);
	if (ret)
		return ret;

	if (cs_idx >= devtype->cs_count)
		return -EINVAL;

	ret = of_property_read_u32_array(np, "fsl,weim-cs-timing",
			value, devtype->cs_regs_count);
	if (ret)
		return ret;

	dev_dbg(weim->dev, "setting up cs for %s\n", np->name);

	/* set the timing for WEIM */
	for (i = 0; i < devtype->cs_regs_count; i++)
		writel(value[i], weim->base + cs_idx * devtype->cs_stride + i * 4);

	return 0;
}

static int weim_parse_dt(struct imx_weim *weim)
{
	struct device_node *child;
	int ret;

	for_each_child_of_node(weim->dev->of_node, child) {
		if (!child->name)
			continue;

		ret = weim_timing_setup(weim, child);
		if (ret) {
			dev_err(weim->dev, "%s set timing failed.\n",
				child->full_name);
			return ret;
		}
	}

	ret = of_platform_populate(weim->dev->of_node, NULL, weim->dev);
	if (ret)
		dev_err(weim->dev, "%s fail to create devices.\n",
			weim->dev->of_node->full_name);
	return ret;
}

static int weim_probe(struct device *dev)
{
	struct resource *iores;
	struct imx_weim_devtype *devtype;
	struct imx_weim *weim;
	int ret;

	ret = dev_get_drvdata(dev, (const void **)&devtype);
	if (ret)
		return ret;

	weim = xzalloc(sizeof(*weim));

	weim->dev = dev;
	weim->devtype = devtype;

	/* get the resource */
	iores = dev_request_mem_resource(dev, 0);
	if (IS_ERR(iores)) {
		ret = PTR_ERR(iores);
		goto weim_err;
	}
	weim->base = IOMEM(iores->start);

	/* parse the device node */
	ret = weim_parse_dt(weim);
	if (ret) {
		goto weim_err;
	}

	dev_info(dev, "WEIM driver registered.\n");

	return 0;

weim_err:
	return ret;
}

static struct driver weim_driver = {
	.name = "imx-weim",
	.of_compatible = DRV_OF_COMPAT(weim_id_table),
	.probe   = weim_probe,
};
device_platform_driver(weim_driver);
