/*
 * This header provides constants for binding mediatek.isp.
 *
 */
#ifndef _DT_BINDINGS_ISP_H
#define _DT_BINDINGS_ISP_H

#define MTK_ISP_ISP0	0
#define MTK_ISP_ISP1	1
#define MTK_ISP_ISP2	2
#define MTK_ISP_ISP3	3
#define MTK_ISP_ISP4	4
#define MTK_ISP_ISP5	5

#define MTK_ISP_ISP_CAM_BASE_ID		0x00
#define MTK_ISP_ISP_DIP_BASE_ID		0x10
#define MTK_ISP_ISP_CAMSV_BASE_ID	0x20
#define MTK_ISP_ISP_RBFC_BASE_ID	0x30
#define MTK_ISP_ISP_CAMSYS_BASE_ID	0x40
#define MTK_ISP_ISP_IMGSYS_BASE_ID	0x50
#define MTK_ISP_ISP_BE_BASE_ID		0x60
#define MTK_ISP_ISP_SENINF_BASE_ID	0x70


#define MTK_ISP_ISP_CAM1	(MTK_ISP_ISP_CAM_BASE_ID + 0)
#define MTK_ISP_ISP_CAM2	(MTK_ISP_ISP_CAM_BASE_ID + 1)
#define MTK_ISP_ISP_UNI		(MTK_ISP_ISP_CAM_BASE_ID + 2)

#define MTK_ISP_ISP_CAMSV0	(MTK_ISP_ISP_CAMSV_BASE_ID + 0)
#define MTK_ISP_ISP_CAMSV1	(MTK_ISP_ISP_CAMSV_BASE_ID + 1)
#define MTK_ISP_ISP_CAMSV2	(MTK_ISP_ISP_CAMSV_BASE_ID + 2)
#define MTK_ISP_ISP_CAMSV3	(MTK_ISP_ISP_CAMSV_BASE_ID + 3)
#define MTK_ISP_ISP_CAMSV4	(MTK_ISP_ISP_CAMSV_BASE_ID + 4)
#define MTK_ISP_ISP_CAMSV5	(MTK_ISP_ISP_CAMSV_BASE_ID + 5)

#define MTK_ISP_ISP_DIPA	(MTK_ISP_ISP_DIP_BASE_ID + 0)

#define MTK_ISP_ISP_CAMSYS_SIDE0	(MTK_ISP_ISP_CAMSYS_BASE_ID + 0)
#define MTK_ISP_ISP_CAMSYS_SIDE1	(MTK_ISP_ISP_CAMSYS_BASE_ID + 1)
#define MTK_ISP_ISP_CAMSYS_GAZE0	(MTK_ISP_ISP_CAMSYS_BASE_ID + 2)
#define MTK_ISP_ISP_CAMSYS_GAZE1	(MTK_ISP_ISP_CAMSYS_BASE_ID + 3)
#define MTK_ISP_ISP_IMGSYS_SIDE0	(MTK_ISP_ISP_IMGSYS_BASE_ID + 0)
#define MTK_ISP_ISP_IMGSYS_SIDE1	(MTK_ISP_ISP_IMGSYS_BASE_ID + 1)
#define MTK_ISP_ISP_IMGSYS_GAZE0	(MTK_ISP_ISP_IMGSYS_BASE_ID + 2)
#define MTK_ISP_ISP_IMGSYS_GAZE1	(MTK_ISP_ISP_IMGSYS_BASE_ID + 3)

#define MTK_ISP_ISP_RBFC_RRZO_A	(MTK_ISP_ISP_RBFC_BASE_ID + 0)
#define MTK_ISP_ISP_RBFC_RRZO_B	(MTK_ISP_ISP_RBFC_BASE_ID + 1)
#define MTK_ISP_ISP_RBFC_REN_A	(MTK_ISP_ISP_RBFC_BASE_ID + 2)
#define MTK_ISP_ISP_RBFC_REN_B	(MTK_ISP_ISP_RBFC_BASE_ID + 3)
#define MTK_ISP_ISP_RBFC_WEN_A	(MTK_ISP_ISP_RBFC_BASE_ID + 4)
#define MTK_ISP_ISP_RBFC_WEN_B	(MTK_ISP_ISP_RBFC_BASE_ID + 5)

#define MTK_ISP_ISP_SENINF1	(MTK_ISP_ISP_SENINF_BASE_ID + 0)
#define MTK_ISP_ISP_SENINF2	(MTK_ISP_ISP_SENINF_BASE_ID + 1)
#define MTK_ISP_ISP_SENINF3	(MTK_ISP_ISP_SENINF_BASE_ID + 2)
#define MTK_ISP_ISP_SENINF4	(MTK_ISP_ISP_SENINF_BASE_ID + 3)
#define MTK_ISP_ISP_SENINF5	(MTK_ISP_ISP_SENINF_BASE_ID + 4)
#define MTK_ISP_ISP_SENINF6	(MTK_ISP_ISP_SENINF_BASE_ID + 5)
#define MTK_ISP_ISP_CSI0	(MTK_ISP_ISP_SENINF_BASE_ID + 6)
#define MTK_ISP_ISP_CSI1	(MTK_ISP_ISP_SENINF_BASE_ID + 7)
#define MTK_ISP_ISP_CSI2	(MTK_ISP_ISP_SENINF_BASE_ID + 8)
#define MTK_ISP_ISP_CSI3	(MTK_ISP_ISP_SENINF_BASE_ID + 9)
#define MTK_ISP_ISP_CSI4	(MTK_ISP_ISP_SENINF_BASE_ID + 10)
#define MTK_ISP_ISP_CSI5	(MTK_ISP_ISP_SENINF_BASE_ID + 11)
#define MTK_ISP_ISP_CSI6	(MTK_ISP_ISP_SENINF_BASE_ID + 12)
#define MTK_ISP_ISP_CSI7	(MTK_ISP_ISP_SENINF_BASE_ID + 13)
#define MTK_ISP_ISP_CSI8	(MTK_ISP_ISP_SENINF_BASE_ID + 14)
#define MTK_ISP_ISP_SENINF_MUX1	(MTK_ISP_ISP_SENINF_BASE_ID + 15)
#define MTK_ISP_ISP_SENINF_MUX2	(MTK_ISP_ISP_SENINF_BASE_ID + 16)
#define MTK_ISP_ISP_SENINF_MUX3	(MTK_ISP_ISP_SENINF_BASE_ID + 17)
#define MTK_ISP_ISP_SENINF_MUX4	(MTK_ISP_ISP_SENINF_BASE_ID + 18)
#define MTK_ISP_ISP_SENINF_MUX5	(MTK_ISP_ISP_SENINF_BASE_ID + 19)
#define MTK_ISP_ISP_SENINF_MUX6	(MTK_ISP_ISP_SENINF_BASE_ID + 20)
#define MTK_ISP_ISP_SENINF_MUX7	(MTK_ISP_ISP_SENINF_BASE_ID + 21)
#define MTK_ISP_ISP_SENINF_MUX8	(MTK_ISP_ISP_SENINF_BASE_ID + 22)

#define MTK_ISP_ISP_BE0		(MTK_ISP_ISP_BE_BASE_ID + 0)
#define MTK_ISP_ISP_BE1		(MTK_ISP_ISP_BE_BASE_ID + 1)
#define MTK_ISP_ISP_BE2		(MTK_ISP_ISP_BE_BASE_ID + 2)
#define MTK_ISP_ISP_BE3		(MTK_ISP_ISP_BE_BASE_ID + 3)

#define MTK_CAMERA_SIDE_LL		0
#define MTK_CAMERA_SIDE_LR		1
#define MTK_CAMERA_SIDE_RL		2
#define MTK_CAMERA_SIDE_RR		3
#define MTK_CAMERA_GAZE_L		4
#define MTK_CAMERA_GAZE_R		5
#define MTK_CAMERA_MAX_NUM		6

#endif  /* _DT_BINDINGS_ISP_H */
