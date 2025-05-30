// SPDX-License-Identifier: GPL-2.0
/*
 * core.c - DesignWare USB3 DRD Controller Core file
 *
 * Copyright (C) 2010-2011 Texas Instruments Incorporated - https://www.ti.com
 *
 * Authors: Felipe Balbi <balbi@ti.com>,
 *	    Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 */

#include <common.h>
#include <linux/clk.h>
#include <linux/phy/phy.h>
#include <dma.h>
#include <driver.h>
#include <init.h>
#include <linux/reset.h>

#include "core.h"
#include "gadget.h"
#include "io.h"

#include "debug.h"

#define DWC3_DEFAULT_AUTOSUSPEND_DELAY	5000 /* ms */

/**
 * dwc3_get_dr_mode - Validates and sets dr_mode
 * @dwc: pointer to our context structure
 */
static int dwc3_get_dr_mode(struct dwc3 *dwc)
{
	enum usb_dr_mode mode;
	struct device *dev = dwc->dev;
	unsigned int hw_mode;

	if (dwc->dr_mode == USB_DR_MODE_UNKNOWN)
		dwc->dr_mode = USB_DR_MODE_OTG;

	mode = dwc->dr_mode;
	hw_mode = DWC3_GHWPARAMS0_MODE(dwc->hwparams.hwparams0);

	switch (hw_mode) {
	case DWC3_GHWPARAMS0_MODE_GADGET:
		if (IS_ENABLED(CONFIG_USB_DWC3_HOST)) {
			dev_err(dev,
				"Controller does not support host mode.\n");
			return -EINVAL;
		}
		mode = USB_DR_MODE_PERIPHERAL;
		break;
	case DWC3_GHWPARAMS0_MODE_HOST:
		if (IS_ENABLED(CONFIG_USB_DWC3_GADGET)) {
			dev_err(dev,
				"Controller does not support device mode.\n");
			return -EINVAL;
		}
		mode = USB_DR_MODE_HOST;
		break;
	default:
		if (IS_ENABLED(CONFIG_USB_DWC3_HOST))
			mode = USB_DR_MODE_HOST;
		else if (IS_ENABLED(CONFIG_USB_DWC3_GADGET))
			mode = USB_DR_MODE_PERIPHERAL;

		/*
		 * DWC_usb31 and DWC_usb3 v3.30a and higher do not support OTG
		 * mode. If the controller supports DRD but the dr_mode is not
		 * specified or set to OTG, then set the mode to peripheral.
		 */
		if (mode == USB_DR_MODE_OTG && !dwc->edev &&
		    (!IS_ENABLED(CONFIG_USB_ROLE_SWITCH) ||
		     !of_property_read_bool(dwc->dev->of_node, "usb-role-switch")) &&
		    !DWC3_VER_IS_PRIOR(DWC3, 330A))
			mode = USB_DR_MODE_PERIPHERAL;
	}

	if (mode != dwc->dr_mode) {
		dev_warn(dev,
			 "Configuration mismatch. dr_mode forced to %s\n",
			 mode == USB_DR_MODE_HOST ? "host" : "gadget");

		dwc->dr_mode = mode;
	}

	return 0;
}

void dwc3_set_prtcap(struct dwc3 *dwc, u32 mode)
{
	u32 reg;

	reg = dwc3_readl(dwc->regs, DWC3_GCTL);
	reg &= ~(DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_OTG));
	reg |= DWC3_GCTL_PRTCAPDIR(mode);
	dwc3_writel(dwc->regs, DWC3_GCTL, reg);

	dwc->current_dr_role = mode;
}

u32 dwc3_core_fifo_space(struct dwc3_ep *dep, u8 type)
{
	struct dwc3		*dwc = dep->dwc;
	u32			reg;

	dwc3_writel(dwc->regs, DWC3_GDBGFIFOSPACE,
			DWC3_GDBGFIFOSPACE_NUM(dep->number) |
			DWC3_GDBGFIFOSPACE_TYPE(type));

	reg = dwc3_readl(dwc->regs, DWC3_GDBGFIFOSPACE);

	return DWC3_GDBGFIFOSPACE_SPACE_AVAILABLE(reg);
}

/**
 * dwc3_core_soft_reset - Issues core soft reset and PHY reset
 * @dwc: pointer to our context structure
 */
int dwc3_core_soft_reset(struct dwc3 *dwc)
{
	u32		reg;
	int		retries = 1000;

	/*
	 * We're resetting only the device side because, if we're in host mode,
	 * XHCI driver will reset the host block. If dwc3 was configured for
	 * host-only mode, then we can return early.
	 */
	if (dwc->current_dr_role == DWC3_GCTL_PRTCAP_HOST)
		return 0;

	reg = dwc3_readl(dwc->regs, DWC3_DCTL);
	reg |= DWC3_DCTL_CSFTRST;
	reg &= ~DWC3_DCTL_RUN_STOP;
	dwc3_gadget_dctl_write_safe(dwc, reg);

	/*
	 * For DWC_usb31 controller 1.90a and later, the DCTL.CSFRST bit
	 * is cleared only after all the clocks are synchronized. This can
	 * take a little more than 50ms. Set the polling rate at 20ms
	 * for 10 times instead.
	 */
	if (DWC3_VER_IS_WITHIN(DWC31, 190A, ANY) || DWC3_IP_IS(DWC32))
		retries = 10;

	do {
		reg = dwc3_readl(dwc->regs, DWC3_DCTL);
		if (!(reg & DWC3_DCTL_CSFTRST))
			goto done;

		if (DWC3_VER_IS_WITHIN(DWC31, 190A, ANY) || DWC3_IP_IS(DWC32))
			mdelay(20);
		else
			udelay(1);
	} while (--retries);

	dev_warn(dwc->dev, "DWC3 controller soft reset failed.\n");
	return -ETIMEDOUT;

done:
	/*
	 * For DWC_usb31 controller 1.80a and prior, once DCTL.CSFRST bit
	 * is cleared, we must wait at least 50ms before accessing the PHY
	 * domain (synchronization delay).
	 */
	if (DWC3_VER_IS_WITHIN(DWC31, ANY, 180A))
		mdelay(50);

	return 0;
}

/*
 * dwc3_frame_length_adjustment - Adjusts frame length if required
 * @dwc3: Pointer to our controller context structure
 */
static void dwc3_frame_length_adjustment(struct dwc3 *dwc)
{
	u32 reg;
	u32 dft;

	if (DWC3_VER_IS_PRIOR(DWC3, 250A))
		return;

	if (dwc->fladj == 0)
		return;

	reg = dwc3_readl(dwc->regs, DWC3_GFLADJ);
	dft = reg & DWC3_GFLADJ_30MHZ_MASK;
	if (dft != dwc->fladj) {
		reg &= ~DWC3_GFLADJ_30MHZ_MASK;
		reg |= DWC3_GFLADJ_30MHZ_SDBND_SEL | dwc->fladj;
		dwc3_writel(dwc->regs, DWC3_GFLADJ, reg);
	}
}

/**
 * dwc3_ref_clk_period - Reference clock period configuration
 *		Default reference clock period depends on hardware
 *		configuration. For systems with reference clock that differs
 *		from the default, this will set clock period in DWC3_GUCTL
 *		register.
 * @dwc: Pointer to our controller context structure
 */
static void dwc3_ref_clk_period(struct dwc3 *dwc)
{
	u32 reg;
	u32 dft;

	if (DWC3_VER_IS_PRIOR(DWC3, 250A))
		return;

	if (dwc->fladj == 0)
		return;

	reg = dwc3_readl(dwc->regs, DWC3_GFLADJ);
	dft = reg & DWC3_GFLADJ_30MHZ_MASK;
	reg &= ~DWC3_GFLADJ_30MHZ_MASK;
	reg |= DWC3_GFLADJ_30MHZ_SDBND_SEL | dwc->fladj;
	dwc3_writel(dwc->regs, DWC3_GFLADJ, reg);
}

/**
 * dwc3_free_one_event_buffer - Frees one event buffer
 * @dwc: Pointer to our controller context structure
 * @evt: Pointer to event buffer to be freed
 */
static void dwc3_free_one_event_buffer(struct dwc3 *dwc,
		struct dwc3_event_buffer *evt)
{
	dma_free_coherent(evt->buf, evt->dma, evt->length);
}

/**
 * dwc3_alloc_one_event_buffer - Allocates one event buffer structure
 * @dwc: Pointer to our controller context structure
 * @length: size of the event buffer
 *
 * Returns a pointer to the allocated event buffer structure on success
 * otherwise ERR_PTR(errno).
 */
static struct dwc3_event_buffer *dwc3_alloc_one_event_buffer(struct dwc3 *dwc,
		unsigned int length)
{
	struct dwc3_event_buffer	*evt;

	evt = kzalloc(sizeof(*evt), GFP_KERNEL);
	if (!evt)
		return ERR_PTR(-ENOMEM);

	evt->dwc	= dwc;
	evt->length	= length;
	evt->cache	= kzalloc(length, GFP_KERNEL);
	if (!evt->cache)
		return ERR_PTR(-ENOMEM);

	evt->buf	= dma_alloc_coherent(length, &evt->dma);
	if (!evt->buf)
		return ERR_PTR(-ENOMEM);

	return evt;
}

/**
 * dwc3_free_event_buffers - frees all allocated event buffers
 * @dwc: Pointer to our controller context structure
 */
static void dwc3_free_event_buffers(struct dwc3 *dwc)
{
	struct dwc3_event_buffer	*evt;

	evt = dwc->ev_buf;
	if (evt)
		dwc3_free_one_event_buffer(dwc, evt);
}

/**
 * dwc3_alloc_event_buffers - Allocates @num event buffers of size @length
 * @dwc: pointer to our controller context structure
 * @length: size of event buffer
 *
 * Returns 0 on success otherwise negative errno. In the error case, dwc
 * may contain some buffers allocated but not all which were requested.
 */
static int dwc3_alloc_event_buffers(struct dwc3 *dwc, unsigned int length)
{
	struct dwc3_event_buffer *evt;

	evt = dwc3_alloc_one_event_buffer(dwc, length);
	if (IS_ERR(evt)) {
		dev_err(dwc->dev, "can't allocate event buffer\n");
		return PTR_ERR(evt);
	}
	dwc->ev_buf = evt;

	return 0;
}

/**
 * dwc3_event_buffers_setup - setup our allocated event buffers
 * @dwc: pointer to our controller context structure
 *
 * Returns 0 on success otherwise negative errno.
 */
int dwc3_event_buffers_setup(struct dwc3 *dwc)
{
	struct dwc3_event_buffer	*evt;

	evt = dwc->ev_buf;
	evt->lpos = 0;
	dwc3_writel(dwc->regs, DWC3_GEVNTADRLO(0),
			lower_32_bits(evt->dma));
	dwc3_writel(dwc->regs, DWC3_GEVNTADRHI(0),
			upper_32_bits(evt->dma));
	dwc3_writel(dwc->regs, DWC3_GEVNTSIZ(0),
			DWC3_GEVNTSIZ_SIZE(evt->length));
	dwc3_writel(dwc->regs, DWC3_GEVNTCOUNT(0), 0);

	return 0;
}

void dwc3_event_buffers_cleanup(struct dwc3 *dwc)
{
	struct dwc3_event_buffer	*evt;

	evt = dwc->ev_buf;

	evt->lpos = 0;

	dwc3_writel(dwc->regs, DWC3_GEVNTADRLO(0), 0);
	dwc3_writel(dwc->regs, DWC3_GEVNTADRHI(0), 0);
	dwc3_writel(dwc->regs, DWC3_GEVNTSIZ(0), DWC3_GEVNTSIZ_INTMASK
			| DWC3_GEVNTSIZ_SIZE(0));
	dwc3_writel(dwc->regs, DWC3_GEVNTCOUNT(0), 0);
}

static int dwc3_alloc_scratch_buffers(struct dwc3 *dwc)
{
	if (!dwc->has_hibernation)
		return 0;

	if (!dwc->nr_scratch)
		return 0;

	dwc->scratchbuf = kmalloc_array(dwc->nr_scratch,
			DWC3_SCRATCHBUF_SIZE, GFP_KERNEL);
	if (!dwc->scratchbuf)
		return -ENOMEM;

	return 0;
}

static int dwc3_setup_scratch_buffers(struct dwc3 *dwc)
{
	dma_addr_t scratch_addr;
	u32 param;
	int ret;

	if (!dwc->has_hibernation)
		return 0;

	if (!dwc->nr_scratch)
		return 0;

	 /* should never fall here */
	if (!WARN_ON(dwc->scratchbuf))
		return 0;

	scratch_addr = dma_map_single(dwc->sysdev, dwc->scratchbuf,
			dwc->nr_scratch * DWC3_SCRATCHBUF_SIZE,
			DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dwc->sysdev, scratch_addr)) {
		dev_err(dwc->sysdev, "failed to map scratch buffer\n");
		ret = -EFAULT;
		goto err0;
	}

	dwc->scratch_addr = scratch_addr;

	param = lower_32_bits(scratch_addr);

	ret = dwc3_send_gadget_generic_command(dwc,
			DWC3_DGCMD_SET_SCRATCHPAD_ADDR_LO, param);
	if (ret < 0)
		goto err1;

	param = upper_32_bits(scratch_addr);

	ret = dwc3_send_gadget_generic_command(dwc,
			DWC3_DGCMD_SET_SCRATCHPAD_ADDR_HI, param);
	if (ret < 0)
		goto err1;

	return 0;

err1:
	dma_unmap_single(dwc->sysdev, dwc->scratch_addr, dwc->nr_scratch *
			DWC3_SCRATCHBUF_SIZE, DMA_BIDIRECTIONAL);

err0:
	return ret;
}

static void dwc3_free_scratch_buffers(struct dwc3 *dwc)
{
	if (!dwc->has_hibernation)
		return;

	if (!dwc->nr_scratch)
		return;

	 /* should never fall here */
	if (!WARN_ON(dwc->scratchbuf))
		return;

	dma_unmap_single(dwc->sysdev, dwc->scratch_addr, dwc->nr_scratch *
			DWC3_SCRATCHBUF_SIZE, DMA_BIDIRECTIONAL);
	kfree(dwc->scratchbuf);
}

static void dwc3_core_num_eps(struct dwc3 *dwc)
{
	struct dwc3_hwparams	*parms = &dwc->hwparams;

	dwc->num_eps = DWC3_NUM_EPS(parms);
}

static void dwc3_cache_hwparams(struct dwc3 *dwc)
{
	struct dwc3_hwparams	*parms = &dwc->hwparams;

	parms->hwparams0 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS0);
	parms->hwparams1 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS1);
	parms->hwparams2 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS2);
	parms->hwparams3 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS3);
	parms->hwparams4 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS4);
	parms->hwparams5 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS5);
	parms->hwparams6 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS6);
	parms->hwparams7 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS7);
	parms->hwparams8 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS8);

	if (DWC3_IP_IS(DWC32))
		parms->hwparams9 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS9);
}

static int dwc3_core_ulpi_init(struct dwc3 *dwc)
{
	int intf;
	int ret = 0;

	intf = DWC3_GHWPARAMS3_HSPHY_IFC(dwc->hwparams.hwparams3);

	if (intf == DWC3_GHWPARAMS3_HSPHY_IFC_ULPI ||
	    (intf == DWC3_GHWPARAMS3_HSPHY_IFC_UTMI_ULPI &&
	     dwc->hsphy_interface &&
	     !strncmp(dwc->hsphy_interface, "ulpi", 4)))
		ret = dwc3_ulpi_init(dwc);

	return ret;
}

/**
 * dwc3_phy_setup - Configure USB PHY Interface of DWC3 Core
 * @dwc: Pointer to our controller context structure
 *
 * Returns 0 on success. The USB PHY interfaces are configured but not
 * initialized. The PHY interfaces and the PHYs get initialized together with
 * the core in dwc3_core_init.
 */
static int dwc3_phy_setup(struct dwc3 *dwc)
{
	unsigned int hw_mode;
	u32 reg;

	hw_mode = DWC3_GHWPARAMS0_MODE(dwc->hwparams.hwparams0);

	reg = dwc3_readl(dwc->regs, DWC3_GUSB3PIPECTL(0));

	/*
	 * Make sure UX_EXIT_PX is cleared as that causes issues with some
	 * PHYs. Also, this bit is not supposed to be used in normal operation.
	 */
	reg &= ~DWC3_GUSB3PIPECTL_UX_EXIT_PX;

	/*
	 * Above 1.94a, it is recommended to set DWC3_GUSB3PIPECTL_SUSPHY
	 * to '0' during coreConsultant configuration. So default value
	 * will be '0' when the core is reset. Application needs to set it
	 * to '1' after the core initialization is completed.
	 */
	if (!DWC3_VER_IS_WITHIN(DWC3, ANY, 194A))
		reg |= DWC3_GUSB3PIPECTL_SUSPHY;

	/*
	 * For DRD controllers, GUSB3PIPECTL.SUSPENDENABLE must be cleared after
	 * power-on reset, and it can be set after core initialization, which is
	 * after device soft-reset during initialization.
	 */
	if (hw_mode == DWC3_GHWPARAMS0_MODE_DRD)
		reg &= ~DWC3_GUSB3PIPECTL_SUSPHY;

	if (dwc->u2ss_inp3_quirk)
		reg |= DWC3_GUSB3PIPECTL_U2SSINP3OK;

	if (dwc->dis_rxdet_inp3_quirk)
		reg |= DWC3_GUSB3PIPECTL_DISRXDETINP3;

	if (dwc->req_p1p2p3_quirk)
		reg |= DWC3_GUSB3PIPECTL_REQP1P2P3;

	if (dwc->del_p1p2p3_quirk)
		reg |= DWC3_GUSB3PIPECTL_DEP1P2P3_EN;

	if (dwc->del_phy_power_chg_quirk)
		reg |= DWC3_GUSB3PIPECTL_DEPOCHANGE;

	if (dwc->lfps_filter_quirk)
		reg |= DWC3_GUSB3PIPECTL_LFPSFILT;

	if (dwc->rx_detect_poll_quirk)
		reg |= DWC3_GUSB3PIPECTL_RX_DETOPOLL;

	if (dwc->tx_de_emphasis_quirk)
		reg |= DWC3_GUSB3PIPECTL_TX_DEEPH(dwc->tx_de_emphasis);

	if (dwc->dis_u3_susphy_quirk)
		reg &= ~DWC3_GUSB3PIPECTL_SUSPHY;

	if (dwc->dis_del_phy_power_chg_quirk)
		reg &= ~DWC3_GUSB3PIPECTL_DEPOCHANGE;

	dwc3_writel(dwc->regs, DWC3_GUSB3PIPECTL(0), reg);

	reg = dwc3_readl(dwc->regs, DWC3_GUSB2PHYCFG(0));

	/* Select the HS PHY interface */
	switch (DWC3_GHWPARAMS3_HSPHY_IFC(dwc->hwparams.hwparams3)) {
	case DWC3_GHWPARAMS3_HSPHY_IFC_UTMI_ULPI:
		if (dwc->hsphy_interface &&
				!strncmp(dwc->hsphy_interface, "utmi", 4)) {
			reg &= ~DWC3_GUSB2PHYCFG_ULPI_UTMI;
			break;
		} else if (dwc->hsphy_interface &&
				!strncmp(dwc->hsphy_interface, "ulpi", 4)) {
			reg |= DWC3_GUSB2PHYCFG_ULPI_UTMI;
			dwc3_writel(dwc->regs, DWC3_GUSB2PHYCFG(0), reg);
		} else {
			/* Relying on default value. */
			if (!(reg & DWC3_GUSB2PHYCFG_ULPI_UTMI))
				break;
		}
		fallthrough;
	case DWC3_GHWPARAMS3_HSPHY_IFC_ULPI:
	default:
		break;
	}

	switch (dwc->hsphy_mode) {
	case USBPHY_INTERFACE_MODE_UTMI:
		reg &= ~(DWC3_GUSB2PHYCFG_PHYIF_MASK |
		       DWC3_GUSB2PHYCFG_USBTRDTIM_MASK);
		reg |= DWC3_GUSB2PHYCFG_PHYIF(UTMI_PHYIF_8_BIT) |
		       DWC3_GUSB2PHYCFG_USBTRDTIM(USBTRDTIM_UTMI_8_BIT);
		break;
	case USBPHY_INTERFACE_MODE_UTMIW:
		reg &= ~(DWC3_GUSB2PHYCFG_PHYIF_MASK |
		       DWC3_GUSB2PHYCFG_USBTRDTIM_MASK);
		reg |= DWC3_GUSB2PHYCFG_PHYIF(UTMI_PHYIF_16_BIT) |
		       DWC3_GUSB2PHYCFG_USBTRDTIM(USBTRDTIM_UTMI_16_BIT);
		break;
	default:
		break;
	}

	/*
	 * Above 1.94a, it is recommended to set DWC3_GUSB2PHYCFG_SUSPHY to
	 * '0' during coreConsultant configuration. So default value will
	 * be '0' when the core is reset. Application needs to set it to
	 * '1' after the core initialization is completed.
	 */
	if (!DWC3_VER_IS_WITHIN(DWC3, ANY, 194A))
		reg |= DWC3_GUSB2PHYCFG_SUSPHY;

	/*
	 * For DRD controllers, GUSB2PHYCFG.SUSPHY must be cleared after
	 * power-on reset, and it can be set after core initialization, which is
	 * after device soft-reset during initialization.
	 */
	if (hw_mode == DWC3_GHWPARAMS0_MODE_DRD)
		reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;

	if (dwc->dis_u2_susphy_quirk)
		reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;

	if (dwc->dis_enblslpm_quirk)
		reg &= ~DWC3_GUSB2PHYCFG_ENBLSLPM;
	else
		reg |= DWC3_GUSB2PHYCFG_ENBLSLPM;

	if (dwc->dis_u2_freeclk_exists_quirk || dwc->gfladj_refclk_lpm_sel)
		reg &= ~DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS;

	dwc3_writel(dwc->regs, DWC3_GUSB2PHYCFG(0), reg);

	return 0;
}

static void dwc3_core_exit(struct dwc3 *dwc)
{
	dwc3_event_buffers_cleanup(dwc);

	usb_phy_set_suspend(dwc->usb2_phy, 1);
	usb_phy_set_suspend(dwc->usb3_phy, 1);
	phy_power_off(dwc->usb2_generic_phy);
	phy_power_off(dwc->usb3_generic_phy);

	usb_phy_shutdown(dwc->usb2_phy);
	usb_phy_shutdown(dwc->usb3_phy);
	phy_exit(dwc->usb2_generic_phy);
	phy_exit(dwc->usb3_generic_phy);

	reset_control_assert(dwc->reset);
}

static bool dwc3_core_is_valid(struct dwc3 *dwc)
{
	u32 reg;

	reg = dwc3_readl(dwc->regs, DWC3_GSNPSID);
	dwc->ip = DWC3_GSNPS_ID(reg);

	/* This should read as U3 followed by revision number */
	if (DWC3_IP_IS(DWC3)) {
		dwc->revision = reg;
	} else if (DWC3_IP_IS(DWC31) || DWC3_IP_IS(DWC32)) {
		dwc->revision = dwc3_readl(dwc->regs, DWC3_VER_NUMBER);
		dwc->version_type = dwc3_readl(dwc->regs, DWC3_VER_TYPE);
	} else {
		return false;
	}

	return true;
}

static void dwc3_core_setup_global_control(struct dwc3 *dwc)
{
	u32 hwparams4 = dwc->hwparams.hwparams4;
	u32 reg;

	reg = dwc3_readl(dwc->regs, DWC3_GCTL);
	reg &= ~DWC3_GCTL_SCALEDOWN_MASK;

	switch (DWC3_GHWPARAMS1_EN_PWROPT(dwc->hwparams.hwparams1)) {
	case DWC3_GHWPARAMS1_EN_PWROPT_CLK:
		/**
		 * WORKAROUND: DWC3 revisions between 2.10a and 2.50a have an
		 * issue which would cause xHCI compliance tests to fail.
		 *
		 * Because of that we cannot enable clock gating on such
		 * configurations.
		 *
		 * Refers to:
		 *
		 * STAR#9000588375: Clock Gating, SOF Issues when ref_clk-Based
		 * SOF/ITP Mode Used
		 */
		if ((dwc->dr_mode == USB_DR_MODE_HOST ||
				dwc->dr_mode == USB_DR_MODE_OTG) &&
				DWC3_VER_IS_WITHIN(DWC3, 210A, 250A))
			reg |= DWC3_GCTL_DSBLCLKGTNG | DWC3_GCTL_SOFITPSYNC;
		else
			reg &= ~DWC3_GCTL_DSBLCLKGTNG;
		break;
	case DWC3_GHWPARAMS1_EN_PWROPT_HIB:
		/* enable hibernation here */
		dwc->nr_scratch = DWC3_GHWPARAMS4_HIBER_SCRATCHBUFS(hwparams4);

		/*
		 * REVISIT Enabling this bit so that host-mode hibernation
		 * will work. Device-mode hibernation is not yet implemented.
		 */
		reg |= DWC3_GCTL_GBLHIBERNATIONEN;
		break;
	default:
		/* nothing */
		break;
	}

	/* check if current dwc3 is on simulation board */
	if (dwc->hwparams.hwparams6 & DWC3_GHWPARAMS6_EN_FPGA) {
		dev_info(dwc->dev, "Running with FPGA optimizations\n");
		dwc->is_fpga = true;
	}

	WARN_ONCE(dwc->disable_scramble_quirk && !dwc->is_fpga,
			"disable_scramble cannot be used on non-FPGA builds\n");

	if (dwc->disable_scramble_quirk && dwc->is_fpga)
		reg |= DWC3_GCTL_DISSCRAMBLE;
	else
		reg &= ~DWC3_GCTL_DISSCRAMBLE;

	if (dwc->u2exit_lfps_quirk)
		reg |= DWC3_GCTL_U2EXIT_LFPS;

	/*
	 * WORKAROUND: DWC3 revisions <1.90a have a bug
	 * where the device can fail to connect at SuperSpeed
	 * and falls back to high-speed mode which causes
	 * the device to enter a Connect/Disconnect loop
	 */
	if (DWC3_VER_IS_PRIOR(DWC3, 190A))
		reg |= DWC3_GCTL_U2RSTECN;

	dwc3_writel(dwc->regs, DWC3_GCTL, reg);
}

static int dwc3_core_get_phy(struct dwc3 *dwc);
static int dwc3_core_ulpi_init(struct dwc3 *dwc);

/* set global incr burst type configuration registers */
static void dwc3_set_incr_burst_type(struct dwc3 *dwc)
{
	struct device *dev = dwc->dev;
	/* incrx_mode : for INCR burst type. */
	bool incrx_mode;
	/* incrx_size : for size of INCRX burst. */
	u32 incrx_size;
	u32 *vals;
	u32 cfg;
	int ntype;
	int ret;
	int i;

	cfg = dwc3_readl(dwc->regs, DWC3_GSBUSCFG0);

	/*
	 * Handle property "snps,incr-burst-type-adjustment".
	 * Get the number of value from this property:
	 * result <= 0, means this property is not supported.
	 * result = 1, means INCRx burst mode supported.
	 * result > 1, means undefined length burst mode supported.
	 */
	ntype = of_property_count_elems_of_size(dev->of_node,
						"snps,incr-burst-type-adjustment",
						sizeof(u32));
	if (ntype <= 0)
		return;

	vals = kcalloc(ntype, sizeof(u32), GFP_KERNEL);
	if (!vals)
		return;

	/* Get INCR burst type, and parse it */
	ret = of_property_read_u32_array(dev->of_node,
			"snps,incr-burst-type-adjustment", vals, ntype);
	if (ret) {
		kfree(vals);
		dev_err(dev, "Error to get property\n");
		return;
	}

	incrx_size = *vals;

	if (ntype > 1) {
		/* INCRX (undefined length) burst mode */
		incrx_mode = INCRX_UNDEF_LENGTH_BURST_MODE;
		for (i = 1; i < ntype; i++) {
			if (vals[i] > incrx_size)
				incrx_size = vals[i];
		}
	} else {
		/* INCRX burst mode */
		incrx_mode = INCRX_BURST_MODE;
	}

	kfree(vals);

	/* Enable Undefined Length INCR Burst and Enable INCRx Burst */
	cfg &= ~DWC3_GSBUSCFG0_INCRBRST_MASK;
	if (incrx_mode)
		cfg |= DWC3_GSBUSCFG0_INCRBRSTENA;
	switch (incrx_size) {
	case 256:
		cfg |= DWC3_GSBUSCFG0_INCR256BRSTENA;
		break;
	case 128:
		cfg |= DWC3_GSBUSCFG0_INCR128BRSTENA;
		break;
	case 64:
		cfg |= DWC3_GSBUSCFG0_INCR64BRSTENA;
		break;
	case 32:
		cfg |= DWC3_GSBUSCFG0_INCR32BRSTENA;
		break;
	case 16:
		cfg |= DWC3_GSBUSCFG0_INCR16BRSTENA;
		break;
	case 8:
		cfg |= DWC3_GSBUSCFG0_INCR8BRSTENA;
		break;
	case 4:
		cfg |= DWC3_GSBUSCFG0_INCR4BRSTENA;
		break;
	case 1:
		break;
	default:
		dev_err(dev, "Invalid property\n");
		break;
	}

	dwc3_writel(dwc->regs, DWC3_GSBUSCFG0, cfg);
}

/**
 * dwc3_core_init - Low-level initialization of DWC3 Core
 * @dwc: Pointer to our controller context structure
 *
 * Returns 0 on success otherwise negative errno.
 */
static int dwc3_core_init(struct dwc3 *dwc)
{
	unsigned int		hw_mode;
	u32			reg;
	int			ret;

	hw_mode = DWC3_GHWPARAMS0_MODE(dwc->hwparams.hwparams0);

	/*
	 * Write Linux Version Code to our GUID register so it's easy to figure
	 * out which kernel version a bug was found.
	 */
	dwc3_writel(dwc->regs, DWC3_GUID, 0xdeadbeef);

	ret = dwc3_phy_setup(dwc);
	if (ret)
		goto err0;

	if (!dwc->ulpi_ready) {
		ret = dwc3_core_ulpi_init(dwc);
		if (ret) {
			if (ret == -ETIMEDOUT) {
				dwc3_core_soft_reset(dwc);
				ret = -EPROBE_DEFER;
			}
			goto err0;
		}
		dwc->ulpi_ready = true;
	}

	if (!dwc->phys_ready) {
		ret = dwc3_core_get_phy(dwc);
		if (ret)
			goto err0a;
		dwc->phys_ready = true;
	}

	usb_phy_init(dwc->usb2_phy);
	usb_phy_init(dwc->usb3_phy);
	ret = phy_init(dwc->usb2_generic_phy);
	if (ret < 0)
		goto err0a;

	ret = phy_init(dwc->usb3_generic_phy);
	if (ret < 0) {
		phy_exit(dwc->usb2_generic_phy);
		goto err0a;
	}

	ret = dwc3_core_soft_reset(dwc);
	if (ret)
		goto err1;

	if (hw_mode == DWC3_GHWPARAMS0_MODE_DRD &&
	    !DWC3_VER_IS_WITHIN(DWC3, ANY, 194A)) {
		if (!dwc->dis_u3_susphy_quirk) {
			reg = dwc3_readl(dwc->regs, DWC3_GUSB3PIPECTL(0));
			reg |= DWC3_GUSB3PIPECTL_SUSPHY;
			dwc3_writel(dwc->regs, DWC3_GUSB3PIPECTL(0), reg);
		}

		if (!dwc->dis_u2_susphy_quirk) {
			reg = dwc3_readl(dwc->regs, DWC3_GUSB2PHYCFG(0));
			reg |= DWC3_GUSB2PHYCFG_SUSPHY;
			dwc3_writel(dwc->regs, DWC3_GUSB2PHYCFG(0), reg);
		}
	}

	dwc3_core_setup_global_control(dwc);
	dwc3_core_num_eps(dwc);

	ret = dwc3_setup_scratch_buffers(dwc);
	if (ret)
		goto err1;

	/* Adjust Frame Length */
	dwc3_frame_length_adjustment(dwc);

	/* Adjust Reference Clock Period */
	dwc3_ref_clk_period(dwc);

	dwc3_set_incr_burst_type(dwc);

	usb_phy_set_suspend(dwc->usb2_phy, 0);
	usb_phy_set_suspend(dwc->usb3_phy, 0);
	ret = phy_power_on(dwc->usb2_generic_phy);
	if (ret < 0)
		goto err2;

	ret = phy_power_on(dwc->usb3_generic_phy);
	if (ret < 0)
		goto err3;

	ret = dwc3_event_buffers_setup(dwc);
	if (ret) {
		dev_err(dwc->dev, "failed to setup event buffers\n");
		goto err4;
	}

	/*
	 * ENDXFER polling is available on version 3.10a and later of
	 * the DWC_usb3 controller. It is NOT available in the
	 * DWC_usb31 controller.
	 */
	if (DWC3_VER_IS_WITHIN(DWC3, 310A, ANY)) {
		reg = dwc3_readl(dwc->regs, DWC3_GUCTL2);
		reg |= DWC3_GUCTL2_RST_ACTBITLATER;
		dwc3_writel(dwc->regs, DWC3_GUCTL2, reg);
	}

	/*
	 * When configured in HOST mode, after issuing U3/L2 exit controller
	 * fails to send proper CRC checksum in CRC5 feild. Because of this
	 * behaviour Transaction Error is generated, resulting in reset and
	 * re-enumeration of usb device attached. All the termsel, xcvrsel,
	 * opmode becomes 0 during end of resume. Enabling bit 10 of GUCTL1
	 * will correct this problem. This option is to support certain
	 * legacy ULPI PHYs.
	 */
	if (dwc->resume_hs_terminations) {
		reg = dwc3_readl(dwc->regs, DWC3_GUCTL1);
		reg |= DWC3_GUCTL1_RESUME_OPMODE_HS_HOST;
		dwc3_writel(dwc->regs, DWC3_GUCTL1, reg);
	}

	if (!DWC3_VER_IS_PRIOR(DWC3, 250A)) {
		reg = dwc3_readl(dwc->regs, DWC3_GUCTL1);

		/*
		 * Enable hardware control of sending remote wakeup
		 * in HS when the device is in the L1 state.
		 */
		if (!DWC3_VER_IS_PRIOR(DWC3, 290A))
			reg |= DWC3_GUCTL1_DEV_L1_EXIT_BY_HW;

		/*
		 * Decouple USB 2.0 L1 & L2 events which will allow for
		 * gadget driver to only receive U3/L2 suspend & wakeup
		 * events and prevent the more frequent L1 LPM transitions
		 * from interrupting the driver.
		 */
		if (!DWC3_VER_IS_PRIOR(DWC3, 300A))
			reg |= DWC3_GUCTL1_DEV_DECOUPLE_L1L2_EVT;

		if (dwc->dis_tx_ipgap_linecheck_quirk)
			reg |= DWC3_GUCTL1_TX_IPGAP_LINECHECK_DIS;

		if (dwc->parkmode_disable_ss_quirk)
			reg |= DWC3_GUCTL1_PARKMODE_DISABLE_SS;

		if (DWC3_VER_IS_WITHIN(DWC3, 290A, ANY) &&
		    (dwc->maximum_speed == USB_SPEED_HIGH ||
		     dwc->maximum_speed == USB_SPEED_FULL))
			reg |= DWC3_GUCTL1_DEV_FORCE_20_CLK_FOR_30_CLK;

		dwc3_writel(dwc->regs, DWC3_GUCTL1, reg);
	}

	if (dwc->dr_mode == USB_DR_MODE_HOST ||
	    dwc->dr_mode == USB_DR_MODE_OTG) {
		reg = dwc3_readl(dwc->regs, DWC3_GUCTL);

		/*
		 * Enable Auto retry Feature to make the controller operating in
		 * Host mode on seeing transaction errors(CRC errors or internal
		 * overrun scenerios) on IN transfers to reply to the device
		 * with a non-terminating retry ACK (i.e, an ACK transcation
		 * packet with Retry=1 & Nump != 0)
		 */
		reg |= DWC3_GUCTL_HSTINAUTORETRY;

		dwc3_writel(dwc->regs, DWC3_GUCTL, reg);
	}

	/*
	 * Must config both number of packets and max burst settings to enable
	 * RX and/or TX threshold.
	 */
	if (!DWC3_IP_IS(DWC3) && dwc->dr_mode == USB_DR_MODE_HOST) {
		u8 rx_thr_num = dwc->rx_thr_num_pkt_prd;
		u8 rx_maxburst = dwc->rx_max_burst_prd;
		u8 tx_thr_num = dwc->tx_thr_num_pkt_prd;
		u8 tx_maxburst = dwc->tx_max_burst_prd;

		if (rx_thr_num && rx_maxburst) {
			reg = dwc3_readl(dwc->regs, DWC3_GRXTHRCFG);
			reg |= DWC31_RXTHRNUMPKTSEL_PRD;

			reg &= ~DWC31_RXTHRNUMPKT_PRD(~0);
			reg |= DWC31_RXTHRNUMPKT_PRD(rx_thr_num);

			reg &= ~DWC31_MAXRXBURSTSIZE_PRD(~0);
			reg |= DWC31_MAXRXBURSTSIZE_PRD(rx_maxburst);

			dwc3_writel(dwc->regs, DWC3_GRXTHRCFG, reg);
		}

		if (tx_thr_num && tx_maxburst) {
			reg = dwc3_readl(dwc->regs, DWC3_GTXTHRCFG);
			reg |= DWC31_TXTHRNUMPKTSEL_PRD;

			reg &= ~DWC31_TXTHRNUMPKT_PRD(~0);
			reg |= DWC31_TXTHRNUMPKT_PRD(tx_thr_num);

			reg &= ~DWC31_MAXTXBURSTSIZE_PRD(~0);
			reg |= DWC31_MAXTXBURSTSIZE_PRD(tx_maxburst);

			dwc3_writel(dwc->regs, DWC3_GTXTHRCFG, reg);
		}
	}

	return 0;

err4:
	phy_power_off(dwc->usb3_generic_phy);

err3:
	phy_power_off(dwc->usb2_generic_phy);

err2:
	usb_phy_set_suspend(dwc->usb2_phy, 1);
	usb_phy_set_suspend(dwc->usb3_phy, 1);

err1:
	usb_phy_shutdown(dwc->usb2_phy);
	usb_phy_shutdown(dwc->usb3_phy);
	phy_exit(dwc->usb2_generic_phy);
	phy_exit(dwc->usb3_generic_phy);

err0a:
	dwc3_ulpi_exit(dwc);

err0:
	return ret;
}

static int dwc3_core_get_phy(struct dwc3 *dwc)
{
	struct device		*dev = dwc->dev;
	int ret;

	dwc->usb2_generic_phy = phy_get(dev, "usb2-phy");
	if (IS_ERR(dwc->usb2_generic_phy)) {
		ret = PTR_ERR(dwc->usb2_generic_phy);
		if (ret == -ENOSYS || ret == -ENODEV)
			dwc->usb2_generic_phy = NULL;
		else
			return dev_err_probe(dev, ret, "no usb2 phy configured\n");
	}

	dwc->usb3_generic_phy = phy_get(dev, "usb3-phy");
	if (IS_ERR(dwc->usb3_generic_phy)) {
		ret = PTR_ERR(dwc->usb3_generic_phy);
		if (ret == -ENOSYS || ret == -ENODEV)
			dwc->usb3_generic_phy = NULL;
		else
			return dev_err_probe(dev, ret, "no usb3 phy configured\n");
	}

	return 0;
}

static int dwc3_set_dr_mode(void *ctx, enum usb_dr_mode mode)
{
	struct dwc3 *dwc = ctx;
	struct device *dev = dwc->dev;
	int ret;

	switch (mode) {
	case USB_DR_MODE_PERIPHERAL:
		dwc3_set_prtcap(dwc, DWC3_GCTL_PRTCAP_DEVICE);

//		if (dwc->usb2_phy)
//			otg_set_vbus(dwc->usb2_phy->otg, false);
		phy_set_mode(dwc->usb2_generic_phy, PHY_MODE_USB_DEVICE);
		phy_set_mode(dwc->usb3_generic_phy, PHY_MODE_USB_DEVICE);

		ret = dwc3_gadget_init(dwc);
		if (ret)
			return dev_err_probe(dev, ret, "failed to initialize gadget\n");
		break;
	case USB_DR_MODE_HOST:
		dwc3_set_prtcap(dwc, DWC3_GCTL_PRTCAP_HOST);

//		if (dwc->usb2_phy)
//			otg_set_vbus(dwc->usb2_phy->otg, true);
		phy_set_mode(dwc->usb2_generic_phy, PHY_MODE_USB_HOST);
		phy_set_mode(dwc->usb3_generic_phy, PHY_MODE_USB_HOST);

		ret = dwc3_host_init(dwc);
		if (ret)
			return dev_err_probe(dev, ret, "failed to initialize host\n");
		break;
	default:
		dev_err(dev, "Unsupported mode of operation %d\n", dwc->dr_mode);
		return -EINVAL;
	}

	return 0;
}

static int dwc3_core_init_mode(struct dwc3 *dwc)
{
	if (dwc->dr_mode == USB_DR_MODE_OTG)
		return usb_register_otg_device(dwc->dev, dwc3_set_dr_mode, dwc);
	else
		return dwc3_set_dr_mode(dwc, dwc->dr_mode);
}

static void dwc3_core_exit_mode(struct dwc3 *dwc)
{
	switch (dwc->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
		dwc3_gadget_exit(dwc);
		break;
	case USB_DR_MODE_HOST:
		dwc3_host_exit(dwc);
		break;
	default:
		/* do nothing */
		break;
	}

	/* de-assert DRVVBUS for HOST and OTG mode */
	dwc3_set_prtcap(dwc, DWC3_GCTL_PRTCAP_DEVICE);
}

static void dwc3_get_properties(struct dwc3 *dwc)
{
	struct device		*dev = dwc->dev;
	u8			lpm_nyet_threshold;
	u8			tx_de_emphasis;
	u8			hird_threshold;
	u8			rx_thr_num_pkt_prd = 0;
	u8			rx_max_burst_prd = 0;
	u8			tx_thr_num_pkt_prd = 0;
	u8			tx_max_burst_prd = 0;
	u8			tx_fifo_resize_max_num;

	/* default to highest possible threshold */
	lpm_nyet_threshold = 0xf;

	/* default to -3.5dB de-emphasis */
	tx_de_emphasis = 1;

	/*
	 * default to assert utmi_sleep_n and use maximum allowed HIRD
	 * threshold value of 0b1100
	 */
	hird_threshold = 12;

	/*
	 * default to a TXFIFO size large enough to fit 6 max packets.  This
	 * allows for systems with larger bus latencies to have some headroom
	 * for endpoints that have a large bMaxBurst value.
	 */
	tx_fifo_resize_max_num = 6;

	dwc->maximum_speed = of_usb_get_maximum_speed(dev->of_node, NULL);
//	dwc->max_ssp_rate = usb_get_maximum_ssp_rate(dev);
	dwc->dr_mode = of_usb_get_dr_mode(dev->of_node, NULL);
	dwc->hsphy_mode = of_usb_get_phy_mode(dev->of_node, NULL);

	dwc->sysdev_is_parent = of_property_read_bool(dev->of_node,
				"linux,sysdev_is_parent");
	if (dwc->sysdev_is_parent)
		dwc->sysdev = dwc->dev->parent;
	else
		dwc->sysdev = dwc->dev;

	dwc->has_lpm_erratum = of_property_read_bool(dev->of_node,
				"snps,has-lpm-erratum");
	of_property_read_u8(dev->of_node, "snps,lpm-nyet-threshold",
				&lpm_nyet_threshold);
	dwc->is_utmi_l1_suspend = of_property_read_bool(dev->of_node,
				"snps,is-utmi-l1-suspend");
	of_property_read_u8(dev->of_node, "snps,hird-threshold",
				&hird_threshold);
	dwc->dis_start_transfer_quirk = of_property_read_bool(dev->of_node,
				"snps,dis-start-transfer-quirk");
	dwc->usb3_lpm_capable = of_property_read_bool(dev->of_node,
				"snps,usb3_lpm_capable");
	dwc->usb2_lpm_disable = of_property_read_bool(dev->of_node,
				"snps,usb2-lpm-disable");
	dwc->usb2_gadget_lpm_disable = of_property_read_bool(dev->of_node,
				"snps,usb2-gadget-lpm-disable");
	of_property_read_u8(dev->of_node, "snps,rx-thr-num-pkt-prd",
				&rx_thr_num_pkt_prd);
	of_property_read_u8(dev->of_node, "snps,rx-max-burst-prd",
				&rx_max_burst_prd);
	of_property_read_u8(dev->of_node, "snps,tx-thr-num-pkt-prd",
				&tx_thr_num_pkt_prd);
	of_property_read_u8(dev->of_node, "snps,tx-max-burst-prd",
				&tx_max_burst_prd);
	dwc->do_fifo_resize = of_property_read_bool(dev->of_node,
							"tx-fifo-resize");
	if (dwc->do_fifo_resize)
		of_property_read_u8(dev->of_node, "tx-fifo-max-num",
					&tx_fifo_resize_max_num);

	dwc->disable_scramble_quirk = of_property_read_bool(dev->of_node,
				"snps,disable_scramble_quirk");
	dwc->u2exit_lfps_quirk = of_property_read_bool(dev->of_node,
				"snps,u2exit_lfps_quirk");
	dwc->u2ss_inp3_quirk = of_property_read_bool(dev->of_node,
				"snps,u2ss_inp3_quirk");
	dwc->req_p1p2p3_quirk = of_property_read_bool(dev->of_node,
				"snps,req_p1p2p3_quirk");
	dwc->del_p1p2p3_quirk = of_property_read_bool(dev->of_node,
				"snps,del_p1p2p3_quirk");
	dwc->del_phy_power_chg_quirk = of_property_read_bool(dev->of_node,
				"snps,del_phy_power_chg_quirk");
	dwc->lfps_filter_quirk = of_property_read_bool(dev->of_node,
				"snps,lfps_filter_quirk");
	dwc->rx_detect_poll_quirk = of_property_read_bool(dev->of_node,
				"snps,rx_detect_poll_quirk");
	dwc->dis_u3_susphy_quirk = of_property_read_bool(dev->of_node,
				"snps,dis_u3_susphy_quirk");
	dwc->dis_u2_susphy_quirk = of_property_read_bool(dev->of_node,
				"snps,dis_u2_susphy_quirk");
	dwc->dis_enblslpm_quirk = of_property_read_bool(dev->of_node,
				"snps,dis_enblslpm_quirk");
	dwc->dis_u1_entry_quirk = of_property_read_bool(dev->of_node,
				"snps,dis-u1-entry-quirk");
	dwc->dis_u2_entry_quirk = of_property_read_bool(dev->of_node,
				"snps,dis-u2-entry-quirk");
	dwc->dis_rxdet_inp3_quirk = of_property_read_bool(dev->of_node,
				"snps,dis_rxdet_inp3_quirk");
	dwc->dis_u2_freeclk_exists_quirk = of_property_read_bool(dev->of_node,
				"snps,dis-u2-freeclk-exists-quirk");
	dwc->dis_del_phy_power_chg_quirk = of_property_read_bool(dev->of_node,
				"snps,dis-del-phy-power-chg-quirk");
	dwc->dis_tx_ipgap_linecheck_quirk = of_property_read_bool(dev->of_node,
				"snps,dis-tx-ipgap-linecheck-quirk");
	dwc->resume_hs_terminations = of_property_read_bool(dev->of_node,
				"snps,resume-hs-terminations");
	dwc->parkmode_disable_ss_quirk = of_property_read_bool(dev->of_node,
				"snps,parkmode-disable-ss-quirk");
	dwc->gfladj_refclk_lpm_sel = of_property_read_bool(dev->of_node,
				"snps,gfladj-refclk-lpm-sel-quirk");

	dwc->tx_de_emphasis_quirk = of_property_read_bool(dev->of_node,
				"snps,tx_de_emphasis_quirk");
	of_property_read_u8(dev->of_node, "snps,tx_de_emphasis",
				&tx_de_emphasis);
	of_property_read_string(dev->of_node, "snps,hsphy_interface",
				    &dwc->hsphy_interface);
	of_property_read_u32(dev->of_node, "snps,quirk-frame-length-adjustment",
				 &dwc->fladj);
	of_property_read_u32(dev->of_node, "snps,ref-clock-period-ns",
				 &dwc->ref_clk_per);

	dwc->dis_metastability_quirk = of_property_read_bool(dev->of_node,
				"snps,dis_metastability_quirk");

	dwc->dis_split_quirk = of_property_read_bool(dev->of_node,
				"snps,dis-split-quirk");

	dwc->lpm_nyet_threshold = lpm_nyet_threshold;
	dwc->tx_de_emphasis = tx_de_emphasis;

	dwc->hird_threshold = hird_threshold;

	dwc->rx_thr_num_pkt_prd = rx_thr_num_pkt_prd;
	dwc->rx_max_burst_prd = rx_max_burst_prd;

	dwc->tx_thr_num_pkt_prd = tx_thr_num_pkt_prd;
	dwc->tx_max_burst_prd = tx_max_burst_prd;

	dwc->imod_interval = 0;

	dwc->tx_fifo_resize_max_num = tx_fifo_resize_max_num;
}

/* check whether the core supports IMOD */
bool dwc3_has_imod(struct dwc3 *dwc)
{
	return DWC3_VER_IS_WITHIN(DWC3, 300A, ANY) ||
		DWC3_VER_IS_WITHIN(DWC31, 120A, ANY) ||
		DWC3_IP_IS(DWC32);
}

static void dwc3_check_params(struct dwc3 *dwc)
{
	struct device *dev = dwc->dev;
	unsigned int hwparam_gen =
		DWC3_GHWPARAMS3_SSPHY_IFC(dwc->hwparams.hwparams3);

	/* Check for proper value of imod_interval */
	if (dwc->imod_interval && !dwc3_has_imod(dwc)) {
		dev_warn(dwc->dev, "Interrupt moderation not supported\n");
		dwc->imod_interval = 0;
	}

	/*
	 * Workaround for STAR 9000961433 which affects only version
	 * 3.00a of the DWC_usb3 core. This prevents the controller
	 * interrupt from being masked while handling events. IMOD
	 * allows us to work around this issue. Enable it for the
	 * affected version.
	 */
	if (!dwc->imod_interval &&
	    DWC3_VER_IS(DWC3, 300A))
		dwc->imod_interval = 1;

	/* Check the maximum_speed parameter */
	switch (dwc->maximum_speed) {
	case USB_SPEED_FULL:
	case USB_SPEED_HIGH:
		break;
	case USB_SPEED_SUPER:
		if (hwparam_gen == DWC3_GHWPARAMS3_SSPHY_IFC_DIS)
			dev_warn(dev, "UDC doesn't support Gen 1\n");
		break;
	case USB_SPEED_SUPER_PLUS:
		if ((DWC3_IP_IS(DWC32) &&
		     hwparam_gen == DWC3_GHWPARAMS3_SSPHY_IFC_DIS) ||
		    (!DWC3_IP_IS(DWC32) &&
		     hwparam_gen != DWC3_GHWPARAMS3_SSPHY_IFC_GEN2))
			dev_warn(dev, "UDC doesn't support SSP\n");
		break;
	default:
		dev_err(dev, "invalid maximum_speed parameter %d\n",
			dwc->maximum_speed);
		fallthrough;
	case USB_SPEED_UNKNOWN:
		switch (hwparam_gen) {
		case DWC3_GHWPARAMS3_SSPHY_IFC_GEN2:
			dwc->maximum_speed = USB_SPEED_SUPER_PLUS;
			break;
		case DWC3_GHWPARAMS3_SSPHY_IFC_GEN1:
			if (DWC3_IP_IS(DWC32))
				dwc->maximum_speed = USB_SPEED_SUPER_PLUS;
			else
				dwc->maximum_speed = USB_SPEED_SUPER;
			break;
		case DWC3_GHWPARAMS3_SSPHY_IFC_DIS:
			dwc->maximum_speed = USB_SPEED_HIGH;
			break;
		default:
			dwc->maximum_speed = USB_SPEED_SUPER;
			break;
		}
		break;
	}

	/*
	 * Currently the controller does not have visibility into the HW
	 * parameter to determine the maximum number of lanes the HW supports.
	 * If the number of lanes is not specified in the device property, then
	 * set the default to support dual-lane for DWC_usb32 and single-lane
	 * for DWC_usb31 for super-speed-plus.
	 */
	if (dwc->maximum_speed == USB_SPEED_SUPER_PLUS) {
		switch (dwc->max_ssp_rate) {
		case USB_SSP_GEN_2x1:
			if (hwparam_gen == DWC3_GHWPARAMS3_SSPHY_IFC_GEN1)
				dev_warn(dev, "UDC only supports Gen 1\n");
			break;
		case USB_SSP_GEN_1x2:
		case USB_SSP_GEN_2x2:
			if (DWC3_IP_IS(DWC31))
				dev_warn(dev, "UDC only supports single lane\n");
			break;
		case USB_SSP_GEN_UNKNOWN:
		default:
			switch (hwparam_gen) {
			case DWC3_GHWPARAMS3_SSPHY_IFC_GEN2:
				if (DWC3_IP_IS(DWC32))
					dwc->max_ssp_rate = USB_SSP_GEN_2x2;
				else
					dwc->max_ssp_rate = USB_SSP_GEN_2x1;
				break;
			case DWC3_GHWPARAMS3_SSPHY_IFC_GEN1:
				if (DWC3_IP_IS(DWC32))
					dwc->max_ssp_rate = USB_SSP_GEN_1x2;
				break;
			}
			break;
		}
	}
}

static int dwc3_probe(struct device *dev)
{
	struct dwc3		*dwc;
	int			ret;

	dwc = xzalloc(sizeof(*dwc));
	dev->priv = dwc;

	dwc->dev = dev;
	dwc->regs = dev_get_mem_region(dwc->dev, 0) + DWC3_GLOBALS_REGS_START;

	dwc3_get_properties(dwc);

	if (dev->of_node) {
		ret = clk_bulk_get_all(dev, &dwc->clks);
		if (ret < 0)
			return ret;

		dwc->num_clks = ret;
	}

	ret = clk_bulk_enable(dwc->num_clks, dwc->clks);
	if (ret)
		return ret;

	dwc->reset = reset_control_get(dev, NULL);
	if (IS_ERR(dwc->reset)) {
		dev_err(dev, "Failed to get reset control: %pe\n", dwc->reset);
		return PTR_ERR(dwc->reset);
	}

	reset_control_assert(dwc->reset);
	mdelay(1);
	reset_control_deassert(dwc->reset);

	if (!dwc3_core_is_valid(dwc)) {
		dev_err(dwc->dev, "this is not a DesignWare USB3 DRD Core\n");
		return -ENODEV;
	}

	dwc3_core_soft_reset(dwc);

	dwc3_cache_hwparams(dwc);

	ret = dwc3_alloc_event_buffers(dwc, DWC3_EVENT_BUFFERS_SIZE);
	if (ret) {
		dev_err(dwc->dev, "failed to allocate event buffers\n");
		return -ENOMEM;
	}

	ret = dwc3_get_dr_mode(dwc);
	if (ret)
		return ret;

	ret = dwc3_alloc_scratch_buffers(dwc);
	if (ret)
		return ret;

	ret = dwc3_core_init(dwc);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to initialize core: %d\n", ret);
		return ret;
	}

	dwc3_check_params(dwc);

	ret = dwc3_core_init_mode(dwc);
	if (ret)
		return ret;

	return 0;
}

static void dwc3_remove(struct device *dev)
{
	struct dwc3 *dwc = dev->priv;

	dwc3_core_exit_mode(dwc);
	dwc3_core_exit(dwc);
	clk_bulk_put(dwc->num_clks, dwc->clks);
	dwc3_free_event_buffers(dwc);
	dwc3_free_scratch_buffers(dwc);
}

static const struct of_device_id of_dwc3_match[] = {
	{
		.compatible = "snps,dwc3",
	},
	{
		.compatible = "synopsys,dwc3",
	},
	{
		.compatible = "rockchip,rk3568-dwc3",
	},
	{ },
};

static struct driver dwc3_driver = {
	.probe = dwc3_probe,
	.remove	= dwc3_remove,
	.name = "dwc3",
	.of_compatible = DRV_OF_COMPAT(of_dwc3_match),
};
device_platform_driver(dwc3_driver);
