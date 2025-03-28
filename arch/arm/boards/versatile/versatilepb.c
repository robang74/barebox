// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2010 B Labs Ltd,
 * http://l4dev.org
 * Author: Alexey Zaytsev <alexey.zaytsev@gmail.com>
 *
 * Based on mach-nomadik
 * Copyright (C) 2009-2010 Jean-Christophe PLAGNIOL-VILLARD <plagnio@jcrosoft.com>
 */

#include <common.h>
#include <init.h>
#include <asm/armlinux.h>
#include <asm/system_info.h>
#include <generated/mach-types.h>
#include <mach/versatile/init.h>
#include <mach/versatile/platform.h>
#include <environment.h>
#include <linux/sizes.h>
#include <platform_data/eth-smc91111.h>

static int vpb_console_init(void)
{
	char *hostname = "versatilepb-unknown";
	char *model = "ARM Versatile PB";

	if (cpu_is_arm926()) {
		hostname = "versatilepb-arm926";
		model = "ARM Versatile PB (arm926)";
	} else if (cpu_is_arm1176()) {
		hostname = "versatilepb-arm1176";
		model = "ARM Versatile PB (arm1176)";
	}

	barebox_set_hostname(hostname);
	barebox_set_model(model);

	versatile_register_uart(0);
	return 0;
}
console_initcall(vpb_console_init);

static struct smc91c111_pdata net_pdata = {
	.qemu_fixup = 1,
};

static int vpb_devices_init(void)
{
	add_cfi_flash_device(DEVICE_ID_DYNAMIC, VERSATILE_FLASH_BASE, VERSATILE_FLASH_SIZE, 0);
	devfs_add_partition("nor0", 0x00000, SZ_512K, DEVFS_PARTITION_FIXED, "self");
	devfs_add_partition("nor0", SZ_512K, SZ_512K, DEVFS_PARTITION_FIXED, "env0");

	add_generic_device("smc91c111", DEVICE_ID_DYNAMIC, NULL, VERSATILE_ETH_BASE,
			64 * 1024, IORESOURCE_MEM, &net_pdata);

	armlinux_set_architecture(MACH_TYPE_VERSATILE_PB);

	return 0;
}
device_initcall(vpb_devices_init);
