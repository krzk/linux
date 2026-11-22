/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __QCOM_SOCINFO_H__
#define __QCOM_SOCINFO_H__

#include <linux/types.h>

/*
 * SMEM item id, used to acquire handles to respective
 * SMEM region.
 */
#define SMEM_HW_SW_BUILD_ID		137

#define SMEM_SOCINFO_BUILD_ID_LENGTH	32
#define SMEM_SOCINFO_CHIP_ID_LENGTH	32

/*
 * SoC version type with major number in the upper 16 bits and minor
 * number in the lower 16 bits.
 */
#define SOCINFO_MAJOR(ver) (((ver) >> 16) & 0xffff)
#define SOCINFO_MINOR(ver) ((ver) & 0xffff)
#define SOCINFO_VERSION(maj, min)  ((((maj) & 0xffff) << 16)|((min) & 0xffff))

/* Socinfo SMEM item structure */
struct socinfo {
	__le32 fmt;
	__le32 id;
	__le32 ver;
	char build_id[SMEM_SOCINFO_BUILD_ID_LENGTH];
	/* Version 2 */
	__le32 raw_id;
	__le32 raw_ver;
	/* Version 3 */
	__le32 hw_plat;
	/* Version 4 */
	__le32 plat_ver;
	/* Version 5 */
	__le32 accessory_chip;
	/* Version 6 */
	__le32 hw_plat_subtype;
	/* Version 7 */
	__le32 pmic_model;
	__le32 pmic_die_rev;
	/* Version 8 */
	__le32 pmic_model_1;
	__le32 pmic_die_rev_1;
	__le32 pmic_model_2;
	__le32 pmic_die_rev_2;
	/* Version 9 */
	__le32 foundry_id;
	/* Version 10 */
	__le32 serial_num;
	/* Version 11 */
	__le32 num_pmics;
	__le32 pmic_array_offset;
	/* Version 12 */
	__le32 chip_family;
	__le32 raw_device_family;
	__le32 raw_device_num;
	/* Version 13 */
	__le32 nproduct_id;
	char chip_id[SMEM_SOCINFO_CHIP_ID_LENGTH];
	/* Version 14 */
	__le32 num_clusters;
	__le32 ncluster_array_offset;
	__le32 num_subset_parts;
	__le32 nsubset_parts_array_offset;
	/* Version 15 */
	__le32 nmodem_supported;
	/* Version 16 */
	__le32  feature_code;
	__le32  pcode;
	__le32  npartnamemap_offset;
	__le32  nnum_partname_mapping;
	/* Version 17 */
	__le32 oem_variant;
	/* Version 18 */
	__le32 num_kvps;
	__le32 kvps_offset;
	/* Version 19 */
	__le32 num_func_clusters;
	__le32 boot_cluster;
	__le32 boot_core;
	/* Version 20 */
	__le32 raw_package_type;
	/* Version 21, 22, 23 */
	__le32 reserve1[4];
};

/* Internal feature codes */
enum qcom_socinfo_feature_code {
	/* External feature codes */
	SOCINFO_FC_UNKNOWN = 0x0,
	SOCINFO_FC_AA,
	SOCINFO_FC_AB,
	SOCINFO_FC_AC,
	SOCINFO_FC_AD,
	SOCINFO_FC_AE,
	SOCINFO_FC_AF,
	SOCINFO_FC_AG,
	SOCINFO_FC_AH,
};

/* Internal feature codes */
/* Valid values: 0 <= n <= 0xf */
#define SOCINFO_FC_Yn(n)		(0xf1 + (n))
#define SOCINFO_FC_INT_MAX		SOCINFO_FC_Yn(0xf)

/* Product codes */
#define SOCINFO_PC_UNKNOWN		0
#define SOCINFO_PCn(n)			((n) + 1)
#define SOCINFO_PC_RESERVE		(BIT(31) - 1)

/* SoC IDs */
#define QCOM_ID_SM8450			457
#define QCOM_ID_SA8295P			460
#define QCOM_ID_SA8540P			461
#define QCOM_ID_QCM4290			469
#define QCOM_ID_QCS4290			470
#define QCOM_ID_SM7325			475
#define QCOM_ID_SM8450_2		480
#define QCOM_ID_SM8450_3		482
#define QCOM_ID_SC7280			487
#define QCOM_ID_SC7180P			495
#define QCOM_ID_QCM6490			497
#define QCOM_ID_QCS6490			498
#define QCOM_ID_SM7325P			499
#define QCOM_ID_IPQ5000			503
#define QCOM_ID_IPQ0509			504
#define QCOM_ID_IPQ0518			505
#define QCOM_ID_SM7450			506
#define QCOM_ID_SM6375			507
#define QCOM_ID_IPQ9514			510
#define QCOM_ID_IPQ9550			511
#define QCOM_ID_IPQ9554			512
#define QCOM_ID_IPQ9570			513
#define QCOM_ID_IPQ9574			514
#define QCOM_ID_SM8550			519
#define QCOM_ID_IPQ5016			520
#define QCOM_ID_IPQ9510			521
#define QCOM_ID_QRB4210			523
#define QCOM_ID_QRB2210			524
#define QCOM_ID_SAR2130P		525
#define QCOM_ID_SM8475			530
#define QCOM_ID_SM8475P			531
#define QCOM_ID_SA8255P			532
#define QCOM_ID_SA8650P			533
#define QCOM_ID_SA8775P			534
#define QCOM_ID_QRU1000			539
#define QCOM_ID_SM8475_2		540
#define QCOM_ID_QDU1000			545
#define QCOM_ID_SM7450P			547
#define QCOM_ID_X1E80100		555
#define QCOM_ID_SM8650			557
#define QCOM_ID_SM4450			568
#define QCOM_ID_SAR1130P		579
#define QCOM_ID_QDU1010			587
#define QCOM_ID_QRU1032			588
#define QCOM_ID_QRU1052			589
#define QCOM_ID_QRU1062			590
#define QCOM_ID_IPQ5332			592
#define QCOM_ID_IPQ5322			593
#define QCOM_ID_IPQ5312			594
#define QCOM_ID_IPQ5302			595
#define QCOM_ID_QCS8550			603
#define QCOM_ID_QCM8550			604
#define QCOM_ID_SM8750			618
#define QCOM_ID_IPQ5300			624
#define QCOM_ID_SM7635			636
#define QCOM_ID_SM6650			640
#define QCOM_ID_SM6650P			641
#define QCOM_ID_IPQ5321			650
#define QCOM_ID_IPQ5424			651
#define QCOM_ID_QCM6690			657
#define QCOM_ID_QCS6690			658
#define QCOM_ID_SM7750			659
#define QCOM_ID_SM8850			660
#define QCOM_ID_QCS9100			667
#define QCOM_ID_IPQ5404			671
#define QCOM_ID_QCS8300			674
#define QCOM_ID_QCS8275			675
#define QCOM_ID_QCS9075			676
#define QCOM_ID_QCS615			680
#define QCOM_ID_SA8797P			690
#define QCOM_ID_SM8975			707
#define QCOM_ID_CQ7790M			731
#define QCOM_ID_CQ7790S			732
#define QCOM_ID_SM8950			735
#define QCOM_ID_CQ2390M			756
#define QCOM_ID_CQ2390S			758
#define QCOM_ID_IQ2390S			759
#define QCOM_ID_IPQ5200			765
#define QCOM_ID_IPQ5210			766
#define QCOM_ID_QCF2200			767
#define QCOM_ID_QCF3200			768
#define QCOM_ID_QCF3210			769
#define QCOM_ID_IPQ9620			770
#define QCOM_ID_IPQ9650			771
#define QCOM_ID_IPQ9610			778
#define QCOM_ID_IPQ9630			779
#define QCOM_ID_IPQ9640			780
#define QCOM_ID_IPQ9670			781
#define QCOM_ID_QCF3211			801
#define QCOM_ID_IPQ5211			802

#endif
