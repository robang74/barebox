// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for Microchip USB251xB USB 2.0 Hi-Speed Hub Controller
 * Configuration via SMBus.
 *
 * Copyright (c) 2017 SKIDATA AG
 *
 * This work is based on the USB3503 driver by Dongjin Kim and
 * a not-accepted patch by Fabien Lahoudere, see:
 * https://patchwork.kernel.org/patch/9257715/
 */

#include <common.h>
#include <init.h>
#include <driver.h>
#include <i2c/i2c.h>
#include <of.h>
#include <gpio.h>
#include <of_gpio.h>
#include <of_device.h>

#include <linux/nls.h>

/* Internal Register Set Addresses & Default Values acc. to DS00001692C */
#define USB251XB_ADDR_VENDOR_ID_LSB	0x00
#define USB251XB_ADDR_VENDOR_ID_MSB	0x01
#define USB251XB_DEF_VENDOR_ID		0x0424

#define USB251XB_ADDR_PRODUCT_ID_LSB	0x02
#define USB251XB_ADDR_PRODUCT_ID_MSB	0x03
#define USB251XB_DEF_PRODUCT_ID_12	0x2512 /* USB2512B/12Bi */
#define USB251XB_DEF_PRODUCT_ID_13	0x2513 /* USB2513B/13Bi */
#define USB251XB_DEF_PRODUCT_ID_14	0x2514 /* USB2514B/14Bi */
#define USB251XB_DEF_PRODUCT_ID_17	0x2517 /* USB2517/17i */

#define USB251XB_ADDR_DEVICE_ID_LSB	0x04
#define USB251XB_ADDR_DEVICE_ID_MSB	0x05
#define USB251XB_DEF_DEVICE_ID		0x0BB3

#define USB251XB_ADDR_CONFIG_DATA_1	0x06
#define USB251XB_DEF_CONFIG_DATA_1	0x9B
#define USB251XB_ADDR_CONFIG_DATA_2	0x07
#define USB251XB_DEF_CONFIG_DATA_2	0x20
#define USB251XB_ADDR_CONFIG_DATA_3	0x08
#define USB251XB_DEF_CONFIG_DATA_3	0x02

#define USB251XB_ADDR_NON_REMOVABLE_DEVICES	0x09
#define USB251XB_DEF_NON_REMOVABLE_DEVICES	0x00

#define USB251XB_ADDR_PORT_DISABLE_SELF	0x0A
#define USB251XB_DEF_PORT_DISABLE_SELF	0x00
#define USB251XB_ADDR_PORT_DISABLE_BUS	0x0B
#define USB251XB_DEF_PORT_DISABLE_BUS	0x00

#define USB251XB_ADDR_MAX_POWER_SELF	0x0C
#define USB251XB_DEF_MAX_POWER_SELF	0x01
#define USB251XB_ADDR_MAX_POWER_BUS	0x0D
#define USB251XB_DEF_MAX_POWER_BUS	0x32

#define USB251XB_ADDR_MAX_CURRENT_SELF	0x0E
#define USB251XB_DEF_MAX_CURRENT_SELF	0x01
#define USB251XB_ADDR_MAX_CURRENT_BUS	0x0F
#define USB251XB_DEF_MAX_CURRENT_BUS	0x32

#define USB251XB_ADDR_POWER_ON_TIME	0x10
#define USB251XB_DEF_POWER_ON_TIME	0x32

#define USB251XB_ADDR_LANGUAGE_ID_HIGH	0x11
#define USB251XB_ADDR_LANGUAGE_ID_LOW	0x12
#define USB251XB_DEF_LANGUAGE_ID	0x0000

#define USB251XB_STRING_BUFSIZE			62
#define USB251XB_ADDR_MANUFACTURER_STRING_LEN	0x13
#define USB251XB_ADDR_MANUFACTURER_STRING	0x16
#define USB251XB_DEF_MANUFACTURER_STRING	"Microchip"

#define USB251XB_ADDR_PRODUCT_STRING_LEN	0x14
#define USB251XB_ADDR_PRODUCT_STRING		0x54
#define USB251XB_DEF_PRODUCT_STRING		"USB251xB/xBi/7i"

#define USB251XB_ADDR_SERIAL_STRING_LEN		0x15
#define USB251XB_ADDR_SERIAL_STRING		0x92
#define USB251XB_DEF_SERIAL_STRING		""

#define USB251XB_ADDR_BATTERY_CHARGING_ENABLE	0xD0
#define USB251XB_DEF_BATTERY_CHARGING_ENABLE	0x00

#define USB251XB_ADDR_BOOST_UP	0xF6
#define USB251XB_DEF_BOOST_UP	0x00
#define USB251XB_ADDR_BOOST_57	0xF7
#define USB251XB_DEF_BOOST_57	0x00
#define USB251XB_ADDR_BOOST_14	0xF8
#define USB251XB_DEF_BOOST_14	0x00

#define USB251XB_ADDR_PORT_SWAP	0xFA
#define USB251XB_DEF_PORT_SWAP	0x00

#define USB251XB_ADDR_PORT_MAP_12	0xFB
#define USB251XB_DEF_PORT_MAP_12	0x00
#define USB251XB_ADDR_PORT_MAP_34	0xFC
#define USB251XB_DEF_PORT_MAP_34	0x00 /* USB251{3B/i,4B/i,7/i} only */
#define USB251XB_ADDR_PORT_MAP_56	0xFD
#define USB251XB_DEF_PORT_MAP_56	0x00 /* USB2517/i only */
#define USB251XB_ADDR_PORT_MAP_7	0xFE
#define USB251XB_DEF_PORT_MAP_7		0x00 /* USB2517/i only */

#define USB251XB_ADDR_STATUS_COMMAND		0xFF
#define USB251XB_STATUS_COMMAND_SMBUS_DOWN	0x04
#define USB251XB_STATUS_COMMAND_RESET		0x02
#define USB251XB_STATUS_COMMAND_ATTACH		0x01

#define USB251XB_I2C_REG_SZ	0x100
#define USB251XB_I2C_WRITE_SZ	0x10

#define DRIVER_NAME	"usb251xb"
#define DRIVER_DESC	"Microchip USB 2.0 Hi-Speed Hub Controller"

struct usb251xb {
	struct device *dev;
	struct i2c_client *i2c;
	u8 skip_config;
	int gpio_reset;
	u16 vendor_id;
	u16 product_id;
	u16 device_id;
	u8  conf_data1;
	u8  conf_data2;
	u8  conf_data3;
	u8  non_rem_dev;
	u8  port_disable_sp;
	u8  port_disable_bp;
	u8  max_power_sp;
	u8  max_power_bp;
	u8  max_current_sp;
	u8  max_current_bp;
	u8  power_on_time;
	u16 lang_id;
	u8 manufacturer_len;
	u8 product_len;
	u8 serial_len;
	char manufacturer[USB251XB_STRING_BUFSIZE];
	char product[USB251XB_STRING_BUFSIZE];
	char serial[USB251XB_STRING_BUFSIZE];
	u8  bat_charge_en;
	u8  boost_up;
	u8  boost_57;
	u8  boost_14;
	u8  port_swap;
	u8  port_map12;
	u8  port_map34;
	u8  port_map56;
	u8  port_map7;
	u8  status;
};

struct usb251xb_data {
	u16 product_id;
	u8 port_cnt;
	bool led_support;
	bool bat_support;
	char product_str[USB251XB_STRING_BUFSIZE / 2]; /* ASCII string */
};

static const struct usb251xb_data usb2512b_data = {
	.product_id = 0x2512,
	.port_cnt = 2,
	.led_support = false,
	.bat_support = true,
	.product_str = "USB2512B",
};

static const struct usb251xb_data usb2512bi_data = {
	.product_id = 0x2512,
	.port_cnt = 2,
	.led_support = false,
	.bat_support = true,
	.product_str = "USB2512Bi",
};

static const struct usb251xb_data usb2513b_data = {
	.product_id = 0x2513,
	.port_cnt = 3,
	.led_support = false,
	.bat_support = true,
	.product_str = "USB2513B",
};

static const struct usb251xb_data usb2513bi_data = {
	.product_id = 0x2513,
	.port_cnt = 3,
	.led_support = false,
	.bat_support = true,
	.product_str = "USB2513Bi",
};

static const struct usb251xb_data usb2514b_data = {
	.product_id = 0x2514,
	.port_cnt = 4,
	.led_support = false,
	.bat_support = true,
	.product_str = "USB2514B",
};

static const struct usb251xb_data usb2514bi_data = {
	.product_id = 0x2514,
	.port_cnt = 4,
	.led_support = false,
	.bat_support = true,
	.product_str = "USB2514Bi",
};

static const struct usb251xb_data usb2517_data = {
	.product_id = 0x2517,
	.port_cnt = 7,
	.led_support = true,
	.bat_support = false,
	.product_str = "USB2517",
};

static const struct usb251xb_data usb2517i_data = {
	.product_id = 0x2517,
	.port_cnt = 7,
	.led_support = true,
	.bat_support = false,
	.product_str = "USB2517i",
};

static void usb251xb_reset(struct usb251xb *hub, int state)
{
	if (!gpio_is_valid(hub->gpio_reset))
		return;

	gpio_set_active(hub->gpio_reset, state);

	/* wait for hub recovery/stabilization */
	if (!state)
		udelay(750);	/* >=500us at power on */
	else
		udelay(10);	/* >=1us at power down */
}

static int usb251xb_connect(struct usb251xb *hub)
{
	struct device *dev = hub->dev;
	int err, i;
	char i2c_wb[USB251XB_I2C_REG_SZ];

	memset(i2c_wb, 0, USB251XB_I2C_REG_SZ);

	if (hub->skip_config) {
		dev_info(dev, "Skip hub configuration, only attach.\n");
		i2c_wb[0] = 0x01;
		i2c_wb[1] = USB251XB_STATUS_COMMAND_ATTACH;

		usb251xb_reset(hub, 0);

		err = i2c_smbus_write_i2c_block_data(hub->i2c,
				USB251XB_ADDR_STATUS_COMMAND, 2, i2c_wb);
		if (err) {
			dev_err(dev, "attaching hub failed: %d\n", err);
			return err;
		}
		return 0;
	}

	i2c_wb[USB251XB_ADDR_VENDOR_ID_MSB]     = (hub->vendor_id >> 8) & 0xFF;
	i2c_wb[USB251XB_ADDR_VENDOR_ID_LSB]     = hub->vendor_id & 0xFF;
	i2c_wb[USB251XB_ADDR_PRODUCT_ID_MSB]    = (hub->product_id >> 8) & 0xFF;
	i2c_wb[USB251XB_ADDR_PRODUCT_ID_LSB]    = hub->product_id & 0xFF;
	i2c_wb[USB251XB_ADDR_DEVICE_ID_MSB]     = (hub->device_id >> 8) & 0xFF;
	i2c_wb[USB251XB_ADDR_DEVICE_ID_LSB]     = hub->device_id & 0xFF;
	i2c_wb[USB251XB_ADDR_CONFIG_DATA_1]     = hub->conf_data1;
	i2c_wb[USB251XB_ADDR_CONFIG_DATA_2]     = hub->conf_data2;
	i2c_wb[USB251XB_ADDR_CONFIG_DATA_3]     = hub->conf_data3;
	i2c_wb[USB251XB_ADDR_NON_REMOVABLE_DEVICES] = hub->non_rem_dev;
	i2c_wb[USB251XB_ADDR_PORT_DISABLE_SELF] = hub->port_disable_sp;
	i2c_wb[USB251XB_ADDR_PORT_DISABLE_BUS]  = hub->port_disable_bp;
	i2c_wb[USB251XB_ADDR_MAX_POWER_SELF]    = hub->max_power_sp;
	i2c_wb[USB251XB_ADDR_MAX_POWER_BUS]     = hub->max_power_bp;
	i2c_wb[USB251XB_ADDR_MAX_CURRENT_SELF]  = hub->max_current_sp;
	i2c_wb[USB251XB_ADDR_MAX_CURRENT_BUS]   = hub->max_current_bp;
	i2c_wb[USB251XB_ADDR_POWER_ON_TIME]     = hub->power_on_time;
	i2c_wb[USB251XB_ADDR_LANGUAGE_ID_HIGH]  = (hub->lang_id >> 8) & 0xFF;
	i2c_wb[USB251XB_ADDR_LANGUAGE_ID_LOW]   = hub->lang_id & 0xFF;
	i2c_wb[USB251XB_ADDR_MANUFACTURER_STRING_LEN] = hub->manufacturer_len;
	i2c_wb[USB251XB_ADDR_PRODUCT_STRING_LEN]      = hub->product_len;
	i2c_wb[USB251XB_ADDR_SERIAL_STRING_LEN]       = hub->serial_len;
	memcpy(&i2c_wb[USB251XB_ADDR_MANUFACTURER_STRING], hub->manufacturer,
	       USB251XB_STRING_BUFSIZE);
	memcpy(&i2c_wb[USB251XB_ADDR_SERIAL_STRING], hub->serial,
	       USB251XB_STRING_BUFSIZE);
	memcpy(&i2c_wb[USB251XB_ADDR_PRODUCT_STRING], hub->product,
	       USB251XB_STRING_BUFSIZE);
	i2c_wb[USB251XB_ADDR_BATTERY_CHARGING_ENABLE] = hub->bat_charge_en;
	i2c_wb[USB251XB_ADDR_BOOST_UP]          = hub->boost_up;
	i2c_wb[USB251XB_ADDR_BOOST_57]          = hub->boost_57;
	i2c_wb[USB251XB_ADDR_BOOST_14]          = hub->boost_14;
	i2c_wb[USB251XB_ADDR_PORT_SWAP]         = hub->port_swap;
	i2c_wb[USB251XB_ADDR_PORT_MAP_12]       = hub->port_map12;
	i2c_wb[USB251XB_ADDR_PORT_MAP_34]       = hub->port_map34;
	i2c_wb[USB251XB_ADDR_PORT_MAP_56]       = hub->port_map56;
	i2c_wb[USB251XB_ADDR_PORT_MAP_7]        = hub->port_map7;
	i2c_wb[USB251XB_ADDR_STATUS_COMMAND] = USB251XB_STATUS_COMMAND_ATTACH;

	usb251xb_reset(hub, 0);

	/* write registers */
	for (i = 0; i < (USB251XB_I2C_REG_SZ / USB251XB_I2C_WRITE_SZ); i++) {
		int offset = i * USB251XB_I2C_WRITE_SZ;
		char wbuf[USB251XB_I2C_WRITE_SZ + 1];

		/* The first data byte transferred tells the hub how many data
		 * bytes will follow (byte count).
		 */
		wbuf[0] = USB251XB_I2C_WRITE_SZ;
		memcpy(&wbuf[1], &i2c_wb[offset], USB251XB_I2C_WRITE_SZ);

		dev_dbg(dev, "writing %d byte block %d to 0x%02X\n",
			USB251XB_I2C_WRITE_SZ, i, offset);

		err = i2c_smbus_write_i2c_block_data(hub->i2c, offset,
						     USB251XB_I2C_WRITE_SZ + 1,
						     wbuf);
		if (err)
			goto out_err;
	}

	dev_info(dev, "Hub configuration was successful.\n");
	return 0;

out_err:
	dev_err(dev, "configuring block %d failed: %d\n", i, err);
	return err;
}

#ifdef CONFIG_OFDEVICE
static int usb251xb_get_ofdata(struct usb251xb *hub,
			       struct usb251xb_data *data)
{
	struct device *dev = hub->dev;
	struct device_node *np = dev->of_node;
	int len, i;
	u32 port, property_u32 = 0;
	const u32 *cproperty_u32;
	const char *cproperty_char;
	enum of_gpio_flags of_flags;
	unsigned long flags = GPIOF_OUT_INIT_ACTIVE;
	char str[USB251XB_STRING_BUFSIZE / 2];
	struct property *prop;
	const __be32 *p;

	if (!np) {
		dev_err(dev, "failed to get ofdata\n");
		return -ENODEV;
	}

	if (of_get_property(np, "skip-config", NULL))
		hub->skip_config = 1;
	else
		hub->skip_config = 0;

	hub->gpio_reset = of_get_named_gpio_flags(np, "reset-gpios", 0,
						  &of_flags);
	if (gpio_is_valid(hub->gpio_reset)) {
		char *name;
		int ret;

		if (of_flags & OF_GPIO_ACTIVE_LOW)
			flags |= GPIOF_ACTIVE_LOW;

		name = basprintf("%s reset", dev_name(dev));
		ret  = gpio_request_one(hub->gpio_reset, flags, name);
		if (ret < 0)
			return ret;
	} else if (hub->gpio_reset == -EPROBE_DEFER) {
		return -EPROBE_DEFER;
	}

	if (of_property_read_u16_array(np, "vendor-id", &hub->vendor_id, 1))
		hub->vendor_id = USB251XB_DEF_VENDOR_ID;

	if (of_property_read_u16_array(np, "product-id",
				       &hub->product_id, 1))
		hub->product_id = data->product_id;

	if (of_property_read_u16_array(np, "device-id", &hub->device_id, 1))
		hub->device_id = USB251XB_DEF_DEVICE_ID;

	hub->conf_data1 = USB251XB_DEF_CONFIG_DATA_1;
	if (of_get_property(np, "self-powered", NULL)) {
		hub->conf_data1 |= BIT(7);

		/* Configure Over-Current sens when self-powered */
		hub->conf_data1 &= ~BIT(2);
		if (of_get_property(np, "ganged-sensing", NULL))
			hub->conf_data1 &= ~BIT(1);
		else if (of_get_property(np, "individual-sensing", NULL))
			hub->conf_data1 |= BIT(1);
	} else if (of_get_property(np, "bus-powered", NULL)) {
		hub->conf_data1 &= ~BIT(7);

		/* Disable Over-Current sense when bus-powered */
		hub->conf_data1 |= BIT(2);
	}

	if (of_get_property(np, "disable-hi-speed", NULL))
		hub->conf_data1 |= BIT(5);

	if (of_get_property(np, "multi-tt", NULL))
		hub->conf_data1 |= BIT(4);
	else if (of_get_property(np, "single-tt", NULL))
		hub->conf_data1 &= ~BIT(4);

	if (of_get_property(np, "disable-eop", NULL))
		hub->conf_data1 |= BIT(3);

	if (of_get_property(np, "individual-port-switching", NULL))
		hub->conf_data1 |= BIT(0);
	else if (of_get_property(np, "ganged-port-switching", NULL))
		hub->conf_data1 &= ~BIT(0);

	hub->conf_data2 = USB251XB_DEF_CONFIG_DATA_2;
	if (of_get_property(np, "dynamic-power-switching", NULL))
		hub->conf_data2 |= BIT(7);

	if (!of_property_read_u32(np, "oc-delay-us", &property_u32)) {
		if (property_u32 == 100) {
			/* 100 us*/
			hub->conf_data2 &= ~BIT(5);
			hub->conf_data2 &= ~BIT(4);
		} else if (property_u32 == 4000) {
			/* 4 ms */
			hub->conf_data2 &= ~BIT(5);
			hub->conf_data2 |= BIT(4);
		} else if (property_u32 == 16000) {
			/* 16 ms */
			hub->conf_data2 |= BIT(5);
			hub->conf_data2 |= BIT(4);
		} else {
			/* 8 ms (DEFAULT) */
			hub->conf_data2 |= BIT(5);
			hub->conf_data2 &= ~BIT(4);
		}
	}

	if (of_get_property(np, "compound-device", NULL))
		hub->conf_data2 |= BIT(3);

	hub->conf_data3 = USB251XB_DEF_CONFIG_DATA_3;
	if (of_get_property(np, "port-mapping-mode", NULL))
		hub->conf_data3 |= BIT(3);

	if (data->led_support && of_get_property(np, "led-usb-mode", NULL))
		hub->conf_data3 &= ~BIT(1);

	if (of_get_property(np, "string-support", NULL))
		hub->conf_data3 |= BIT(0);

	hub->non_rem_dev = USB251XB_DEF_NON_REMOVABLE_DEVICES;
	cproperty_u32 = of_get_property(np, "non-removable-ports", &len);
	if (cproperty_u32 && (len / sizeof(u32)) > 0) {
		for (i = 0; i < len / sizeof(u32); i++) {
			u32 port = be32_to_cpu(cproperty_u32[i]);

			if ((port >= 1) && (port <= data->port_cnt))
				hub->non_rem_dev |= BIT(port);
			else
				dev_warn(dev, "NRD port %u doesn't exist\n",
					port);
		}
	}

	hub->port_disable_sp = USB251XB_DEF_PORT_DISABLE_SELF;
	cproperty_u32 = of_get_property(np, "sp-disabled-ports", &len);
	if (cproperty_u32 && (len / sizeof(u32)) > 0) {
		for (i = 0; i < len / sizeof(u32); i++) {
			u32 port = be32_to_cpu(cproperty_u32[i]);

			if ((port >= 1) && (port <= data->port_cnt))
				hub->port_disable_sp |= BIT(port);
			else
				dev_warn(dev, "PDS port %u doesn't exist\n",
					port);
		}
	}

	hub->port_disable_bp = USB251XB_DEF_PORT_DISABLE_BUS;
	cproperty_u32 = of_get_property(np, "bp-disabled-ports", &len);
	if (cproperty_u32 && (len / sizeof(u32)) > 0) {
		for (i = 0; i < len / sizeof(u32); i++) {
			u32 port = be32_to_cpu(cproperty_u32[i]);

			if ((port >= 1) && (port <= data->port_cnt))
				hub->port_disable_bp |= BIT(port);
			else
				dev_warn(dev, "PDB port %u doesn't exist\n",
					port);
		}
	}

	hub->max_power_sp = USB251XB_DEF_MAX_POWER_SELF;
	if (!of_property_read_u32(np, "sp-max-total-current-microamp",
	    &property_u32))
		hub->max_power_sp = min_t(u8, property_u32 / 2000, 50);

	hub->max_power_bp = USB251XB_DEF_MAX_POWER_BUS;
	if (!of_property_read_u32(np, "bp-max-total-current-microamp",
	    &property_u32))
		hub->max_power_bp = min_t(u8, property_u32 / 2000, 255);

	hub->max_current_sp = USB251XB_DEF_MAX_CURRENT_SELF;
	if (!of_property_read_u32(np, "sp-max-removable-current-microamp",
	    &property_u32))
		hub->max_current_sp = min_t(u8, property_u32 / 2000, 50);

	hub->max_current_bp = USB251XB_DEF_MAX_CURRENT_BUS;
	if (!of_property_read_u32(np, "bp-max-removable-current-microamp",
	    &property_u32))
		hub->max_current_bp = min_t(u8, property_u32 / 2000, 255);

	hub->power_on_time = USB251XB_DEF_POWER_ON_TIME;
	if (!of_property_read_u32(np, "power-on-time-ms", &property_u32))
		hub->power_on_time = min_t(u8, property_u32 / 2, 255);

	if (of_property_read_u16_array(np, "language-id", &hub->lang_id, 1))
		hub->lang_id = USB251XB_DEF_LANGUAGE_ID;

	cproperty_char = of_get_property(np, "manufacturer", NULL);
	strlcpy(str, cproperty_char ? : USB251XB_DEF_MANUFACTURER_STRING,
		sizeof(str));
	hub->manufacturer_len = strlen(str) & 0xFF;
	memset(hub->manufacturer, 0, USB251XB_STRING_BUFSIZE);
	len = min_t(size_t, USB251XB_STRING_BUFSIZE / 2, strlen(str));
	len = utf8s_to_utf16s(str, len, UTF16_LITTLE_ENDIAN,
			      (wchar_t *)hub->manufacturer,
			      USB251XB_STRING_BUFSIZE);

	cproperty_char = of_get_property(np, "product", NULL);
	strlcpy(str, cproperty_char ? : data->product_str, sizeof(str));
	hub->product_len = strlen(str) & 0xFF;
	memset(hub->product, 0, USB251XB_STRING_BUFSIZE);
	len = min_t(size_t, USB251XB_STRING_BUFSIZE / 2, strlen(str));
	len = utf8s_to_utf16s(str, len, UTF16_LITTLE_ENDIAN,
			      (wchar_t *)hub->product,
			      USB251XB_STRING_BUFSIZE);

	cproperty_char = of_get_property(np, "serial", NULL);
	strlcpy(str, cproperty_char ? : USB251XB_DEF_SERIAL_STRING,
		sizeof(str));
	hub->serial_len = strlen(str) & 0xFF;
	memset(hub->serial, 0, USB251XB_STRING_BUFSIZE);
	len = min_t(size_t, USB251XB_STRING_BUFSIZE / 2, strlen(str));
	len = utf8s_to_utf16s(str, len, UTF16_LITTLE_ENDIAN,
			      (wchar_t *)hub->serial,
			      USB251XB_STRING_BUFSIZE);

	/*
	 * The datasheet documents the register as 'Port Swap' but the register
	 * controls the USB DP/DM signal swapping
	 */
	hub->port_swap = USB251XB_DEF_PORT_SWAP;
	of_property_for_each_u32(np, "swap-dx-lanes", prop, p, port) {
		if (port <= data->port_cnt)
			hub->port_swap |= BIT(port);
	}

	/* The following parameters are currently not exposed to devicetree, but
	 * may be as soon as needed.
	 */
	hub->bat_charge_en = USB251XB_DEF_BATTERY_CHARGING_ENABLE;
	hub->boost_up = USB251XB_DEF_BOOST_UP;
	hub->boost_57 = USB251XB_DEF_BOOST_57;
	hub->boost_14 = USB251XB_DEF_BOOST_14;
	hub->port_map12 = USB251XB_DEF_PORT_MAP_12;
	hub->port_map34 = USB251XB_DEF_PORT_MAP_34;
	hub->port_map56 = USB251XB_DEF_PORT_MAP_56;
	hub->port_map7  = USB251XB_DEF_PORT_MAP_7;

	return 0;
}

static const struct of_device_id usb251xb_of_match[] = {
	{
		.compatible = "microchip,usb2512b",
		.data = &usb2512b_data,
	}, {
		.compatible = "microchip,usb2512bi",
		.data = &usb2512bi_data,
	}, {
		.compatible = "microchip,usb2513b",
		.data = &usb2513b_data,
	}, {
		.compatible = "microchip,usb2513bi",
		.data = &usb2513bi_data,
	}, {
		.compatible = "microchip,usb2514b",
		.data = &usb2514b_data,
	}, {
		.compatible = "microchip,usb2514bi",
		.data = &usb2514bi_data,
	}, {
		.compatible = "microchip,usb2517",
		.data = &usb2517_data,
	}, {
		.compatible = "microchip,usb2517i",
		.data = &usb2517i_data,
	}, {
		/* sentinel */
	}
};
#else /* CONFIG_OFDEVICE */
static int usb251xb_get_ofdata(struct usb251xb *hub,
			       struct usb251xb_data *data)
{
	return 0;
}
#endif /* CONFIG_OFDEVICE */

static int usb251xb_probe(struct usb251xb *hub)
{
	struct device *dev = hub->dev;
	struct device_node *np = dev->of_node;
	const struct of_device_id *of_id = of_match_device(usb251xb_of_match,
							   dev);
	int err;

	if (np) {
		err = usb251xb_get_ofdata(hub,
					  (struct usb251xb_data *)of_id->data);
		if (err) {
			dev_err(dev, "failed to get ofdata: %d\n", err);
			return err;
		}
	}

	err = usb251xb_connect(hub);
	if (err) {
		dev_err(dev, "Failed to connect hub (%d)\n", err);
		return err;
	}

	dev_info(dev, "Hub probed successfully\n");

	return 0;
}

static int usb251xb_i2c_probe(struct device *dev)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct usb251xb *hub;

	hub = xzalloc(sizeof(struct usb251xb));

	i2c_set_clientdata(i2c, hub);
	hub->dev = &i2c->dev;
	hub->i2c = i2c;

	return usb251xb_probe(hub);
}

static const struct platform_device_id usb251xb_id[] = {
	{ "usb2512b", 0 },
	{ "usb2512bi", 0 },
	{ "usb2513b", 0 },
	{ "usb2513bi", 0 },
	{ "usb2514b", 0 },
	{ "usb2514bi", 0 },
	{ "usb2517", 0 },
	{ "usb2517i", 0 },
	{ /* sentinel */ }
};

static struct driver usb251xb_i2c_driver = {
	.name = DRIVER_NAME,
	.probe    = usb251xb_i2c_probe,
	.id_table = usb251xb_id,
	.of_compatible = DRV_OF_COMPAT(usb251xb_of_match),
};
device_i2c_driver(usb251xb_i2c_driver);
