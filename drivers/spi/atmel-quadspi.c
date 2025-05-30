// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Atmel QSPI Controller
 *
 * Copyright (C) 2015 Atmel Corporation
 * Copyright (C) 2018 Cryptera A/S
 *
 * Author: Cyrille Pitchen <cyrille.pitchen@atmel.com>
 * Author: Piotr Bugalski <bugalski.piotr@gmail.com>
 *
 * This driver is based on drivers/mtd/spi-nor/fsl-quadspi.c from Freescale.
 */

#include <common.h>
#include <init.h>
#include <driver.h>
#include <errno.h>
#include <clock.h>
#include <xfuncs.h>
#include <gpio.h>
#include <of_gpio.h>
#include <io.h>
#include <spi/spi.h>
#include <mach/at91/iomux.h>
#include <mach/at91/board.h>
#include <mach/at91/cpu.h>
#include <linux/clk.h>
#include <linux/err.h>

#include <linux/kernel.h>
#include <linux/spi/spi-mem.h>
#include <of_device.h>
#include <linux/iopoll.h>

/* QSPI register offsets */
#define QSPI_CR      0x0000  /* Control Register */
#define QSPI_MR      0x0004  /* Mode Register */
#define QSPI_RD      0x0008  /* Receive Data Register */
#define QSPI_TD      0x000c  /* Transmit Data Register */
#define QSPI_SR      0x0010  /* Status Register */
#define QSPI_IER     0x0014  /* Interrupt Enable Register */
#define QSPI_IDR     0x0018  /* Interrupt Disable Register */
#define QSPI_IMR     0x001c  /* Interrupt Mask Register */
#define QSPI_SCR     0x0020  /* Serial Clock Register */

#define QSPI_IAR     0x0030  /* Instruction Address Register */
#define QSPI_ICR     0x0034  /* Instruction Code Register */
#define QSPI_WICR    0x0034  /* Write Instruction Code Register */
#define QSPI_IFR     0x0038  /* Instruction Frame Register */
#define QSPI_RICR    0x003C  /* Read Instruction Code Register */

#define QSPI_SMR     0x0040  /* Scrambling Mode Register */
#define QSPI_SKR     0x0044  /* Scrambling Key Register */

#define QSPI_WPMR    0x00E4  /* Write Protection Mode Register */
#define QSPI_WPSR    0x00E8  /* Write Protection Status Register */

#define QSPI_VERSION 0x00FC  /* Version Register */


/* Bitfields in QSPI_CR (Control Register) */
#define QSPI_CR_QSPIEN                  BIT(0)
#define QSPI_CR_QSPIDIS                 BIT(1)
#define QSPI_CR_SWRST                   BIT(7)
#define QSPI_CR_LASTXFER                BIT(24)

/* Bitfields in QSPI_MR (Mode Register) */
#define QSPI_MR_SMM                     BIT(0)
#define QSPI_MR_LLB                     BIT(1)
#define QSPI_MR_WDRBT                   BIT(2)
#define QSPI_MR_SMRM                    BIT(3)
#define QSPI_MR_CSMODE_MASK             GENMASK(5, 4)
#define QSPI_MR_CSMODE_NOT_RELOADED     (0 << 4)
#define QSPI_MR_CSMODE_LASTXFER         (1 << 4)
#define QSPI_MR_CSMODE_SYSTEMATICALLY   (2 << 4)
#define QSPI_MR_NBBITS_MASK             GENMASK(11, 8)
#define QSPI_MR_NBBITS(n)               ((((n) - 8) << 8) & QSPI_MR_NBBITS_MASK)
#define QSPI_MR_DLYBCT_MASK             GENMASK(23, 16)
#define QSPI_MR_DLYBCT(n)               (((n) << 16) & QSPI_MR_DLYBCT_MASK)
#define QSPI_MR_DLYCS_MASK              GENMASK(31, 24)
#define QSPI_MR_DLYCS(n)                (((n) << 24) & QSPI_MR_DLYCS_MASK)

/* Bitfields in QSPI_SR/QSPI_IER/QSPI_IDR/QSPI_IMR  */
#define QSPI_SR_RDRF                    BIT(0)
#define QSPI_SR_TDRE                    BIT(1)
#define QSPI_SR_TXEMPTY                 BIT(2)
#define QSPI_SR_OVRES                   BIT(3)
#define QSPI_SR_CSR                     BIT(8)
#define QSPI_SR_CSS                     BIT(9)
#define QSPI_SR_INSTRE                  BIT(10)
#define QSPI_SR_QSPIENS                 BIT(24)

#define QSPI_SR_CMD_COMPLETED	(QSPI_SR_INSTRE | QSPI_SR_CSR)

/* Bitfields in QSPI_SCR (Serial Clock Register) */
#define QSPI_SCR_CPOL                   BIT(0)
#define QSPI_SCR_CPHA                   BIT(1)
#define QSPI_SCR_SCBR_MASK              GENMASK(15, 8)
#define QSPI_SCR_SCBR(n)                (((n) << 8) & QSPI_SCR_SCBR_MASK)
#define QSPI_SCR_DLYBS_MASK             GENMASK(23, 16)
#define QSPI_SCR_DLYBS(n)               (((n) << 16) & QSPI_SCR_DLYBS_MASK)

/* Bitfields in QSPI_ICR (Read/Write Instruction Code Register) */
#define QSPI_ICR_INST_MASK              GENMASK(7, 0)
#define QSPI_ICR_INST(inst)             (((inst) << 0) & QSPI_ICR_INST_MASK)
#define QSPI_ICR_OPT_MASK               GENMASK(23, 16)
#define QSPI_ICR_OPT(opt)               (((opt) << 16) & QSPI_ICR_OPT_MASK)

/* Bitfields in QSPI_IFR (Instruction Frame Register) */
#define QSPI_IFR_WIDTH_MASK             GENMASK(2, 0)
#define QSPI_IFR_WIDTH_SINGLE_BIT_SPI   (0 << 0)
#define QSPI_IFR_WIDTH_DUAL_OUTPUT      (1 << 0)
#define QSPI_IFR_WIDTH_QUAD_OUTPUT      (2 << 0)
#define QSPI_IFR_WIDTH_DUAL_IO          (3 << 0)
#define QSPI_IFR_WIDTH_QUAD_IO          (4 << 0)
#define QSPI_IFR_WIDTH_DUAL_CMD         (5 << 0)
#define QSPI_IFR_WIDTH_QUAD_CMD         (6 << 0)
#define QSPI_IFR_INSTEN                 BIT(4)
#define QSPI_IFR_ADDREN                 BIT(5)
#define QSPI_IFR_OPTEN                  BIT(6)
#define QSPI_IFR_DATAEN                 BIT(7)
#define QSPI_IFR_OPTL_MASK              GENMASK(9, 8)
#define QSPI_IFR_OPTL_1BIT              (0 << 8)
#define QSPI_IFR_OPTL_2BIT              (1 << 8)
#define QSPI_IFR_OPTL_4BIT              (2 << 8)
#define QSPI_IFR_OPTL_8BIT              (3 << 8)
#define QSPI_IFR_ADDRL                  BIT(10)
#define QSPI_IFR_TFRTYP_MEM		BIT(12)
#define QSPI_IFR_SAMA5D2_WRITE_TRSFR	BIT(13)
#define QSPI_IFR_CRM                    BIT(14)
#define QSPI_IFR_NBDUM_MASK             GENMASK(20, 16)
#define QSPI_IFR_NBDUM(n)               (((n) << 16) & QSPI_IFR_NBDUM_MASK)
#define QSPI_IFR_APBTFRTYP_READ		BIT(24)	/* Defined in SAM9X60 */

/* Bitfields in QSPI_SMR (Scrambling Mode Register) */
#define QSPI_SMR_SCREN                  BIT(0)
#define QSPI_SMR_RVDIS                  BIT(1)

/* Bitfields in QSPI_WPMR (Write Protection Mode Register) */
#define QSPI_WPMR_WPEN                  BIT(0)
#define QSPI_WPMR_WPKEY_MASK            GENMASK(31, 8)
#define QSPI_WPMR_WPKEY(wpkey)          (((wpkey) << 8) & QSPI_WPMR_WPKEY_MASK)

/* Bitfields in QSPI_WPSR (Write Protection Status Register) */
#define QSPI_WPSR_WPVS                  BIT(0)
#define QSPI_WPSR_WPVSRC_MASK           GENMASK(15, 8)
#define QSPI_WPSR_WPVSRC(src)           (((src) << 8) & QSPI_WPSR_WPVSRC)

struct atmel_qspi_caps {
	bool has_qspick;
	bool has_ricr;
};

struct atmel_qspi {
	struct spi_controller ctlr;
	void __iomem		*regs;
	void __iomem		*mem;
	struct clk		*pclk;
	struct clk		*qspick;
	const struct atmel_qspi_caps *caps;
	u32			mr;
};

struct atmel_qspi_mode {
	u8 cmd_buswidth;
	u8 addr_buswidth;
	u8 data_buswidth;
	u32 config;
};

static const struct atmel_qspi_mode atmel_qspi_modes[] = {
	{ 1, 1, 1, QSPI_IFR_WIDTH_SINGLE_BIT_SPI },
	{ 1, 1, 2, QSPI_IFR_WIDTH_DUAL_OUTPUT },
	{ 1, 1, 4, QSPI_IFR_WIDTH_QUAD_OUTPUT },
	{ 1, 2, 2, QSPI_IFR_WIDTH_DUAL_IO },
	{ 1, 4, 4, QSPI_IFR_WIDTH_QUAD_IO },
	{ 2, 2, 2, QSPI_IFR_WIDTH_DUAL_CMD },
	{ 4, 4, 4, QSPI_IFR_WIDTH_QUAD_CMD },
};

static inline bool atmel_qspi_is_compatible(const struct spi_mem_op *op,
					    const struct atmel_qspi_mode *mode)
{
	if (op->cmd.buswidth != mode->cmd_buswidth)
		return false;

	if (op->addr.nbytes && op->addr.buswidth != mode->addr_buswidth)
		return false;

	if (op->data.nbytes && op->data.buswidth != mode->data_buswidth)
		return false;

	return true;
}

static int atmel_qspi_find_mode(const struct spi_mem_op *op)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(atmel_qspi_modes); i++)
		if (atmel_qspi_is_compatible(op, &atmel_qspi_modes[i]))
			return i;

	return -ENOTSUPP;
}

static bool atmel_qspi_supports_op(struct spi_mem *mem,
				   const struct spi_mem_op *op)
{
	if (atmel_qspi_find_mode(op) < 0)
		return false;

	/* special case not supported by hardware */
	if (op->addr.nbytes == 2 && op->cmd.buswidth != op->addr.buswidth &&
	    op->dummy.nbytes == 0)
		return false;

	return true;
}

static int atmel_qspi_set_cfg(struct atmel_qspi *aq,
			      const struct spi_mem_op *op, u32 *offset)
{
	u32 iar, icr, ifr;
	u32 dummy_cycles = 0;
	int mode;

	iar = 0;
	icr = QSPI_ICR_INST(op->cmd.opcode);
	ifr = QSPI_IFR_INSTEN;

	mode = atmel_qspi_find_mode(op);
	if (mode < 0)
		return mode;
	ifr |= atmel_qspi_modes[mode].config;

	if (op->dummy.buswidth && op->dummy.nbytes)
		dummy_cycles = op->dummy.nbytes * 8 / op->dummy.buswidth;

	/*
	 * The controller allows 24 and 32-bit addressing while NAND-flash
	 * requires 16-bit long. Handling 8-bit long addresses is done using
	 * the option field. For the 16-bit addresses, the workaround depends
	 * of the number of requested dummy bits. If there are 8 or more dummy
	 * cycles, the address is shifted and sent with the first dummy byte.
	 * Otherwise opcode is disabled and the first byte of the address
	 * contains the command opcode (works only if the opcode and address
	 * use the same buswidth). The limitation is when the 16-bit address is
	 * used without enough dummy cycles and the opcode is using a different
	 * buswidth than the address.
	 */
	if (op->addr.buswidth) {
		switch (op->addr.nbytes) {
		case 0:
			break;
		case 1:
			ifr |= QSPI_IFR_OPTEN | QSPI_IFR_OPTL_8BIT;
			icr |= QSPI_ICR_OPT(op->addr.val & 0xff);
			break;
		case 2:
			if (dummy_cycles < 8 / op->addr.buswidth) {
				ifr &= ~QSPI_IFR_INSTEN;
				ifr |= QSPI_IFR_ADDREN;
				iar = (op->cmd.opcode << 16) |
					(op->addr.val & 0xffff);
			} else {
				ifr |= QSPI_IFR_ADDREN;
				iar = (op->addr.val << 8) & 0xffffff;
				dummy_cycles -= 8 / op->addr.buswidth;
			}
			break;
		case 3:
			ifr |= QSPI_IFR_ADDREN;
			iar = op->addr.val & 0xffffff;
			break;
		case 4:
			ifr |= QSPI_IFR_ADDREN | QSPI_IFR_ADDRL;
			iar = op->addr.val & 0x7ffffff;
			break;
		default:
			return -ENOTSUPP;
		}
	}

	/* offset of the data access in the QSPI memory space */
	*offset = iar;

	/* Set number of dummy cycles */
	if (dummy_cycles)
		ifr |= QSPI_IFR_NBDUM(dummy_cycles);

	/* Set data enable */
	if (op->data.nbytes)
		ifr |= QSPI_IFR_DATAEN;

	/*
	 * If the QSPI controller is set in regular SPI mode, set it in
	 * Serial Memory Mode (SMM).
	 */
	if (aq->mr != QSPI_MR_SMM) {
		writel_relaxed(QSPI_MR_SMM, aq->regs + QSPI_MR);
		aq->mr = QSPI_MR_SMM;
	}

	/* Clear pending interrupts */
	(void)readl_relaxed(aq->regs + QSPI_SR);

	if (aq->caps->has_ricr) {
		if (!op->addr.nbytes && op->data.dir == SPI_MEM_DATA_IN)
			ifr |= QSPI_IFR_APBTFRTYP_READ;

		/* Set QSPI Instruction Frame registers */
		writel_relaxed(iar, aq->regs + QSPI_IAR);
		if (op->data.dir == SPI_MEM_DATA_IN)
			writel_relaxed(icr, aq->regs + QSPI_RICR);
		else
			writel_relaxed(icr, aq->regs + QSPI_WICR);
		writel_relaxed(ifr, aq->regs + QSPI_IFR);
	} else {
		if (op->data.dir == SPI_MEM_DATA_OUT)
			ifr |= QSPI_IFR_SAMA5D2_WRITE_TRSFR;

		/* Set QSPI Instruction Frame registers */
		writel_relaxed(iar, aq->regs + QSPI_IAR);
		writel_relaxed(icr, aq->regs + QSPI_ICR);
		writel_relaxed(ifr, aq->regs + QSPI_IFR);
	}

	return 0;
}

static int atmel_qspi_exec_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct atmel_qspi *aq = spi_controller_get_devdata(mem->spi->controller);
	u32 sr, imr, offset;
	int err;

	err = atmel_qspi_set_cfg(aq, op, &offset);
	if (err)
		return err;

	/* Skip to the final steps if there is no data */
	if (op->data.nbytes) {
		/* Dummy read of QSPI_IFR to synchronize APB and AHB accesses */
		(void)readl_relaxed(aq->regs + QSPI_IFR);

		/* Send/Receive data */
		if (op->data.dir == SPI_MEM_DATA_IN)
			memcpy_fromio(op->data.buf.in, aq->mem + offset,
				       op->data.nbytes);
		else
			memcpy_toio(aq->mem + offset, op->data.buf.out,
				     op->data.nbytes);

		/* Release the chip-select */
		writel_relaxed(QSPI_CR_LASTXFER, aq->regs + QSPI_CR);
	}

	/* Poll INSTruction End and Chip Select Rise flags. */
	imr = QSPI_SR_CMD_COMPLETED;
	return readl_poll_timeout(aq->regs + QSPI_SR, sr, (sr & imr) == imr,
				  1000000);
}

static const char *atmel_qspi_get_name(struct spi_mem *mem)
{
	return dev_name(&mem->spi->dev);
}

static const struct spi_controller_mem_ops atmel_qspi_mem_ops = {
	.supports_op = atmel_qspi_supports_op,
	.exec_op = atmel_qspi_exec_op,
	.get_name = atmel_qspi_get_name
};

static int atmel_qspi_setup(struct spi_device *spi)
{
	struct atmel_qspi *aq = container_of(spi->controller, struct atmel_qspi,
					     ctlr);
	unsigned long src_rate;
	u32 scr, scbr;

	if (!spi->max_speed_hz)
		return -EINVAL;

	src_rate = clk_get_rate(aq->pclk);
	if (!src_rate)
		return -EINVAL;

	/* Compute the QSPI baudrate */
	scbr = DIV_ROUND_UP(src_rate, spi->max_speed_hz);
	if (scbr > 0)
		scbr--;

	scr = QSPI_SCR_SCBR(scbr);
	writel_relaxed(scr, aq->regs + QSPI_SCR);

	return 0;
}

static int atmel_qspi_init(struct atmel_qspi *aq)
{
	/* Reset the QSPI controller */
	writel_relaxed(QSPI_CR_SWRST, aq->regs + QSPI_CR);

	/* Set the QSPI controller by default in Serial Memory Mode */
	writel_relaxed(QSPI_MR_SMM, aq->regs + QSPI_MR);
	aq->mr = QSPI_MR_SMM;

	/* Enable the QSPI controller */
	writel_relaxed(QSPI_CR_QSPIEN, aq->regs + QSPI_CR);

	return 0;
}

static int atmel_qspi_probe(struct device *dev)
{
	struct spi_controller *ctrl;
	struct atmel_qspi *aq;
	int err = 0;

	aq = xzalloc(sizeof(*aq));
	ctrl = &aq->ctlr;

	/* ctrl->mode_bits = SPI_RX_DUAL | SPI_RX_QUAD | SPI_TX_DUAL | SPI_TX_QUAD; */

	ctrl->dev = dev;
	ctrl->setup = atmel_qspi_setup;
	ctrl->bus_num = -1;
	ctrl->mem_ops = &atmel_qspi_mem_ops;
	ctrl->num_chipselect = 1;

	spi_controller_set_devdata(ctrl, aq);

	/* Map the registers */
	aq->regs = dev_request_mem_region_by_name(dev, "qspi_base");
	if (IS_ERR(aq->regs)) {
		dev_err(dev, "missing registers\n");
		err = PTR_ERR(aq->regs);
		goto exit;
	}

	/* Map the AHB memory */
	aq->mem = dev_request_mem_region_by_name(dev, "qspi_mmap");
	if (IS_ERR(aq->mem)) {
		dev_err(dev, "missing AHB memory\n");
		err = PTR_ERR(aq->mem);
		goto exit;
	}

	/* Get the peripheral clock */
	aq->pclk = clk_get(dev, "pclk");
	if (IS_ERR(aq->pclk))
		aq->pclk = clk_get(dev, NULL);

	if (IS_ERR(aq->pclk)) {
		dev_err(dev, "missing peripheral clock\n");
		err = PTR_ERR(aq->pclk);
		goto exit;
	}

	/* Enable the peripheral clock */
	err = clk_enable(aq->pclk);
	if (err) {
		dev_err(dev, "failed to enable the peripheral clock\n");
		goto exit;
	}

	aq->caps = of_device_get_match_data(dev);
	if (!aq->caps) {
		dev_err(dev, "Could not retrieve QSPI caps\n");
		err = -EINVAL;
		goto exit;
	}

	if (aq->caps->has_qspick) {
		/* Get the QSPI system clock */
		aq->qspick = clk_get(dev, "qspick");
		if (IS_ERR(aq->qspick)) {
			dev_err(dev, "missing system clock\n");
			err = PTR_ERR(aq->qspick);
			goto disable_pclk;
		}

		/* Enable the QSPI system clock */
		err = clk_enable(aq->qspick);
		if (err) {
			dev_err(dev, "failed to enable the QSPI system clock\n");
			goto disable_pclk;
		}
	}

	err = atmel_qspi_init(aq);
	if (err)
		goto disable_qspick;

	err = spi_register_controller(ctrl);
	if (err)
		goto disable_qspick;

	return 0;

disable_qspick:
	clk_disable(aq->qspick);
disable_pclk:
	clk_disable(aq->pclk);
exit:
	return err;
}

static const struct atmel_qspi_caps atmel_sama5d2_qspi_caps = {};

static const struct atmel_qspi_caps atmel_sam9x60_qspi_caps = {
	.has_qspick = true,
	.has_ricr = true,
};

static const struct of_device_id atmel_qspi_dt_ids[] = {
	{
		.compatible = "atmel,sama5d2-qspi",
		.data = &atmel_sama5d2_qspi_caps,
	},
	{
		.compatible = "microchip,sam9x60-qspi",
		.data = &atmel_sam9x60_qspi_caps,
	},
	{ /* sentinel */ }
};

static struct driver atmel_qspi_driver = {
	.name	= "atmel_qspi",
	.of_compatible	= atmel_qspi_dt_ids,
	.probe		= atmel_qspi_probe,
};
device_platform_driver(atmel_qspi_driver);
