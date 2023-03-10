/*
 * Copyright (C) 2018 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __MTK_WARP_REG_H__
#define __MTK_WARP_REG_H__


/* ----------------- Register Definitions ------------------- */
#define SYS_0						0x00000000
	#define SYS_0_ENG_EN				BIT(0)
	#define SYS_0_PROC_MODE				BIT(1)
	#define SYS_0_PROC_EDGE_VAL_SEL			BIT(2)
	#define SYS_0_PROC_RSV0				BIT(3)
	#define SYS_0_SW_RST				BIT(4)
	#define SYS_0_DBG_MEM_STOP			BIT(5)
	#define SYS_0_DBG_OUT_STOP			BIT(6)
	#define SYS_0_DBG_OUT_DUMP_EN			BIT(7)
	#define SYS_0_ENG_EN_BYSOF			BIT(8)
	#define SYS_0_PROC_RSV1				GENMASK(15, 9)
	#define SYS_0_PROC_BORDER_VAL			GENMASK(23, 16)
	#define SYS_0_PROC_UNMAP_VAL			GENMASK(31, 24)
#define WPE_INT_CTRL					0x00000004
	#define WPE_INT_CTRL_WPE_INT_MASK		GENMASK(15, 0)
	#define WPE_INT_CTRL_WPE_INT_CTRL_RSV		GENMASK(31, 16)
#define WPE_INT_STATUS					0x00000008
	#define WPE_INT_STATUS_WPE_INT_STATUS		GENMASK(1, 0)
	#define WPE_INT_STATUS_WPE_RSV_INT_STATUS	GENMASK(15, 5)
	#define WPE_INT_STATUS_WPE_INT_RAW_STATUS	GENMASK(17, 16)
	#define WPE_INT_STATUS_WPE_RSV_INT_RAW_STATUS	GENMASK(31, 21)
#define WPE_STATUS					0x0000000c
	#define WPE_STATUS_WPE_MS_IDLE			BIT(0)
	#define WPE_STATUS_WPE_VC0_IDLE			BIT(1)
	#define WPE_STATUS_WPE_VC1_IDLE			BIT(2)
	#define WPE_STATUS_WPE_PS_IDLE			BIT(3)
	#define WPE_STATUS_WPE_TC_IDLE			BIT(4)
	#define WPE_STATUS_WPE_TF_IDLE			BIT(5)
	#define WPE_STATUS_WPE_LB_IDLE			BIT(6)
	#define WPE_STATUS_WPE_MF_IDLE			BIT(7)
	#define WPE_STATUS_WPE_MS_FIN			BIT(8)
	#define WPE_STATUS_WPE_MF_MEM_FIN		BIT(9)
	#define WPE_STATUS_WPE_DONE			BIT(10)
	#define WPE_STATUS_WPE_ALL_IDLE			BIT(11)
	#define WPE_STATUS_WPE_STATUS_RSV		GENMASK(31, 12)
#define SRC_IMG_0					0x00000010
	#define SRC_IMG_0_SRC_IMG_WIDTH			GENMASK(15, 0)
	#define SRC_IMG_0_SRC_IMG_HEIGHT		GENMASK(31, 16)
#define SRC_IMG_1					0x00000014
	#define SRC_IMG_1_SRC_IMG_NO0_BASEADDR		GENMASK(31, 0)
#define SRC_IMG_2					0x00000018
	#define SRC_IMG_2_SRC_IMG_NO0_STRIDE		GENMASK(31, 0)
#define SRC_IMG_3					0x0000001c
	#define SRC_IMG_3_SRC_IMG_NO1_BASEADDR		GENMASK(31, 0)
#define SRC_IMG_4					0x00000020
	#define SRC_IMG_4_SRC_IMG_NO1_STRIDE		GENMASK(31, 0)
#define DST_IMG_0					0x00000024
	#define DST_IMG_0_DST_IMG_WIDTH			GENMASK(15, 0)
	#define DST_IMG_0_DST_IMG_HEIGHT		GENMASK(31, 16)
#define DST_IMG_1					0x00000028
	#define DST_IMG_1_DST_IMG_BASEADDR		GENMASK(31, 0)
#define DST_IMG_2					0x0000002c
	#define DST_IMG_2_DST_IMG_STRIDE		GENMASK(31, 0)
#define DST_IMG_3					0x00000030
	#define DST_IMG_3_DST_VLD_BASEADDR		GENMASK(31, 0)
#define DST_IMG_4					0x00000034
	#define DST_IMG_4_DST_VLD_STRIDE		GENMASK(31, 0)
#define WARPMAP_0					0x00000038
	#define WARPMAP_0_VTX_TBL_WIDTH			GENMASK(7, 0)
	#define WARPMAP_0_VTX_TBL_HEIGHT		GENMASK(15, 8)
#define WARPMAP_1					0x0000003c
	#define WARPMAP_1_VTX_TBL_NO0_BASEADDR		GENMASK(31, 0)
#define WARPMAP_2					0x00000040
	#define WARPMAP_2_VTX_TBL_NO0_STRIDE		GENMASK(15, 0)
	#define WARPMAP_2_VTX_TBL_NO1_STRIDE		GENMASK(31, 16)
#define WARPMAP_3					0x00000044
	#define WARPMAP_3_VTX_TBL_NO1_BASEADDR		GENMASK(31, 0)
#define RBFC_1						0x00000048
	#define RBFC_1_RBFC_EN				BIT(0)
	#define RBFC_1_RBFC_RSV1			GENMASK(15, 10)
	#define RBFC_1_RBFC_RANGE			GENMASK(31, 16)
#define RBFC_2						0x0000004c
	#define RBFC_2_RBFC_STEP			GENMASK(31, 0)
#define WPE_CG_CTRL					0x00000050
	#define WPE_CG_CTRL_IDLE_CG_EN			BIT(0)
	#define WPE_CG_CTRL_STALL_CG_EN			BIT(1)
	#define WPE_CG_CTRL_CG_RSV			GENMASK(31, 2)
#define RSP_0						0x00000054
	#define RSP_0_RSP_TBL_SEL			GENMASK(4, 0)
	#define RSP_0_RSP_RSV0				GENMASK(15, 5)
#define RSP_1						0x00000058
	#define RSP_1_RSP_COEF_0			GENMASK(9, 0)
	#define RSP_1_RSP_COEF_1			GENMASK(25, 16)
#define RSP_2						0x0000005c
	#define RSP_2_RSP_COEF_2			GENMASK(9, 0)
	#define RSP_2_RSP_COEF_3			GENMASK(25, 16)
#define RSP_3						0x00000060
	#define RSP_3_RSP_COEF_4			GENMASK(9, 0)
	#define RSP_3_RSP_COEF_5			GENMASK(25, 16)
#define RSP_4						0x00000064
	#define RSP_4_RSP_COEF_6			GENMASK(9, 0)
	#define RSP_4_RSP_COEF_7			GENMASK(25, 16)
#define RSP_5						0x00000068
	#define RSP_5_RSP_COEF_8			GENMASK(9, 0)
	#define RSP_5_RSP_COEF_9			GENMASK(25, 16)
#define RSP_6						0x0000006c
	#define RSP_6_RSP_COEF_10			GENMASK(9, 0)
	#define RSP_6_RSP_COEF_11			GENMASK(25, 16)
#define RSP_7						0x00000070
	#define RSP_7_RSP_COEF_12			GENMASK(9, 0)
	#define RSP_7_RSP_COEF_13			GENMASK(25, 16)
#define RSP_8						0x00000074
	#define RSP_8_RSP_COEF_14			GENMASK(9, 0)
	#define RSP_8_RSP_COEF_15			GENMASK(25, 16)
#define RSP_9						0x00000078
	#define RSP_9_RSP_COEF_16			GENMASK(9, 0)
	#define RSP_9_RSP_COEF_17			GENMASK(25, 16)
#define RSP_10						0x0000007c
	#define RSP_10_RSP_COEF_18			GENMASK(9, 0)
	#define RSP_10_RSP_COEF_19			GENMASK(25, 16)
#define RSP_11						0x00000080
	#define RSP_11_RSP_COEF_20			GENMASK(9, 0)
	#define RSP_11_RSP_COEF_21			GENMASK(25, 16)
#define RSP_12						0x00000084
	#define RSP_12_RSP_COEF_22			GENMASK(9, 0)
	#define RSP_12_RSP_COEF_23			GENMASK(25, 16)
#define RSP_13						0x00000088
	#define RSP_13_RSP_COEF_24			GENMASK(9, 0)
	#define RSP_13_RSP_COEF_25			GENMASK(25, 16)
#define RSP_14						0x0000008c
	#define RSP_14_RSP_COEF_26			GENMASK(9, 0)
	#define RSP_14_RSP_COEF_27			GENMASK(25, 16)
#define RSP_15						0x00000090
	#define RSP_15_RSP_COEF_28			GENMASK(9, 0)
	#define RSP_15_RSP_COEF_29			GENMASK(25, 16)
#define RSP_16						0x00000094
	#define RSP_16_RSP_COEF_30			GENMASK(9, 0)
	#define RSP_16_RSP_COEF_31			GENMASK(25, 16)
#define RSP_17						0x00000098
	#define RSP_17_RSP_COEF_32			GENMASK(9, 0)
	#define RSP_17_RSP_COEF_33			GENMASK(25, 16)
#define RSP_18						0x0000009c
	#define RSP_18_RSP_COEF_34			GENMASK(9, 0)
	#define RSP_18_RSP_COEF_35			GENMASK(25, 16)
#define RSP_19						0x000000a0
	#define RSP_19_RSP_COEF_36			GENMASK(9, 0)
	#define RSP_19_RSP_COEF_37			GENMASK(25, 16)
#define RSP_20						0x000000a4
	#define RSP_20_RSP_COEF_38			GENMASK(9, 0)
	#define RSP_20_RSP_COEF_39			GENMASK(25, 16)
#define RSP_21						0x000000a8
	#define RSP_21_RSP_COEF_40			GENMASK(9, 0)
	#define RSP_21_RSP_COEF_41			GENMASK(25, 16)
#define RSP_22						0x000000ac
	#define RSP_22_RSP_COEF_42			GENMASK(9, 0)
	#define RSP_22_RSP_COEF_43			GENMASK(25, 16)
#define RSP_23						0x000000b0
	#define RSP_23_RSP_COEF_44			GENMASK(9, 0)
	#define RSP_23_RSP_COEF_45			GENMASK(25, 16)
#define RSP_24						0x000000b4
	#define RSP_24_RSP_COEF_46			GENMASK(9, 0)
	#define RSP_24_RSP_COEF_47			GENMASK(25, 16)
#define RSP_25						0x000000b8
	#define RSP_25_RSP_COEF_48			GENMASK(9, 0)
	#define RSP_25_RSP_COEF_49			GENMASK(25, 16)
#define RSP_26						0x000000bc
	#define RSP_26_RSP_COEF_50			GENMASK(9, 0)
	#define RSP_26_RSP_COEF_51			GENMASK(25, 16)
#define RSP_27						0x000000c0
	#define RSP_27_RSP_COEF_52			GENMASK(9, 0)
	#define RSP_27_RSP_COEF_53			GENMASK(25, 16)
#define RSP_28						0x000000c4
	#define RSP_28_RSP_COEF_54			GENMASK(9, 0)
	#define RSP_28_RSP_COEF_55			GENMASK(25, 16)
#define RSP_29						0x000000c8
	#define RSP_29_RSP_COEF_56			GENMASK(9, 0)
	#define RSP_29_RSP_COEF_57			GENMASK(25, 16)
#define RSP_30						0x000000cc
	#define RSP_30_RSP_COEF_58			GENMASK(9, 0)
	#define RSP_30_RSP_COEF_59			GENMASK(25, 16)
#define RSP_31						0x000000d0
	#define RSP_31_RSP_COEF_60			GENMASK(9, 0)
	#define RSP_31_RSP_COEF_61			GENMASK(25, 16)
#define RSP_32						0x000000d4
	#define RSP_32_RSP_COEF_62			GENMASK(9, 0)
	#define RSP_32_RSP_COEF_63			GENMASK(25, 16)
#define RSP_33						0x000000d8
	#define RSP_33_RSP_COEF_64			GENMASK(9, 0)
	#define RSP_33_RSP_COEF_RSV			GENMASK(25, 16)
#define VERSION						0x000000dc
	#define VERSION_VERSION				GENMASK(31, 0)
#define SYS_1						0x00000100
	#define SYS_1_PROC_MODE				GENMASK(1, 0)
#define SRC_IMG_5					0x00000114
	#define SRC_IMG_5_SRC_IMG_NO2_BASEADDR		GENMASK(31, 0)
#define SRC_IMG_6					0x00000118
	#define SRC_IMG_6_SRC_IMG_NO2_STRIDE		GENMASK(31, 0)
#define SRC_IMG_7					0x0000011c
	#define SRC_IMG_7_SRC_IMG_NO3_BASEADDR		GENMASK(31, 0)
#define SRC_IMG_8					0x00000120
	#define SRC_IMG_8_SRC_IMG_NO3_STRIDE		GENMASK(31, 0)
#define WARPMAP_4					0x0000013c
	#define WARPMAP_4_VTX_TBL_NO2_BASEADDR		GENMASK(31, 0)
#define WARPMAP_5					0x00000140
	#define WARPMAP_5_VTX_TBL_NO2_STRIDE		GENMASK(15, 0)
	#define WARPMAP_5_VTX_TBL_NO3_STRIDE		GENMASK(31, 16)
#define WARPMAP_6					0x00000144
	#define WARPMAP_6_VTX_TBL_NO3_BASEADDR		GENMASK(31, 0)


#endif /*__MTK_WARP_REG_H__*/
