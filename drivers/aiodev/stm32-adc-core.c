// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018, STMicroelectronics - All Rights Reserved
 * Author: Fabrice Gasnier <fabrice.gasnier@st.com>
 *
 * Originally based on the Linux kernel v4.18 drivers/iio/adc/stm32-adc-core.c.
 */

#include <common.h>
#include <linux/clk.h>
#include <regulator.h>
#include <linux/bitops.h>
#include "stm32-adc-core.h"

/* STM32H7 - common registers for all ADC instances */
#define STM32H7_ADC_CCR			(STM32_ADCX_COMN_OFFSET + 0x08)

/* STM32H7_ADC_CCR - bit fields */
#define STM32H7_PRESC_SHIFT		18
#define STM32H7_PRESC_MASK		GENMASK(21, 18)
#define STM32H7_CKMODE_SHIFT		16
#define STM32H7_CKMODE_MASK		GENMASK(17, 16)

/* STM32 H7 maximum analog clock rate (from datasheet) */
#define STM32H7_ADC_MAX_CLK_RATE	36000000

/**
 * struct stm32h7_adc_ck_spec - specification for stm32h7 adc clock
 * @ckmode: ADC clock mode, Async or sync with prescaler.
 * @presc: prescaler bitfield for async clock mode
 * @div: prescaler division ratio
 */
struct stm32h7_adc_ck_spec {
	u32 ckmode;
	u32 presc;
	int div;
};

static const struct stm32h7_adc_ck_spec stm32h7_adc_ckmodes_spec[] = {
	/* 00: CK_ADC[1..3]: Asynchronous clock modes */
	{ 0, 0, 1 },
	{ 0, 1, 2 },
	{ 0, 2, 4 },
	{ 0, 3, 6 },
	{ 0, 4, 8 },
	{ 0, 5, 10 },
	{ 0, 6, 12 },
	{ 0, 7, 16 },
	{ 0, 8, 32 },
	{ 0, 9, 64 },
	{ 0, 10, 128 },
	{ 0, 11, 256 },
	/* HCLK used: Synchronous clock modes (1, 2 or 4 prescaler) */
	{ 1, 0, 1 },
	{ 2, 0, 2 },
	{ 3, 0, 4 },
};

static int stm32h7_adc_clk_sel(struct device *dev,
			       struct stm32_adc_common *common)
{
	u32 ckmode, presc;
	unsigned long rate;
	unsigned int i;
	int div;

	/* stm32h7 bus clock is common for all ADC instances (mandatory) */
	if (!common->bclk) {
		dev_err(dev, "No bclk clock found\n");
		return -ENOENT;
	}

	/*
	 * stm32h7 can use either 'bus' or 'adc' clock for analog circuitry.
	 * So, choice is to have bus clock mandatory and adc clock optional.
	 * If optional 'adc' clock has been found, then try to use it first.
	 */
	if (common->aclk) {
		/*
		 * Asynchronous clock modes (e.g. ckmode == 0)
		 * From spec: PLL output musn't exceed max rate
		 */
		rate = clk_get_rate(common->aclk);
		if (!rate) {
			dev_err(dev, "Invalid aclk rate: 0\n");
			return -EINVAL;
		}

		for (i = 0; i < ARRAY_SIZE(stm32h7_adc_ckmodes_spec); i++) {
			ckmode = stm32h7_adc_ckmodes_spec[i].ckmode;
			presc = stm32h7_adc_ckmodes_spec[i].presc;
			div = stm32h7_adc_ckmodes_spec[i].div;

			if (ckmode)
				continue;

			if ((rate / div) <= STM32H7_ADC_MAX_CLK_RATE)
				goto out;
		}
	}

	/* Synchronous clock modes (e.g. ckmode is 1, 2 or 3) */
	rate = clk_get_rate(common->bclk);
	if (!rate) {
		dev_err(dev, "Invalid bus clock rate: 0\n");
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(stm32h7_adc_ckmodes_spec); i++) {
		ckmode = stm32h7_adc_ckmodes_spec[i].ckmode;
		presc = stm32h7_adc_ckmodes_spec[i].presc;
		div = stm32h7_adc_ckmodes_spec[i].div;

		if (!ckmode)
			continue;

		if ((rate / div) <= STM32H7_ADC_MAX_CLK_RATE)
			goto out;
	}

	dev_err(dev, "clk selection failed\n");
	return -EINVAL;

out:
	/* rate used later by each ADC instance to control BOOST mode */
	common->rate = rate / div;

	/* Set common clock mode and prescaler */
	clrsetbits_le32(common->base + STM32H7_ADC_CCR,
			STM32H7_CKMODE_MASK | STM32H7_PRESC_MASK,
			ckmode << STM32H7_CKMODE_SHIFT |
			presc << STM32H7_PRESC_SHIFT);

	dev_dbg(dev, "Using %s clock/%d source at %ld kHz\n",
		ckmode ? "bus" : "adc", div, common->rate / 1000);

	return 0;
}

static int stm32_adc_core_probe(struct device *dev)
{
	struct stm32_adc_common *common;
	int ret;

	common = xzalloc(sizeof(*common));

	common->vref = regulator_get(dev, "vref");
	if (IS_ERR(common->vref)) {
		dev_err(dev, "can't get vref-supply: %pe\n", common->vref);
		return PTR_ERR(common->vref);
	}

	ret = regulator_get_voltage(common->vref);
	if (ret < 0) {
		dev_err(dev, "can't get vref-supply value: %d\n", ret);
		return ret;
	}
	common->vref_uv = ret;

	common->aclk = clk_get(dev, "adc");
	if (!IS_ERR(common->aclk)) {
		ret = clk_enable(common->aclk);
		if (ret) {
			dev_err(dev, "Can't enable aclk: %d\n", ret);
			return ret;
		}
	}

	common->bclk = clk_get(dev, "bus");
	if (!IS_ERR(common->bclk)) {
		ret = clk_enable(common->bclk);
		if (ret) {
			dev_err(dev, "Can't enable bclk: %d\n", ret);
			goto err_aclk_disable;
		}
	}

	common->base = dev_request_mem_region(dev, 0);
	if (IS_ERR(common->base)) {
		dev_err(dev, "can't get address\n");
		return -ENOENT;
	}

	ret = stm32h7_adc_clk_sel(dev, common);
	if (ret)
		goto err_bclk_disable;

	dev->priv = common;
	return of_platform_populate(dev->of_node, NULL, dev);

err_bclk_disable:
	clk_disable(common->bclk);

err_aclk_disable:
	clk_disable(common->aclk);

	return ret;
}

static const struct of_device_id stm32_adc_core_ids[] = {
	{ .compatible = "st,stm32h7-adc-core" },
	{ .compatible = "st,stm32mp1-adc-core" },
	{}
};

static struct driver stm32_adc_core_driver = {
	.name		= "stm32-adc-core",
	.probe		= stm32_adc_core_probe,
	.of_compatible	= DRV_OF_COMPAT(stm32_adc_core_ids),
};
device_platform_driver(stm32_adc_core_driver);
