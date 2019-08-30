/*
 * AppliedMicro X-Gene SoC SATA Host Controller Driver
 *
 * Copyright (c) 2014, Applied Micro Circuits Corporation
 * Author: Loc Ho <lho@apm.com>
 *         Tuan Phan <tphan@apm.com>
 *         Suman Tripathi <stripathi@apm.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/platform_device.h>
#include <linux/ahci_platform.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/acpi.h>
#include <linux/efi.h>
#include <linux/phy/phy.h>
#include "ahci.h"

/* Controller who PHY shared with SGMII Ethernet PHY */
#define XGENE_AHCI_SGMII_DTS		"apm,xgene-ahci-sgmii"
#define XGENE_ACHI_SGMII_ACPI		1

/* Controller who PHY (internal reference clock macro) shared with PCIe */
#define XGENE_AHCI_PCIE_DTS		"apm,xgene-ahci-pcie"
#define XGENE_ACHI_PCIE_ACPI		0

/* Max # of disk per a controller */
#define MAX_AHCI_CHN_PERCTR		2

/* MUX CSR */
#define SATA_ENET_CONFIG_REG		0x00000000
#define  CFG_SATA_ENET_SELECT_MASK	0x00000001

/* SATA core host controller CSR */
#define SLVRDERRATTRIBUTES		0x00000000
#define SLVWRERRATTRIBUTES		0x00000004
#define MSTRDERRATTRIBUTES		0x00000008
#define MSTWRERRATTRIBUTES		0x0000000c
#define BUSCTLREG			0x00000014
#define IOFMSTRWAUX			0x00000018
#define INTSTATUSMASK			0x0000002c
#define ERRINTSTATUS			0x00000030
#define ERRINTSTATUSMASK		0x00000034

/* SATA host AHCI CSR */
#define PORTCFG				0x000000a4
#define  PORTADDR_SET(dst, src) \
		(((dst) & ~0x0000003f) | (((u32)(src)) & 0x0000003f))
#define PORTPHY1CFG		0x000000a8
#define PORTPHY1CFG_FRCPHYRDY_SET(dst, src) \
		(((dst) & ~0x00100000) | (((u32)(src) << 0x14) & 0x00100000))
#define PORTPHY2CFG			0x000000ac
#define PORTPHY3CFG			0x000000b0
#define PORTPHY4CFG			0x000000b4
#define PORTPHY5CFG			0x000000b8
#define SCTL0				0x0000012C
#define PORTPHY5CFG_RTCHG_SET(dst, src) \
		(((dst) & ~0xfff00000) | (((u32)(src) << 0x14) & 0xfff00000))
#define PORTAXICFG_EN_CONTEXT_SET(dst, src) \
		(((dst) & ~0x01000000) | (((u32)(src) << 0x18) & 0x01000000))
#define PORTAXICFG			0x000000bc
#define PORTAXICFG_OUTTRANS_SET(dst, src) \
		(((dst) & ~0x00f00000) | (((u32)(src) << 0x14) & 0x00f00000))
#define PTC 				0xc8
#define PTC_RXWM_SET(dst, src)		\
		(((dst) & ~0x0000007f) | (((u32) (src) << 0) & 0x0000007f))

/* SATA host controller AXI CSR */
#define INT_SLV_TMOMASK			0x00000010

/* SATA diagnostic CSR */
#define CFG_MEM_RAM_SHUTDOWN		0x00000070
#define BLOCK_MEM_RDY			0x00000074

/* AHBC IOB flush CSR */
#define CFG_AMA_MODE			0x0000e014
#define CFG_RD2WR_EN			0x00000002

#define RXTX_REG7			0x00e
#define  RXTX_REG7_RESETB_RXD_MASK	0x00000100
#define  RXTX_REG7_RESETB_RXA_MASK	0x00000080
#define SATA_ENET_SDS_IND_CMD_REG	0x0000003c
#define  CFG_IND_WR_CMD_MASK		0x00000001
#define  CFG_IND_RD_CMD_MASK		0x00000002
#define  CFG_IND_CMD_DONE_MASK		0x00000004
#define  CFG_IND_ADDR_SET(dst, src) \
		(((dst) & ~0x003ffff0) | (((u32) (src) << 4) & 0x003ffff0))
#define SATA_ENET_SDS_IND_RDATA_REG	0x00000040
#define SATA_ENET_SDS_IND_WDATA_REG	0x00000044
#define SERDES_PLL_INDIRECT_OFFSET	0x0000
#define SERDES_PLL_REF_INDIRECT_OFFSET	0x20000
#define SERDES_INDIRECT_OFFSET		0x0400
#define SERDES_LANE_STRIDE		0x0200
#define SERDES_LANE_X4_STRIDE		0x30000

enum param_type {
	POST_LINK_UP_SSD = 0, /* ssd drives detected after link up */
	POST_LINK_UP_HDD = 1, /* HDD drives detected after link up */
	PRE_HARDRESET = 2,    /* Before COMINIT sequence */
	POST_HARDRESET = 3,   /* After COMINIT sequence */
	POST_LINK_UP_PHY_RESET = 4, /* PHY reset after link up */
};

#if defined(CONFIG_ARCH_MSLIM)
extern int xgene_ahci_port_start(struct ata_port *ap);
extern  int xgene_ahci_port_resume(struct ata_port *ap);
extern int xgene_ahci_softreset_mslim(struct ata_link *link, unsigned int *class,
				      unsigned long deadline);
extern void xgene_ahci_qc_prep_mslim(struct ata_queued_cmd *qc);
extern int xgene_ahci_restart_engine_mslim(struct ata_port *ap);
#endif

struct xgene_ahci_context {
	struct ahci_host_priv *hpriv;
	struct device *dev;
	u8 last_cmd[MAX_AHCI_CHN_PERCTR]; /* tracking the last command issued*/
	void __iomem *csr_core;		/* Core CSR address of IP */
	void __iomem *csr_diag;		/* Diag CSR address of IP */
	void __iomem *csr_sds;          /* Serdes CSR address of IP */
	void __iomem *csr_axi;		/* AXI CSR address of IP */
	void __iomem *csr_mux;		/* MUX CSR address of IP */
};

static void ahci_sds_wr(void __iomem *csr_base, u32 indirect_cmd_reg,
		   u32 indirect_data_reg, u32 addr, u32 data)
{
	unsigned long deadline = jiffies + HZ;
	u32 val;
	u32 cmd;

	cmd = CFG_IND_WR_CMD_MASK | CFG_IND_CMD_DONE_MASK;
	cmd = CFG_IND_ADDR_SET(cmd, addr);
	writel(data, csr_base + indirect_data_reg);
	readl(csr_base + indirect_data_reg); /* Force a barrier */
	writel(cmd, csr_base + indirect_cmd_reg);
	readl(csr_base + indirect_cmd_reg); /* Force a barrier */
	do {
		val = readl(csr_base + indirect_cmd_reg);
	} while (!(val & CFG_IND_CMD_DONE_MASK)&&
		 time_before(jiffies, deadline));
	if (!(val & CFG_IND_CMD_DONE_MASK))
		pr_err("SDS WR timeout at 0x%p offset 0x%08X value 0x%08X\n",
		       csr_base + indirect_cmd_reg, addr, data);
}

static void ahci_sds_rd(void __iomem *csr_base, u32 indirect_cmd_reg,
		   u32 indirect_data_reg, u32 addr, u32 *data)
{
	unsigned long deadline = jiffies + HZ;
	u32 val;
	u32 cmd;

	cmd = CFG_IND_RD_CMD_MASK | CFG_IND_CMD_DONE_MASK;
	cmd = CFG_IND_ADDR_SET(cmd, addr);
	writel(cmd, csr_base + indirect_cmd_reg);
	readl(csr_base + indirect_cmd_reg); /* Force a barrier */
	do {
		val = readl(csr_base + indirect_cmd_reg);
	} while (!(val & CFG_IND_CMD_DONE_MASK)&&
		 time_before(jiffies, deadline));
	*data = readl(csr_base + indirect_data_reg);
	if (!(val & CFG_IND_CMD_DONE_MASK))
		pr_err("SDS WR timeout at 0x%p offset 0x%08X value 0x%08X\n",
		       csr_base + indirect_cmd_reg, addr, *data);
}



static void ahci_serdes_wr(struct xgene_ahci_context *ctx, int lane, u32 reg, u32 data)
{
	void __iomem *sds_base = ctx->csr_sds;
	u32 cmd_reg;
	u32 wr_reg;
	u32 rd_reg;
	u32 val;
	
	cmd_reg = SATA_ENET_SDS_IND_CMD_REG;
	wr_reg = SATA_ENET_SDS_IND_WDATA_REG;
	rd_reg = SATA_ENET_SDS_IND_RDATA_REG;

	reg += (lane / 4) * SERDES_LANE_X4_STRIDE;
	reg += SERDES_INDIRECT_OFFSET;
	reg += (lane % 4) * SERDES_LANE_STRIDE;
	ahci_sds_wr(sds_base, cmd_reg, wr_reg, reg, data);
	ahci_sds_rd(sds_base, cmd_reg, rd_reg, reg, &val);
	pr_debug("SERDES WR addr 0x%X value 0x%08X <-> 0x%08X\n", reg, data,
		 val);
}

static void ahci_serdes_rd(struct xgene_ahci_context *ctx, int lane, u32 reg, u32 *data)
{
	void __iomem *sds_base = ctx->csr_sds;
	u32 cmd_reg;
	u32 rd_reg;

	cmd_reg = SATA_ENET_SDS_IND_CMD_REG;
	rd_reg = SATA_ENET_SDS_IND_RDATA_REG;

	reg += (lane / 4) * SERDES_LANE_X4_STRIDE;
	reg += SERDES_INDIRECT_OFFSET;
	reg += (lane % 4) * SERDES_LANE_STRIDE;
	ahci_sds_rd(sds_base, cmd_reg, rd_reg, reg, data);
	pr_debug("SERDES RD addr 0x%X value 0x%08X\n", reg, *data);
}

static void ahci_serdes_clrbits(struct xgene_ahci_context *ctx, int lane, u32 reg,
			   u32 bits)
{
	u32 val;

	ahci_serdes_rd(ctx, lane, reg, &val);
	val &= ~bits;
	ahci_serdes_wr(ctx, lane, reg, val);
}

static void ahci_serdes_setbits(struct xgene_ahci_context *ctx, int lane, u32 reg,
			   u32 bits)
{
	u32 val;

	ahci_serdes_rd(ctx, lane, reg, &val);
	val |= bits;
	ahci_serdes_wr(ctx, lane, reg, val);
}

static int xgene_ahci_init_memram(struct xgene_ahci_context *ctx)
{
	dev_dbg(ctx->dev, "Release memory from shutdown\n");
	writel(0x0, ctx->csr_diag + CFG_MEM_RAM_SHUTDOWN);
	readl(ctx->csr_diag + CFG_MEM_RAM_SHUTDOWN); /* Force a barrier */
	msleep(1);	/* reset may take up to 1ms */
	if (readl(ctx->csr_diag + BLOCK_MEM_RDY) != 0xFFFFFFFF) {
		dev_err(ctx->dev, "failed to release memory from shutdown\n");
		return -ENODEV;
	}
	return 0;
}

static int xgene_ahci_is_memram_inited(struct xgene_ahci_context *ctx)
{
	void __iomem *diagcsr = ctx->csr_diag;

	if (readl(diagcsr + CFG_MEM_RAM_SHUTDOWN) == 0 &&
	    readl(diagcsr + BLOCK_MEM_RDY) == 0xFFFFFFFF)
		return 1;
	return 0;
}

static char *apm88xxx_chip_revision(void)
{
	#define MIDR_EL1_REV_MASK			0x0000000f 
	#define REVIDR_EL1_MINOR_REV_MASK	0x00000007 
	#define EFUSE0_SHADOW_VERSION_SHIFT	28
	#define EFUSE0_SHADOW_VERSION_MASK	0xF
	u32 val;
	void *efuse;
	void *jtag;

#if defined(CONFIG_ARCH_MSLIM)
	efuse = ioremap(0xC054A000ULL, 0x100);
	jtag = ioremap(0x17000004ULL, 0x100);
#else
	efuse = ioremap(0x1054A000ULL, 0x100);
	jtag = ioremap(0x17000004ULL, 0x100);
#endif
	if (efuse == NULL || jtag == NULL) {
		if (efuse)
			iounmap(efuse);
		if (jtag)
			iounmap(jtag);
		return 0;
	}
#ifndef CONFIG_ARCH_MSLIM
	asm volatile("mrs %0, midr_el1" : "=r" (val));
	val &= MIDR_EL1_REV_MASK;
#else
	val = 1; /* Assume B0 */
#endif
	if (val == 0){
#ifndef CONFIG_ARCH_MSLIM
		asm volatile("mrs %0, revidr_el1" : "=r" (val));
#endif
		val &= REVIDR_EL1_MINOR_REV_MASK;
		switch (val) {
			case 0:
				return "A0";
			case 1:
				return "A1";
			case 2:
				val = (readl(efuse) >> 
						EFUSE0_SHADOW_VERSION_SHIFT)
					& EFUSE0_SHADOW_VERSION_MASK;
				if (val == 0x1) 
					return "A2";
				else 
					return "A3";
		}
	} else if (val == 1)
		return "B0";

	return "Unknown";
}


static int xgene_ahci_is_preB0(void)
{
	const char *revision = apm88xxx_chip_revision();
	if (!strcmp(revision, "B0"))
		return 0;
	else
		return 1;
}

static int xgene_ahci_wait_for_multiple_qc_complete(struct ata_port *ap, 
                                                    void __iomem *reg, unsigned
						     int val, unsigned long 
						     interval, unsigned long 
						     timeout) 
{
	unsigned long deadline;
	unsigned int tmp;

	tmp = ioread32(reg);
	deadline = ata_deadline(jiffies, timeout);

	while ((tmp != val) && (time_before(jiffies, deadline))) {
		udelay(interval);
		tmp = ioread32(reg);
	}

	return tmp;
}

/**
 * xgene_ahci_restart_engine - Restart the dma engine.
 * @ap : ATA port of interest
 *
 * Restarts the dma engine inside the controller.
 */
static int xgene_ahci_restart_engine(struct ata_port *ap)
{
	struct ahci_host_priv *hpriv = ap->host->private_data;
	struct ahci_port_priv *pp = ap->private_data;
	void __iomem *port_mmio = ahci_port_base(ap);
	u32 fbs;

	/* wait for multiple outstanding commands to complete */	
	if (xgene_ahci_wait_for_multiple_qc_complete(ap, port_mmio + 
				                 PORT_CMD_ISSUE, 0x0, 1, 100))
		  return -EBUSY;

	ahci_stop_engine(ap);
	ahci_start_fis_rx(ap);
	if (pp->fbs_supported) {
		fbs = readl(port_mmio + PORT_FBS);
		writel(fbs | PORT_FBS_EN, port_mmio + PORT_FBS);
		fbs = readl(port_mmio + PORT_FBS);
	}	

	hpriv->start_engine(ap);

	return 0;
}

/**
 * xgene_ahci_qc_issue - Issue commands to the device
 * @qc: Command to issue
 *
 * Due to Hardware errata for IDENTIFY DEVICE command, the controller cannot
 * clear the BSY bit after receiving the PIO setup FIS. This results in the dma
 * state machine goes into the CMFatalErrorUpdate state and locks up. By
 * restarting the dma engine, it removes the controller out of lock up state.
 */
static unsigned int xgene_ahci_qc_issue(struct ata_queued_cmd *qc)
{
	struct ata_port *ap = qc->ap;
	struct ahci_host_priv *hpriv = ap->host->private_data;
	struct xgene_ahci_context *ctx = hpriv->plat_data;
	int rc = 0;

	if (unlikely(ctx->last_cmd[ap->port_no] == ATA_CMD_ID_ATA))
#if defined(CONFIG_ARCH_MSLIM)		
		xgene_ahci_restart_engine_mslim(ap);
#else
		xgene_ahci_restart_engine(ap);
#endif		
	rc = ahci_qc_issue(qc);

	/* Save the last command issued */
	ctx->last_cmd[ap->port_no] = qc->tf.command;

	return rc;
}

/**
 * xgene_ahci_read_id - Read ID data from the specified device
 * @dev: device
 * @tf: proposed taskfile
 * @id: data buffer
 *
 * This custom read ID function is required due to the fact that the HW
 * does not support DEVSLP. 
 */
static unsigned int xgene_ahci_read_id(struct ata_device *dev,
				       struct ata_taskfile *tf, u16 *id)
{
	u32 err_mask;

	err_mask = ata_do_dev_read_id(dev, tf, id);
	if (err_mask)
		return err_mask;

	/*
	 * Mask reserved area. Word78 spec of Link Power Management
	 * bit15-8: reserved
	 * bit7: NCQ autosence
	 * bit6: Software settings preservation supported
	 * bit5: reserved
	 * bit4: In-order sata delivery supported
	 * bit3: DIPM requests supported
	 * bit2: DMA Setup FIS Auto-Activate optimization supported
	 * bit1: DMA Setup FIX non-Zero buffer offsets supported
	 * bit0: Reserved
	 *
	 * Clear reserved bit 8 (DEVSLP bit) as we don't support DEVSLP
	 */
	id[ATA_ID_FEATURE_SUPP] &= ~(1 << 8);

	return 0;
}

static void xgene_ahci_set_phy_cfg(struct xgene_ahci_context *ctx, int channel)
{
	void __iomem *mmio = ctx->hpriv->mmio;
	u32 val;

	dev_dbg(ctx->dev, "port configure mmio 0x%p channel %d\n",
		mmio, channel);
	val = readl(mmio + PORTCFG);
	val = PORTADDR_SET(val, channel == 0 ? 2 : 3);
	writel(val, mmio + PORTCFG);
	readl(mmio + PORTCFG);  /* Force a barrier */
	/* Disable fix rate */
	writel(0x0001fffe, mmio + PORTPHY1CFG);
	readl(mmio + PORTPHY1CFG); /* Force a barrier */
	writel(0x28183219, mmio + PORTPHY2CFG);
	readl(mmio + PORTPHY2CFG); /* Force a barrier */
	writel(0x13081008, mmio + PORTPHY3CFG);
	readl(mmio + PORTPHY3CFG); /* Force a barrier */
	writel(0x00480815, mmio + PORTPHY4CFG);
	readl(mmio + PORTPHY4CFG); /* Force a barrier */
	/* Set window negotiation */
	val = readl(mmio + PORTPHY5CFG);
	val = PORTPHY5CFG_RTCHG_SET(val, 0x300);
	writel(val, mmio + PORTPHY5CFG);
	readl(mmio + PORTPHY5CFG); /* Force a barrier */
	val = readl(mmio + PORTAXICFG);
	val = PORTAXICFG_EN_CONTEXT_SET(val, 0x1); /* Enable context mgmt */
	val = PORTAXICFG_OUTTRANS_SET(val, 0xe); /* Set outstanding */
	writel(val, mmio + PORTAXICFG);
	readl(mmio + PORTAXICFG); /* Force a barrier */
	val = readl(mmio + PTC);
	val = PTC_RXWM_SET(val, 0x30);
	writel(val, mmio + PTC);
}

static void xgene_ahci_force_port_phy_rdy(struct xgene_ahci_context *ctx,
				     int channel, int force)
{
	void __iomem *mmio = ctx->hpriv->mmio;
	u32 val;

	val = readl(mmio + PORTCFG);
	val = PORTADDR_SET(val, channel == 0 ? 2 : 3);
	writel(val, mmio + PORTCFG);
	readl(mmio + PORTCFG);	/* Force a barrier */
	val = readl(mmio + PORTPHY1CFG);
	val = PORTPHY1CFG_FRCPHYRDY_SET(val, force);
	writel(val, mmio + PORTPHY1CFG);
}

static void xgene_ahci_port_phy_restart(struct ata_link *link)
{
	struct ata_port *port = link->ap;
	struct ahci_host_priv *hpriv = port->host->private_data;
	struct xgene_ahci_context *ctx = hpriv->plat_data;

	xgene_ahci_force_port_phy_rdy(ctx, port->port_no, 1);
	xgene_ahci_force_port_phy_rdy(ctx, port->port_no, 0);
}

static void xgene_ahci_phy_reset_rxd(struct xgene_ahci_context *ctx, int lane)
{
	/* Reset digital Rx */
	ahci_serdes_clrbits(ctx, lane, RXTX_REG7, RXTX_REG7_RESETB_RXD_MASK);
	/* As per PHY design spec, the reset requires a minimum of 100us. */
	usleep_range(100, 150);
	ahci_serdes_setbits(ctx, lane, RXTX_REG7, RXTX_REG7_RESETB_RXD_MASK);
}


/**
 * xgene_ahci_do_hardreset - Issue the actual COMRESET
 * @link: link to reset
 * @deadline: deadline jiffies for the operation
 * @online: Return value to indicate if device online
 *
 * Due to the limitation of the hardware PHY, a difference set of setting is
 * required for each supported disk speed - Gen3 (6.0Gbps), Gen2 (3.0Gbps),
 * and Gen1 (1.5Gbps). Otherwise during long IO stress test, the PHY will
 * report disparity error and etc. In addition, during COMRESET, there can
 * be error reported in the register PORT_SCR_ERR. For SERR_DISPARITY and
 * SERR_10B_8B_ERR, the PHY receiver line must be reseted. 
 * The following algorithm is followed to proper configure the hardware 
 * PHY during COMRESET:
 *
 * Alg Part 1:
 * 1. Start the PHY at Gen3 speed (default setting)
 * 2. Issue the COMRESET
 * 3. If no link, go to Alg Part 3
 * 4. If link up, determine if the negotiated speed matches the PHY
 *    configured speed
 * 5. If they matched, go to Alg Part 2
 * 6. If they do not matched and first time, configure the PHY for the linked
 *    up disk speed and repeat step 2
 * 7. Go to Alg Part 2
 *
 * Alg Part 2:
 * 1. On link up, if there are any SERR_DISPARITY and SERR_10B_8B_ERR error
 *    reported in the register PORT_SCR_ERR, then reset the PHY receiver line
 * 2. Go to Alg Part 3
 *
 * Alg Part 3:
 * 1. Clear any pending from register PORT_SCR_ERR.
 *
 * NOTE: For the initial version, Until the underlying PHY supports 
 * 	 an method to reset the receiver line, on detection of 
 * 	 SERR_DISPARITY or SERR_10B_8B_ERR errors,an warning message 
 * 	 will be printed. 	 
 */
static int xgene_ahci_do_hardreset(struct ata_link *link,
				   unsigned long deadline, bool *online)
{
	const unsigned long *timing = sata_ehc_deb_timing(&link->eh_context);
	struct ata_port *ap = link->ap;
	struct ahci_host_priv *hpriv = ap->host->private_data;
	struct xgene_ahci_context *ctx = hpriv->plat_data;
	struct ahci_port_priv *pp = ap->private_data;
	u8 *d2h_fis = pp->rx_fis + RX_FIS_D2H_REG;
	void __iomem *port_mmio = ahci_port_base(ap);
	struct ata_taskfile tf;
	#define MAX_LINK_DOWN_RETRY 4
	int link_down_retry = 0;
	int rc;
	u32 val;
	int i;

hardreset_retry:
	/* clear D2H reception area to properly wait for D2H FIS */
	ata_tf_init(link->device, &tf);
	tf.command = ATA_BUSY;
	ata_tf_to_fis(&tf, 0, 0, d2h_fis);
	
	rc = sata_link_hardreset(link, timing, deadline, online,
				 ahci_check_ready);
	if (*online && xgene_ahci_is_preB0()) {
	/* Clear SER_DISPARITY/SER_10B_8B_ERR if set due to errata */
		for (i = 0; i < 5; i++) {
			/* Check if error bit set */
			val = readl(port_mmio + PORT_SCR_ERR);
			if (!(val & (SERR_DISPARITY | SERR_10B_8B_ERR)))
				break;
			/* Clear any error due to errata */
			xgene_ahci_force_port_phy_rdy(ctx, ap->port_no, 1);
			/* Reset the PHY Rx path */
			xgene_ahci_phy_reset_rxd(ctx, ap->port_no);	
			xgene_ahci_force_port_phy_rdy(ctx, ap->port_no, 0);
			/* Clear all errors */
			val = readl(port_mmio + PORT_SCR_ERR);
			writel(val, port_mmio + PORT_SCR_ERR);
		}
	} 
	
	if (!*online) {
		if (link_down_retry++ < MAX_LINK_DOWN_RETRY) 
			 goto hardreset_retry;
	}
	
	val = readl(port_mmio + PORT_SCR_ERR);
	if (val & (SERR_DISPARITY | SERR_10B_8B_ERR))
		dev_warn(ctx->dev, "link has error\n");

	/* clear all errors if any pending */
	val = readl(port_mmio + PORT_SCR_ERR);
	writel(val, port_mmio + PORT_SCR_ERR);

	return rc;
}

/* Custom ahci_hardreset  
 *
 * Due to HW errata, the phy has different transmitt boost parameters for ssd 
 * drives and HDD drives. So we need to set the transmitt boost paramters for    
 * the ssd and hdd drives after the COMRESET sequence. We need to retry the
 * COMRESET sequence because the phy reports link down at one shot.
 */
static int xgene_ahci_hardreset(struct ata_link *link, unsigned int *class,
				unsigned long deadline)
{
	struct ata_port *ap = link->ap;
        struct ahci_host_priv *hpriv = ap->host->private_data;
	void __iomem *port_mmio = ahci_port_base(ap);
	bool online;
	int rc;
	u32 portcmd_saved = 0;
	u32 portclb_saved = 0;
	u32 portclbhi_saved = 0;
	u32 portrxfis_saved = 0;
	u32 portrxfishi_saved = 0;

	if (xgene_ahci_is_preB0()) {
		/* As hardreset resets these CSR, save it to restore later */
		portcmd_saved = readl(port_mmio + PORT_CMD);
		portclb_saved = readl(port_mmio + PORT_LST_ADDR);
		portclbhi_saved = readl(port_mmio + PORT_LST_ADDR_HI);
		portrxfis_saved = readl(port_mmio + PORT_FIS_ADDR);
		portrxfishi_saved = readl(port_mmio + PORT_FIS_ADDR_HI);
	}	

	ahci_stop_engine(ap);

	rc = xgene_ahci_do_hardreset(link, deadline, &online);

	if (xgene_ahci_is_preB0()) {
		/* As controller hardreset clears them, restore them */
		writel(portcmd_saved, port_mmio + PORT_CMD);
		writel(portclb_saved, port_mmio + PORT_LST_ADDR);
		writel(portclbhi_saved, port_mmio + PORT_LST_ADDR_HI);
		writel(portrxfis_saved, port_mmio + PORT_FIS_ADDR);
		writel(portrxfishi_saved, port_mmio + PORT_FIS_ADDR_HI);
	}

	hpriv->start_engine(ap);

	if (online)
		*class = ahci_dev_classify(ap);

	return rc;
}

static void xgene_ahci_host_stop(struct ata_host *host)
{
	struct ahci_host_priv *hpriv = host->private_data;

	ahci_platform_disable_resources(hpriv);
}

void xgene_ahci_disable_ctx(struct ahci_host_priv *hpriv, int channel) 
{
	void __iomem *mmio = hpriv->mmio;
	u32 val;

	val = readl(mmio + PORTCFG);
	val = PORTADDR_SET(val, channel == 0 ? 2 : 3);
	writel(val, mmio + PORTCFG);
	readl(mmio + PORTCFG);  
	val = readl(mmio + PORTAXICFG);
	val = PORTAXICFG_EN_CONTEXT_SET(val, 0x0); /* Disable context mgmt */
	writel(val, mmio + PORTAXICFG);
}

static int xgene_ahci_softreset(struct ata_link *link, unsigned int *class,
			  unsigned long deadline)
{
	int pmp = sata_srst_pmp(link);
	struct ata_port *ap = link->ap;
	struct ahci_host_priv *hpriv = ap->host->private_data;
	u32 rc;

	rc = ahci_do_softreset(link, class, pmp, deadline, ahci_check_ready);
	
	if (*class == ATA_DEV_PMP) {
		/* Disable context manager */
		xgene_ahci_disable_ctx(hpriv, ap->port_no);
	}

	return rc;
}

static struct ata_port_operations xgene_ahci_ops = {
	.inherits = &ahci_ops,
	.host_stop = xgene_ahci_host_stop,
	.hardreset = xgene_ahci_hardreset,
	.read_id = xgene_ahci_read_id,
	.qc_issue = xgene_ahci_qc_issue,
#if defined(CONFIG_ARCH_MSLIM)
	.port_start = xgene_ahci_port_start,
	.port_resume = xgene_ahci_port_resume,
	.qc_prep = xgene_ahci_qc_prep_mslim,
	.softreset = xgene_ahci_softreset_mslim,
	.pmp_softreset = xgene_ahci_softreset_mslim,
#else	
	.softreset = xgene_ahci_softreset,
#endif
};
static const struct ata_port_info xgene_ahci_port_info = {
	AHCI_HFLAGS(AHCI_HFLAG_NO_NCQ | AHCI_HFLAG_BROKEN_FIS_ON | 
		    AHCI_HFLAG_BROKEN_PMP_FIELD | AHCI_HFLAG_YES_FBS),
	.flags = AHCI_FLAG_COMMON | ATA_FLAG_PMP,	
	.pio_mask = ATA_PIO4,
	.udma_mask = ATA_UDMA6,
	.port_ops = &xgene_ahci_ops,
};

static int xgene_ahci_hw_init(struct ahci_host_priv *hpriv)
{
	struct xgene_ahci_context *ctx = hpriv->plat_data;
	int i;
	int rc;
	u32 val;

	/* Remove IP RAM out of shutdown */
	rc = xgene_ahci_init_memram(ctx);
	if (rc)
		return rc;

	for (i = 0; i < MAX_AHCI_CHN_PERCTR; i++)
		xgene_ahci_set_phy_cfg(ctx, i);

	/* AXI disable Mask */
	writel(0xffffffff, hpriv->mmio + HOST_IRQ_STAT);
	readl(hpriv->mmio + HOST_IRQ_STAT); /* Force a barrier */
	writel(0, ctx->csr_core + INTSTATUSMASK);
	val = readl(ctx->csr_core + INTSTATUSMASK); /* Force a barrier */
	dev_dbg(ctx->dev, "top level interrupt mask 0x%X value 0x%08X\n",
		INTSTATUSMASK, val);

	writel(0x0, ctx->csr_core + ERRINTSTATUSMASK);
	readl(ctx->csr_core + ERRINTSTATUSMASK); /* Force a barrier */
	writel(0x0, ctx->csr_axi + INT_SLV_TMOMASK);
	readl(ctx->csr_axi + INT_SLV_TMOMASK);

	/* Enable AXI Interrupt */
	writel(0xffffffff, ctx->csr_core + SLVRDERRATTRIBUTES);
	writel(0xffffffff, ctx->csr_core + SLVWRERRATTRIBUTES);
	writel(0xffffffff, ctx->csr_core + MSTRDERRATTRIBUTES);
	writel(0xffffffff, ctx->csr_core + MSTWRERRATTRIBUTES);

	/* Enable coherency */
#if !defined(CONFIG_ARCH_MSLIM)	
	val = readl(ctx->csr_core + BUSCTLREG);
	val &= ~0x00000002;     /* Enable write coherency */
	val &= ~0x00000001;     /* Enable read coherency */
	writel(val, ctx->csr_core + BUSCTLREG);

	val = readl(ctx->csr_core + IOFMSTRWAUX);
	val |= (1 << 3);        /* Enable read coherency */
	val |= (1 << 9);        /* Enable write coherency */
	writel(val, ctx->csr_core + IOFMSTRWAUX);
	val = readl(ctx->csr_core + IOFMSTRWAUX);
	dev_dbg(ctx->dev, "coherency 0x%X value 0x%08X\n",
		IOFMSTRWAUX, val);
#endif
	return rc;
}

static int xgene_ahci_mux_select(struct xgene_ahci_context *ctx)
{
	u32 val;

	/* Check for optional MUX resource */
	if (IS_ERR(ctx->csr_mux))
		return 0;

	val = readl(ctx->csr_mux + SATA_ENET_CONFIG_REG);
	val &= ~CFG_SATA_ENET_SELECT_MASK;
	writel(val, ctx->csr_mux + SATA_ENET_CONFIG_REG);
	val = readl(ctx->csr_mux + SATA_ENET_CONFIG_REG);
	return val & CFG_SATA_ENET_SELECT_MASK ? -1 : 0;
}

static const struct acpi_device_id xgene_ahci_acpi_match[] = {
	{"APMC0D00", XGENE_ACHI_SGMII_ACPI},
	{"APMC0D0D", XGENE_ACHI_SGMII_ACPI},
	{"APMC0D09", XGENE_ACHI_PCIE_ACPI},
	{},
};
MODULE_DEVICE_TABLE(acpi, xgene_ahci_acpi_match);

static int xgene_ahci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ahci_host_priv *hpriv;
	struct xgene_ahci_context *ctx;
	struct resource *res;
	int rc;

	/*
	 * When both ACPI and DTS are enabled, custom ACPI built-in ACPI
	 * table, and booting via DTS, we need to skip the probe of the
	 * built-in ACPI table probe.
	 */
	if (!efi_enabled(EFI_BOOT) && dev->of_node == NULL)
		return -ENODEV;

	hpriv = ahci_platform_get_resources(pdev);
	if (IS_ERR(hpriv))
		return PTR_ERR(hpriv);

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	hpriv->plat_data = ctx;
	ctx->hpriv = hpriv;
	ctx->dev = dev;

	/* Retrieve the IP core resource */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	ctx->csr_core = devm_ioremap_resource(dev, res);
	if (IS_ERR(ctx->csr_core))
		return PTR_ERR(ctx->csr_core);

	/* Retrieve the IP diagnostic resource */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	ctx->csr_diag = devm_ioremap_resource(dev, res);
	if (IS_ERR(ctx->csr_diag))
		return PTR_ERR(ctx->csr_diag);

	/* Retrieve the IP AXI resource */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 3);
	ctx->csr_axi = devm_ioremap_resource(dev, res);
	if (IS_ERR(ctx->csr_axi))
		return PTR_ERR(ctx->csr_axi);

	/* Retrive the serdes csr resource */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 4);
	ctx->csr_sds = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(ctx->csr_sds))
		return PTR_ERR(ctx->csr_sds);
	
	/* Retrieve the optional IP mux resource */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 5);
	ctx->csr_mux = devm_ioremap_resource(dev, res);
	
	dev_dbg(dev, "VAddr 0x%p Mmio VAddr 0x%p\n", ctx->csr_core,
		hpriv->mmio);

	if ((rc = xgene_ahci_mux_select(ctx))) {
		dev_err(dev, "SATA mux selection failed error %d\n", rc);
		return -ENODEV;
	}

	if (xgene_ahci_is_memram_inited(ctx)) {
		dev_info(dev, "skip clock and PHY initialization\n");
		goto skip_clk_phy;
	}

	/* Due to errata, HW requires full toggle transition */
	rc = ahci_platform_enable_clks(hpriv);
	if (rc) {
		goto disable_resources;
	}
	ahci_platform_disable_clks(hpriv);

	rc = ahci_platform_enable_resources(hpriv);
	if (rc) 
		goto disable_resources;

	/* Configure the host controller */
	xgene_ahci_hw_init(hpriv);

skip_clk_phy:	
	/*
	 * Setup DMA mask. This is preliminary until the DMA range is sorted
	 * out.
	 */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3, 12, 0)
	pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;
	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(64);
#else
	/* Setup DMA mask - 32 for 32-bit system and 64 for 64-bit system */
	rc = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(8*sizeof(void *)));
	if (rc) {
		dev_err(dev, "Unable to set dma mask\n");
		goto disable_resources;
	}
#endif
	rc = ahci_platform_init_host(pdev, hpriv, &xgene_ahci_port_info, 0, 0);
	if (rc)
		goto disable_resources;

	dev_dbg(dev, "X-Gene SATA host controller initialized\n");
	return 0;

disable_resources:
	ahci_platform_disable_resources(hpriv);
	return rc;
}

static const struct of_device_id xgene_ahci_of_match[] = {
	{.compatible = "apm,xgene-ahci"},
	{ }
};
MODULE_DEVICE_TABLE(of, xgene_ahci_of_match);

static struct platform_driver xgene_ahci_driver = {
	.probe = xgene_ahci_probe,
	.remove = ata_platform_remove_one,
	.driver = {
		.name = "xgene-ahci",
		.owner = THIS_MODULE,
		.of_match_table = xgene_ahci_of_match,
		.acpi_match_table = ACPI_PTR(xgene_ahci_acpi_match),
	},
};

module_platform_driver(xgene_ahci_driver);

MODULE_DESCRIPTION("APM X-Gene AHCI SATA driver");
MODULE_AUTHOR("Loc Ho <lho@apm.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.4");
