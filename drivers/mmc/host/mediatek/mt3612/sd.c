/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

 /**
 * @file sd.c
 * The sd.c file is driver main function for eMMC host controller.
 */

#ifdef pr_fmt
#undef pr_fmt
#endif

#define pr_fmt(fmt) "["KBUILD_MODNAME"]" fmt

#include <generated/autoconf.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/irq.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/printk.h>
#include <asm/page.h>
#include <linux/gpio.h>
/* #include <mt-plat/mtk_boot.h> */
/* #include <mt-plat/mtk_lpae.h> */

#include "mtk_sd.h"
#include <mmc/core/core.h>
#include <mmc/card/queue.h>

#ifdef CONFIG_MTK_HIBERNATION
#include <mtk_hibernate_dpm.h>
#endif

#ifdef CONFIG_PWR_LOSS_MTK_TEST
#include <mach/power_loss_test.h>
#else
#define MVG_EMMC_CHECK_BUSY_AND_RESET(...)
#define MVG_EMMC_SETUP(...)
#define MVG_EMMC_RESET(...)
#define MVG_EMMC_WRITE_MATCH(...)
#define MVG_EMMC_ERASE_MATCH(...)
#define MVG_EMMC_ERASE_RESET(...)
#define MVG_EMMC_DECLARE_INT32(...)
#endif

#include "dbg.h"
#include "msdc_tune.h"

#define CAPACITY_2G             (2 * 1024 * 1024 * 1024ULL)

/* FIX ME: Check if its reference in mtk_sd_misc.h can be removed */
u32 g_emmc_mode_switch;
/** @ingroup type_group_linux_eMMC_def
 * @brief eMMC Flush definations
 * @{
 */
/** max flush count */
#define MSDC_MAX_FLUSH_COUNT    (3)
/** define for unflushed */
#define CACHE_UN_FLUSHED        (0)
/** define for flushed */
#define CACHE_FLUSHED           (1)
/**
 * @}
 */

#ifdef MTK_MSDC_USE_CACHE
static unsigned int g_cache_status = CACHE_UN_FLUSHED;
static unsigned long long g_flush_data_size;
static unsigned int g_flush_error_count;
static int g_flush_error_happened;
#endif

/* if eMMC cache of specific vendor shall be disabled,
 * fill CID.MID into g_emmc_cache_quirk[]
 * exmple:
 * g_emmc_cache_quirk[0] = CID_MANFID_HYNIX;
 * g_emmc_cache_quirk[1] = CID_MANFID_SAMSUNG;
 */
unsigned char g_emmc_cache_quirk[256];
/** @ingroup type_group_linux_eMMC_def
 * @brief eMMC device manu ID definitions
 * @{
 */
/** Sandisk */
#define CID_MANFID_SANDISK		0x2
/** TOSHIBA */
#define CID_MANFID_TOSHIBA		0x11
/** MICRON */
#define CID_MANFID_MICRON		0x13
/** Samsung */
#define CID_MANFID_SAMSUNG		0x15
/** Sandisk new */
#define CID_MANFID_SANDISK_NEW		0x45
/** Hynix */
#define CID_MANFID_HYNIX		0x90
/** KSI */
#define CID_MANFID_KSI			0x70
/**
 * @}
 */

static u16 u_sdio_irq_counter;
static u16 u_msdc_irq_counter;

struct msdc_host *ghost;
int src_clk_control;

bool emmc_sleep_failed;
static struct workqueue_struct *wq_init;

#ifdef ENABLE_FOR_MSDC_KERNEL44
static bool sdio_use_dvfs;
#endif

#define DRV_NAME                "mtk-msdc"

#define MSDC_COOKIE_PIO         (1<<0)
#define MSDC_COOKIE_ASYNC       (1<<1)

#define msdc_use_async(x)       (x & MSDC_COOKIE_ASYNC)
#define msdc_use_async_dma(x)   (msdc_use_async(x) && (!(x & MSDC_COOKIE_PIO)))
#define msdc_use_async_pio(x)   (msdc_use_async(x) && ((x & MSDC_COOKIE_PIO)))

u8 g_emmc_id;
unsigned int cd_gpio;

int msdc_rsp[] = {
	0,                      /* RESP_NONE */
	1,                      /* RESP_R1 */
	2,                      /* RESP_R2 */
	3,                      /* RESP_R3 */
	4,                      /* RESP_R4 */
	1,                      /* RESP_R5 */
	1,                      /* RESP_R6 */
	1,                      /* RESP_R7 */
	7,                      /* RESP_R1b */
};

/* For Inhanced DMA */
#define msdc_init_gpd_ex(gpd, extlen, cmd, arg, blknum) \
	do { \
		((struct gpd_t *)gpd)->extlen = extlen; \
		((struct gpd_t *)gpd)->cmd    = cmd; \
		((struct gpd_t *)gpd)->arg    = arg; \
		((struct gpd_t *)gpd)->blknum = blknum; \
	} while (0)

#define msdc_init_bd(bd, blkpad, dwpad, dptr, dlen) \
	do { \
		WARN_ON(dlen > 0xFFFFFFUL); \
		((struct bd_t *)bd)->blkpad = blkpad; \
		((struct bd_t *)bd)->dwpad = dwpad; \
		((struct bd_t *)bd)->ptrh4 = (u32)((dptr >> 32) & 0xF); \
		((struct bd_t *)bd)->ptr = (u32)(dptr & 0xFFFFFFFFUL); \
		((struct bd_t *)bd)->buflen = dlen; \
	} while (0)

#ifdef CONFIG_NEED_SG_DMA_LENGTH
#define msdc_sg_len(sg, dma)    ((dma) ? (sg)->dma_length : (sg)->length)
#else
#define msdc_sg_len(sg, dma)    sg_dma_len(sg)
#endif

#define msdc_dma_on()           MSDC_CLR_BIT32(MSDC_CFG, MSDC_CFG_PIO)
#define msdc_dma_off()          MSDC_SET_BIT32(MSDC_CFG, MSDC_CFG_PIO)

/***************************************************************
* BEGIN register dump functions
***************************************************************/
#define PRINTF_REGISTER_BUFFER_SIZE 512
#define ONE_REGISTER_STRING_SIZE    14
#define MSDC_XTS_ENABLE
#ifdef MSDC_XTS_ENABLE
u8 aes_switch_on;
#endif
/**********************************************************
* AutoK Basic Interface Implenment                        *
**********************************************************/
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for iniital aes engine
 * @param *host: msdc host manipulator pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
#ifdef MSDC_XTS_ENABLE
static void msdc_xts_init(struct msdc_host *host)
{
	void __iomem *base = host->base;

	MSDC_WRITE32(MSDC_AES_CFG_GP1, AES_DATA_UNIT_SIZE_512_BYTE |
		     AES_KEY_SIZE_128_BIT | AES_MODE_XTS);
	aes_switch_on = 1;

}
#endif

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for aes flow control
 * @param *host: msdc host manipulator pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void aes_engine_ctrl(struct msdc_host *host, uint32_t opcode)
{
#ifdef MSDC_XTS_ENABLE
	void __iomem *base = host->base;
	uint32_t aes_sw_bak;

	if (opcode == MMC_WRITE_MULTIPLE_BLOCK ||
	    opcode == MMC_WRITE_BLOCK ||
	    opcode == MMC_READ_MULTIPLE_BLOCK ||
	    opcode == MMC_READ_SINGLE_BLOCK) {
		if (aes_switch_on)
			MSDC_SET_FIELD(MSDC_AES_SWST, AES_BYPASS, 0);
	}

	if ((MSDC_READ32(MSDC_AES_EN) & AES_EN) &&
	   !(MSDC_READ32(MSDC_AES_SWST) & AES_BYPASS)) {
		aes_sw_bak = MSDC_READ32(MSDC_AES_SWST);
		aes_sw_bak &= 0xFFFFFFFC;
		if (opcode == MMC_WRITE_MULTIPLE_BLOCK ||
		    opcode == MMC_WRITE_BLOCK) {
			MSDC_WRITE32(MSDC_AES_SWST,
				     (aes_sw_bak | AES_SWITCH_START_ENC));

			if (!(aes_sw_bak & AES_BYPASS))
				while ((MSDC_READ32(MSDC_AES_SWST) &
					0xfffffffc))
					;
		} else if (opcode == MMC_READ_MULTIPLE_BLOCK ||
			   opcode == MMC_READ_SINGLE_BLOCK) {
			MSDC_WRITE32(MSDC_AES_SWST,
				     (aes_sw_bak | AES_SWITCH_START_DEC));

			if (!(aes_sw_bak & AES_BYPASS))
				while ((MSDC_READ32(MSDC_AES_SWST) &
					0xfffffffc))
					;
		}
	}
#endif
}

/** @ingroup type_group_linux_eMMC_ExtFn
 * @par Description
 *     Function for control AES engine ON or OFF
 * @param *host: msdc host manipulator pointer
 * @param on:
 *     0: AES off, 1: AES On
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
void msdc_aes_control(struct msdc_host *host, u8 on)
{
#ifdef MSDC_XTS_ENABLE
	if (on)
		aes_switch_on = 1;
	else
		aes_switch_on = 0;
#endif
}
EXPORT_SYMBOL(msdc_aes_control);

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for disable xts function
 * @param *host: msdc host manipulator pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
void msdc_xts_disable(struct msdc_host *host)
{
#ifdef MSDC_XTS_ENABLE
	void __iomem *base = host->base;

	MSDC_SET_FIELD(MSDC_AES_SWST, AES_BYPASS, 1);
#endif
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for dump register value for debug
 * @param *host: msdc host manipulator pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_dump_register(struct msdc_host *host)
{
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for dump debug register value
 * @param *host: msdc host manipulator pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_dump_dbg_register(struct msdc_host *host)
{
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for dump necessary information for debug
 * @param id: msdc host id
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
void msdc_dump_info(u32 id)
{
	struct msdc_host *host = mtk_msdc_host[id];

	if (host == NULL) {
		pr_err("msdc host<%d> null\n", id);
		return;
	}

	if (host->tuning_in_progress == true)
		return;

	/* when detect card, timeout log is not needed */
	if (!sd_register_zone[id])
		return;

	host->prev_cmd_cause_dump++;
	if (host->prev_cmd_cause_dump > 1)
		return;

	msdc_dump_register(host);
	INIT_MSG("latest_INT_status<0x%.8x>", latest_int_status[id]);

	mdelay(10);
	msdc_dump_dbg_register(host);
}
/***************************************************************
* END register dump functions
***************************************************************/

/*
 * for AHB read / write debug
 * return DMA status.
 */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for get data access mode(PIO,DMA)
 * @param id: msdc host id
 * @return
 *     0: PIO mode \n
 *     1: DMA read \n
 *     2: DMA write \n
 *   -1: Error
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
int msdc_get_dma_status(int host_id)
{
	if (host_id < 0 || host_id >= HOST_MAX_NUM) {
		pr_err("[%s] failed to get dma status, invalid host_id %d\n",
			__func__, host_id);
	} else if (msdc_latest_transfer_mode[host_id] == MODE_DMA) {
		if (msdc_latest_op[host_id] == OPER_TYPE_READ)
			return 1;       /* DMA read */
		else if (msdc_latest_op[host_id] == OPER_TYPE_WRITE)
			return 2;       /* DMA write */
	} else if (msdc_latest_transfer_mode[host_id] == MODE_PIO) {
		return 0;               /* PIO mode */
	}

	return -1;
}
EXPORT_SYMBOL(msdc_get_dma_status);

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for clearing FIFO
 * @param id: msdc host id
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
void msdc_clr_fifo(unsigned int id)
{
	int retry = 3, cnt = 1000;
	void __iomem *base;

	if (id >= HOST_MAX_NUM)
		return;
	base = mtk_msdc_host[id]->base;

	if (MSDC_READ32(MSDC_DMA_CFG) & MSDC_DMA_CFG_STS) {
		pr_err("<<<WARN>>>: msdc%d, clear FIFO when DMA active, MSDC_DMA_CFG=0x%x\n",
			id, MSDC_READ32(MSDC_DMA_CFG));
		show_stack(current, NULL);
		MSDC_SET_FIELD(MSDC_DMA_CTRL, MSDC_DMA_CTRL_STOP, 1);
		msdc_retry((MSDC_READ32(MSDC_DMA_CFG) & MSDC_DMA_CFG_STS),
			retry, cnt, id);
		if (retry == 0) {
			pr_err("<<<WARN>>>: msdc%d, faield to stop DMA before clear FIFO, MSDC_DMA_CFG=0x%x\n",
				id, MSDC_READ32(MSDC_DMA_CFG));
			return;
		}
	}

	retry = 3;
	cnt = 1000;
	MSDC_SET_BIT32(MSDC_FIFOCS, MSDC_FIFOCS_CLR);
	msdc_retry(MSDC_READ32(MSDC_FIFOCS) & MSDC_FIFOCS_CLR, retry, cnt, id);
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for set sclk rate and wait clock stable
 * @param *host: msdc host manipulator pointer
 * @param mode: clock mode
 * @param div: clock divider setting
 * @param hs400_div_dis: HS400 clock divider mode enable/disable
 * @return
 *     Always 0
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
int msdc_clk_stable(struct msdc_host *host, u32 mode, u32 div,
	u32 hs400_div_dis)
{
	void __iomem *base = host->base;
	int retry = 0;
	int cnt = 1000;
	int retry_cnt = 1;
	int ret;

	do {
		retry = 3;
		MSDC_SET_FIELD(MSDC_CFG,
			MSDC_CFG_CKMOD_HS400 | MSDC_CFG_CKMOD | MSDC_CFG_CKDIV,
			(hs400_div_dis << 14) | (mode << 12) |
				((div + retry_cnt) % 0xfff));
		/* MSDC_SET_FIELD(MSDC_CFG, MSDC_CFG_CKMOD, mode); */
		msdc_retry(!(MSDC_READ32(MSDC_CFG) & MSDC_CFG_CKSTB), retry,
			cnt, host->id);
		if (retry == 0) {
			pr_err("msdc%d host->onclock(%d)\n", host->id,
				host->core_clkon);
			pr_err("msdc%d on clock failed ===> retry twice\n",
				host->id);

			/* msdc_clk_disable(host); */
			ret = msdc_clk_enable(host);
			if (ret < 0)
				pr_err("msdc%d enable clock fail\n", host->id);

			msdc_dump_info(host->id);
			host->prev_cmd_cause_dump = 0;
		}
		retry = 3;
		MSDC_SET_FIELD(MSDC_CFG, MSDC_CFG_CKDIV, div);
		msdc_retry(!(MSDC_READ32(MSDC_CFG) & MSDC_CFG_CKSTB), retry,
			cnt, host->id);
		if (retry == 0)
			msdc_dump_info(host->id);
		msdc_reset_hw(host->id);
		if (retry_cnt == 2)
			break;
		retry_cnt++;
	} while (!retry);

	return 0;
}

#define msdc_irq_save(val) \
	do { \
		val = MSDC_READ32(MSDC_INTEN); \
		MSDC_CLR_BIT32(MSDC_INTEN, val); \
	} while (0)

#define msdc_irq_restore(val) \
	MSDC_SET_BIT32(MSDC_INTEN, val)

/* set the edge of data sampling */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for set the edge of data sampling
 * @param *host: msdc host manipulator pointer
 * @param clock_mode: latch clock mode selection
 * @param mode: smapling edge mode
 * @param type: pad mode/type select
 * @param *edge: sampling edge selection
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_set_smpl(struct msdc_host *host, u32 clock_mode, u8 mode,
		u8 type, u8 *edge)
{
	void __iomem *base = host->base;
	u32 mode1;

	switch (type) {
	case TYPE_CMD_RESP_EDGE:
		if (clock_mode == 3) {
			MSDC_SET_FIELD(EMMC50_CFG0,
				MSDC_EMMC50_CFG_PADCMD_LATCHCK, 0);
			MSDC_SET_FIELD(EMMC50_CFG0,
				MSDC_EMMC50_CFG_CMD_RESP_SEL, 0);
		#ifdef CONFIG_MACH_FPGA
			MSDC_SET_FIELD(EMMC50_CFG0,
				MSDC_EMMC50_CFG_CMD_EDGE_SEL, 1);
		#else
		  MSDC_SET_FIELD(EMMC50_CFG0,
				MSDC_EMMC50_CFG_CMD_EDGE_SEL, 0);
		#endif
		} else {
			MSDC_SET_FIELD(EMMC50_CFG0,
				MSDC_EMMC50_CFG_CMD_EDGE_SEL, 0);
		}

		if (mode == MSDC_SMPL_RISING || mode == MSDC_SMPL_FALLING) {
		#ifdef CONFIG_MACH_FPGA
			MSDC_SET_FIELD(MSDC_IOCON, MSDC_IOCON_RSPL, 0);
		#else
			MSDC_SET_FIELD(MSDC_IOCON, MSDC_IOCON_RSPL, mode);
		#endif
		} else {
			ERR_MSG("invalid resp parameter: type=%d, mode=%d\n",
				type, mode);
		}
		break;
	case TYPE_WRITE_CRC_EDGE:
		if (clock_mode == 3) {
			/*latch write crc status at DS pin*/
			MSDC_SET_FIELD(EMMC50_CFG0,
				MSDC_EMMC50_CFG_CRC_STS_SEL, 1);
		} else {
			/*latch write crc status at CLK pin*/
			MSDC_SET_FIELD(EMMC50_CFG0,
				MSDC_EMMC50_CFG_CRC_STS_SEL, 0);
		}
		if (mode == MSDC_SMPL_RISING || mode == MSDC_SMPL_FALLING) {
			if (clock_mode == 3) {
			#ifdef CONFIG_MACH_FPGA
				MSDC_SET_FIELD(EMMC50_CFG0,
					MSDC_EMMC50_CFG_CRC_STS_EDGE, 0);
			#else
				MSDC_SET_FIELD(EMMC50_CFG0,
					MSDC_EMMC50_CFG_CRC_STS_EDGE, mode);
			#endif
			} else {
			#ifdef CONFIG_MACH_FPGA
				MSDC_SET_FIELD(MSDC_PATCH_BIT2,
					MSDC_PB2_CFGCRCSTSEDGE, 0);
			#else
				MSDC_SET_FIELD(MSDC_PATCH_BIT2,
					MSDC_PB2_CFGCRCSTSEDGE, mode);
			#endif
			}
		} else {
			ERR_MSG("invalid crc parameter: type=%d, mode=%d\n",
				type, mode);
		}
		break;
	case TYPE_READ_DATA_EDGE:
		if (clock_mode == 3) {
			/* for HS400, start bit is output on both edge */
			#ifdef CONFIG_MACH_FPGA
			MSDC_SET_FIELD(EMMC50_PAD_DS_TUNE,
				 MSDC_EMMC50_PAD_DS_TUNE_DLY3, 0x04);
			MSDC_SET_FIELD(EMMC50_PAD_DS_TUNE,
				 MSDC_EMMC50_PAD_DS_TUNE_DLY2, 0);
			MSDC_SET_FIELD(EMMC50_PAD_DS_TUNE,
				 MSDC_EMMC50_PAD_DS_TUNE_DLY1, 1);
			MSDC_SET_FIELD(EMMC50_PAD_DS_TUNE,
				 MSDC_EMMC50_PAD_DS_TUNE_DLY2SEL, 0);
			MSDC_SET_FIELD(EMMC50_PAD_DS_TUNE,
				 MSDC_EMMC50_PAD_DS_TUNE_DLYSEL, 0);
			MSDC_SET_FIELD(EMMC50_CFG0,
				 MSDC_EMMC50_CFG_CRC_STS_SEL, 1);
			MSDC_SET_FIELD(EMMC50_CFG0,
					MSDC_EMMC50_CFG_CRC_STS_EDGE, 0);
			MSDC_SET_FIELD(MSDC_CFG, MSDC_CFG_START_BIT,
				START_AT_RISING);
			#else
			MSDC_SET_FIELD(MSDC_CFG, MSDC_CFG_START_BIT,
				START_AT_RISING_AND_FALLING);
			#endif
			MSDC_SET_FIELD(MSDC_IOCON, MSDC_IOCON_R_D_SMPL, 1);
			MSDC_SET_FIELD(MSDC_PATCH_BIT0,
				MSDC_PB0_RD_DAT_SEL, mode);
		} else {
			/* for the other modes, start bit is only output on
			 * rising edge; but DDR50 can try falling edge
			 * if error casued by pad delay
			 */
			MSDC_SET_FIELD(MSDC_CFG, MSDC_CFG_START_BIT,
				START_AT_RISING);
			if (host->mclk >= 1000000) {
				MSDC_SET_FIELD(MSDC_IOCON,
				 MSDC_IOCON_R_D_SMPL, 1);
				MSDC_SET_FIELD(MSDC_IOCON, MSDC_IOCON_RSPL, 1);
				MSDC_SET_FIELD(MSDC_PATCH_BIT0,
				MSDC_PB0_RD_DAT_SEL, 1);
			} else {
				MSDC_SET_FIELD(MSDC_IOCON,
				 MSDC_IOCON_R_D_SMPL, 0);
				MSDC_SET_FIELD(MSDC_IOCON, MSDC_IOCON_RSPL, 0);
				MSDC_SET_FIELD(MSDC_PATCH_BIT0,
				MSDC_PB0_RD_DAT_SEL, 0);
			}
			/* MSDC_SET_FIELD(MSDC_IOCON, MSDC_IOCON_RSPL, 0); */
			MSDC_SET_FIELD(EMMC50_CFG0,
				MSDC_EMMC50_CFG_CRC_STS_SEL, 0);
		}
		if (mode == MSDC_SMPL_RISING || mode == MSDC_SMPL_FALLING) {
			MSDC_SET_FIELD(MSDC_IOCON, MSDC_IOCON_R_D_SMPL_SEL, 0);
			/* MSDC_SET_FIELD(MSDC_PATCH_BIT0,
			*	MSDC_PB0_RD_DAT_SEL, mode);
			*/
			ERR_MSG("MSDC_PATCH_BIT0 MSDC_PB0_RD_DAT_SEL:mode=%d\n",
			 mode);
		} else {
			ERR_MSG("invalid read parameter: type=%d, mode=%d\n",
				type, mode);
		}
		MSDC_GET_FIELD(EMMC50_CFG0, MSDC_EMMC50_CFG_CRC_STS_SEL, mode1);
		ERR_MSG("==MSDC_EMMC50_CFG_CRC_STS_SEL=0x%x: ==\r\n", mode1);
		break;
	default:
		ERR_MSG("invalid parameter: type=%d, mode=%d\n", type, mode);
		break;
	}
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for set sampling edge for CMD/DAT pad
 * @param *host: msdc host manipulator pointer
 * @param clock_mode: latch clock mode selection
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_set_smpl_all(struct msdc_host *host, u32 clock_mode)
{
	msdc_set_smpl(host, clock_mode, host->hw->cmd_edge,
		TYPE_CMD_RESP_EDGE, NULL);
	msdc_set_smpl(host, clock_mode, host->hw->rdata_edge,
		TYPE_READ_DATA_EDGE, NULL);
	msdc_set_smpl(host, clock_mode, host->hw->wdata_edge,
		TYPE_WRITE_CRC_EDGE, NULL);
}

/* sd card change voltage wait time =
 * (1/freq) * SDC_VOL_CHG_CNT(default 0x145)
 */
#define msdc_set_vol_change_wait_count(count) \
	MSDC_SET_FIELD(SDC_VOL_CHG, SDC_VOL_CHG_CNT, (count))

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for enable/disable end bit check function
 * @param *host: msdc host manipulator pointer
 * @param enable: enable/disable end bit check
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_set_check_endbit(struct msdc_host *host, bool enable)
{
	void __iomem *base = host->base;

	if (enable) {
		MSDC_SET_BIT32(SDC_CMD_STS, SDC_CMD_STS_INDEX_CHECK);
		MSDC_SET_BIT32(SDC_CMD_STS, SDC_CMD_STS_ENDBIT_CHECK);
	} else {
		MSDC_CLR_BIT32(SDC_CMD_STS, SDC_CMD_STS_INDEX_CHECK);
		MSDC_CLR_BIT32(SDC_CMD_STS, SDC_CMD_STS_ENDBIT_CHECK);
	}
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for enable/disable end bit check function
 * @param *host: msdc host manipulator pointer
 * @param on: enable/disable sclk out
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_clksrc_onoff(struct msdc_host *host, u32 on)
{
	void __iomem *base = host->base;
	u32 div, mode, hs400_div_dis;
	/* int ret; */

	if ((on) && (host->core_clkon == 0)) {

		/* ret = msdc_clk_enable(host); */
		/* if (ret < 0) */
		/*	pr_err("msdc%d enable clock fail\n", host->id); */

		host->core_clkon = 1;
		udelay(10);

		MSDC_SET_FIELD(MSDC_CFG, MSDC_CFG_MODE, MSDC_SDMMC);

		MSDC_GET_FIELD(MSDC_CFG, MSDC_CFG_CKMOD, mode);
		MSDC_GET_FIELD(MSDC_CFG, MSDC_CFG_CKDIV, div);
		MSDC_GET_FIELD(MSDC_CFG, MSDC_CFG_CKMOD_HS400, hs400_div_dis);
		msdc_clk_stable(host, mode, div, hs400_div_dis);
	} else if ((!on) && (host->core_clkon == 1) &&
		 (!((host->hw->flags & MSDC_SDIO_IRQ) && src_clk_control))) {

		/* Disable DVFS handshake */
		if (is_card_sdio(host) || (host->hw->flags & MSDC_SDIO_IRQ)) {
#ifdef ENABLE_FOR_MSDC_KERNEL44
			if (sdio_use_dvfs == 1)
				spm_msdc_dvfs_setting(MSDC2_DVFS, 0);
#endif
		}

		/* MSDC_SET_FIELD(MSDC_CFG, MSDC_CFG_MODE, MSDC_MS); */

		/* msdc_clk_disable(host); */

		host->core_clkon = 0;

	}
}

/* host doesn't need the clock on */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for gating clock
 * @param *host: msdc host manipulator pointer
 * @param delay: delay time for clock gating
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_gate_clock(struct msdc_host *host, int delay)
{

}

/* host does need the clock on */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for releasing gated clock
 * @param *host: msdc host manipulator pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_ungate_clock(struct msdc_host *host)
{

}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for setting timeout value
 * @param *host: msdc host manipulator pointer
 * @param ns: timeout value by unit ns
 * @param clks: clock count for extend timeout
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_set_timeout(struct msdc_host *host, u32 ns, u32 clks)
{
	void __iomem *base = host->base;
	u32 timeout, clk_ns;
	u32 mode = 0;

	host->timeout_ns = ns;
	host->timeout_clks = clks;
	if (host->sclk == 0) {
		timeout = 0;
	} else {
		clk_ns  = 1000000000UL / host->sclk;
		timeout = (ns + clk_ns - 1) / clk_ns + clks;
		/* in 1048576 sclk cycle unit */
		timeout = (timeout + (1 << 20) - 1) >> 20;
		MSDC_GET_FIELD(MSDC_CFG, MSDC_CFG_CKMOD, mode);
		/*DDR mode will double the clk cycles for data timeout*/
		timeout = mode >= 2 ? timeout * 2 : timeout;
		timeout = timeout > 1 ? timeout - 1 : 0;
		timeout = timeout > 255 ? 255 : timeout;
	}
	MSDC_SET_FIELD(SDC_CFG, SDC_CFG_DTOC, timeout);
}

/* msdc_eirq_sdio() will be called when EIRQ(for WIFI) */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to handle external IRQ for SDIO
 * @param *data: msdc host manipulator pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_eirq_sdio(void *data)
{
	struct msdc_host *host = (struct msdc_host *)data;

	N_MSG(INT, "SDIO EINT");
#ifdef SDIO_ERROR_BYPASS
	if (host->sdio_error != -EILSEQ) {
#endif
		mmc_signal_sdio_irq(host->mmc);
#ifdef SDIO_ERROR_BYPASS
	}
#endif
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to change sclk rate
 * @param *host: msdc host manipulator pointer
 * @param timing: speed mode
 * @param hz: target sclk rate
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
void msdc_set_mclk(struct msdc_host *host, unsigned char timing, u32 hz)
{
	void __iomem *base = host->base;
	u32 mode;
	u32 flags;
	u32 div;
	u32 sclk;
	u32 hclk = host->hclk;
	u32 hs400_div_dis = 0; /* FOR MSDC_CFG.HS400CKMOD */

	if (!hz) { /* set mmc system clock to 0*/
		if (is_card_sdio(host) || (host->hw->flags & MSDC_SDIO_IRQ)) {
			host->saved_para.hz = hz;
#ifdef SDIO_ERROR_BYPASS
			host->sdio_error = 0;
#endif
		}
		host->mclk = 0;
		msdc_reset_hw(host->id);
		return;
	}

	msdc_irq_save(flags);

	if (timing == MMC_TIMING_MMC_HS400) {
		mode = 0x3; /* HS400 mode */
		if (hz >= hclk/2) {
			hs400_div_dis = 1;
			div = 0;
			sclk = hclk/2;
		} else {
			hs400_div_dis = 0;
			if (hz >= (hclk >> 2)) {
				div  = 0;         /* mean div = 1/4 */
				sclk = hclk >> 2; /* sclk = clk / 4 */
			} else {
				div  = (hclk + ((hz << 2) - 1)) / (hz << 2);
				sclk = (hclk >> 2) / div;
				div  = (div >> 1);
			}
		}
	} else if ((timing == MMC_TIMING_UHS_DDR50)
		|| (timing == MMC_TIMING_MMC_DDR52)) {
		mode = 0x2; /* ddr mode and use divisor */
		if (hz >= (hclk >> 2)) {
			div  = 0;         /* mean div = 1/4 */
			sclk = hclk >> 2; /* sclk = clk / 4 */
		} else {
			div  = (hclk + ((hz << 2) - 1)) / (hz << 2);
			sclk = (hclk >> 2) / div;
			div  = (div >> 1);
		}
		MSDC_SET_FIELD(EMMC50_CFG0, MSDC_EMMC50_CFG_CRC_STS_SEL, 1);
#if !defined(CONFIG_MACH_FPGA)
	} else if (hz >= hclk) {
		mode = 0x1; /* no divisor */
		div  = 0;
		sclk = hclk;
#endif
	} else {
		mode = 0x0; /* use divisor */
		if (hz >= (hclk >> 1)) {
			div  = 0;         /* mean div = 1/2 */
			sclk = hclk >> 1; /* sclk = clk / 2 */
		} else {
			div  = (hclk + ((hz << 2) - 1)) / (hz << 2);
			sclk = (hclk >> 2) / div;
		}
		MSDC_SET_FIELD(EMMC50_CFG0, MSDC_EMMC50_CFG_CRC_STS_SEL, 1);
	}

	msdc_clk_stable(host, mode, div, hs400_div_dis);

	host->sclk = sclk;
	host->mclk = hz;
	host->timing = timing;

	/* need because clk changed.*/
	msdc_set_timeout(host, host->timeout_ns, host->timeout_clks);

	msdc_set_smpl_all(host, mode);

	pr_err("msdc%d -> !!! Set<%dKHz> Source<%dKHz> -> sclk<%dKHz> timing<%d> mode<%d> div<%d> hs400_div_dis<%d>",
		host->id, hz/1000, hclk/1000, sclk/1000, (int)timing, mode, div,
		hs400_div_dis);

	msdc_irq_restore(flags);
}

static u32 wints_cmd = MSDC_INT_CMDRDY | MSDC_INT_RSPCRCERR | MSDC_INT_CMDTMO |
		       MSDC_INT_ACMDRDY | MSDC_INT_ACMDCRCERR |
		       MSDC_INT_ACMDTMO;
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for msdc sending command
 * @param *host: msdc host manipulator pointer
 * @param *cmd: mmc command pointer
 * @param timeout: command timeout setting
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static unsigned int msdc_command_start(struct msdc_host   *host,
	struct mmc_command *cmd,
	unsigned long       timeout)
{
	void __iomem *base = host->base;
	u32 opcode = cmd->opcode;
	u32 rawcmd;
	u32 resp;
	unsigned long tmo;
	struct mmc_command *sbc = NULL;
	char *str;

	if (host->data && host->data->mrq && host->data->mrq->sbc)
		sbc = host->data->mrq->sbc;

	/* Protocol layer does not provide response type, but our hardware needs
	 * to know exact type, not just size!
	 */
#ifdef MSDC_XTS_ENABLE
	msdc_xts_disable(host);
#endif

	switch (opcode) {
	case MMC_SEND_OP_COND:
	case SD_APP_OP_COND:
		resp = RESP_R3;
		break;
	case MMC_SET_RELATIVE_ADDR:
	/*case SD_SEND_RELATIVE_ADDR:*/
		/* Since SD_SEND_RELATIVE_ADDR=MMC_SET_RELATIVE_ADDR=3,
		 * only one is allowed in switch case.
		 */
		resp = (mmc_cmd_type(cmd) == MMC_CMD_BCR) ? RESP_R6 : RESP_R1;
		break;
	case MMC_FAST_IO:
		resp = RESP_R4;
		break;
	case MMC_GO_IRQ_STATE:
		resp = RESP_R5;
		break;
	case MMC_SELECT_CARD:
		resp = (cmd->arg != 0) ? RESP_R1 : RESP_NONE;
		host->app_cmd_arg = cmd->arg;
		N_MSG(PWR, "msdc%d select card<0x%.8x>", host->id, cmd->arg);
		break;
	case SD_IO_RW_DIRECT:
	case SD_IO_RW_EXTENDED:
		/* SDIO workaround. */
		resp = RESP_R1;
		break;
	case SD_SEND_IF_COND:
		resp = RESP_R1;
		break;
	/* Ignore crc errors when sending status cmd to poll for busy
	 * MMC_RSP_CRC will be set, then mmc_resp_type will return
	 * MMC_RSP_NONE. CMD13 will not receive resp
	 */
	case MMC_SEND_STATUS:
		resp = RESP_R1;
		break;
	default:
		switch (mmc_resp_type(cmd)) {
		case MMC_RSP_R1:
			resp = RESP_R1;
			break;
		case MMC_RSP_R1B:
			resp = RESP_R1B;
			break;
		case MMC_RSP_R2:
			resp = RESP_R2;
			break;
		case MMC_RSP_R3:
			resp = RESP_R3;
			break;
		case MMC_RSP_NONE:
		default:
			resp = RESP_NONE;
			break;
		}
	}

	cmd->error = 0;
	/* rawcmd :
	 * vol_swt << 30 | auto_cmd << 28 | blklen << 16 | go_irq << 15 |
	 * stop << 14 | rw << 13 | dtype << 11 | rsptyp << 7 | brk << 6 |
	 * opcode
	 */

	rawcmd = opcode | msdc_rsp[resp] << 7 | host->blksz << 16;

	switch (opcode) {
	case MMC_READ_MULTIPLE_BLOCK:
	case MMC_WRITE_MULTIPLE_BLOCK:
		rawcmd |= (2 << 11);
		if (opcode == MMC_WRITE_MULTIPLE_BLOCK)
			rawcmd |= (1 << 13);
		if (host->autocmd & MSDC_AUTOCMD12) {
			rawcmd |= (1 << 28);
			N_MSG(CMD, "AUTOCMD12 is set, addr<0x%x>", cmd->arg);
#ifdef MTK_MSDC_USE_CMD23
		} else if ((host->autocmd & MSDC_AUTOCMD23)) {
			unsigned int reg_blk_num;

			rawcmd |= (1 << 29);
			if (sbc) {
				/* if block number is greater than 0xFFFF,
				 * CMD23 arg will fail to set it.
				 */
				reg_blk_num = MSDC_READ32(SDC_BLK_NUM);
				if (reg_blk_num != (sbc->arg & 0xFFFF))
					pr_err("msdc%d: acmd23 arg(0x%x) fail to match block num(0x%x), SDC_BLK_NUM(0x%x)\n",
						host->id, sbc->arg,
						cmd->data->blocks, reg_blk_num);
				N_MSG(CMD, "AUTOCMD23 addr<0x%x>, arg<0x%x> ",
					cmd->arg, sbc->arg);
			}
#endif /* end of MTK_MSDC_USE_CMD23 */
		}
		break;

	case MMC_READ_SINGLE_BLOCK:
	case MMC_SEND_TUNING_BLOCK:
	case MMC_SEND_TUNING_BLOCK_HS200:
		rawcmd |= (1 << 11);
		break;
	case MMC_WRITE_BLOCK:
		rawcmd |= ((1 << 11) | (1 << 13));
		break;
#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
	case MMC_READ_REQUESTED_QUEUE:
		rawcmd |= (2 << 11);
		break;
	case MMC_WRITE_REQUESTED_QUEUE:
		rawcmd |= ((2 << 11) | (1 << 13));
		break;
	case MMC_CMDQ_TASK_MGMT:
		break;
#endif
	case SD_IO_RW_EXTENDED:
		if (cmd->data->flags & MMC_DATA_WRITE)
			rawcmd |= (1 << 13);
		if (cmd->data->blocks > 1)
			rawcmd |= (2 << 11);
		else
			rawcmd |= (1 << 11);
		break;
	case SD_IO_RW_DIRECT:
		if (cmd->flags == (unsigned int)-1)
			rawcmd |= (1 << 14);
		break;
	case SD_SWITCH_VOLTAGE:
		rawcmd |= (1 << 30);
		break;
	case SD_APP_SEND_SCR:
	case SD_APP_SEND_NUM_WR_BLKS:
	case MMC_SEND_WRITE_PROT:
	/*case MMC_SEND_WRITE_PROT_TYPE:*/
		rawcmd |= (1 << 11);
		break;
	case SD_SWITCH:
	case SD_APP_SD_STATUS:
	case MMC_SEND_EXT_CSD:
		if (mmc_cmd_type(cmd) == MMC_CMD_ADTC)
			rawcmd |= (1 << 11);
		break;
	case MMC_STOP_TRANSMISSION:
		rawcmd |= (1 << 14);
		rawcmd &= ~(0x0FFF << 16);
		break;
	}

	N_MSG(CMD, "CMD<%d><0x%.8x> Arg<0x%.8x>", opcode, rawcmd, cmd->arg);
	tmo = jiffies + timeout;

	if (opcode == MMC_SEND_STATUS) {
		while (sdc_is_cmd_busy()) {
			if (time_after(jiffies, tmo)) {
				str = "cmd_busy";
				goto err;
			}
		}
	} else {
		while (sdc_is_busy()) {
			if (time_after(jiffies, tmo)) {
				str = "cmd_busy";
				goto err;
			}
		}
	}

	host->cmd = cmd;
	host->cmd_rsp = resp;

#ifdef MSDC_XTS_ENABLE
	MSDC_WRITE32(MSDC_AES_CTR0_GP0, cmd->arg);
	MSDC_WRITE32(MSDC_AES_CTR1_GP0, 0);
	MSDC_WRITE32(MSDC_AES_CTR2_GP0, 0);
	MSDC_WRITE32(MSDC_AES_CTR3_GP0, 0);
	MSDC_WRITE32(MSDC_AES_CTR0_GP1, cmd->arg);
	MSDC_WRITE32(MSDC_AES_CTR1_GP1, 0);
	MSDC_WRITE32(MSDC_AES_CTR2_GP1, 0);
	MSDC_WRITE32(MSDC_AES_CTR3_GP1, 0);
#endif

	/* use polling way */
	MSDC_CLR_BIT32(MSDC_INTEN, wints_cmd);

	aes_engine_ctrl(host, opcode);
	sdc_send_cmd(rawcmd, cmd->arg);

	return 0;

err:
	ERR_MSG("XXX %s timeout: before CMD<%d>", str, opcode);
	cmd->error = (unsigned int)-ETIMEDOUT;
	msdc_dump_register(host);
	msdc_reset_hw(host->id);

	return cmd->error;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for polling command response
 * @param *host: msdc host manipulator pointer
 * @param *cmd: mmc command pointer
 * @param timeout: command timeout setting
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static u32 msdc_command_resp_polling(struct msdc_host *host,
	struct mmc_command *cmd,
	unsigned long       timeout)
{
	void __iomem *base = host->base;
	u32 intsts;
	u32 resp;
	unsigned long tmo;

	u32 cmdsts = MSDC_INT_CMDRDY | MSDC_INT_RSPCRCERR | MSDC_INT_CMDTMO;
#ifdef MTK_MSDC_USE_CMD23
	struct mmc_command *sbc = NULL;

	if (host->autocmd & MSDC_AUTOCMD23) {
		if (host->data && host->data->mrq && host->data->mrq->sbc)
			sbc = host->data->mrq->sbc;

		/* autocmd interrupt disabled, used polling way */
		cmdsts |= MSDC_INT_ACMDCRCERR | MSDC_INT_ACMDTMO;
	}
#endif

	resp = host->cmd_rsp;

	/*polling */
	tmo = jiffies + timeout;
	while (1) {
		intsts = MSDC_READ32(MSDC_INT);
		if ((intsts & cmdsts) != 0) {
			/* clear all int flag */
#ifdef MTK_MSDC_USE_CMD23
			/* need clear autocmd23 command ready interrupt */
			intsts &= (cmdsts | MSDC_INT_ACMDRDY);
#else
			intsts &= cmdsts;
#endif
			MSDC_WRITE32(MSDC_INT, intsts);
			break;
		}

		if (time_after(jiffies, tmo)) {
			pr_err("[%s]: CMD<%d> polling_for_completion timeout ARG<0x%.8x>",
				__func__, cmd->opcode, cmd->arg);
			cmd->error = (unsigned int)-ETIMEDOUT;
			host->sw_timeout++;
			msdc_dump_info(host->id);
			msdc_reset_hw(host->id);
			goto out;
		}
	}

#ifdef MTK_MSDC_ERROR_TUNE_DEBUG
	msdc_error_tune_debug1(host, cmd, sbc, &intsts);
#endif

	/* command interrupts */
	if  (!(intsts & cmdsts))
		goto out;

#ifdef MTK_MSDC_USE_CMD23
	if (intsts & MSDC_INT_CMDRDY) {
#else
	if (intsts & (MSDC_INT_CMDRDY | MSDC_INT_ACMDRDY)) {
#endif
		u32 *rsp = NULL;

		rsp = &cmd->resp[0];
		switch (host->cmd_rsp) {
		case RESP_NONE:
			break;
		case RESP_R2:
			*rsp++ = MSDC_READ32(SDC_RESP3);
			*rsp++ = MSDC_READ32(SDC_RESP2);
			*rsp++ = MSDC_READ32(SDC_RESP1);
			*rsp++ = MSDC_READ32(SDC_RESP0);
			break;
		default: /* Response types 1, 3, 4, 5, 6, 7(1b) */
			*rsp = MSDC_READ32(SDC_RESP0);
			if ((cmd->opcode == 13) || (cmd->opcode == 25)) {
				/* Only print msg on this error */
				if (*rsp & R1_WP_VIOLATION) {
					pr_err("[%s]: CMD<%d> resp<0x%.8x>, write protection violation\n",
						__func__, cmd->opcode,
						*rsp);
				}

				if ((*rsp & R1_OUT_OF_RANGE)
				 && (host->hw->host_function != MSDC_SDIO)) {
					pr_err("[%s]: CMD<%d> resp<0x%.8x>,bit31=1,force make crc error\n",
						__func__, cmd->opcode,
						*rsp);
					/*cmd->error = (unsigned int)-EILSEQ;*/
				}
			}
			break;
		}
	} else if (intsts & MSDC_INT_RSPCRCERR) {
		cmd->error = (unsigned int)-EILSEQ;
		if ((cmd->opcode != 19) && (cmd->opcode != 21))
			pr_err("[%s]: CMD<%d> MSDC_INT_RSPCRCERR Arg<0x%.8x>",
				__func__, cmd->opcode, cmd->arg);
		if (((mmc_resp_type(cmd) == MMC_RSP_R1B) || (cmd->opcode == 13))
			&& (host->hw->host_function != MSDC_SDIO)) {
			pr_err("[%s]: CMD<%d> ARG<0x%.8X> is R1B, CRC not reset hw\n",
				__func__, cmd->opcode, cmd->arg);
		} else {
			msdc_reset_hw(host->id);
		}
	} else if (intsts & MSDC_INT_CMDTMO) {
		cmd->error = (unsigned int)-ETIMEDOUT;
		if ((cmd->opcode != 19) && (cmd->opcode != 21))
			pr_err("[%s]: CMD<%d> MSDC_INT_CMDTMO Arg<0x%.8x>",
				__func__, cmd->opcode, cmd->arg);
		if ((cmd->opcode != 52) && (cmd->opcode != 8)
		 && (cmd->opcode != 55) && (cmd->opcode != 19)
		 && (cmd->opcode != 21) && (cmd->opcode != 1)
		 && (cmd->opcode != 5 ||
		     (host->mmc->card && mmc_card_mmc(host->mmc->card)))
		 && (cmd->opcode != 13 || g_emmc_mode_switch == 0)) {
			msdc_dump_info(host->id);
		}
		if (cmd->opcode == MMC_STOP_TRANSMISSION) {
			cmd->error = 0;
			pr_err("send stop TMO, device status: %x\n",
				host->device_status);
		}

		if (((mmc_resp_type(cmd) == MMC_RSP_R1B) || (cmd->opcode == 13))
			&& (host->hw->host_function != MSDC_SDIO)) {
			pr_err("[%s]: CMD<%d> ARG<0x%.8X> is R1B, TMO not reset hw\n",
				__func__, cmd->opcode, cmd->arg);
		} else {
			msdc_reset_hw(host->id);
		}
	}
#ifdef MTK_MSDC_USE_CMD23
	if ((sbc != NULL) && (host->autocmd & MSDC_AUTOCMD23)) {
		if (intsts & MSDC_INT_ACMDRDY) {
			/* do nothing */
		} else if (intsts & MSDC_INT_ACMDCRCERR) {
			pr_err("[%s]: autocmd23 crc error\n",
				__func__);
			sbc->error = (unsigned int)-EILSEQ;
			/* record the error info in current cmd struct */
			cmd->error = (unsigned int)-EILSEQ;
			/* host->error |= REQ_CMD23_EIO; */
			msdc_reset_hw(host->id);
		} else if (intsts & MSDC_INT_ACMDTMO) {
			pr_err("[%s]: autocmd23 tmo error\n",
				__func__);
			sbc->error = (unsigned int)-ETIMEDOUT;
			/* record the error info in current cmd struct */
			cmd->error = (unsigned int)-ETIMEDOUT;
			msdc_dump_info(host->id);
			/* host->error |= REQ_CMD23_TMO; */
			msdc_reset_hw(host->id);
		}
	}
#endif /* end of MTK_MSDC_USE_CMD23 */

out:
	host->cmd = NULL;

	if (!cmd->data && !cmd->error)
		host->prev_cmd_cause_dump = 0;

	return cmd->error;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for sending command
 * @param *host: msdc host manipulator pointer
 * @param *cmd: mmc command pointer
 * @param timeout: command timeout setting
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static unsigned int msdc_do_command(struct msdc_host *host,
	struct mmc_command *cmd,
	unsigned long       timeout)
{
	MVG_EMMC_DECLARE_INT32(delay_ns);
	MVG_EMMC_DECLARE_INT32(delay_us);
	MVG_EMMC_DECLARE_INT32(delay_ms);

	MVG_EMMC_ERASE_MATCH(host, (u64)cmd->arg, delay_ms, delay_us,
		delay_ns, cmd->opcode);

	if (msdc_command_start(host, cmd, timeout))
		goto end;

	MVG_EMMC_ERASE_RESET(delay_ms, delay_us, cmd->opcode);

	if (msdc_command_resp_polling(host, cmd, timeout))
		goto end;

end:
	if (cmd->opcode == MMC_SEND_STATUS && cmd->error == 0)
		host->device_status = cmd->resp[0];

	N_MSG(CMD, "        return<%d> resp<0x%.8x>", cmd->error, cmd->resp[0]);

	return cmd->error;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to send STOP command
 * @param *host: msdc host manipulator pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_send_stop(struct msdc_host *host)
{
	struct mmc_command stop = {0};
	struct mmc_request mrq = {0};
	u32 err;

	stop.opcode = MMC_STOP_TRANSMISSION;
	stop.arg = 0;
	stop.flags = MMC_RSP_R1B | MMC_CMD_AC;

	mrq.cmd = &stop;
	stop.mrq = &mrq;
	stop.data = NULL;

	err = msdc_do_command(host, &stop, CMD_TIMEOUT);
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for executing tuning function
 * @param *mmc: mmc host manipulator pointer
 * @param opcode: operation command code
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     Always 0
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_execute_tuning(struct mmc_host *mmc, u32 opcode)
{
#ifndef CONFIG_MACH_FPGA
	struct msdc_host *host = mmc_priv(mmc);
	int ret = 0;

	/* u32 mode; */

	/*
	* MSDC_GET_FIELD(EMMC50_CFG0, MSDC_EMMC50_CFG_CRC_STS_SEL, mode);
	* ERR_MSG("msdc_execute_tuning-1 MSDC_EMMC50_CFG_CRC_STS_SEL=0x%x:
	* \r\n",mode);
	*/
	msdc_init_tune_path(host, mmc->ios.timing);
	host->tuning_in_progress = true;
	/*
	* MSDC_GET_FIELD(EMMC50_CFG0, MSDC_EMMC50_CFG_CRC_STS_SEL, mode);
	* ERR_MSG("msdc_execute_tuning-2 MSDC_EMMC50_CFG_CRC_STS_SEL=0x%x:
	* \r\n",mode);
	*/
	msdc_ungate_clock(host);
	if (host->hw->host_function == MSDC_SD)
		ret = sd_execute_dvfs_autok(host, opcode, NULL);
	else if (host->hw->host_function == MSDC_EMMC)
		ret = emmc_execute_dvfs_autok(host, opcode, NULL);
	else if (host->hw->host_function == MSDC_SDIO)
		sdio_execute_dvfs_autok(host);

	host->tuning_in_progress = false;
	if (ret)
		msdc_dump_info(host->id);

	msdc_gate_clock(host, 1);
	/*
	* MSDC_GET_FIELD(EMMC50_CFG0, MSDC_EMMC50_CFG_CRC_STS_SEL, mode);
	* ERR_MSG("msdc_execute_tuning-3 MSDC_EMMC50_CFG_CRC_STS_SEL=0x%x:
	* \r\n",mode);
	*/
#endif
	return 0;
}

#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to send command for CMDQ feature
 * @param *host: msdc host manipulator pointer
 * @param *cmd: mmc command pointer
 * @param timeout: command timeout setting
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static unsigned int msdc_cmdq_command_start(struct msdc_host *host,
	struct mmc_command *cmd,
	unsigned long timeout)
{
	void __iomem *base = host->base;
	unsigned long tmo;
	u32 wints_cq_cmd = MSDC_INT_CMDRDY | MSDC_INT_RSPCRCERR
		| MSDC_INT_CMDTMO;

	switch (cmd->opcode) {
	case MMC_SET_QUEUE_CONTEXT:
	case MMC_QUEUE_READ_ADDRESS:
	case MMC_SEND_STATUS:
		break;
	default:
		pr_err("[%s]: ERROR, only CMD44/CMD45/CMD13 can issue\n",
			__func__);
		break;
	}

	cmd->error = 0;

	N_MSG(CMD, "CMD<%d>        Arg<0x%.8x>", cmd->opcode, cmd->arg);

	tmo = jiffies + timeout;

	while (sdc_is_cmd_busy()) {
		if (time_after(jiffies, tmo)) {
			ERR_MSG("[%s]: XXX cmd_busy timeout: before CMD<%d>",
				__func__, cmd->opcode);
			cmd->error = (unsigned int)-ETIMEDOUT;
			msdc_reset_hw(host->id);
			return cmd->error;
		}
	}

	host->cmd = cmd;
	host->cmd_rsp = RESP_R1;

	/* use polling way */
	MSDC_CLR_BIT32(MSDC_INTEN, wints_cq_cmd);
	sdc_send_cmdq_cmd(cmd->opcode, cmd->arg);

	return 0;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for polling command response of CMDQ command
 * @param *host: msdc host manipulator pointer
 * @param *cmd: mmc command pointer
 * @param timeout: command timeout setting
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static unsigned int msdc_cmdq_command_resp_polling(struct msdc_host *host,
	struct mmc_command *cmd,
	unsigned long timeout)
{
	void __iomem *base = host->base;
	u32 intsts;
	u32 resp;
	unsigned long tmo;
	u32 cmdsts = MSDC_INT_CMDRDY | MSDC_INT_RSPCRCERR | MSDC_INT_CMDTMO;

	resp = host->cmd_rsp;

	/* polling */
	tmo = jiffies + timeout;
	while (1) {
		intsts = MSDC_READ32(MSDC_INT);
		if ((intsts & cmdsts) != 0) {
			/* clear all int flag */
			intsts &= cmdsts;
			MSDC_WRITE32(MSDC_INT, intsts);
			break;
		}

		if (time_after(jiffies, tmo)) {
			pr_err("[%s]: msdc%d CMD<%d> polling_for_completion timeout ARG<0x%.8x>",
				__func__, host->id, cmd->opcode, cmd->arg);
			cmd->error = (unsigned int)-ETIMEDOUT;
			host->sw_timeout++;
			msdc_dump_info(host->id);
			goto out;
		}
	}

	/* command interrupts */
	if (intsts & cmdsts) {
		if (intsts & MSDC_INT_CMDRDY) {
			u32 *rsp = NULL;

			rsp = &cmd->resp[0];
			switch (host->cmd_rsp) {
			case RESP_NONE:
				break;
			case RESP_R2:
				*rsp++ = MSDC_READ32(SDC_RESP3);
				*rsp++ = MSDC_READ32(SDC_RESP2);
				*rsp++ = MSDC_READ32(SDC_RESP1);
				*rsp++ = MSDC_READ32(SDC_RESP0);
				break;
			default: /* Response types 1, 3, 4, 5, 6, 7(1b) */
				*rsp = MSDC_READ32(SDC_RESP0);
				break;
			}
		} else if (intsts & MSDC_INT_RSPCRCERR) {
			cmd->error = (unsigned int)-EILSEQ;
			pr_err("[%s]: msdc%d XXX CMD<%d> MSDC_INT_RSPCRCERR Arg<0x%.8x>",
				__func__, host->id, cmd->opcode, cmd->arg);
			msdc_dump_info(host->id);
		} else if (intsts & MSDC_INT_CMDTMO) {
			cmd->error = (unsigned int)-ETIMEDOUT;
			pr_err("[%s]: msdc%d XXX CMD<%d> MSDC_INT_CMDTMO Arg<0x%.8x>",
				__func__, host->id, cmd->opcode, cmd->arg);
			msdc_dump_info(host->id);
		}
	}
out:
	host->cmd = NULL;
	MSDC_SET_FIELD(EMMC51_CFG0, MSDC_EMMC51_CFG_CMDQEN, (0));

	if (!cmd->data && !cmd->error)
		host->prev_cmd_cause_dump = 0;

	return cmd->error;
}

/* do command queue command - CMD44, CMD45, CMD13(QSR)
 * use another register set
 */
 /** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to send command for CMDQ feature
 * @param *host: msdc host manipulator pointer
 * @param *cmd: mmc command pointer
 * @param timeout: command timeout setting
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static unsigned int msdc_do_cmdq_command(struct msdc_host *host,
	struct mmc_command *cmd,
	unsigned long timeout)
{
	if (msdc_cmdq_command_start(host, cmd, timeout))
		goto end;

	if (msdc_cmdq_command_resp_polling(host, cmd, timeout))
		goto end;
end:
	N_MSG(CMD, "return<%d> resp<0x%.8x>", cmd->error, cmd->resp[0]);
	return cmd->error;
}
#endif

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to send CMD13 to get card status
 * @param *mmc: mmc host manipulator pointer
 * @param *host: msdc host manipulator pointer
 * @param *status: command response
 * @return
 *     If 0, OK. \n
 *     If others, fail.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_get_card_status(struct mmc_host *mmc, struct msdc_host *host,
	u32 *status)
{
	struct mmc_command cmd = { 0 };
	struct mmc_request mrq = { 0 };
	u32 err;

	cmd.opcode = MMC_SEND_STATUS;   /* CMD13 */
	cmd.arg = host->app_cmd_arg;
	cmd.flags = MMC_RSP_SPI_R2 | MMC_RSP_R1 | MMC_CMD_AC;

	mrq.cmd = &cmd;
	cmd.mrq = &mrq;
	cmd.data = NULL;

	/* tune until CMD13 pass. */
#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
	if (host->mmc->card->ext_csd.cmdq_mode_en)
		err = msdc_do_cmdq_command(host, &cmd,  CMD_TIMEOUT);
	else
#endif
		err = msdc_do_command(host, &cmd, CMD_TIMEOUT);

	if (status)
		*status = cmd.resp[0];

	return err;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to trigger RESET pin
 * @param *host: msdc host manipulator pointer
 * @param mode: RESET pin pull mode
 * @param force_reset: indicate if force resetr
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_pin_reset(struct msdc_host *host, int mode, int force_reset)
{
	struct msdc_hw *hw = (struct msdc_hw *)host->hw;
	void __iomem *base = host->base;

	/* Config reset pin */
	if ((hw->flags & MSDC_RST_PIN_EN) || force_reset) {
		if (mode == MSDC_PIN_PULL_UP)
			MSDC_CLR_BIT32(EMMC_IOCON, EMMC_IOCON_BOOTRST);
		else
			MSDC_SET_BIT32(EMMC_IOCON, EMMC_IOCON_BOOTRST);
	}
}

/* called by ops.set_ios */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to set DAT width
 * @param *host: msdc host manipulator pointer
 * @param width: target DAT width(1/4/8)
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_set_buswidth(struct msdc_host *host, u32 width)
{
	void __iomem *base = host->base;
	u32 val = MSDC_READ32(SDC_CFG);

	val &= ~SDC_CFG_BUSWIDTH;

	switch (width) {
	default:
	case MMC_BUS_WIDTH_1:
		val |= (MSDC_BUS_1BITS << 16);
		break;
	case MMC_BUS_WIDTH_4:
		val |= (MSDC_BUS_4BITS << 16);
		break;
	case MMC_BUS_WIDTH_8:
		val |= (MSDC_BUS_8BITS << 16);
		break;
	}

	MSDC_WRITE32(SDC_CFG, val);
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for msdc HW initialization
 * @param *host: msdc host manipulator pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_init_hw(struct msdc_host *host)
{
	void __iomem *base = host->base;
#ifndef CONFIG_MACH_FPGA
	void __iomem *base_top = host->base_top;
#endif

	/* Power on */
	/* msdc_pin_reset(host, MSDC_PIN_PULL_UP, 0); */

	msdc_ungate_clock(host);

	/* Configure to MMC/SD mode */
	MSDC_SET_FIELD(MSDC_CFG, MSDC_CFG_MODE, MSDC_SDMMC);

	/* Reset */
	msdc_reset_hw(host->id);

	/* Disable card detection */
	MSDC_CLR_BIT32(MSDC_PS, MSDC_PS_CDEN);

	/* Disable and clear all interrupts */
	MSDC_CLR_BIT32(MSDC_INTEN, MSDC_READ32(MSDC_INTEN));
	MSDC_WRITE32(MSDC_INT, MSDC_READ32(MSDC_INT));

	/* reset tuning parameter */
#ifndef CONFIG_MACH_FPGA
	msdc_init_tune_setting(host);
#endif

	/* disable endbit check */
	if (host->id == 1)
		msdc_set_check_endbit(host, 0);

	/* For safety, clear SDC_CFG.SDIO_IDE (INT_DET_EN) & set SDC_CFG.SDIO
	 *  in pre-loader,uboot,kernel drivers.
	 */
	/* Enable SDIO mode. it's must otherwise sdio cmd5 failed */
	/* MSDC_SET_BIT32(SDC_CFG, SDC_CFG_SDIO); */  /* test */

	if (host->hw->flags & MSDC_SDIO_IRQ) {
		ghost = host;
		/* enable sdio detection when drivers needs */
		MSDC_SET_BIT32(SDC_CFG, SDC_CFG_SDIOIDE);
	} else {
		MSDC_CLR_BIT32(SDC_CFG, SDC_CFG_SDIOIDE);
	}

	msdc_set_smt(host, SMT_ENABLE);
	msdc_set_driving(host, &host->hw->driving);

	/* Defalut SDIO GPIO mode is worng */
	if (host->hw->host_function == MSDC_SDIO)
		msdc_set_pin_mode(host);

	msdc_set_ies(host, IES_ENABLE);

	/* write crc timeout detection */
	MSDC_SET_FIELD(MSDC_PATCH_BIT0, MSDC_PB0_DETWR_CRCTMO, 1);

	/* Configure to default data timeout */
	MSDC_SET_FIELD(SDC_CFG, SDC_CFG_DTOC, DEFAULT_DTOC);

	msdc_set_buswidth(host, MMC_BUS_WIDTH_1);

	/* Configure support 64G */
	MSDC_SET_BIT32(MSDC_PATCH_BIT2, MSDC_PB2_SUPPORT64G);

	MSDC_SET_BIT32(SDC_CMD_STS, SDC_CMD_STS_RX_ENHANCE_EN);
#ifndef CONFIG_MACH_FPGA
	MSDC_WRITE32(EMMC_TOP_CONTROL, 0);
	MSDC_SET_BIT32(EMMC_TOP_CONTROL, PAD_DAT_RD_RXDLY_SEL);
	MSDC_CLR_BIT32(EMMC_TOP_CONTROL, DATA_K_VALUE_SEL);
	MSDC_SET_BIT32(EMMC_TOP_CMD, PAD_CMD_RD_RXDLY_SEL);
#endif

	MSDC_SET_FIELD(MSDC_PATCH_BIT2, MSDC_PB2_RESPWAITCNT, 3);
	/* use async fifo, then no need tune internal delay */
	MSDC_CLR_BIT32(MSDC_PATCH_BIT2, MSDC_PB2_CFGRESP);
	MSDC_SET_BIT32(MSDC_PATCH_BIT2, MSDC_PB2_CFGCRCSTS);

	host->need_tune = TUNE_NONE;
	host->tune_smpl_times = 0;
	host->reautok_times = 0;

	msdc_gate_clock(host, 1);
#ifdef MSDC_XTS_ENABLE
	msdc_xts_init(host);
#endif
}

/*
 * card hw reset if error
 * 1. reset pin:    DONW => 2us  => UP => 200us
 * 2. power:        OFF  => 10us => ON => 200us
*/

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to reset device and initialize host HW
 * @param *mmc: mmc host manipulator pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_card_reset(struct mmc_host *mmc)
{
	struct msdc_host *host = mmc_priv(mmc);

	pr_err("XXX msdc%d reset card\n", host->id);

	if (host->power_control) {
		host->power_control(host, 0);
		udelay(10);
		host->power_control(host, 1);
	}
	usleep_range(200, 500);

	msdc_pin_reset(host, MSDC_PIN_PULL_DOWN, 1);
	udelay(2);
	msdc_pin_reset(host, MSDC_PIN_PULL_UP, 1);
	usleep_range(200, 500);
	msdc_init_hw(host);
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to set host power mode
 * @param *host: msdc host manipulator pointer
 * @param mode: power mode
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_set_power_mode(struct msdc_host *host, u8 mode)
{
	N_MSG(CFG, "Set power mode(%d)", mode);
	host->power_mode = mode;
}

#ifdef CONFIG_PM
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to set msdc pm state
 * @param state: assign msdc host pm state
 * @param *data: msdc host manipulator pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_pm(pm_message_t state, void *data)
{
	struct msdc_host *host = (struct msdc_host *)data;
	void __iomem *base = host->base;

	int evt = state.event;

	msdc_ungate_clock(host);

	if (evt == PM_EVENT_SUSPEND || evt == PM_EVENT_USER_SUSPEND) {
		if (host->suspend)
			goto end;

		if (evt == PM_EVENT_SUSPEND &&
		     host->power_mode == MMC_POWER_OFF)
			goto end;

		host->suspend = 1;
		host->pm_state = state;

		N_MSG(PWR, "msdc%d -> %s Suspend", host->id,
			evt == PM_EVENT_SUSPEND ? "PM" : "USR");
		if (host->hw->flags & MSDC_SYS_SUSPEND) {
			if (host->hw->host_function == MSDC_EMMC) {
				msdc_save_timing_setting(host, 1);
				msdc_set_power_mode(host, MMC_POWER_OFF);
			}
			/* FIX ME: check if it can be removed
			 * since msdc_set_power_mode will do it
			 */
			msdc_set_tdsel(host, 1, 0);
		} else {
			host->mmc->pm_flags |= MMC_PM_IGNORE_PM_NOTIFY;
			mmc_remove_host(host->mmc);
		}
	} else if (evt == PM_EVENT_RESUME || evt == PM_EVENT_USER_RESUME) {
		if (!host->suspend)
			goto end;

		if (evt == PM_EVENT_RESUME
			&& host->pm_state.event == PM_EVENT_USER_SUSPEND) {
			ERR_MSG("PM Resume when in USR Suspend");
			goto end;
		}

		host->suspend = 0;
		host->pm_state = state;

		N_MSG(PWR, "msdc%d -> %s Resume", host->id,
			evt == PM_EVENT_RESUME ? "PM" : "USR");

		if (!(host->hw->flags & MSDC_SYS_SUSPEND)) {
			host->mmc->pm_flags |= MMC_PM_IGNORE_PM_NOTIFY;
			mmc_add_host(host->mmc);
			goto end;
		}

		/* Begin for host->hw->flags & MSDC_SYS_SUSPEND*/
		/* FIX ME: check if it can be removed
		 * since msdc_set_power_mode will do it
		 */
		msdc_set_tdsel(host, 1, 0);

		if (host->hw->host_function == MSDC_EMMC) {
			msdc_reset_hw(host->id);
			msdc_set_power_mode(host, MMC_POWER_ON);
			msdc_restore_timing_setting(host);

			if (emmc_sleep_failed) {
				msdc_card_reset(host->mmc);
				mdelay(200);
				mmc_card_clr_sleep(host->mmc->card);
				emmc_sleep_failed = 0;
				host->mmc->ios.timing = MMC_TIMING_LEGACY;
				mmc_set_clock(host->mmc, 260000);
			}
		}
	}

end:
#ifdef SDIO_ERROR_BYPASS
	if (is_card_sdio(host))
		host->sdio_error = 0;
#endif
	if ((evt == PM_EVENT_SUSPEND) || (evt == PM_EVENT_USER_SUSPEND)) {
		if ((host->hw->host_function == MSDC_SDIO) &&
		    (evt == PM_EVENT_USER_SUSPEND)) {
			pr_err("msdc%d -> MSDC Device Request Suspend",
				host->id);
		}
		msdc_gate_clock(host, 0);
	} else {
		msdc_gate_clock(host, 1);
	}

	if (host->hw->host_function == MSDC_SDIO) {
		host->mmc->pm_flags |= MMC_PM_KEEP_POWER;
		host->mmc->rescan_entered = 0;
	}
}
#endif

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to switch eMMC access partition
 * @param *host: msdc host manipulator pointer
 * @param part_id: partition id
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
int msdc_switch_part(struct msdc_host *host, char part_id)
{
	int ret = 0;
	u8 *l_buf;

	if (!host || !host->mmc || !host->mmc->card)
		return -ENOMEDIUM;

	ret = mmc_get_ext_csd(host->mmc->card, &l_buf);
	if (ret)
		return ret;

	if ((part_id >= 0) && (part_id != (l_buf[EXT_CSD_PART_CONFIG] & 0x7))) {
		l_buf[EXT_CSD_PART_CONFIG] &= ~0x7;
		l_buf[EXT_CSD_PART_CONFIG] |= 0x0;
		ret = mmc_switch(host->mmc->card, 0, EXT_CSD_PART_CONFIG,
			l_buf[EXT_CSD_PART_CONFIG], 1000);
	}
	kfree(l_buf);

	return ret;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to enable/disable msdc cache mode
 * @param *data: mmc data pointer
 * @return
 *     Always 0
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_cache_onoff(struct mmc_data *data)
{
	u8 *ptr = (u8 *) sg_virt(data->sg);

#if defined(MTK_MSDC_USE_CACHE) && !defined(CONFIG_MACH_FPGA)
	int i;
	enum boot_mode_t mode;

	/*
	 * Enable cache by boot mode:
	 * 1. Enable in normal boot up, alarm, and sw reboot
	 * 2. Disable cache in other modes
	 */
	mode = get_boot_mode();
	if ((mode != NORMAL_BOOT) && (mode != ALARM_BOOT)
		&& (mode != SW_REBOOT)) {
		/* Set cache_size as 0 so that mmc layer won't enable cache */
		*(ptr + 252) = *(ptr + 251) = *(ptr + 250) = *(ptr + 249) = 0;
		return 0;
	}

	/*
	 * Enable cache by eMMC vendor
	 * disable emmc cache if eMMC vendor is in emmc_cache_quirk[]
	 */
	for (i = 0; g_emmc_cache_quirk[i] != 0 ; i++) {
		if (g_emmc_cache_quirk[i] == g_emmc_id) {
			/* Set cache_size as 0
			 * so that mmc layer won't enable cache
			 */
			*(ptr + 252) = *(ptr + 251) = 0;
			*(ptr + 250) = *(ptr + 249) = 0;
			break;
		}
	}
#else
	*(ptr + 252) = *(ptr + 251) = *(ptr + 250) = *(ptr + 249) = 0;
#endif

	return 0;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to get device sector count
 * @param *data: mmc data pointer
 * @return
 *     sectors: device sector count
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static unsigned int msdc_get_sectors(struct mmc_data *data)
{
	u8 *ptr;
	unsigned int sectors;

	ptr = (u8 *) sg_virt(data->sg);

	sectors = *(ptr + EXT_CSD_SEC_CNT + 0) << 0 |
		 *(ptr + EXT_CSD_SEC_CNT + 1) << 8 |
		 *(ptr + EXT_CSD_SEC_CNT + 2) << 16 |
		 *(ptr + EXT_CSD_SEC_CNT + 3) << 24;

	return sectors;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for eMMC cache feature control
 * @param *host: msdc host manipulator pointer
 * @param enable:enable/disable cache feature
 * @param *status:  returned commad response
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
int msdc_cache_ctrl(struct msdc_host *host, unsigned int enable,
	u32 *status)
{
	struct mmc_command cmd = { 0 };
	struct mmc_request mrq = { 0 };
	u32 err;

	cmd.opcode = MMC_SWITCH;
	cmd.arg = (MMC_SWITCH_MODE_WRITE_BYTE << 24)
		| (EXT_CSD_CACHE_CTRL << 16) | (!!enable << 8)
		| EXT_CSD_CMD_SET_NORMAL;
	cmd.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;

	mrq.cmd = &cmd;
	cmd.mrq = &mrq;
	cmd.data = NULL;

	ERR_MSG("do eMMC %s Cache\n", (enable ? "enable" : "disable"));
	err = msdc_do_command(host, &cmd, CMD_TIMEOUT);

	if (status)
		*status = cmd.resp[0];
	if (!err) {
		host->mmc->card->ext_csd.cache_ctrl = !!enable;
		/* FIX ME, check if the next 2 line can be removed */
		host->autocmd |= MSDC_AUTOCMD23;
	}

	return err;
}

#ifdef MTK_MSDC_USE_CACHE
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to update eMMC cache flush status
 * @param *host: msdc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @param *data: mmc data pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_update_cache_flush_status(struct msdc_host *host,
	struct mmc_request *mrq, struct mmc_data *data,
	u32 l_bypass_flush)
{
	struct mmc_command *cmd = mrq->cmd;
	struct mmc_command *sbc = NULL;

	if (!check_mmc_cache_ctrl(host->mmc->card))
		return;

#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
	if (check_mmc_cmd47(cmd->opcode))
		sbc = host->mmc->areq_que[(cmd->arg>>16)&0x1f]->mrq_que->sbc;
	else
#endif
	if (check_mmc_cmd2425(cmd->opcode))
		sbc = mrq->sbc;

	if (sbc) {
		if ((host->error == 0)
		 && ((sbc->arg & (0x1 << 24)) || (sbc->arg & (0x1 << 31)))) {
			/* if reliable write, or force prg write succeed,
			 * do set cache flushed status
			 */
			if (g_cache_status == CACHE_UN_FLUSHED) {
				g_cache_status = CACHE_FLUSHED;
				g_flush_data_size = 0;
			}
		} else if (host->error == 0) {
			/* if normal write succee,
			 * do clear the cache flushed status
			 */
			if (g_cache_status == CACHE_FLUSHED)
				g_cache_status = CACHE_UN_FLUSHED;

			g_flush_data_size += data->blocks;
		} else if (host->error) {
			g_flush_data_size += data->blocks;
			ERR_MSG("write error happened, g_flush_data_size=%lld",
				g_flush_data_size);
		}
	} else if (l_bypass_flush == 0) {
		if (host->error == 0) {
			/* if flush cache of emmc device successfully,
			 * do set the cache flushed status
			 */
			g_cache_status = CACHE_FLUSHED;
			g_flush_data_size = 0;
		} else {
			g_flush_error_happened = 1;
		}
	}
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to check eMMC cache error
 * @param *host: msdc host manipulator pointer
 * @param *cmd: mmc command pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_check_cache_flush_error(struct msdc_host *host,
	struct mmc_command *cmd)
{
	if (g_flush_error_happened &&
	    check_mmc_cache_ctrl(host->mmc->card) &&
	    check_mmc_cache_flush_cmd(cmd)) {
		g_flush_error_count++;
		g_flush_error_happened = 0;

		/*
		 * if reinit emmc at resume, cache should not be enabled
		 * because too much flush error, so add cache quirk for
		 * this emmmc.
		 * if awake emmc at resume, cache should not be enabled
		 * because too much flush error, so force set cache_size = 0
		 */
		if (g_flush_error_count >= MSDC_MAX_FLUSH_COUNT) {
			if (!msdc_cache_ctrl(host, 0, NULL)) {
				g_emmc_cache_quirk[0] = g_emmc_id;
				host->mmc->card->ext_csd.cache_size = 0;
			}
			pr_err("msdc%d:flush cache error count = %d, Disable cache\n",
				host->id, g_flush_error_count);
		}
	}
}
#endif

/*--------------------------------------------------------------------------*/
/* mmc_host_ops members                                                     */
/*--------------------------------------------------------------------------*/
/* The abort condition when PIO read/write
 *  tmo:
 */
 /** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to abort access in PIO mode
 * @param *host: msdc host manipulator pointer
 * @param *data: mmc data pointer
 * @param timo: timeout setting
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_pio_abort(struct msdc_host *host, struct mmc_data *data,
	unsigned long tmo)
{
	int  ret = 0;
	void __iomem *base = host->base;

	if (atomic_read(&host->abort))
		ret = 1;

	if (time_after(jiffies, tmo)) {
		data->error = (unsigned int)-ETIMEDOUT;
		ERR_MSG("XXX PIO Data Timeout: CMD<%d>",
			host->mrq->cmd->opcode);
		msdc_dump_info(host->id);
		ret = 1;
	}

	if (ret) {
		msdc_reset_hw(host->id);
		ERR_MSG("msdc pio find abort");
	}

	return ret;
}

/*
 *  Need to add a timeout, or WDT timeout, system reboot.
 */
/* pio mode data read/write */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for PIO mode reading
 * @param *host: msdc host manipulator pointer
 * @param *data: mmc data pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_pio_read(struct msdc_host *host, struct mmc_data *data)
{
	struct scatterlist *sg = data->sg;
	void __iomem *base = host->base;
	u32 num = data->sg_len;
	u32 *ptr;
	u8 *u8ptr;
	u32 left = 0;
	u32 count, size = 0;
	u32 wints = MSDC_INTEN_DATTMO | MSDC_INTEN_DATCRCERR
		| MSDC_INTEN_XFER_COMPL;
	u32 ints = 0;
	bool get_xfer_done = 0;
	unsigned long tmo = jiffies + DAT_TIMEOUT;
	struct page *hmpage = NULL;
	int i = 0, subpage = 0, totalpages = 0;
	int flag = 0;
	ulong kaddr[DIV_ROUND_UP(MAX_SGMT_SZ, PAGE_SIZE)];

	WARN_ON(sg == NULL);
	/* MSDC_CLR_BIT32(MSDC_INTEN, wints); */
	while (1) {
		if (!get_xfer_done) {
			ints = MSDC_READ32(MSDC_INT);
			latest_int_status[host->id] = ints;
			ints &= wints;
			MSDC_WRITE32(MSDC_INT, ints);
		}
		if (ints & (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR))
			goto error;
		else if (ints & MSDC_INT_XFER_COMPL)
			get_xfer_done = 1;

		if (get_xfer_done && (num == 0) && (left == 0))
			break;
		if (msdc_pio_abort(host, data, tmo))
			goto end;
		if ((num == 0) && (left == 0))
			continue;
		left = msdc_sg_len(sg, host->dma_xfer);
		ptr = sg_virt(sg);
		flag = 0;

		if  ((ptr != NULL) &&
		     !(PageHighMem((struct page *)(sg->page_link & ~0x3)))) {
			goto check_fifo1;
		}

		hmpage = (struct page *)(sg->page_link & ~0x3);
		totalpages = DIV_ROUND_UP((left + sg->offset), PAGE_SIZE);
		subpage = (left + sg->offset) % PAGE_SIZE;
		if (subpage != 0 || (sg->offset != 0))
			/* N_MSG(OPS, "msdc%d: read size or start
			 * not align, hmpage %lx,sg offset %x\n",
			 *	host->id, (ulong)hmpage,
			 *	sg->offset);
			 */
		for (i = 0; i < totalpages; i++) {
			kaddr[i] = (ulong) kmap(hmpage + i);
			if ((i > 0) && ((kaddr[i] - kaddr[i - 1]) != PAGE_SIZE))
				flag = 1;
			if (!kaddr[i])
				ERR_MSG("msdc0:kmap failed %lx", kaddr[i]);
		}

		ptr = sg_virt(sg);

		if (ptr == NULL)
			ERR_MSG("msdc0:sg_virt %p", ptr);

		if (flag == 0)
			goto check_fifo1;

		/* High memory and more than 1 va address va
		 * and not continuous
		 */
		/* pr_err("msdc0: kmap not continuous %x %x %x\n",
		 * left,kaddr[i],kaddr[i-1]);
		 */
		for (i = 0; i < totalpages; i++) {
			left = PAGE_SIZE;
			ptr = (u32 *) kaddr[i];

			if (i == 0) {
				left = PAGE_SIZE - sg->offset;
				ptr = (u32 *) (kaddr[i] + sg->offset);
			}
			if ((subpage != 0) && (i == (totalpages-1)))
				left = subpage;

check_fifo1:
			if ((flag == 1) && (left == 0))
				continue;
			else if ((flag == 0) && (left == 0))
				goto check_fifo_end;

			if ((msdc_rxfifocnt() >= MSDC_FIFO_THD) &&
			    (left >= MSDC_FIFO_THD)) {
				count = MSDC_FIFO_THD >> 2;
				do {
#ifdef MTK_MSDC_DUMP_FIFO
					pr_debug("0x%x ", msdc_fifo_read32());
#else
					*ptr++ = msdc_fifo_read32();
#endif
				} while (--count);
				left -= MSDC_FIFO_THD;
			} else if ((left < MSDC_FIFO_THD) &&
				    msdc_rxfifocnt() >= left) {
				while (left > 3) {
#ifdef MTK_MSDC_DUMP_FIFO
					pr_debug("0x%x ", msdc_fifo_read32());
#else
					*ptr++ = msdc_fifo_read32();
#endif
					left -= 4;
				}

				u8ptr = (u8 *) ptr;
				while (left) {
#ifdef MTK_MSDC_DUMP_FIFO
					pr_debug("0x%x ", msdc_fifo_read8());
#else
					*u8ptr++ = msdc_fifo_read8();
#endif
					left--;
					pr_err("[mmc] msdc_pio_read-1: flag=%d, eft=%d\n",
					 flag, left);
				}
			} else {
				ints = MSDC_READ32(MSDC_INT);
				latest_int_status[host->id] = ints;

				if (ints&
				    (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR)) {
					MSDC_WRITE32(MSDC_INT, ints);
					goto error;
				}
			}

			if (msdc_pio_abort(host, data, tmo))
				goto end;

			goto check_fifo1;
		}

check_fifo_end:
		if (hmpage != NULL) {
			/* pr_err("read msdc0:unmap %x\n", hmpage); */
			for (i = 0; i < totalpages; i++)
				kunmap(hmpage + i);

			hmpage = NULL;
		}
		size += msdc_sg_len(sg, host->dma_xfer);
		sg = sg_next(sg);
		num--;
	}
 end:
	if (hmpage != NULL) {
		for (i = 0; i < totalpages; i++)
			kunmap(hmpage + i);
		/* pr_err("msdc0 read unmap:\n"); */
	}
	data->bytes_xfered += size;
	N_MSG(FIO, "        PIO Read<%d>bytes", size);

	if (data->error)
		ERR_MSG("read pio data->error<%d> left<%d> size<%d>",
			data->error, left, size);

	if (!data->error)
		host->prev_cmd_cause_dump = 0;

	return data->error;

error:
	if (ints & MSDC_INT_DATCRCERR) {
		ERR_MSG("DAT CRC error (0x%x), Left DAT: %d bytes\n",
			ints, left);
		data->error = (unsigned int)-EILSEQ;
	} else if (ints & MSDC_INT_DATTMO) {
		ERR_MSG("DAT TMO error (0x%x), Left DAT: %d bytes\n",
			ints, left);
		msdc_dump_info(host->id);
		data->error = (unsigned int)-ETIMEDOUT;
	}
	msdc_reset_hw(host->id);

	goto end;
}

/* please make sure won't using PIO when size >= 512
 * which means, memory card block read/write won't using pio
 * then don't need to handle the CMD12 when data error.
 */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for PIO mode write
 * @param *host: msdc host manipulator pointer
 * @param *data: mmc data pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_pio_write(struct msdc_host *host, struct mmc_data *data)
{
	void __iomem *base = host->base;
	struct scatterlist *sg = data->sg;
	u32 num = data->sg_len;
	u32 *ptr;
	u8 *u8ptr;
	u32 left = 0;
	u32 count, size = 0;
	u32 wints = MSDC_INTEN_DATTMO | MSDC_INTEN_DATCRCERR
		| MSDC_INTEN_XFER_COMPL;
	bool get_xfer_done = 0;
	unsigned long tmo = jiffies + DAT_TIMEOUT;
	u32 ints = 0;
	struct page *hmpage = NULL;
	int i = 0, totalpages = 0;
	int flag, subpage = 0;
	ulong kaddr[DIV_ROUND_UP(MAX_SGMT_SZ, PAGE_SIZE)];

	/* MSDC_CLR_BIT32(MSDC_INTEN, wints); */
	while (1) {
		if (!get_xfer_done) {
			ints = MSDC_READ32(MSDC_INT);
			latest_int_status[host->id] = ints;
			ints &= wints;
			MSDC_WRITE32(MSDC_INT, ints);
		}
		if (ints & (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR))
			goto error;
		else if (ints & MSDC_INT_XFER_COMPL)
			get_xfer_done = 1;

		if ((get_xfer_done == 1) && (num == 0) && (left == 0))
			break;
		if (msdc_pio_abort(host, data, tmo))
			goto end;
		if ((num == 0) && (left == 0))
			continue;
		left = msdc_sg_len(sg, host->dma_xfer);
		ptr = sg_virt(sg);

		flag = 0;

		/* High memory must kmap, if already mapped,
		 * only add counter
		 */
		if  ((ptr != NULL) &&
		     !(PageHighMem((struct page *)(sg->page_link & ~0x3))))
			goto check_fifo1;

		hmpage = (struct page *)(sg->page_link & ~0x3);
		totalpages = DIV_ROUND_UP(left + sg->offset, PAGE_SIZE);
		subpage = (left + sg->offset) % PAGE_SIZE;

		/* if ((subpage != 0) || (sg->offset != 0))
		 *	N_MSG(OPS, "msdc%d: write size or start not align
		 *  %x, %x,hmpage %lx,sg offset %x\n",
		 *		host->id, subpage, left, (ulong)hmpage,
		 *		sg->offset);
		 */

		/* Kmap all need pages, */
		for (i = 0; i < totalpages; i++) {
			kaddr[i] = (ulong) kmap(hmpage + i);
			if ((i > 0) && ((kaddr[i] - kaddr[i - 1]) != PAGE_SIZE))
				flag = 1;
			if (!kaddr[i])
				ERR_MSG("msdc0:kmap failed %lx\n", kaddr[i]);
		}

		ptr = sg_virt(sg);

		if (ptr == NULL)
			ERR_MSG("msdc0:write sg_virt %p\n", ptr);

		if (flag == 0)
			goto check_fifo1;

		/* High memory and more than 1 va address va
		 * may be not continuous
		 */
		/* pr_err(ERR "msdc0:w kmap not continuous %x %x %x\n",
		 * left, kaddr[i], kaddr[i-1]);
		 */
		for (i = 0; i < totalpages; i++) {
			left = PAGE_SIZE;
			ptr = (u32 *) kaddr[i];

			if (i == 0) {
				left = PAGE_SIZE - sg->offset;
				ptr = (u32 *) (kaddr[i] + sg->offset);
			}
			if (subpage != 0 && (i == (totalpages - 1)))
				left = subpage;

check_fifo1:
			if ((flag == 1) && (left == 0))
				continue;
			else if ((flag == 0) && (left == 0))
				goto check_fifo_end;

			if (left >= MSDC_FIFO_SZ && msdc_txfifocnt() == 0) {
				count = MSDC_FIFO_SZ >> 2;
				do {
					msdc_fifo_write32(*ptr);
					ptr++;
				} while (--count);
				left -= MSDC_FIFO_SZ;
			} else if (left < MSDC_FIFO_SZ &&
				   msdc_txfifocnt() == 0) {
				while (left > 3) {
					msdc_fifo_write32(*ptr);
					ptr++;
					left -= 4;
				}
				u8ptr = (u8 *) ptr;
				while (left) {
					msdc_fifo_write8(*u8ptr);
					u8ptr++;
					left--;
				}
			} else {
				ints = MSDC_READ32(MSDC_INT);
				latest_int_status[host->id] = ints;

				if (ints&
				    (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR)) {
					MSDC_WRITE32(MSDC_INT, ints);
					goto error;
				}
			}

			if (msdc_pio_abort(host, data, tmo))
				goto end;

			goto check_fifo1;
		}

check_fifo_end:
		if (hmpage != NULL) {
			for (i = 0; i < totalpages; i++)
				kunmap(hmpage + i);

			hmpage = NULL;

		}
		size += msdc_sg_len(sg, host->dma_xfer);
		sg = sg_next(sg);
		num--;
	}
 end:
	if (hmpage != NULL) {
		for (i = 0; i < totalpages; i++)
			kunmap(hmpage + i);
		pr_err("msdc0 write unmap 0x%x:\n", left);
	}
	data->bytes_xfered += size;
	N_MSG(FIO, "        PIO Write<%d>bytes", size);

	if (data->error)
		ERR_MSG("write pio data->error<%d> left<%d> size<%d>",
			data->error, left, size);

	if (!data->error)
		host->prev_cmd_cause_dump = 0;

	return data->error;

error:
	if (ints & MSDC_INT_DATCRCERR) {
		ERR_MSG("DAT CRC error (0x%x), Left DAT: %d bytes\n",
			ints, left);
		data->error = (unsigned int)-EILSEQ;
	} else if (ints & MSDC_INT_DATTMO) {
		ERR_MSG("DAT TMO error (0x%x), Left DAT: %d bytes\n",
			ints, left);
		msdc_dump_info(host->id);
		data->error = (unsigned int)-ETIMEDOUT;
	}
	msdc_reset_hw(host->id);

	goto end;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to start DMA transaction
 * @param *host: msdc host manipulator pointer
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_dma_start(struct msdc_host *host)
{
	void __iomem *base = host->base;
	u32 wints = MSDC_INTEN_XFER_COMPL | MSDC_INTEN_DATTMO
		| MSDC_INTEN_DATCRCERR;

	if (host->autocmd & MSDC_AUTOCMD12)
		wints |= MSDC_INT_ACMDCRCERR | MSDC_INT_ACMDTMO
			| MSDC_INT_ACMDRDY;
	MSDC_SET_FIELD(MSDC_DMA_CTRL, MSDC_DMA_CTRL_START, 1);

	MSDC_SET_BIT32(MSDC_INTEN, wints);

	N_MSG(DMA, "DMA start");
	/* Schedule delayed work to check if data0 keeps busy */
	if (host->data && host->data->flags & MMC_DATA_WRITE) {
		if (host->hw->host_function == MSDC_EMMC)
			/* insure eMMC write time */
			host->write_timeout_ms = 20 * 1000;
		else
			host->write_timeout_ms = min_t(u32, max_t(u32,
				host->data->blocks * 500,
				host->data->timeout_ns / 1000000), 10 * 1000);
		schedule_delayed_work(&host->write_timeout,
			msecs_to_jiffies(host->write_timeout_ms));
		N_MSG(DMA, "DMA Data Busy Timeout:%u ms, schedule_delayed_work",
			host->write_timeout_ms);
	}
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to stop DMA transaction
 * @param *host: msdc host manipulator pointer
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_dma_stop(struct msdc_host *host)
{
	void __iomem *base = host->base;
	int retry = 500;
	int count = 1000;
	u32 wints = MSDC_INTEN_XFER_COMPL | MSDC_INTEN_DATTMO
		| MSDC_INTEN_DATCRCERR;

	/* Clear DMA data busy timeout */
	if (host->data && (host->data->flags & MMC_DATA_WRITE)) {
		cancel_delayed_work(&host->write_timeout);
		N_MSG(DMA, "DMA Data Busy Timeout:%u ms, cancel_delayed_work",
			host->write_timeout_ms);
		host->write_timeout_ms = 0; /* clear timeout */
	}

	/* handle autocmd12 error in msdc_irq */
	if (host->autocmd & MSDC_AUTOCMD12)
		wints |= MSDC_INT_ACMDCRCERR | MSDC_INT_ACMDTMO
			| MSDC_INT_ACMDRDY;
	N_MSG(DMA, "DMA status: 0x%.8x", MSDC_READ32(MSDC_DMA_CFG));

	MSDC_SET_FIELD(MSDC_DMA_CTRL, MSDC_DMA_CTRL_STOP, 1);
	msdc_retry((MSDC_READ32(MSDC_DMA_CFG) & MSDC_DMA_CFG_STS), retry,
		count, host->id);
	if (retry == 0)
		ERR_MSG("DMA stop retry timeout");

	MSDC_CLR_BIT32(MSDC_INTEN, wints); /* Not just xfer_comp */

	N_MSG(DMA, "DMA stop");
}

/* calc checksum */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to calculate data checksum
 * @param *buf: data buffer pointer
 * @param len: data length
 * @return
 *     checksum
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static u8 msdc_dma_calcs(u8 *buf, u32 len)
{
	u32 i, sum = 0;

	for (i = 0; i < len; i++)
		sum += buf[i];
	return 0xFF - (u8) sum;
}

/* gpd bd setup + dma registers */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to configure DMA related setting
 * @param *host: msdc host manipulator pointer
 * @param *dma: msdc dma pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_dma_config(struct msdc_host *host, struct msdc_dma *dma)
{
	void __iomem *base = host->base;
	u32 sglen = dma->sglen;
	u32 j, bdlen;
	dma_addr_t dma_address;
	u32 dma_len;
	u8  blkpad, dwpad, chksum;
	struct scatterlist *sg = dma->sg;
	struct gpd_t *gpd;
	struct bd_t *bd, vbd = {0};

	switch (dma->mode) {
	case MSDC_MODE_DMA_BASIC:
		WARN_ON(dma->sglen != 1);
		dma_address = sg_dma_address(sg);
		dma_len = msdc_sg_len(sg, host->dma_xfer);

		N_MSG(DMA, "BASIC DMA len<%x> dma_address<%llx>",
			dma_len, (u64)dma_address);

		MSDC_SET_FIELD(MSDC_DMA_SA_HIGH, MSDC_DMA_SURR_ADDR_HIGH4BIT,
			(u32) ((dma_address >> 32) & 0xF));
		MSDC_WRITE32(MSDC_DMA_SA, (u32) (dma_address & 0xFFFFFFFFUL));

		MSDC_SET_FIELD(MSDC_DMA_CTRL, MSDC_DMA_CTRL_LASTBUF, 1);
		MSDC_WRITE32(MSDC_DMA_LEN, dma_len);
		MSDC_SET_FIELD(MSDC_DMA_CTRL, MSDC_DMA_CTRL_BRUSTSZ,
			dma->burstsz);
		MSDC_SET_FIELD(MSDC_DMA_CTRL, MSDC_DMA_CTRL_MODE, 0);
		break;
	case MSDC_MODE_DMA_DESC:
		blkpad = (dma->flags & DMA_FLAG_PAD_BLOCK) ? 1 : 0;
		dwpad  = (dma->flags & DMA_FLAG_PAD_DWORD) ? 1 : 0;
		chksum = (dma->flags & DMA_FLAG_EN_CHKSUM) ? 1 : 0;

		WARN_ON(sglen > MAX_BD_PER_GPD);

		gpd = dma->gpd;
		bd  = dma->bd;
		bdlen = sglen;

		gpd->hwo = 1;   /* hw will clear it */
		gpd->bdp = 1;
		gpd->chksum = 0;        /* need to clear first. */
		if (chksum)
			gpd->chksum = msdc_dma_calcs((u8 *) gpd, 16);

		for (j = 0; j < bdlen; j++) {
#ifdef MSDC_DMA_VIOLATION_DEBUG
			if (g_dma_debug[host->id] &&
			    (msdc_latest_op[host->id] == OPER_TYPE_READ)) {
				pr_debug("[%s] msdc%d read to 0x10000\n",
					__func__, host->id);
				dma_address = 0x10000;
			} else
#endif
				dma_address = sg_dma_address(sg);

			dma_len = msdc_sg_len(sg, host->dma_xfer);

			N_MSG(DMA, "DESC DMA len<%x> dma_address<%llx>",
				dma_len, (u64)dma_address);

			memcpy(&vbd, &bd[j], sizeof(struct bd_t));

			msdc_init_bd(&vbd, blkpad, dwpad, dma_address,
				dma_len);

			if (j == bdlen - 1)
				vbd.eol = 1;  /* the last bd */
			else
				vbd.eol = 0;

			/* checksume need to clear first */
			vbd.chksum = 0;
			if (chksum)
				vbd.chksum = msdc_dma_calcs((u8 *) (&vbd), 16);

			memcpy(&bd[j], &vbd, sizeof(struct bd_t));

			sg++;
		}
#ifdef MSDC_DMA_VIOLATION_DEBUG
		if (g_dma_debug[host->id] &&
		    (msdc_latest_op[host->id] == OPER_TYPE_READ))
			g_dma_debug[host->id] = 0;
#endif

		dma->used_gpd += 2;
		dma->used_bd += bdlen;

		MSDC_SET_FIELD(MSDC_DMA_CFG, MSDC_DMA_CFG_DECSEN, chksum);
		MSDC_SET_FIELD(MSDC_DMA_CTRL, MSDC_DMA_CTRL_BRUSTSZ,
			dma->burstsz);
		MSDC_SET_FIELD(MSDC_DMA_CTRL, MSDC_DMA_CTRL_MODE, 1);

		MSDC_SET_FIELD(MSDC_DMA_SA_HIGH, MSDC_DMA_SURR_ADDR_HIGH4BIT,
			(u32) ((dma->gpd_addr >> 32) & 0xF));
		MSDC_WRITE32(MSDC_DMA_SA, (u32) (dma->gpd_addr & 0xFFFFFFFFUL));
		break;

	default:
		break;
	}

	N_MSG(DMA, "DMA_CTRL = 0x%x", MSDC_READ32(MSDC_DMA_CTRL));
	N_MSG(DMA, "DMA_CFG  = 0x%x", MSDC_READ32(MSDC_DMA_CFG));
	N_MSG(DMA, "DMA_SA_HIGH   = 0x%x", MSDC_READ32(MSDC_DMA_SA_HIGH));
	N_MSG(DMA, "DMA_SA   = 0x%x", MSDC_READ32(MSDC_DMA_SA));

	return 0;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to setup DMA configuration for each DMA mode
 * @param *host: msdc host manipulator pointer
 * @param *dma: msdc dma pointer
 * @param *sg: scatter list table
 * @param sglen: scatter table length
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_dma_setup(struct msdc_host *host, struct msdc_dma *dma,
	struct scatterlist *sg, unsigned int sglen)
{
	WARN_ON(sglen > MAX_BD_NUM);     /* not support currently */

	dma->sg = sg;
	dma->flags = DMA_FLAG_EN_CHKSUM;
	/* dma->flags = DMA_FLAG_NONE; */ /* CHECKME */
	dma->sglen = sglen;
	dma->xfersz = host->xfer_size;
	dma->burstsz = MSDC_BRUST_64B;

	if (sglen == 1)
		dma->mode = MSDC_MODE_DMA_BASIC;
	else
		dma->mode = MSDC_MODE_DMA_DESC;

	N_MSG(DMA, "DMA mode<%d> sglen<%d> xfersz<%d>", dma->mode, dma->sglen,
		dma->xfersz);

	msdc_dma_config(host, dma);
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to clear variable related to DMA mode
 * @param *host: msdc host manipulator pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_dma_clear(struct msdc_host *host)
{
	void __iomem *base = host->base;

	host->data = NULL;
	host->mrq = NULL;
	host->dma_xfer = 0;
	msdc_dma_off();
	host->dma.used_bd = 0;
	host->dma.used_gpd = 0;
	host->blksz = 0;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to dump information related to cmd and data
 * @param *host: msdc host manipulator pointer
 * @param *cmd: mmc command pointer
 * @param *data: mmc data pointer
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_log_data_cmd(struct msdc_host *host, struct mmc_command *cmd,
	struct mmc_data *data)
{
	if (!(is_card_sdio(host) || (host->hw->flags & MSDC_SDIO_IRQ))) {
		if (!check_mmc_cmd2425(cmd->opcode) &&
		    !check_mmc_cmd1718(cmd->opcode)) {
			N_MSG(NRW, "CMD<%3d> Resp<0x%8x> size<%d>",
				cmd->opcode, cmd->resp[0],
				data->blksz * data->blocks);
		} else if (cmd->opcode != 13) { /* by pass CMD13 */
			N_MSG(NRW, "CMD<%3d> Resp<%8x %8x %8x %8x>",
				cmd->opcode, cmd->resp[0],
				cmd->resp[1], cmd->resp[2], cmd->resp[3]);
		} else {
			N_MSG(RW, "CMD<%3d> Resp<0x%8x> block<%d>",
				cmd->opcode, cmd->resp[0], data->blocks);
		}
	}
}

/** @ingroup type_group_linux_eMMC_def
 * @brief eMMC request mode definations
 * @{
 */
/** non-async request */
#define NON_ASYNC_REQ           0
/** async request */
#define ASYNC_REQ               1
/**
 * @}
 */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for interface to send STOP command(CMD12/23)
 * @param *host: msdc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @param req_type: request type(async/non-sync)
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_if_send_stop(struct msdc_host *host,
	struct mmc_request *mrq, int req_type)
{

	if (!check_mmc_cmd1825(mrq->cmd->opcode))
		return 0;

#ifdef MTK_MSDC_USE_CMD23
	if ((host->hw->host_function == MSDC_EMMC) && (req_type != ASYNC_REQ)) {
		/* multi r/w without cmd23 and autocmd12, need manual cmd12 */
		/* if PIO mode and autocmd23 enable, cmd12 need send,
		 * because autocmd23 is disable under PIO
		 */
		if (((mrq->sbc == NULL) &&
		     !(host->autocmd & MSDC_AUTOCMD12))
		 || (!host->dma_xfer && mrq->sbc &&
		     (host->autocmd & MSDC_AUTOCMD23))) {
			if (msdc_do_command(host, mrq->cmd->data->stop,
				CMD_TIMEOUT) != 0)
				return 1;
		}
	} else
#endif
	{
		if (!mrq->cmd->data || !mrq->cmd->data->stop)
			return 0;
		if ((mrq->cmd->error != 0)
		 || (mrq->cmd->data->error != 0)
		 || !(host->autocmd & MSDC_AUTOCMD12)) {
			if (msdc_do_command(host, mrq->cmd->data->stop,
				CMD_TIMEOUT) != 0)
				return 1;
		}
	}

	return 0;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for RW transaction with sync DMA mode
 * @param *host: msdc host manipulator pointer
 * @param *cmd: mmc command pointer
 * @param *data: mmc data pointer
 * @param *mrq: mmc request pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_rw_cmd_using_sync_dma(struct mmc_host *mmc,
				      struct mmc_command *cmd,
				      struct mmc_data *data,
				      struct mmc_request *mrq)
{
	struct msdc_host *host = mmc_priv(mmc);
	void __iomem *base = host->base;
	int dir;

	pr_err("[mmc] msdc_rw_cmd_using_sync_dma\n");
	msdc_dma_on();  /* enable DMA mode first!! */

	init_completion(&host->xfer_done);

	if (msdc_command_start(host, cmd, CMD_TIMEOUT) != 0)
		return -1;

	dir = data->flags & MMC_DATA_READ ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	(void)dma_map_sg(mmc_dev(mmc), data->sg, data->sg_len, dir);

	if (msdc_command_resp_polling(host, cmd, CMD_TIMEOUT) != 0)
		return -2;

	/* start DMA after response, so that DMA nned not be stopped
	 * if response error occurs
	 */
	msdc_dma_setup(host, &host->dma, data->sg, data->sg_len);
	msdc_dma_start(host);

	spin_unlock(&host->lock);
	if (!wait_for_completion_timeout(&host->xfer_done, DAT_TIMEOUT)) {
		ERR_MSG("XXX CMD<%d> ARG<0x%x> wait xfer_done<%d> timeout!!",
			cmd->opcode, cmd->arg, data->blocks * data->blksz);

		host->sw_timeout++;

		msdc_dump_info(host->id);
		data->error = (unsigned int)-ETIMEDOUT;
		msdc_reset(host->id);
	}
	spin_lock(&host->lock);

	msdc_dma_stop(host);

	if ((mrq->data && mrq->data->error)
	 || (mrq->stop && mrq->stop->error && (host->autocmd & MSDC_AUTOCMD12))
	 || (mrq->sbc && mrq->sbc->error && (host->autocmd & MSDC_AUTOCMD23))) {
		msdc_clr_fifo(host->id);
		msdc_clr_int();
	}

	host->dma_xfer = 0;
	msdc_dma_off();
	host->dma.used_bd = 0;
	host->dma.used_gpd = 0;
	dma_unmap_sg(mmc_dev(mmc), data->sg, data->sg_len, dir);

	return 0;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to prepare related setting for request
 * @param *host: msdc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_do_request_prepare(struct msdc_host *host,
				   struct mmc_request *mrq)
{
	void __iomem *base = host->base;
	struct mmc_data *data = mrq->cmd->data;
#ifdef MTK_MSDC_USE_CACHE
	u32 l_force_prg = 0;
#endif

#ifdef MTK_MSDC_USE_CMD23
	u32 l_card_no_cmd23 = 0;
#endif
	atomic_set(&host->abort, 0);
#ifndef CONFIG_MTK_EMMC_CQ_SUPPORT
	/* FIX ME: modify as runtime check of enabling of cmdq */
	/* check msdc work ok: RX/TX fifocnt must be zero after last request
	 * if find abnormal, try to reset msdc first
	 */
	if (msdc_txfifocnt() || msdc_rxfifocnt()) {
		pr_err("[SD%d] register abnormal,please check!\n", host->id);
		msdc_reset_hw(host->id);
	}
#endif

	WARN_ON(data->blksz > HOST_MAX_BLKSZ);

	data->error = 0;
	msdc_latest_op[host->id] = (data->flags & MMC_DATA_READ)
		? OPER_TYPE_READ : OPER_TYPE_WRITE;

	host->data = data;

	host->xfer_size = data->blocks * data->blksz;
	host->blksz = data->blksz;

	if (data->flags & MMC_DATA_READ) {
		if ((host->timeout_ns != data->timeout_ns) ||
		    (host->timeout_clks != data->timeout_clks)) {
			msdc_set_timeout(host, data->timeout_ns,
				data->timeout_clks);
		}
	}

	MSDC_WRITE32(SDC_BLK_NUM, data->blocks);

#ifdef MTK_MSDC_USE_CMD23
	if (mrq->sbc) {
		host->autocmd &= ~MSDC_AUTOCMD12;

#ifdef MTK_MSDC_USE_CACHE
		if (check_mmc_cache_ctrl(host->mmc->card)
		 && (mrq->cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK)) {
			l_force_prg = !msdc_can_apply_cache(mrq->cmd->arg,
				data->blocks);
			if (l_force_prg && !(mrq->sbc->arg & (0x1 << 31)))
				mrq->sbc->arg |= (1 << 24);
		}
#endif

		if (0 == (host->autocmd & MSDC_AUTOCMD23)) {
			if (msdc_command_start(host, mrq->sbc,
				CMD_TIMEOUT) != 0)
				return -1;

			/* then wait command done */
			if (msdc_command_resp_polling(host, mrq->sbc,
				CMD_TIMEOUT) != 0) {
				return -2;
			}
		}
	} else {
		/* some sd card may not support cmd23,
		 * some emmc card have problem with cmd23,
		 * so use cmd12 here
		 */
		if (host->hw->host_function != MSDC_SDIO) {
			host->autocmd |= MSDC_AUTOCMD12;
			if (0 != (host->autocmd & MSDC_AUTOCMD23)) {
				host->autocmd &= ~MSDC_AUTOCMD23;
				l_card_no_cmd23 = 1;
			}
		}
	}

	return l_card_no_cmd23;
#else
	if ((host->dma_xfer) && (host->hw->host_function != MSDC_SDIO))
		host->autocmd |= MSDC_AUTOCMD12;

	return 0;
#endif

}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to set request error
 * @param *host: msdc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @param async: async mode option
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_if_set_request_err(struct msdc_host *host,
	struct mmc_request *mrq, int async)
{
	void __iomem *base = host->base;

#ifdef MTK_MSDC_USE_CMD23
	if (mrq->sbc && (mrq->sbc->error == (unsigned int)-EILSEQ))
		host->error |= REQ_CMD_EIO;
	if (mrq->sbc && (mrq->sbc->error == (unsigned int)-ETIMEDOUT)) {
#ifdef CONFIG_MTK_AEE_FEATURE
		aee_kernel_warning_api(__FILE__, __LINE__,
			DB_OPT_NE_JBT_TRACES|DB_OPT_DISPLAY_HANG_DUMP,
			"\n@eMMC FATAL ERROR@\n", "eMMC fatal error ");
#endif
		host->error |= REQ_CMD_TMO;
	}
#endif

	if (mrq->cmd->error == (unsigned int)-EILSEQ) {
		if (((mrq->cmd->opcode == MMC_SELECT_CARD) ||
		     (mrq->cmd->opcode == MMC_SLEEP_AWAKE))
		 && ((host->hw->host_function == MSDC_EMMC) ||
		     (host->hw->host_function == MSDC_SD))) {
			mrq->cmd->error = 0x0;
		} else {
			host->error |= REQ_CMD_EIO;
		}
	}

	if (mrq->cmd->error == (unsigned int)-ETIMEDOUT) {
		if (mrq->cmd->opcode == MMC_SLEEP_AWAKE) {
			if (mrq->cmd->arg & 0x8000) {
				pr_err("Sleep_Awake CMD timeout, MSDC_PS %0x\n",
					MSDC_READ32(MSDC_PS));
				emmc_sleep_failed = 1;
				mrq->cmd->error = 0x0;
				pr_err("eMMC sleep CMD5 TMO will reinit\n");
			} else {
				host->error |= REQ_CMD_TMO;
			}
		} else {
			host->error |= REQ_CMD_TMO;
		}
	}

	if (mrq->data && mrq->data->error) {
		if (mrq->data->flags & MMC_DATA_WRITE)
			host->error |= REQ_CRC_STATUS_ERR;
		else
			host->error |= REQ_DAT_ERR;
		/* FIXME: return cmd error for retry if data CRC error */
		if (!async)
			mrq->cmd->error = (unsigned int)-EILSEQ;
	}

#ifdef NEW_TUNE_K44
	if ((mrq->cmd->opcode != MMC_SEND_TUNING_BLOCK &&
	     mrq->cmd->opcode != MMC_SEND_TUNING_BLOCK_HS200)
	 && ((mrq->cmd->error == -EILSEQ) ||
	     (mrq->sbc && mrq->sbc->error == -EILSEQ) ||
	     (mrq->data && mrq->data->error == -EILSEQ) ||
	     (mrq->stop && mrq->stop->error == -EILSEQ)))
		mmc_retune_needed(host->mmc);
#endif

	if (mrq->stop && (mrq->stop->error == (unsigned int)-EILSEQ))
		host->error |= REQ_STOP_EIO;
	if (mrq->stop && (mrq->stop->error == (unsigned int)-ETIMEDOUT))
		host->error |= REQ_STOP_TMO;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for executing transaction request
 * @param *mmct: mmc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_do_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct msdc_host *host = mmc_priv(mmc);
	struct mmc_command *cmd;
	struct mmc_data *data;
	void __iomem *base = host->base;
	u32 l_autocmd23_is_set = 0;

#ifdef MTK_MSDC_USE_CMD23
	u32 l_card_no_cmd23 = 0;
#ifdef MTK_MSDC_USE_CACHE
	u32 l_bypass_flush = 1;
#endif
#endif
	unsigned int left = 0;
	int ret = 0;
	unsigned long pio_tmo;

	if (host->hw->flags & MSDC_SDIO_IRQ) {
		if ((u_sdio_irq_counter > 0) && ((u_sdio_irq_counter%800) == 0))
			ERR_MSG("sdio=%d,msdc=%d,SDC_CFG=%x INTEN=%x,INT=%x ",
				u_sdio_irq_counter, u_msdc_irq_counter,
				MSDC_READ32(SDC_CFG), MSDC_READ32(MSDC_INTEN),
				MSDC_READ32(MSDC_INT));
	}

	cmd = mrq->cmd;
	data = mrq->cmd->data;

#ifdef MTK_MSDC_USE_CACHE
	if ((host->hw->host_function == MSDC_EMMC)
	 && check_mmc_cache_flush_cmd(cmd)) {
		if (g_cache_status == CACHE_FLUSHED) {
			N_MSG(CHE, "bypass flush command, g_cache_status=%d",
				g_cache_status);
			l_bypass_flush = 1;
			goto done;
		} else
			l_bypass_flush = 0;
	}
#endif

	if (!data) {
#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
		if (check_mmc_cmd13_sqs(cmd)) {
			if (msdc_do_cmdq_command(host, cmd, CMD_TIMEOUT) != 0)
				goto done_no_data;
		} else
#endif
			if (msdc_do_command(host, cmd, CMD_TIMEOUT) != 0)
				goto done_no_data;

		/* Get emmc_id when send ALL_SEND_CID command */
		if ((host->hw->host_function == MSDC_EMMC) &&
			(cmd->opcode == MMC_ALL_SEND_CID))
			g_emmc_id = UNSTUFF_BITS(cmd->resp, 120, 8);

		goto done_no_data;
	}

	if (drv_mode[host->id] == MODE_PIO) {
		host->dma_xfer = 0;
		msdc_latest_transfer_mode[host->id] = MODE_PIO;
	} else if (drv_mode[host->id] == MODE_DMA) {
		host->dma_xfer = 1;
		msdc_latest_transfer_mode[host->id] = MODE_DMA;
	} else if (drv_mode[host->id] == MODE_SIZE_DEP) {
		host->dma_xfer = (host->xfer_size >= dma_size[host->id])
			? 1 : 0;
		msdc_latest_transfer_mode[host->id] =
			host->dma_xfer ? MODE_DMA : MODE_PIO;
	}
	l_card_no_cmd23 = msdc_do_request_prepare(host, mrq);
	if (l_card_no_cmd23 == -1)
		goto done;
	else if (l_card_no_cmd23 == -2)
		goto stop;

	if (host->dma_xfer) {
		ret = msdc_rw_cmd_using_sync_dma(mmc, cmd, data, mrq);
		if (ret == -1)
			goto done;
		else if (ret == -2)
			goto stop;

	} else {
		if (is_card_sdio(host)) {
			msdc_reset_hw(host->id);
			msdc_dma_off();
			data->error = 0;
		}

		host->autocmd &= ~MSDC_AUTOCMD12;
		if (host->autocmd & MSDC_AUTOCMD23) {
			l_autocmd23_is_set = 1;
			host->autocmd &= ~MSDC_AUTOCMD23;
		}

		host->dma_xfer = 0;
		if (msdc_do_command(host, cmd, CMD_TIMEOUT) != 0)
			goto stop;

		/* Secondly: pio data phase */
		if (data->flags & MMC_DATA_READ) {
#ifdef MTK_MSDC_DUMP_FIFO
			pr_debug("[%s]: start pio read\n", __func__);
#endif
			if (msdc_pio_read(host, data)) {
				msdc_gate_clock(host, 0);
				msdc_ungate_clock(host);
				goto stop;      /* need cmd12 */
			}
		} else {
#ifdef MTK_MSDC_DUMP_FIFO
			pr_debug("[%s]: start pio write\n", __func__);
#endif
			if (msdc_pio_write(host, data)) {
				msdc_gate_clock(host, 0);
				msdc_ungate_clock(host);
				goto stop;
			}

			/* For write case: make sure contents in fifo
			 * flushed to device
			 */

			pio_tmo = jiffies + DAT_TIMEOUT;
			while (1) {
				left = msdc_txfifocnt();
				if (left == 0)
					break;

				if (msdc_pio_abort(host, data, pio_tmo))
					break;
			}
		}
	}

stop:
	/* pio mode had disabled autocmd23, restore it for invoking
	 * msdc_if_send_stop()
	 */
	if (l_autocmd23_is_set == 1)
		host->autocmd |= MSDC_AUTOCMD23;

	if (msdc_if_send_stop(host, mrq, NON_ASYNC_REQ))
		goto done;

done:

#ifdef MTK_MSDC_USE_CMD23
	/* for msdc use cmd23, but card not supported(sbc is NULL),
	 *  need enable autocmd23 for next request
	 */
	if (l_card_no_cmd23 == 1) {
		host->autocmd |= MSDC_AUTOCMD23;
		host->autocmd &= ~MSDC_AUTOCMD12;
	}
#endif

	if (data) {
		host->data = NULL;

		/* If eMMC we use is in g_emmc_cache_quirk[] or
		 * MTK_MSDC_USE_CACHE is not set. Driver should return
		 * cache_size = 0 in exd_csd to mmc layer
		 * So, mmc_init_card can disable cache
		 */
		if ((cmd->opcode == MMC_SEND_EXT_CSD) &&
			(host->hw->host_function == MSDC_EMMC)) {
			msdc_cache_onoff(data);
			host->total_sectors = msdc_get_sectors(data);
		}

		host->blksz = 0;

		if (sd_debug_zone[host->id])
			msdc_log_data_cmd(host, cmd, data);
	}

done_no_data:

	msdc_if_set_request_err(host, mrq, 0);

	/* mmc_blk_err_check will also do legacy request
	 * So, use '|=' instead '=' if command error occur
	 * to avoid clearing data error flag
	 */
	if (host->error & REQ_CMD_EIO)
		host->need_tune |= TUNE_LEGACY_CMD;
	else if (host->error & REQ_DAT_ERR)
		host->need_tune = TUNE_LEGACY_DATA_READ;
	else if (host->error & REQ_CRC_STATUS_ERR)
		host->need_tune = TUNE_LEGACY_DATA_WRITE;

	if (host->error & (REQ_CMD_EIO | REQ_DAT_ERR | REQ_CRC_STATUS_ERR))
		host->err_cmd = mrq->cmd->opcode;
#ifdef SDIO_ERROR_BYPASS
	if (is_card_sdio(host) && !host->error)
		host->sdio_error = 0;
#endif

#ifdef MTK_MSDC_USE_CACHE
	msdc_update_cache_flush_status(host, mrq, data, l_bypass_flush);
#endif

	return host->error;
}

#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for discarding CMDQ task
 * @param *mmc: mmc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_do_discard_task_cq(struct mmc_host *mmc,
	struct mmc_request *mrq)
{
	struct msdc_host *host = mmc_priv(mmc);
	u32 task_id;

	task_id = (mrq->sbc->arg >> 16) & 0x1f;
	memset(&mmc->deq_cmd, 0, sizeof(struct mmc_command));
	mmc->deq_cmd.opcode = MMC_CMDQ_TASK_MGMT;
	mmc->deq_cmd.arg = 2 | (task_id << 16);
	mmc->deq_cmd.flags = MMC_RSP_SPI_R2 | MMC_RSP_R1B | MMC_CMD_AC;
	mmc->deq_cmd.data = NULL;
	msdc_do_command(host, &mmc->deq_cmd, CMD_TIMEOUT);

	pr_debug("[%s]: msdc%d, discard task id %d, CMD<%d> arg<0x%08x> rsp<0x%08x>",
		__func__, host->id, task_id, mmc->deq_cmd.opcode,
		mmc->deq_cmd.arg, mmc->deq_cmd.resp[0]);

	return mmc->deq_cmd.error;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for executing request for CMDQ feature
 * @param *mmc: mmc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_do_request_cq(struct mmc_host *mmc,
	struct mmc_request *mrq)
{
	struct msdc_host *host = mmc_priv(mmc);
	struct mmc_command *cmd;
#ifdef MTK_MSDC_USE_CACHE
	u32 l_force_prg = 0;
#endif

	WARN_ON(!mmc || !mrq);

	host->error = 0;
	atomic_set(&host->abort, 0);

	cmd  = mrq->sbc;

	mrq->sbc->error = 0;
	mrq->cmd->error = 0;

#ifdef MTK_MSDC_USE_CACHE
	/* check cache enabled, write direction */
	if (check_mmc_cache_ctrl(host->mmc->card)
	 && !(cmd->arg & (0x1 << 30))) {
		l_force_prg = !msdc_can_apply_cache(mrq->cmd->arg,
			cmd->arg & 0xffff);
		/* check not reliable write */
		if (!(cmd->arg & (0x1 << 31)) && l_force_prg)
			cmd->arg |= (1 << 24);
	}
#endif

	if (msdc_do_cmdq_command(host, cmd, CMD_TIMEOUT) != 0)
		goto done1;

done1:
	if (cmd->error == (unsigned int)-EILSEQ)
		host->error |= REQ_CMD_EIO;
	else if (cmd->error == (unsigned int)-ETIMEDOUT)
		host->error |= REQ_CMD_TMO;

	cmd  = mrq->cmd;

	if (msdc_do_cmdq_command(host, cmd, CMD_TIMEOUT) != 0)
		goto done2;

done2:
	if (cmd->error == (unsigned int)-EILSEQ)
		host->error |= REQ_CMD_EIO;
	else if (cmd->error == (unsigned int)-ETIMEDOUT)
		host->error |= REQ_CMD_TMO;

	return host->error;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to send STOP command and wait device if busy
 * @param *host: msdc host manipulator pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_stop_and_wait_busy(struct msdc_host *host)
{
	void __iomem *base = host->base;
	unsigned long polling_tmo = jiffies + POLLING_BUSY;

	msdc_send_stop(host);
	pr_err("msdc%d, waiting device is not busy\n", host->id);
	while ((MSDC_READ32(MSDC_PS) & 0x10000) != 0x10000) {
		if (time_after(jiffies, polling_tmo)) {
			pr_err("msdc%d, device stuck in PRG!\n",
				host->id);
			return -1;
		}
	}

	return 0;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for executing tuning when error happen
 * @param *mmc: mmc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
#ifndef NEW_TUNE_K44
static int msdc_error_tuning(struct mmc_host *mmc,  struct mmc_request *mrq)
#else
static int msdc_error_tuning(struct mmc_host *mmc)
#endif
{
	struct msdc_host *host = mmc_priv(mmc);
	int ret = 0;
	int autok_err_type = -1;
	unsigned int tune_smpl = 0;

	msdc_ungate_clock(host);
	host->tuning_in_progress = true;

	/* autok_err_type is used for switch edge tune and CMDQ tune */
	if (host->need_tune & (TUNE_ASYNC_CMD | TUNE_LEGACY_CMD))
		autok_err_type = CMD_ERROR;
	else if (host->need_tune & (TUNE_ASYNC_DATA_READ |
		TUNE_LEGACY_DATA_READ))
		autok_err_type = DATA_ERROR;
	else if (host->need_tune & (TUNE_ASYNC_DATA_WRITE |
		TUNE_LEGACY_DATA_WRITE))
		autok_err_type = CRC_STATUS_ERROR;

	/* 1. mmc_blk_err_check will send CMD13 to check device status
	 * Don't autok/switch edge here, or it will cause CMD19 send when
	 * device is not in transfer status
	 * 2. mmc_blk_err_check will send CMD12 to stop transition if device is
	 * in RCV and DATA status.
	 * Don't autok/switch edge here, or it will cause switch edge failed
	 */
#ifndef NEW_TUNE_K44
	if ((mrq->cmd->opcode == MMC_SEND_STATUS ||
	     mrq->cmd->opcode == MMC_STOP_TRANSMISSION)
	 && host->err_cmd != mrq->cmd->opcode) {
		goto end;
	}
#endif

	pr_err("msdc%d saved device status: %x", host->id, host->device_status);
	/* clear device status */
	host->device_status = 0x0;

	/* force send stop command to turn device to transfer status */
	if (msdc_stop_and_wait_busy(host))
		goto recovery;

	if (host->hw->host_function == MSDC_SDIO)
		goto end;

	switch (mmc->ios.timing) {
	case MMC_TIMING_UHS_SDR104:
	case MMC_TIMING_UHS_SDR50:
		pr_err("msdc%d: SD UHS_SDR104/UHS_SDR50 re-autok %d times\n",
			host->id, ++host->reautok_times);
		ret = autok_execute_tuning(host, NULL);
		break;
	case MMC_TIMING_MMC_HS200:
		pr_err("msdc%d: eMMC HS200 re-autok %d times\n",
			host->id, ++host->reautok_times);
#ifdef NEW_TUNE_K44
#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
		if (autok_err_type == CMD_ERROR)
			ret = hs200_execute_tuning_cmd(host, NULL);
		else
#endif
#endif
			ret = hs200_execute_tuning(host, NULL);
		break;
	case MMC_TIMING_MMC_HS400:
		pr_err("msdc%d: eMMC HS400 re-autok %d times\n",
			host->id, ++host->reautok_times);
#ifdef NEW_TUNE_K44
#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
		if (autok_err_type == CMD_ERROR)
			ret = hs400_execute_tuning_cmd(host, NULL);
		else
#endif
#endif
			ret = hs400_execute_tuning(host, NULL);
		break;
	/* Other speed mode will tune smpl */
	default:
		tune_smpl = 1;
		pr_err("msdc%d: tune smpl %d times timing:%d err: %d\n",
			host->id, ++host->tune_smpl_times,
			mmc->ios.timing, autok_err_type);
		autok_low_speed_switch_edge(host, &mmc->ios, autok_err_type);
		break;
	}
	if (ret) {
		/* FIX ME, consider to use msdc_dump_info() to replace all */
		msdc_dump_clock_sts(host);
		pr_err("msdc%d latest_INT_status<0x%.8x>\n", host->id,
			latest_int_status[host->id]);
		msdc_dump_register(host);
		msdc_dump_dbg_register(host);
	}

	/* autok failed three times will try reinit tuning */
	if (host->reautok_times >= 4 || host->tune_smpl_times >= 4) {
recovery:
		pr_err("msdc%d autok error\n", host->id);
		/* eMMC will change to HS200 and lower frequence */
		if (host->hw->host_function == MSDC_EMMC) {
			msdc_dump_register(host);
#ifdef MSDC_SWITCH_MODE_WHEN_ERROR
			ret = emmc_reinit_tuning(mmc);
#endif
		}
	} else if (!tune_smpl) {
		pr_err("msdc%d autok pass\n", host->id);
		host->need_tune |= TUNE_AUTOK_PASS;
	}

end:
	host->tuning_in_progress = false;
	msdc_gate_clock(host, 1);

	return ret;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to execute tunung for cmd response in CMDQ flow
 * @param *mmc: mmc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @param *retry: ret count
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int tune_cmdq_cmdrsp(struct mmc_host *mmc,
	struct mmc_request *mrq, int *retry)
{
	struct msdc_host *host = mmc_priv(mmc);
	void __iomem *base = host->base;
	unsigned long polling_tmo = 0, polling_status_tmo;

	u32 err = 0, status = 0;

	/* time for wait device to return to trans state
	 * when needed to send CMD48
	 */
	polling_status_tmo = jiffies + 30 * HZ;
	do {
		err = msdc_get_card_status(mmc, host, &status);
		if (err) {
			/* wait for transfer done */
			polling_tmo = jiffies + 10 * HZ;
			pr_err("msdc%d waiting data transfer done\n",
				host->id);
			while (mmc->is_data_dma) {
				if (time_after(jiffies, polling_tmo))
					goto error;
			}
			ERR_MSG("get card status, err = %d", err);

			if (msdc_execute_tuning(mmc, MMC_SEND_STATUS)) {
				ERR_MSG("failed to updata cmd para");
				return 1;
			}

			continue;
		}

		if (status & (1 << 22)) {
			/* illegal command */
			(*retry)--;
			ERR_MSG("status = %x, illegal command, retry = %d",
				status, *retry);
			if ((mrq->cmd->error || mrq->sbc->error) && *retry)
				return 0;
			else
				return 1;
		} else {
			if (R1_CURRENT_STATE(status) != R1_STATE_TRAN) {
				if (time_after(jiffies, polling_status_tmo))
					ERR_MSG("wait trans state timeout\n");
				else {
					err = 1;
					continue;
				}
			}
			ERR_MSG("status = %x, discard task, re-send command",
				status);
			err = msdc_do_discard_task_cq(mmc, mrq);
			if (err == (unsigned int)-EIO)
				continue;
			else
				break;
		}
	} while (err);

	/* wait for transfer done */
	polling_tmo = jiffies + 10 * HZ;
	pr_err("msdc%d waiting data transfer done\n", host->id);
	while (mmc->is_data_dma) {
		if (time_after(jiffies, polling_tmo))
			goto error;
	}

#ifdef NEW_TUNE_K44
	if (msdc_error_tuning(mmc)) {
#else
	if (msdc_execute_tuning(mmc, MMC_SEND_STATUS)) {
#endif
		pr_err("msdc%d autok failed\n", host->id);
		return 1;
	}

	return 0;

error:
	ERR_MSG("waiting data transfer done TMO");
	msdc_dump_info(host->id);
	msdc_dma_stop(host);
	msdc_dma_clear(host);
	msdc_reset_hw(host->id);

	return -1;
}
#endif

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to dump information when transaction error happen
 * @param *host: msdc host manipulator pointer
 * @param *cmd: mmc command pointer
 * @param *data: mmc data pointer
 * @param *stop: mmc stop command pointer
 * @param *sbc: mmc sbc command pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_dump_trans_error(struct msdc_host   *host,
	struct mmc_command *cmd,
	struct mmc_data    *data,
	struct mmc_command *stop,
	struct mmc_command *sbc)
{
	if ((cmd->opcode == 52) && (cmd->arg == 0xc00))
		return;
	if ((cmd->opcode == 52) && (cmd->arg == 0x80000c08))
		return;

	/* SDcard bypass SDIO init CMD5 command */
	if (host->hw->host_function == MSDC_SD) {
		if (cmd->opcode == 5)
			return;
	/* eMMC bypss SDIO init command, SDcard init command */
	} else if (host->hw->host_function == MSDC_EMMC) {
		if ((cmd->opcode == 5) || (cmd->opcode == 55))
			return;
	} else {
		if (cmd->opcode == 8)
			return;
	}

	ERR_MSG("XXX CMD<%d><0x%x> Error<%d> Resp<0x%x>", cmd->opcode, cmd->arg,
		cmd->error, cmd->resp[0]);

	if (data) {
		ERR_MSG("XXX DAT block<%d> Error<%d>", data->blocks,
			data->error);
	}
	if (stop) {
		ERR_MSG("XXX STOP<%d> Error<%d> Resp<0x%x>",
			stop->opcode, stop->error, stop->resp[0]);
	}
	if (sbc) {
		ERR_MSG("XXX SBC<%d><0x%x> Error<%d> Resp<0x%x>",
			sbc->opcode, sbc->arg, sbc->error, sbc->resp[0]);
	}

#ifdef SDIO_ERROR_BYPASS
	if (is_card_sdio(host) &&
	    (host->sdio_error != -EILSEQ) &&
	    (cmd->opcode == 53) &&
	    (sg_dma_len(data->sg) > 4)) {
		host->sdio_error = -EILSEQ;
		ERR_MSG("XXX SDIO Error ByPass");
	}
#endif
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for preprocess flow before transaction
 * @param *mmc: mmc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @param is_first_request: index for 1st request or not
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_pre_req(struct mmc_host *mmc, struct mmc_request *mrq,
	bool is_first_req)
{
	struct msdc_host *host = mmc_priv(mmc);
	struct mmc_data *data;
	struct mmc_command *cmd = mrq->cmd;

	data = mrq->data;

	if (!data)
		return;

	data->host_cookie = MSDC_COOKIE_ASYNC;
	if (check_mmc_cmd1718(cmd->opcode) ||
	    check_mmc_cmd2425(cmd->opcode)) {
		host->xfer_size = data->blocks * data->blksz;
		if (drv_mode[host->id] == MODE_PIO) {
			data->host_cookie |= MSDC_COOKIE_PIO;
			msdc_latest_transfer_mode[host->id] = MODE_PIO;
		} else if (drv_mode[host->id] == MODE_DMA) {
			msdc_latest_transfer_mode[host->id] = MODE_DMA;
		} else if (drv_mode[host->id] == MODE_SIZE_DEP) {
			if (host->xfer_size < dma_size[host->id]) {
				data->host_cookie |= MSDC_COOKIE_PIO;
				msdc_latest_transfer_mode[host->id] =
					MODE_PIO;
			} else {
				msdc_latest_transfer_mode[host->id] =
					MODE_DMA;
			}
		}
	}
	if (msdc_use_async_dma(data->host_cookie)) {
		(void)dma_map_sg(mmc_dev(mmc), data->sg, data->sg_len,
			((data->flags & MMC_DATA_READ)
				? DMA_FROM_DEVICE : DMA_TO_DEVICE));
	}
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for postprocess flow after transcation
 * @param *mmc: mmc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @param err: error flag
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_post_req(struct mmc_host *mmc, struct mmc_request *mrq,
	int err)
{
	struct msdc_host *host = mmc_priv(mmc);
	struct mmc_data *data;
	int dir = DMA_FROM_DEVICE;

	data = mrq->data;
	if (data && (msdc_use_async_dma(data->host_cookie))) {
#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
		if (!mmc->card->ext_csd.cmdq_mode_en)
#endif
			host->xfer_size = data->blocks * data->blksz;
		dir = data->flags & MMC_DATA_READ ?
			DMA_FROM_DEVICE : DMA_TO_DEVICE;
		dma_unmap_sg(mmc_dev(mmc), data->sg, data->sg_len, dir);
		data->host_cookie = 0;
		N_MSG(OPS, "CMD<%d> ARG<0x%x> blksz<%d> block<%d> error<%d>",
			mrq->cmd->opcode, mrq->cmd->arg, data->blksz,
			data->blocks, data->error);
	}
	data->host_cookie = 0;

}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for executing transaction with async mode
 * @param *mmc: mmc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_do_request_async(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct msdc_host *host = mmc_priv(mmc);
	struct mmc_command *cmd;
	struct mmc_data *data;
	void __iomem *base = host->base;
	u32 arg;

#ifdef MTK_MSDC_USE_CMD23
	u32 l_card_no_cmd23 = 0;
#endif
	MVG_EMMC_DECLARE_INT32(delay_ns);
	MVG_EMMC_DECLARE_INT32(delay_us);
	MVG_EMMC_DECLARE_INT32(delay_ms);

	msdc_ungate_clock(host);

	host->error = 0;

	spin_lock(&host->lock);

	cmd = mrq->cmd;
	data = mrq->cmd->data;

	host->mrq = mrq;

	host->dma_xfer = 1;
	l_card_no_cmd23 = msdc_do_request_prepare(host, mrq);
	if (l_card_no_cmd23 == -1)
		goto done;
	else if (l_card_no_cmd23 == -2)
		goto stop;

	msdc_dma_on();          /* enable DMA mode first!! */

	if (msdc_command_start(host, cmd, CMD_TIMEOUT) != 0)
		goto done;

	if (msdc_command_resp_polling(host, cmd, CMD_TIMEOUT) != 0)
		goto stop;

	/* for msdc use cmd23, but card not supported(sbc is NULL),
	 * need enable autocmd23 for next request
	 */
	msdc_dma_setup(host, &host->dma, data->sg, data->sg_len);

#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
	mmc->is_data_dma = 1;
#endif

	msdc_dma_start(host);

#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
	if (check_mmc_cmd4647(cmd->opcode))
		arg = mmc->areq_que[((cmd->arg>>16)&0x1f)]->mrq_que->cmd->arg;
	else
#endif
		arg = cmd->arg;

	MVG_EMMC_WRITE_MATCH(host, (u64)arg, delay_ms, delay_us, delay_ns,
		cmd->opcode, host->xfer_size);

	if (sd_debug_zone[host->id])
		msdc_log_data_cmd(host, cmd, data);

	spin_unlock(&host->lock);

#ifdef MTK_MSDC_USE_CMD23
	/* for msdc use cmd23, but card not supported(sbc is NULL),
	 * need enable autocmd23 for next request
	 */
	if (l_card_no_cmd23 == 1) {
		host->autocmd |= MSDC_AUTOCMD23;
		host->autocmd &= ~MSDC_AUTOCMD12;
	}
#endif

#ifdef MTK_MSDC_USE_CACHE
	msdc_update_cache_flush_status(host, mrq, data, 1);
#endif

	return 0;


stop:
	if (msdc_if_send_stop(host, mrq, ASYNC_REQ))
		pr_err("%s send stop error\n", __func__);
done:
#ifdef MTK_MSDC_USE_CMD23
	/* for msdc use cmd23, but card not supported(sbc is NULL),
	 * need enable autocmd23 for next request
	 */
	if (l_card_no_cmd23 == 1) {
		host->autocmd |= MSDC_AUTOCMD23;
		host->autocmd &= ~MSDC_AUTOCMD12;
	}
#endif

	if (sd_debug_zone[host->id])
		msdc_log_data_cmd(host, cmd, data);

	msdc_dma_clear(host);

	msdc_if_set_request_err(host, mrq, 1);

	/* re-autok or try smpl except TMO */
	if (host->error & REQ_CMD_EIO)
		host->need_tune = TUNE_ASYNC_CMD;

#ifdef MTK_MSDC_USE_CACHE
	msdc_update_cache_flush_status(host, mrq, data, 1);
#endif

	if (mrq->done)
		mrq->done(mrq);

	msdc_gate_clock(host, 1);
	spin_unlock(&host->lock);

	return host->error;
}

#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for executing transaction with retry in CMDQ flow
 * @param *host: msdc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_do_cmdq_request_with_retry(struct msdc_host *host,
	struct mmc_request *mrq)
{
	struct mmc_host *mmc;
	struct mmc_command *cmd;
	struct mmc_data *data;
	struct mmc_command *stop = NULL;
	int ret = 0, retry;

	mmc = host->mmc;
	cmd = mrq->cmd;
	data = mrq->cmd->data;
	if (data)
		stop = data->stop;

	retry = 5;
	while (msdc_do_request_cq(mmc, mrq)) {
		msdc_dump_trans_error(host, cmd, data, stop, mrq->sbc);
		if ((cmd->error == (unsigned int)-EILSEQ) ||
			(cmd->error == (unsigned int)-ETIMEDOUT) ||
			(mrq->sbc->error == (unsigned int)-EILSEQ) ||
			(mrq->sbc->error == (unsigned int)-ETIMEDOUT)) {
			ret = tune_cmdq_cmdrsp(mmc, mrq, &retry);
			if (ret)
				return ret;
		} else {
			ERR_MSG("CMD44 and CMD45 error - error %d %d",
				mrq->sbc->error, cmd->error);
			break;
		}
	}

	return ret;
}
#endif

/* ops.request */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for ops request legacy
 * @param *mmc: mmc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_ops_request_legacy(struct mmc_host *mmc,
	struct mmc_request *mrq)
{
	struct msdc_host *host = mmc_priv(mmc);
	struct mmc_command *cmd;
	struct mmc_data *data;
	struct mmc_command *stop = NULL;
	struct timespec sdio_profile_start;
#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
	int ret;
#endif

	host->error = 0;

#ifndef CONFIG_MTK_EMMC_CQ_SUPPORT
	if (host->mrq) {
		ERR_MSG("XXX host->mrq<0x%p> cmd<%d>arg<0x%x>", host->mrq,
			host->mrq->cmd->opcode, host->mrq->cmd->arg);
		WARN_ON(1);
	}
#endif

	/* start to process */
	spin_lock(&host->lock);
	cmd = mrq->cmd;
	data = mrq->cmd->data;
	if (data)
		stop = data->stop;

	msdc_ungate_clock(host);

	if (sdio_pro_enable)
		sdio_get_time(mrq, &sdio_profile_start);

#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
	if (check_mmc_cmd44(mrq->sbc)) {
		ret = msdc_do_cmdq_request_with_retry(host, mrq);
		goto cq_req_done;
	}

	/* only CMD0/12/13 can be send when non-empty queue @ CMDQ on */
	if (mmc->card && mmc->card->ext_csd.cmdq_mode_en
		&& atomic_read(&mmc->areq_cnt)
		&& !check_mmc_cmd001213(cmd->opcode)
		&& !check_mmc_cmd48(cmd->opcode)) {
		ERR_MSG("[%s][WARNING] CMDQ on, sending CMD%d\n",
			__func__, cmd->opcode);
	}
	if (!check_mmc_cmd13_sqs(mrq->cmd))
#endif
		host->mrq = mrq;

	if (msdc_do_request(host->mmc, mrq)) {
		msdc_dump_trans_error(host, cmd, data, stop, mrq->sbc);
	} else {
		/* mmc_blk_err_check will do legacy request without data */
		if (host->need_tune & TUNE_LEGACY_CMD)
			host->need_tune &= ~TUNE_LEGACY_CMD;
		/* Retry legacy data read pass, clear autok pass flag */
		if ((host->need_tune & TUNE_LEGACY_DATA_READ) &&
			mrq->cmd->data) {
			host->need_tune &= ~TUNE_LEGACY_DATA_READ;
			host->need_tune &= ~TUNE_AUTOK_PASS;
			host->reautok_times = 0;
			host->tune_smpl_times = 0;
		}
		/* Retry legacy data write pass, clear autok pass flag */
		if ((host->need_tune & TUNE_LEGACY_DATA_WRITE) &&
			mrq->cmd->data) {
			host->need_tune &= ~TUNE_LEGACY_DATA_WRITE;
			host->need_tune &= ~TUNE_AUTOK_PASS;
			host->reautok_times = 0;
			host->tune_smpl_times = 0;
		}
		/* legacy command err, tune pass, clear autok pass flag */
		if (host->need_tune == TUNE_AUTOK_PASS) {
			host->reautok_times = 0;
			host->tune_smpl_times = 0;
			host->need_tune = TUNE_NONE;
		}
		if (host->need_tune == TUNE_NONE)
			host->err_cmd = -1;
	}

#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
cq_req_done:
#endif

#ifdef MTK_MSDC_USE_CACHE
	msdc_check_cache_flush_error(host, cmd);
#endif

	/* ==== when request done, check if app_cmd ==== */
	if (mrq->cmd->opcode == MMC_APP_CMD) {
		host->app_cmd = 1;
		host->app_cmd_arg = mrq->cmd->arg;      /* save the RCA */
	} else {
		host->app_cmd = 0;
		/* host->app_cmd_arg = 0; */
	}

#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
	/* if not CMDQ CMD44/45 or CMD13, follow orignal flow to clear host->mrq
	 * if it's CMD44/45 or CMD13 QSR, host->mrq may be CMD46,47
	 */
	if (!(check_mmc_cmd13_sqs(mrq->cmd) || check_mmc_cmd44(mrq->sbc)))
#endif
		host->mrq = NULL;

	if (sdio_pro_enable)
		sdio_calc_time(mrq, &sdio_profile_start);

	msdc_gate_clock(host, 1);       /* clear flag. */

	spin_unlock(&host->lock);

	mmc_request_done(mmc, mrq);
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for ops request
 * @param *mmc: mmc host manipulator pointer
 * @param *mrq: mmc request pointer
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_ops_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	int host_cookie = 0;
	struct msdc_host *host = mmc_priv(mmc);

	if ((host->hw->host_function == MSDC_SDIO) &&
	    !(host->trans_lock.active))
		__pm_stay_awake(&host->trans_lock);

	if (mrq->data)
		host_cookie = mrq->data->host_cookie;

#ifndef NEW_TUNE_K44
	if (!(host->tuning_in_progress) && host->need_tune &&
		!(host->need_tune & TUNE_AUTOK_PASS))
		msdc_error_tuning(mmc, mrq);
#endif

	if (is_card_present(host)
		&& host->power_mode == MMC_POWER_OFF
		&& mrq->cmd->opcode == 7 && mrq->cmd->arg == 0
		&& host->hw->host_function == MSDC_SD) {
		ERR_MSG("cmd<7> arg<0x0> card<1> power<0>,return -ENOMEDIUM");
	} else if (!is_card_present(host) ||
		host->power_mode == MMC_POWER_OFF) {
		ERR_MSG("cmd<%d> arg<0x%x> card<%d> power<%d>",
			mrq->cmd->opcode, mrq->cmd->arg,
			is_card_present(host), host->power_mode);
		mrq->cmd->error = (unsigned int)-ENOMEDIUM;
		if (mrq->done)
			mrq->done(mrq); /* call done directly. */
		return;
	}

	/* Async only support DMA and asyc CMD flow */
	if (msdc_use_async_dma(host_cookie))
		msdc_do_request_async(mmc, mrq);
	else
		msdc_ops_request_legacy(mmc, mrq);

	if ((host->hw->host_function == MSDC_SDIO) &&
	    (host->trans_lock.active))
		__pm_relax(&host->trans_lock);
}

/* ops.set_ios */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for config host power mode/bus width/driving/..
 * @param *mmc: mmc host manipulator pointer
 * @param *ios: mmc ios pointer
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
void msdc_ops_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct msdc_host *host = mmc_priv(mmc);

	spin_lock(&host->lock);
	msdc_ungate_clock(host);

	if (host->power_mode != ios->power_mode) {
		switch (ios->power_mode) {
		case MMC_POWER_OFF:
		case MMC_POWER_UP:
			spin_unlock(&host->lock);
			if (ios->power_mode == MMC_POWER_UP
			 && host->power_mode == MMC_POWER_OFF)
				msdc_init_hw(host);
			msdc_set_power_mode(host, ios->power_mode);
			spin_lock(&host->lock);
			break;
		case MMC_POWER_ON:
		default:
			break;
		}
		host->power_mode = ios->power_mode;
	}

	if (host->bus_width != ios->bus_width) {
		msdc_set_buswidth(host, ios->bus_width);
		host->bus_width = ios->bus_width;
	}

	if (msdc_clock_src[host->id] != host->hw->clk_src) {
		host->hw->clk_src = msdc_clock_src[host->id];
		msdc_select_clksrc(host, host->hw->clk_src);
	}

	if (host->timing != ios->timing) {
		/* msdc setting TX parameter */
		msdc_ios_tune_setting(host, ios);
		if (ios->timing == MMC_TIMING_MMC_DDR52)
			msdc_set_mclk(host, ios->timing, ios->clock);
		host->timing = ios->timing;

		/* For MSDC design, driving shall actually depend on clock freq
		 * instead of timing mode. However, we may not be able to
		 * determine driving between 100MHz and 200MHz, e.g., 150MHz
		 * or 180MHz Therefore we select to change driving when
		 * timing mode changes.
		 */
		if (host->hw->host_function == MSDC_SD) {
			if (host->timing == MMC_TIMING_UHS_SDR104) {
				host->hw->driving_applied =
					&host->hw->driving_sd_sdr104;
			} else if (host->timing == MMC_TIMING_UHS_SDR50) {
				host->hw->driving_applied =
					&host->hw->driving_sd_sdr50;
			} else if (host->timing == MMC_TIMING_UHS_DDR50) {
				host->hw->driving_applied =
					&host->hw->driving_sd_ddr50;
			}
			msdc_set_driving(host, host->hw->driving_applied);
		}
	}

	if (host->mclk != ios->clock) {
		msdc_set_mclk(host, ios->timing, ios->clock);
		if (ios->timing == MMC_TIMING_MMC_HS400)
			msdc_execute_tuning(host->mmc,
				MMC_SEND_TUNING_BLOCK_HS200);
	}

	msdc_gate_clock(host, 1);
	spin_unlock(&host->lock);
}

/* ops.get_ro */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for get ro status
 * @param *mmc: mmc host manipulator pointer
 * @return
 *     ro status
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_ops_get_ro(struct mmc_host *mmc)
{
	struct msdc_host *host = mmc_priv(mmc);
	void __iomem *base = host->base;
	unsigned long flags;
	int ro = 0;

	spin_lock_irqsave(&host->lock, flags);
	msdc_ungate_clock(host);
	if (host->hw->flags & MSDC_WP_PIN_EN)
		ro = (MSDC_READ32(MSDC_PS) >> 31);
	msdc_gate_clock(host, 1);
	spin_unlock_irqrestore(&host->lock, flags);

	return ro;
}

/* ops.get_cd */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to get card insert status
 * @param *mmc: mmc host manipulator pointer
 * @return
 *     card insert status
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_ops_get_cd(struct mmc_host *mmc)
{
	struct msdc_host *host = mmc_priv(mmc);
	/* unsigned long flags; */
	int level = 0;

	/* spin_lock_irqsave(&host->lock, flags); */

	/* for sdio, depends on USER_RESUME */
	if (is_card_sdio(host) && !(host->hw->flags & MSDC_SDIO_IRQ)) {
		host->card_inserted =
			(host->pm_state.event == PM_EVENT_USER_RESUME) ? 1 : 0;
		goto end;
	}

	/* for emmc, MSDC_REMOVABLE not set, always return 1 */
	if (mmc->caps & MMC_CAP_NONREMOVABLE) {
		host->card_inserted = 1;
		goto end;
	} else {
#ifdef CONFIG_GPIOLIB
		level = __gpio_get_value(cd_gpio);
#else
		ERR_MSG("Cannot get gpio %d level", cd_gpio);
#endif
		host->card_inserted = (host->hw->cd_level == level) ? 1 : 0;
	}

	if (host->block_bad_card)
		host->card_inserted = 0;
 end:
	/* enable msdc register dump */
	sd_register_zone[host->id] = 1;
	INIT_MSG("Card insert<%d> Block bad card<%d>",
		host->card_inserted, host->block_bad_card);
	/* spin_unlock_irqrestore(&host->lock, flags); */

	return host->card_inserted;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function to get card insert event
 * @param *mmc: mmc host manipulator pointer
 * @return
 *     none
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_ops_card_event(struct mmc_host *mmc)
{
	struct msdc_host *host = mmc_priv(mmc);

	host->block_bad_card = 0;
	host->is_autok_done = 0;
	msdc_ops_get_cd(mmc);
	/* when detect card, timeout log log is not needed */
	sd_register_zone[host->id] = 0;
}

/* ops.enable_sdio_irq */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function fto enable SDIO IRQ
 * @param *mmc: mmc host manipulator pointer
 * @param enable: enable/disable SDIO device interrupt
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_ops_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct msdc_host *host = mmc_priv(mmc);
	struct msdc_hw *hw = host->hw;
	void __iomem *base = host->base;
	unsigned long flags;

	if (hw->flags & MSDC_EXT_SDIO_IRQ) {    /* yes for sdio */
		if (enable)
			hw->enable_sdio_eirq(); /* combo_sdio_enable_eirq */
		else
			hw->disable_sdio_eirq(); /* combo_sdio_disable_eirq */
	} else if (hw->flags & MSDC_SDIO_IRQ) {

		spin_lock_irqsave(&host->sdio_irq_lock, flags);

		if (enable) {
			MSDC_SET_BIT32(MSDC_INTEN, MSDC_INT_SDIOIRQ);
			pr_debug("@#0x%08x @e >%d<\n", MSDC_READ32(MSDC_INTEN),
				host->mmc->sdio_irq_pending);
			if ((MSDC_READ32(MSDC_INTEN) & MSDC_INT_SDIOIRQ) == 0) {
				pr_err("Should never get here >%d<\n",
					host->mmc->sdio_irq_pending);
			}
		} else {
			MSDC_CLR_BIT32(MSDC_INTEN, MSDC_INT_SDIOIRQ);
			pr_debug("@#0x%08x @d\n", (MSDC_READ32(MSDC_INTEN)));
		}

		spin_unlock_irqrestore(&host->sdio_irq_lock, flags);

	}
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for switching IO voltage for SD/SDIO
 * @param *mmc: mmc host manipulator pointer
 * @param *ios: mmc ios pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_ops_switch_volt(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct msdc_host *host = mmc_priv(mmc);
	void __iomem *base = host->base;
	unsigned int status = 0;

	if (host->hw->host_function == MSDC_EMMC)
		return 0;
	if (!host->power_switch)
		return 0;

	switch (ios->signal_voltage) {
	case MMC_SIGNAL_VOLTAGE_330:
		pr_err("%s msdc%d set voltage to 3.3V.\n", __func__, host->id);
		return 0;
	case MMC_SIGNAL_VOLTAGE_180:
		pr_err("%s msdc%d set voltage to 1.8V.\n", __func__, host->id);
		/* switch voltage */
		host->power_switch(host, 1);
		/* Clock is gated by HW after CMD11,
		 * Must keep clock gate 5ms before switch voltage
		 */
		usleep_range(5000, 5500);
		/* start to provide clock to device */
		MSDC_SET_BIT32(MSDC_CFG, MSDC_CFG_BV18SDT);
		/* Delay 1ms wait HW to finish voltage switch */
		usleep_range(1000, 1500);
		status = MSDC_READ32(MSDC_CFG);
		if (!(status & MSDC_CFG_BV18SDT) && (status & MSDC_CFG_BV18PSS))
			return 0;
		pr_warn("%s: 1.8V regulator output did not became stable\n",
			mmc_hostname(mmc));
		return -EAGAIN;
	default:
		return 0;
	}
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for check card if busy
 * @param *mmc: mmc host manipulator pointer
 * @return
 *     If 0, card not busy. \n
 *     If 1, card busy.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_card_busy(struct mmc_host *mmc)
{
	struct msdc_host *host = mmc_priv(mmc);
	void __iomem *base = host->base;
	u32 status = MSDC_READ32(MSDC_PS);

	/* check only DAT0,
	 * since SDIO device may drive DAT1 low to interrupt host
	 */
	if (((status >> 16) & 0x1) != 0x1)
		return 1;

	return 0;
}

/* Add this function to check if no interrupt back after write.
 * It may occur when write crc revice, but busy over data->timeout_ns
 */
 /** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for check if timeout for write transaction
 * @param *work: work pointer
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_check_write_timeout(struct work_struct *work)
{
	struct msdc_host *host =
		container_of(work, struct msdc_host, write_timeout.work);
	void __iomem *base = host->base;
	struct mmc_data  *data = host->data;
	struct mmc_request *mrq = host->mrq;
	struct mmc_host *mmc = host->mmc;
	u32 status = 0;
	u32 state = 0;
	u32 err = 0;
	unsigned long tmo;
	u32 intsts;

	if (!data || !mrq || !mmc)
		return;
	pr_err("[%s]: XXX DMA Data Write Busy Timeout: %u ms, CMD<%d>",
		__func__, host->write_timeout_ms, mrq->cmd->opcode);

	intsts = MSDC_READ32(MSDC_INT);
	/* MSDC have received int, but delay by system. Just print warning */
	if (intsts) {
		pr_err("[%s]: Warning msdc%d ints are delayed by system, ints: %x\n",
			__func__, host->id, intsts);
		msdc_dump_info(host->id);
		return;
	}

	if (msdc_use_async_dma(data->host_cookie)
	 && (host->need_tune == TUNE_NONE)) {
		msdc_dump_info(host->id);
		msdc_dma_stop(host);
		msdc_dma_clear(host);
		msdc_reset_hw(host->id);

		tmo = jiffies + POLLING_BUSY;

		/* check card state, try to bring back to trans state */
		spin_lock(&host->lock);
		do {
			/* if anything wrong, let block driver do error
			 * handling.
			 */
			err = msdc_get_card_status(mmc, host, &status);
			if (err) {
				ERR_MSG("CMD13 ERR<%d>", err);
				break;
			}

			state = R1_CURRENT_STATE(status);
			ERR_MSG("check card state<%d>", state);
			if (state == R1_STATE_DATA || state == R1_STATE_RCV) {
				ERR_MSG("state<%d> need cmd12 to stop", state);
				msdc_send_stop(host);
			} else if (state == R1_STATE_PRG) {
				ERR_MSG("state<%d> card is busy", state);
				spin_unlock(&host->lock);
				msleep(100);
				spin_lock(&host->lock);
			}

			if (time_after(jiffies, tmo)) {
				spin_unlock(&host->lock);
				msleep(100);
				spin_lock(&host->lock);
				break;
			}
		} while (state != R1_STATE_TRAN);
		spin_unlock(&host->lock);

		data->error = (unsigned int)-ETIMEDOUT;
		host->sw_timeout++;

		if (mrq->done)
			mrq->done(mrq);

		msdc_gate_clock(host, 1);
		host->error |= REQ_DAT_ERR;
	} else {
		/* do nothing, since legacy mode or async tuning
		 * have it own timeout.
		 */
		/* complete(&host->xfer_done); */
	}
}

static struct mmc_host_ops mt_msdc_ops = {
	.post_req                      = msdc_post_req,
	.pre_req                       = msdc_pre_req,
	.request                       = msdc_ops_request,
	.set_ios                       = msdc_ops_set_ios,
	.get_ro                        = msdc_ops_get_ro,
	.get_cd                        = msdc_ops_get_cd,
	.card_event                    = msdc_ops_card_event,
	.enable_sdio_irq               = msdc_ops_enable_sdio_irq,
	.start_signal_voltage_switch   = msdc_ops_switch_volt,
	.execute_tuning                = msdc_execute_tuning,
	.hw_reset                      = msdc_card_reset,
	.card_busy                     = msdc_card_busy,
};

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for irq handling function for data complete event
 * @param *host: msdc host manipulator pointer
 * @param *data: mmc data pointer
 * @param error: error flag
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_irq_data_complete(struct msdc_host *host,
	struct mmc_data *data, int error)
{
	void __iomem *base = host->base;
	struct mmc_request *mrq;

	if ((msdc_use_async_dma(data->host_cookie)) &&
	    (!host->tuning_in_progress)) {
		msdc_dma_stop(host);
		if (error) {
			msdc_clr_fifo(host->id);
			msdc_clr_int();
		}

		mrq = host->mrq;
		msdc_dma_clear(host);

		if (error) {
			if ((mrq->data != NULL) &&
			    (mrq->data->flags & MMC_DATA_WRITE)) {
				host->error |= REQ_CRC_STATUS_ERR;
				host->need_tune = TUNE_ASYNC_DATA_WRITE;
			} else {
				host->error |= REQ_DAT_ERR;
				host->need_tune = TUNE_ASYNC_DATA_READ;
			}
			host->err_cmd = mrq->cmd->opcode;
			/* FIXME: return as cmd error for retry
			 * if data CRC error
			 */
#ifndef NEW_TUNE_K44
			mrq->cmd->error = (unsigned int)-EILSEQ;
#endif

#ifdef NEW_TUNE_K44
			if (mrq->data != NULL) {
				if ((mrq->cmd->opcode !=
				     MMC_SEND_TUNING_BLOCK &&
				     mrq->cmd->opcode !=
				     MMC_SEND_TUNING_BLOCK_HS200)
				 && ((mrq->cmd->error == -EILSEQ) ||
				     (mrq->sbc && mrq->sbc->error == -EILSEQ) ||
				     (mrq->data && mrq->data->error ==
				     -EILSEQ) ||
				     (mrq->data && mrq->data->error ==
				     -ETIMEDOUT) ||
				     (mrq->stop && mrq->stop->error ==
				     -EILSEQ)))
					mmc_retune_needed(host->mmc);
			}
#endif
		} else {
			host->error &= ~REQ_DAT_ERR;
			host->need_tune = TUNE_NONE;
			host->reautok_times = 0;
			host->tune_smpl_times = 0;
			host->err_cmd = -1;
			host->prev_cmd_cause_dump = 0;
		}
		if (mrq->done)
			mrq->done(mrq);
		msdc_gate_clock(host, 1);
	} else {
		/* Autocmd12 issued but error, data transfer done INT won't set,
		 * so cmplete is need here
		 */
		complete(&host->xfer_done);
	}
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for msdc IRQ service routine
 * @param irq: irq number
 * @param *dev_id: device id pointer
 * @param timeout: command timeout setting
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static irqreturn_t msdc_irq(int irq, void *dev_id)
{
	struct msdc_host *host = (struct msdc_host *)dev_id;
	struct mmc_data *data = host->data;
	struct mmc_command *cmd = host->cmd;
	struct mmc_command *stop = NULL;
	void __iomem *base = host->base;
	/* int ret; */

	u32 cmdsts = MSDC_INT_RSPCRCERR | MSDC_INT_CMDTMO | MSDC_INT_CMDRDY |
		     MSDC_INT_ACMDCRCERR | MSDC_INT_ACMDTMO | MSDC_INT_ACMDRDY;
	u32 datsts = MSDC_INT_DATCRCERR | MSDC_INT_DATTMO;
	u32 intsts, inten;

#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
	u32 cmdqsts = MSDC_INT_RSPCRCERR | MSDC_INT_CMDTMO | MSDC_INT_CMDRDY;
#endif

	if (host->hw->flags & MSDC_SDIO_IRQ)
		spin_lock(&host->sdio_irq_lock);

	if (host->core_clkon == 0) {
		/* ret = msdc_clk_enable(host); */
		/* if (ret < 0) */
		/*	pr_err("host%d enable clock fail\n", host->id); */
		host->core_clkon = 1;
		MSDC_SET_FIELD(MSDC_CFG, MSDC_CFG_MODE, MSDC_SDMMC);
	}
	intsts = MSDC_READ32(MSDC_INT);

	latest_int_status[host->id] = intsts;
	inten = MSDC_READ32(MSDC_INTEN);

	if (host->hw->flags & MSDC_SDIO_IRQ)
		intsts &= inten;
	else
		inten &= intsts;

#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
	/* Leave CMDQ-related interrupts so that they can be seen
	 * by response polling function
	 */
	intsts &= ~cmdqsts;
#endif

	MSDC_WRITE32(MSDC_INT, intsts); /* clear interrupts */

	/* sdio interrupt */
	if (host->hw->flags & MSDC_SDIO_IRQ) {
		spin_unlock(&host->sdio_irq_lock);

		if (intsts & MSDC_INT_SDIOIRQ)
			mmc_signal_sdio_irq(host->mmc);
	}

	if (data == NULL)
		goto skip_data_interrupts;

#ifdef MTK_MSDC_ERROR_TUNE_DEBUG
	msdc_error_tune_debug2(host, stop, &intsts);
#endif

	stop = data->stop;

#ifdef CONFIG_MTK_EMMC_CQ_SUPPORT
	host->mmc->is_data_dma = 0;
#endif
	if (intsts & MSDC_INT_XFER_COMPL) {
		/* Finished data transfer */
		data->bytes_xfered = host->dma.xfersz;
		msdc_irq_data_complete(host, data, 0);
		return IRQ_HANDLED;
	}

	if (intsts & datsts) {
		/* do basic reset, or stop command will sdc_busy */
		if ((intsts & MSDC_INT_DATTMO)
		 || (host->hw->host_function == MSDC_SDIO))
			msdc_dump_info(host->id);

		if (host->dma_xfer)
			msdc_reset(host->id);
		else
			msdc_reset_hw(host->id);

		atomic_set(&host->abort, 1);    /* For PIO mode exit */

		if (intsts & MSDC_INT_DATTMO) {
			data->error = (unsigned int)-ETIMEDOUT;
			ERR_MSG("CMD<%d> Arg<0x%.8x> MSDC_INT_DATTMO",
				host->mrq->cmd->opcode, host->mrq->cmd->arg);
		} else if (intsts & MSDC_INT_DATCRCERR) {
			data->error = (unsigned int)-EILSEQ;
			/* ERR_MSG("CMD<%d> Arg<0x%.8x> MSDC_INT_DATCRCERR,
			 *  SDC_DCRC_STS<0x%x>",
			 *	host->mrq->cmd->opcode, host->mrq->cmd->arg,
			 *	MSDC_READ32(SDC_DCRC_STS));
			 */
		}

		goto tune;

	}

	if ((stop != NULL) &&
	    (host->autocmd & MSDC_AUTOCMD12) &&
	    (intsts & cmdsts)) {
		if (intsts & MSDC_INT_ACMDRDY)
			goto skip_data_interrupts;
		if (intsts & MSDC_INT_ACMDCRCERR) {
			stop->error = (unsigned int)-EILSEQ;
			host->error |= REQ_STOP_EIO;
		} else if (intsts & MSDC_INT_ACMDTMO) {
			stop->error = (unsigned int)-ETIMEDOUT;
			host->error |= REQ_STOP_TMO;
		}
		if (host->dma_xfer)
			msdc_reset(host->id);
		else
			msdc_reset_hw(host->id);

		goto tune;
	}

skip_data_interrupts:
	/* command interrupts */
	if ((cmd == NULL) || !(intsts & cmdsts))
		goto skip_cmd_interrupts;

#ifdef MTK_MSDC_ERROR_TUNE_DEBUG
	msdc_error_tune_debug1(host, cmd, NULL, &intsts);
#endif

skip_cmd_interrupts:
	if ((host->dma_xfer) && (data != NULL))
		msdc_irq_data_complete(host, data, 1);

	latest_int_status[host->id] = 0;
	return IRQ_HANDLED;

tune:   /* DMA DATA transfer crc error */
	/* PIO mode can't do complete, because not init */
	if (host->dma_xfer)
		msdc_irq_data_complete(host, data, 1);

	return IRQ_HANDLED;
}

/* init gpd and bd list in msdc_drv_probe */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for initialze gpd and bd list when drv_probe
 * @param *host: msdc host manipulator pointer
 * @param *dma: msdc dma pointer
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_init_gpd_bd(struct msdc_host *host, struct msdc_dma *dma)
{
	struct gpd_t *gpd = dma->gpd;
	struct bd_t *bd = dma->bd;
	struct bd_t *ptr, *prev;
	dma_addr_t dma_addr;

	/* we just support one gpd */
	int bdlen = MAX_BD_PER_GPD;

	/* init the 2 gpd */
	memset(gpd, 0, sizeof(struct gpd_t) * 2);
	dma_addr = dma->gpd_addr + sizeof(struct gpd_t);
	gpd->nexth4 = (u32) ((dma_addr >> 32) & 0xF);
	gpd->next = (u32) (dma_addr & 0xFFFFFFFFUL);

	/* gpd->intr = 0; */
	gpd->bdp = 1;           /* hwo, cs, bd pointer */
	/* gpd->ptr  = (void*)virt_to_phys(bd); */
	dma_addr = dma->bd_addr;
	gpd->ptrh4 = (u32) ((dma_addr >> 32) & 0xF); /* physical address */
	gpd->ptr = (u32) (dma_addr & 0xFFFFFFFFUL); /* physical address */

	memset(bd, 0, sizeof(struct bd_t) * bdlen);
	ptr = bd + bdlen - 1;
	while (ptr != bd) {
		prev = ptr - 1;
		dma_addr = dma->bd_addr + sizeof(struct bd_t) * (ptr - bd);
		prev->nexth4 = (u32) ((dma_addr >> 32) & 0xF);
		prev->next = (u32) (dma_addr & 0xFFFFFFFFUL);
		ptr = prev;
	}
}

/* This is called by run_timer_softirq */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for turn off clock when timeout happen
 * @param data: msdc host manipulator pointer
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_timer_pm(unsigned long data)
{

}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for trigger SDIO IRQ
 * @param i_on: msdc host manipulator pointer
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
void SRC_trigger_signal(int i_on)
{
	if ((ghost != NULL) && (ghost->hw->flags & MSDC_SDIO_IRQ)) {
		pr_debug("msdc2 SRC_trigger_signal %d\n", i_on);
		src_clk_control = i_on;
		if (src_clk_control) {
			msdc_clksrc_onoff(ghost, 1);
			/* mb(); */
			if (ghost->mmc->sdio_irq_thread &&
			    (atomic_read(&ghost->mmc->sdio_irq_thread_abort)
				== 0)) {/* if (ghost->mmc->sdio_irq_thread) */
				mmc_signal_sdio_irq(ghost->mmc);
				if (u_msdc_irq_counter < 3)
					pr_debug("msdc2 SRC_trigger_signal mmc_signal_sdio_irq\n");
			}
		}
	}
}
EXPORT_SYMBOL(SRC_trigger_signal);

#ifdef CONFIG_MTK_HIBERNATION
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for driver pm restore
 * @param *device: device pointer
 * @return
 *     Always 0
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_drv_pm_restore_noirq(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct mmc_host *mmc = NULL;
	struct msdc_host *host = NULL;

	WARN_ON(pdev == NULL);
	mmc = platform_get_drvdata(pdev);
	host = mmc_priv(mmc);
	if (host->hw->host_function == MSDC_SD) {
		if (!(mmc->caps & MMC_CAP_NONREMOVABLE)) {
#ifdef CONFIG_GPIOLIB
			if ((host->hw->cd_level == __gpio_get_value(cd_gpio))
			 && host->mmc->card) {
				mmc_card_set_removed(host->mmc->card);
				host->card_inserted = 0;
			}
#endif
		}
		host->block_bad_card = 0;
	}

	return 0;
}
#endif

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for de-init and power down msdc host HW
 * @param *host: msdc host manipulator pointer
 * @return
 *     none..
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_deinit_hw(struct msdc_host *host)
{
	void __iomem *base = host->base;

	/* Disable and clear all interrupts */
	MSDC_CLR_BIT32(MSDC_INTEN, MSDC_READ32(MSDC_INTEN));
	MSDC_WRITE32(MSDC_INT, MSDC_READ32(MSDC_INT));

	/* make sure power down */
	msdc_set_power_mode(host, MMC_POWER_OFF);
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for removing host
 * @param *host: msdc host manipulator pointer
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_remove_host(struct msdc_host *host)
{
	if (host->irq >= 0)
		free_irq(host->irq, host);
	platform_set_drvdata(host->pdev, NULL);
	msdc_deinit_hw(host);
	kfree(host->hw);
	mmc_free_host(host->mmc);
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for adding msdc host
 * @param *work: work pointer
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void msdc_add_host(struct work_struct *work)
{
	int ret;
	struct msdc_host *host = NULL;

	host = container_of(work, struct msdc_host, work_init.work);
	if (host && host->mmc) {
		ret = mmc_add_host(host->mmc);
		if (ret)
			msdc_remove_host(host);
	}
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for driver probe
 * @param *pdev: platform device pointer
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_drv_probe(struct platform_device *pdev)
{
	struct mmc_host *mmc = NULL;
	struct msdc_host *host = NULL;
	struct msdc_hw *hw = NULL;
	void __iomem *base = NULL;
	int ret = 0;

	/* Allocate MMC host for this device */
	mmc = mmc_alloc_host(sizeof(struct msdc_host), &pdev->dev);
	if (!mmc)
		return -ENOMEM;

	ret = msdc_dt_init(pdev, mmc);
	if (ret) {
		mmc_free_host(mmc);
		return ret;
	}

	host = mmc_priv(mmc);
	base = host->base;
	hw = host->hw;

	/* Set host parameters to mmc */
	mmc->ops        = &mt_msdc_ops;
	mmc->f_min      = HOST_MIN_MCLK;
	mmc->f_max      = HOST_MAX_MCLK;
	mmc->ocr_avail  = MSDC_OCR_AVAIL;

	if ((hw->flags & MSDC_SDIO_IRQ) || (hw->flags & MSDC_EXT_SDIO_IRQ))
		mmc->caps |= MMC_CAP_SDIO_IRQ;  /* yes for sdio */
#ifdef MTK_MSDC_USE_CMD23
	if (host->hw->host_function == MSDC_EMMC)
		mmc->caps |= MMC_CAP_CMD23;
#endif
	mmc->caps |= MMC_CAP_ERASE;

	/* MMC core transfer sizes tunable parameters */
	mmc->max_segs = MAX_HW_SGMTS;
	/* mmc->max_phys_segs = MAX_PHY_SGMTS; */
	if (hw->host_function == MSDC_SDIO)
		mmc->max_seg_size  = MAX_SGMT_SZ_SDIO;
	else
		mmc->max_seg_size  = MAX_SGMT_SZ;
	mmc->max_blk_size  = HOST_MAX_BLKSZ;
	mmc->max_req_size  = MAX_REQ_SZ;
	mmc->max_blk_count = MAX_REQ_SZ / 512; /* mmc->max_req_size; */

	host->hclk              = msdc_get_hclk(pdev->id, hw->clk_src);
					/* clocksource to msdc */
	host->pm_state          = PMSG_RESUME;
	host->power_mode        = MMC_POWER_OFF;
	host->power_control     = NULL;
	host->power_switch      = NULL;

	host->dma_mask          = DMA_BIT_MASK(33);
	mmc_dev(mmc)->dma_mask  = &host->dma_mask;

#ifndef CONFIG_MACH_FPGA
	/* FIX ME, consider to move it into msdc_io.c */
	if (msdc_get_ccf_clk_pointer(pdev, host))
		return 1;
#endif
	msdc_set_host_power_control(host);

	host->card_inserted =
		host->mmc->caps & MMC_CAP_NONREMOVABLE ? 1 : 0;
	host->timeout_clks = DEFAULT_DTOC * 1048576;

	if (host->hw->host_function == MSDC_EMMC) {
#ifdef MTK_MSDC_USE_CMD23
		host->autocmd &= ~MSDC_AUTOCMD12;
#if (MSDC_USE_AUTO_CMD23 == 1)
		host->autocmd |= MSDC_AUTOCMD23;
#endif
#else
		host->autocmd |= MSDC_AUTOCMD12;
#endif
	} else if (host->hw->host_function == MSDC_SD) {
		host->autocmd |= MSDC_AUTOCMD12;
	} else {
		host->autocmd &= ~MSDC_AUTOCMD12;
	}

	host->mrq = NULL;

	/* using dma_alloc_coherent */
	host->dma.gpd = dma_alloc_coherent(&pdev->dev,
			MAX_GPD_NUM * sizeof(struct gpd_t),
			&host->dma.gpd_addr, GFP_KERNEL);
	host->dma.bd = dma_alloc_coherent(&pdev->dev,
			MAX_BD_NUM * sizeof(struct bd_t),
			&host->dma.bd_addr, GFP_KERNEL);
	WARN_ON((!host->dma.gpd) || (!host->dma.bd));
	msdc_init_gpd_bd(host, &host->dma);
	msdc_clock_src[host->id] = hw->clk_src;
	mtk_msdc_host[host->id] = host;

	/* for re-autok */
	host->tuning_in_progress = false;
	init_completion(&host->autok_done);
	host->need_tune	= TUNE_NONE;
	host->err_cmd = -1;

	if (host->hw->host_function == MSDC_SDIO)
		wakeup_source_init(&host->trans_lock, "MSDC Transfer Lock");

	INIT_DELAYED_WORK(&host->write_timeout, msdc_check_write_timeout);
	INIT_DELAYED_WORK(&host->work_init, msdc_add_host);

	spin_lock_init(&host->lock);
	spin_lock_init(&host->clk_gate_lock);
	spin_lock_init(&host->remove_bad_card);
	spin_lock_init(&host->sdio_irq_lock);
	/* init dynamtic timer */
	init_timer(&host->timer);
	/* host->timer.expires = jiffies + HZ; */
	host->timer.function = msdc_timer_pm;
	host->timer.data = (unsigned long)host;

	ret = request_irq((unsigned int)host->irq, msdc_irq, IRQF_TRIGGER_NONE,
		DRV_NAME, host);
	if (ret) {
		host->irq = -1;
		goto release;
	}

	MVG_EMMC_SETUP(host);

	if (hw->request_sdio_eirq)
		/* set to combo_sdio_request_eirq() for WIFI */
		/* msdc_eirq_sdio() will be called when EIRQ */
		hw->request_sdio_eirq(msdc_eirq_sdio, (void *)host);

#ifdef CONFIG_PM
	if (hw->register_pm) {/* only for sdio */
		/* function pointer to combo_sdio_register_pm() */
		hw->register_pm(msdc_pm, (void *)host);
		if (hw->flags & MSDC_SYS_SUSPEND) {
			/* will not set for WIFI */
			ERR_MSG("MSDC_SYS_SUSPEND and register_pm both set");
		}
		/* pm not controlled by system but by client. */
		mmc->pm_flags |= MMC_PM_IGNORE_PM_NOTIFY;
	}
#endif

	if (host->hw->host_function == MSDC_EMMC)
		mmc->pm_flags |= MMC_PM_KEEP_POWER;

	platform_set_drvdata(pdev, mmc);
#ifdef CONFIG_MTK_HIBERNATION
	if (pdev->id == 1)
		register_swsusp_restore_noirq_func(ID_M_MSDC,
			msdc_drv_pm_restore_noirq, &(pdev->dev));
#endif

	/* Config card detection pin and enable interrupts */
	if (!(host->mmc->caps & MMC_CAP_NONREMOVABLE)) {
		MSDC_CLR_BIT32(MSDC_PS, MSDC_PS_CDEN);
		MSDC_CLR_BIT32(MSDC_INTEN, MSDC_INTEN_CDSC);
		MSDC_CLR_BIT32(SDC_CFG, SDC_CFG_INSWKUP);
	}

	/* Use ordered workqueue to reduce msdc moudle init time */
	if (!queue_delayed_work(wq_init, &host->work_init, 0)) {
		pr_err("msdc%d queue delay work failed, [%s]L:%d\n",
			host->id, __func__, __LINE__);
		WARN_ON(1);
	}

#ifdef MTK_MSDC_BRINGUP_DEBUG
	pr_debug("[%s]: msdc%d, mmc->caps=0x%x, mmc->caps2=0x%x\n",
		__func__, host->id, mmc->caps, mmc->caps2);
	msdc_dump_clock_sts(host);
#endif

	return 0;

release:
	msdc_remove_host(host);

	return ret;
}

/* 4 device share one driver, using "drvdata" to show difference */
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for driver remove
 * @param *pdev: platform device pointer
 * @return
 *     Always 0
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_drv_remove(struct platform_device *pdev)
{
	struct mmc_host *mmc;
	struct msdc_host *host;
	struct resource *mem;

	mmc = platform_get_drvdata(pdev);
	host = mmc_priv(mmc);

	ERR_MSG("msdc_drv_remove");

#ifndef CONFIG_MACH_FPGA
	/* clock unprepare */
	if (host->clock_control)
		clk_unprepare(host->clock_control);
#endif

	mmc_remove_host(host->mmc);

	dma_free_coherent(NULL, MAX_GPD_NUM * sizeof(struct gpd_t),
		host->dma.gpd, host->dma.gpd_addr);
	dma_free_coherent(NULL, MAX_BD_NUM * sizeof(struct bd_t),
		host->dma.bd, host->dma.bd_addr);

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (mem)
		release_mem_region(mem->start, mem->end - mem->start + 1);

	msdc_remove_host(host);

	return 0;
}

#ifdef CONFIG_PM
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for msdc suspend flow
 * @param *pdev: platform device pointer
 * @param state: pm message
 * @return
 *     If 0, OK. \n
 *     If others, error.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_drv_suspend(struct platform_device *pdev, pm_message_t state)
{
	int ret = 0;
	struct mmc_host *mmc = platform_get_drvdata(pdev);
	struct msdc_host *host;
	void __iomem *base;

	/* FIX ME: consider to remove the next 2 lines */
	if (mmc == NULL)
		return 0;

	host = mmc_priv(mmc);
	base = host->base;

	if (state.event == PM_EVENT_SUSPEND) {
		if  (host->hw->flags & MSDC_SYS_SUSPEND) {
			/* will set for card */
			msdc_pm(state, (void *)host);
		} else {
			/* WIFI slot should be off when enter suspend */
			msdc_gate_clock(host, -1);
			if (host->error == -EBUSY) {
				ret = host->error;
				host->error = 0;
			}
		}
	}

	if (is_card_sdio(host) || (host->hw->flags & MSDC_SDIO_IRQ)) {
		if (host->clk_gate_count > 0) {
			host->error = 0;
			return -EBUSY;
		}
		if (host->saved_para.suspend_flag == 0) {
			host->saved_para.hz = host->mclk;
			if (host->saved_para.hz) {
				host->saved_para.suspend_flag = 1;
				/* mb(); */
				msdc_ungate_clock(host);
				msdc_save_timing_setting(host, 2);
				msdc_gate_clock(host, 0);
				if (host->error == -EBUSY) {
					ret = host->error;
					host->error = 0;
				}
			}
			ERR_MSG("msdc suspend cur_cfg=%x,save_cfg=%x,cur_hz=%d",
				MSDC_READ32(MSDC_CFG),
				host->saved_para.msdc_cfg, host->mclk);
		}
	}
	return ret;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for msdc resume flow
 * @param *pdev: platform device pointer
 * @return
 *     Always 0
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int msdc_drv_resume(struct platform_device *pdev)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);
	struct msdc_host *host = mmc_priv(mmc);
	struct pm_message state;

	state.event = PM_EVENT_RESUME;
	if (mmc && (host->hw->flags & MSDC_SYS_SUSPEND)) {
		/* will set for card; */
		msdc_pm(state, (void *)host);
	}

	/* This mean WIFI not controller by PM */
	if (host->hw->host_function == MSDC_SDIO) {
		host->mmc->pm_flags |= MMC_PM_KEEP_POWER;
		host->mmc->rescan_entered = 0;
	}

	return 0;
}
#endif

static struct platform_driver mt_msdc_driver = {
	.probe = msdc_drv_probe,
	.remove = msdc_drv_remove,
#ifdef CONFIG_PM
	.suspend = msdc_drv_suspend,
	.resume = msdc_drv_resume,
#endif
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = msdc_of_ids,
	},
};

/*--------------------------------------------------------------------------*/
/* module init/exit                                                         */
/*--------------------------------------------------------------------------*/
/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for driver init
 * @param Parameters
 *     none.
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static int __init mt_msdc_init(void)
{
	int ret;

	/* Alloc init workqueue */
	wq_init = alloc_ordered_workqueue("msdc-init", 0);
	if (!wq_init) {
		pr_err("msdc create work_queue failed.[%s]:%d",
			__func__, __LINE__);
		return -1;
	}

	ret = platform_driver_register(&mt_msdc_driver);
	if (ret) {
		pr_err(DRV_NAME ": Can't register driver");
		return ret;
	}

#ifdef CONFIG_PWR_LOSS_MTK_TEST
	/* FIX ME: move to power_loss_test.c */
	msdc_proc_emmc_create();
#endif
	msdc_debug_proc_init();

	pr_debug(DRV_NAME ": MediaTek MSDC Driver\n");

	return 0;
}

/** @ingroup type_group_linux_eMMC_InFn
 * @par Description
 *     Function for driver exit
 * @param Parameters
 *     none.
 * @return
 *     none.
 * @par Boundary case and Limitation
 *     none.
 * @par Error case and Error handling
 *     none
 * @par Call graph and Caller graph (refer to the graph below)
 * @par Refer to the source code
 */
static void __exit mt_msdc_exit(void)
{
	platform_driver_unregister(&mt_msdc_driver);

	if (wq_init) {
		destroy_workqueue(wq_init);
		wq_init = NULL;
	}

#ifdef CONFIG_MTK_HIBERNATION
	unregister_swsusp_restore_noirq_func(ID_M_MSDC);
#endif
}
module_init(mt_msdc_init);
module_exit(mt_msdc_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek SD/MMC Card Driver");
