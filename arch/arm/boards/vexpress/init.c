/*
 * Copyright (C) 2013 Jean-Christophe PLAGNIOL-VILLARD <plagnio@jcrosoft.com>
 *
 * GPLv2 only
 */

#include <common.h>
#include <init.h>
#include <asm/armlinux.h>
#include <asm/system_info.h>
#include <generated/mach-types.h>
#include <mach/vexpress/devices.h>
#include <environment.h>
#include <linux/sizes.h>
#include <io.h>
#include <envfs.h>
#include <globalvar.h>
#include <linux/amba/sp804.h>

#define V2M_SYS_FLASH	0x03c

static int of_fixup_virtio_mmio(struct device_node *root, void *unused)
{
	struct device_node *barebox_root, *np, *parent;

	barebox_root = of_get_root_node();
	if (root == barebox_root)
		return 0;

	for_each_compatible_node_from(np, barebox_root, NULL, "virtio,mmio") {
		if (of_get_parent(np) == barebox_root)
			parent = root;
		else
			parent = of_find_node_by_path_from(root,
							   of_get_parent(np)->full_name);
		if (!parent)
			return -EINVAL;

		of_copy_node(parent, np);
	}

	return 0;
}

static int vexpress_probe(struct device *dev)
{
	char *hostname = "vexpress-unknown";
	int ret = 0;

	if (amba_is_arm_sp804(IOMEM(0x10011000))) {
		vexpress_a9_legacy_init();
		hostname = "vexpress-a9-legacy";
	} else {
		vexpress_init();
		if (cpu_is_cortex_a5())
			hostname = "vexpress-a5";
		else if (cpu_is_cortex_a7())
			hostname = "vexpress-a7";
		else if (cpu_is_cortex_a9())
			hostname = "vexpress-a9";
		else if (cpu_is_cortex_a15())
			hostname = "vexpress-a15";
	}

	writel(1, v2m_sysreg_base + V2M_SYS_FLASH);

	barebox_set_hostname(hostname);

	ret = of_register_fixup(of_fixup_virtio_mmio, NULL);

	return ret;
}

static const struct of_device_id vexpress_of_match[] = {
	{ .compatible = "arm,vexpress,v2p-ca9" },
	{ .compatible = "arm,vexpress,v2p-ca15" },
	{ .compatible = "arm,vexpress" },
	{ /* Sentinel */},
};

static struct driver vexpress_board_driver = {
	.name = "board-vexpress",
	.probe = vexpress_probe,
	.of_compatible = vexpress_of_match,
};

postcore_platform_driver(vexpress_board_driver);
