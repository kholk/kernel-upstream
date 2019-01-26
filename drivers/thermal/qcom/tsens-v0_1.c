// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 */

#include <linux/platform_device.h>
#include "tsens.h"

/* ----- SROT ------ */
#define SROT_CTRL_OFF 0x0000

/* ----- TM ------ */
#define TM_INT_EN_OFF				0x0000
#define TM_Sn_UPPER_LOWER_STATUS_CTRL_OFF	0x0004
#define TM_Sn_STATUS_OFF			0x0030
#define TM_TRDY_OFF				0x005c

/* eeprom layout data for 8916 */
#define MSM8916_BASE0_MASK	0x0000007f
#define MSM8916_BASE1_MASK	0xfe000000
#define MSM8916_BASE0_SHIFT	0
#define MSM8916_BASE1_SHIFT	25

#define MSM8916_S0_P1_MASK	0x00000f80
#define MSM8916_S1_P1_MASK	0x003e0000
#define MSM8916_S2_P1_MASK	0xf8000000
#define MSM8916_S3_P1_MASK	0x000003e0
#define MSM8916_S4_P1_MASK	0x000f8000

#define MSM8916_S0_P2_MASK	0x0001f000
#define MSM8916_S1_P2_MASK	0x07c00000
#define MSM8916_S2_P2_MASK	0x0000001f
#define MSM8916_S3_P2_MASK	0x00007c00
#define MSM8916_S4_P2_MASK	0x01f00000

#define MSM8916_S0_P1_SHIFT	7
#define MSM8916_S1_P1_SHIFT	17
#define MSM8916_S2_P1_SHIFT	27
#define MSM8916_S3_P1_SHIFT	5
#define MSM8916_S4_P1_SHIFT	15

#define MSM8916_S0_P2_SHIFT	12
#define MSM8916_S1_P2_SHIFT	22
#define MSM8916_S2_P2_SHIFT	0
#define MSM8916_S3_P2_SHIFT	10
#define MSM8916_S4_P2_SHIFT	20

#define MSM8916_CAL_SEL_MASK	0xe0000000
#define MSM8916_CAL_SEL_SHIFT	29

/* eeprom layout data for 8974 */
#define BASE1_MASK		0xff
#define S0_P1_MASK		0x3f00
#define S1_P1_MASK		0xfc000
#define S2_P1_MASK		0x3f00000
#define S3_P1_MASK		0xfc000000
#define S4_P1_MASK		0x3f
#define S5_P1_MASK		0xfc0
#define S6_P1_MASK		0x3f000
#define S7_P1_MASK		0xfc0000
#define S8_P1_MASK		0x3f000000
#define S8_P1_MASK_BKP		0x3f
#define S9_P1_MASK		0x3f
#define S9_P1_MASK_BKP		0xfc0
#define S10_P1_MASK		0xfc0
#define S10_P1_MASK_BKP		0x3f000
#define CAL_SEL_0_1		0xc0000000
#define CAL_SEL_2		0x40000000
#define CAL_SEL_SHIFT		30
#define CAL_SEL_SHIFT_2		28

#define S0_P1_SHIFT		8
#define S1_P1_SHIFT		14
#define S2_P1_SHIFT		20
#define S3_P1_SHIFT		26
#define S5_P1_SHIFT		6
#define S6_P1_SHIFT		12
#define S7_P1_SHIFT		18
#define S8_P1_SHIFT		24
#define S9_P1_BKP_SHIFT		6
#define S10_P1_SHIFT		6
#define S10_P1_BKP_SHIFT	12

#define BASE2_SHIFT		12
#define BASE2_BKP_SHIFT		18
#define S0_P2_SHIFT		20
#define S0_P2_BKP_SHIFT		26
#define S1_P2_SHIFT		26
#define S2_P2_BKP_SHIFT		6
#define S3_P2_SHIFT		6
#define S3_P2_BKP_SHIFT		12
#define S4_P2_SHIFT		12
#define S4_P2_BKP_SHIFT		18
#define S5_P2_SHIFT		18
#define S5_P2_BKP_SHIFT		24
#define S6_P2_SHIFT		24
#define S7_P2_BKP_SHIFT		6
#define S8_P2_SHIFT		6
#define S8_P2_BKP_SHIFT		12
#define S9_P2_SHIFT		12
#define S9_P2_BKP_SHIFT		18
#define S10_P2_SHIFT		18
#define S10_P2_BKP_SHIFT	24

#define BASE2_MASK		0xff000
#define BASE2_BKP_MASK		0xfc0000
#define S0_P2_MASK		0x3f00000
#define S0_P2_BKP_MASK		0xfc000000
#define S1_P2_MASK		0xfc000000
#define S1_P2_BKP_MASK		0x3f
#define S2_P2_MASK		0x3f
#define S2_P2_BKP_MASK		0xfc0
#define S3_P2_MASK		0xfc0
#define S3_P2_BKP_MASK		0x3f000
#define S4_P2_MASK		0x3f000
#define S4_P2_BKP_MASK		0xfc0000
#define S5_P2_MASK		0xfc0000
#define S5_P2_BKP_MASK		0x3f000000
#define S6_P2_MASK		0x3f000000
#define S6_P2_BKP_MASK		0x3f
#define S7_P2_MASK		0x3f
#define S7_P2_BKP_MASK		0xfc0
#define S8_P2_MASK		0xfc0
#define S8_P2_BKP_MASK		0x3f000
#define S9_P2_MASK		0x3f000
#define S9_P2_BKP_MASK		0xfc0000
#define S10_P2_MASK		0xfc0000
#define S10_P2_BKP_MASK		0x3f000000

#define BKP_SEL			0x3
#define BKP_REDUN_SEL		0xe0000000
#define BKP_REDUN_SHIFT		29

#define BIT_APPEND		0x3

/* eeprom layout data for 8976 */
#define MSM8976_BASE0_MASK	0xff
#define MSM8976_BASE1_MASK	0xff
#define MSM8976_BASE1_SHIFT	8

#define MSM8976_S0_P1_MASK	0x3f00
#define MSM8976_S1_P1_MASK	0x3f00000
#define MSM8976_S2_P1_MASK	0x3f
#define MSM8976_S3_P1_MASK	0x3f000
#define MSM8976_S4_P1_MASK	0x3f00
#define MSM8976_S5_P1_MASK	0x3f00000
#define MSM8976_S6_P1_MASK	0x3f
#define MSM8976_S7_P1_MASK	0x3f000
#define MSM8976_S8_P1_MASK	0x1f8
#define MSM8976_S9_P1_MASK	0x1f8000
#define MSM8976_S10_P1_MASK	0xf8000000
#define MSM8976_S10_P1_MASK_1	0x1

#define MSM8976_S0_P2_MASK	0xfc000
#define MSM8976_S1_P2_MASK	0xfc000000
#define MSM8976_S2_P2_MASK	0xfc0
#define MSM8976_S3_P2_MASK	0xfc0000
#define MSM8976_S4_P2_MASK	0xfc000
#define MSM8976_S5_P2_MASK	0xfc000000
#define MSM8976_S6_P2_MASK	0xfc0
#define MSM8976_S7_P2_MASK	0xfc0000
#define MSM8976_S8_P2_MASK	0x7e00
#define MSM8976_S9_P2_MASK	0x7e00000
#define MSM8976_S10_P2_MASK	0x7e

#define MSM8976_S0_P1_SHIFT	8
#define MSM8976_S1_P1_SHIFT	20
#define MSM8976_S2_P1_SHIFT	0
#define MSM8976_S3_P1_SHIFT	12
#define MSM8976_S4_P1_SHIFT	8
#define MSM8976_S5_P1_SHIFT	20
#define MSM8976_S6_P1_SHIFT	0
#define MSM8976_S7_P1_SHIFT	12
#define MSM8976_S8_P1_SHIFT	3
#define MSM8976_S9_P1_SHIFT	15
#define MSM8976_S10_P1_SHIFT	27
#define MSM8976_S10_P1_SHIFT_1	0

#define MSM8976_S0_P2_SHIFT	14
#define MSM8976_S1_P2_SHIFT	26
#define MSM8976_S2_P2_SHIFT	6
#define MSM8976_S3_P2_SHIFT	18
#define MSM8976_S4_P2_SHIFT	14
#define MSM8976_S5_P2_SHIFT	26
#define MSM8976_S6_P2_SHIFT	6
#define MSM8976_S7_P2_SHIFT	18
#define MSM8976_S8_P2_SHIFT	9
#define MSM8976_S9_P2_SHIFT	21
#define MSM8976_S10_P2_SHIFT	1

#define MSM8976_CAL_SEL_MASK	0x3

#define MSM8976_CAL_DEGC_PT1	30
#define MSM8976_CAL_DEGC_PT2	120
#define MSM8976_SLOPE_FACTOR	1000
#define MSM8976_SLOPE_DEFAULT	3200

static void compute_intercept_slope_8976(struct tsens_priv *priv,
			      u32 *p1, u32 *p2, u32 mode)
{
	int i;

	priv->sensor[0].slope = 3313;
	priv->sensor[1].slope = 3275;
	priv->sensor[2].slope = 3320;
	priv->sensor[3].slope = 3246;
	priv->sensor[4].slope = 3279;
	priv->sensor[5].slope = 3257;
	priv->sensor[6].slope = 3234;
	priv->sensor[7].slope = 3269;
	priv->sensor[8].slope = 3255;
	priv->sensor[9].slope = 3239;
	priv->sensor[10].slope = 3286;

	for (i = 0; i < priv->num_sensors; i++) {
                dev_dbg(priv->dev,
                        "sensor%d - data_point1:%#x data_point2:%#x\n",
                        i, p1[i], p2[i]);

		priv->sensor[i].offset = (p1[i] * MSM8976_SLOPE_FACTOR) -
				(MSM8976_CAL_DEGC_PT1 *
				priv->sensor[i].slope);
		dev_dbg(priv->dev, "offset:%d\n", priv->sensor[i].offset);
	}
}

static int calibrate_8916(struct tsens_priv *priv)
{
	int base0 = 0, base1 = 0, i;
	u32 p1[5], p2[5];
	int mode = 0;
	u32 *qfprom_cdata, *qfprom_csel;

	qfprom_cdata = (u32 *)qfprom_read(priv->dev, "calib");
	if (IS_ERR(qfprom_cdata))
		return PTR_ERR(qfprom_cdata);

	qfprom_csel = (u32 *)qfprom_read(priv->dev, "calib_sel");
	if (IS_ERR(qfprom_csel))
		return PTR_ERR(qfprom_csel);

	mode = (qfprom_csel[0] & MSM8916_CAL_SEL_MASK) >> MSM8916_CAL_SEL_SHIFT;
	dev_dbg(priv->dev, "calibration mode is %d\n", mode);

	switch (mode) {
	case TWO_PT_CALIB:
		base1 = (qfprom_cdata[1] & MSM8916_BASE1_MASK) >> MSM8916_BASE1_SHIFT;
		p2[0] = (qfprom_cdata[0] & MSM8916_S0_P2_MASK) >> MSM8916_S0_P2_SHIFT;
		p2[1] = (qfprom_cdata[0] & MSM8916_S1_P2_MASK) >> MSM8916_S1_P2_SHIFT;
		p2[2] = (qfprom_cdata[1] & MSM8916_S2_P2_MASK) >> MSM8916_S2_P2_SHIFT;
		p2[3] = (qfprom_cdata[1] & MSM8916_S3_P2_MASK) >> MSM8916_S3_P2_SHIFT;
		p2[4] = (qfprom_cdata[1] & MSM8916_S4_P2_MASK) >> MSM8916_S4_P2_SHIFT;
		for (i = 0; i < priv->num_sensors; i++)
			p2[i] = ((base1 + p2[i]) << 3);
		/* Fall through */
	case ONE_PT_CALIB2:
		base0 = (qfprom_cdata[0] & MSM8916_BASE0_MASK);
		p1[0] = (qfprom_cdata[0] & MSM8916_S0_P1_MASK) >> MSM8916_S0_P1_SHIFT;
		p1[1] = (qfprom_cdata[0] & MSM8916_S1_P1_MASK) >> MSM8916_S1_P1_SHIFT;
		p1[2] = (qfprom_cdata[0] & MSM8916_S2_P1_MASK) >> MSM8916_S2_P1_SHIFT;
		p1[3] = (qfprom_cdata[1] & MSM8916_S3_P1_MASK) >> MSM8916_S3_P1_SHIFT;
		p1[4] = (qfprom_cdata[1] & MSM8916_S4_P1_MASK) >> MSM8916_S4_P1_SHIFT;
		for (i = 0; i < priv->num_sensors; i++)
			p1[i] = (((base0) + p1[i]) << 3);
		break;
	default:
		for (i = 0; i < priv->num_sensors; i++) {
			p1[i] = 500;
			p2[i] = 780;
		}
		break;
	}

	compute_intercept_slope(priv, p1, p2, mode);

	return 0;
}

static int calibrate_8974(struct tsens_priv *priv)
{
	int base1 = 0, base2 = 0, i;
	u32 p1[11], p2[11];
	int mode = 0;
	u32 *calib, *bkp;
	u32 calib_redun_sel;

	calib = (u32 *)qfprom_read(priv->dev, "calib");
	if (IS_ERR(calib))
		return PTR_ERR(calib);

	bkp = (u32 *)qfprom_read(priv->dev, "calib_backup");
	if (IS_ERR(bkp))
		return PTR_ERR(bkp);

	calib_redun_sel =  bkp[1] & BKP_REDUN_SEL;
	calib_redun_sel >>= BKP_REDUN_SHIFT;

	if (calib_redun_sel == BKP_SEL) {
		mode = (calib[4] & CAL_SEL_0_1) >> CAL_SEL_SHIFT;
		mode |= (calib[5] & CAL_SEL_2) >> CAL_SEL_SHIFT_2;

		switch (mode) {
		case TWO_PT_CALIB:
			base2 = (bkp[2] & BASE2_BKP_MASK) >> BASE2_BKP_SHIFT;
			p2[0] = (bkp[2] & S0_P2_BKP_MASK) >> S0_P2_BKP_SHIFT;
			p2[1] = (bkp[3] & S1_P2_BKP_MASK);
			p2[2] = (bkp[3] & S2_P2_BKP_MASK) >> S2_P2_BKP_SHIFT;
			p2[3] = (bkp[3] & S3_P2_BKP_MASK) >> S3_P2_BKP_SHIFT;
			p2[4] = (bkp[3] & S4_P2_BKP_MASK) >> S4_P2_BKP_SHIFT;
			p2[5] = (calib[4] & S5_P2_BKP_MASK) >> S5_P2_BKP_SHIFT;
			p2[6] = (calib[5] & S6_P2_BKP_MASK);
			p2[7] = (calib[5] & S7_P2_BKP_MASK) >> S7_P2_BKP_SHIFT;
			p2[8] = (calib[5] & S8_P2_BKP_MASK) >> S8_P2_BKP_SHIFT;
			p2[9] = (calib[5] & S9_P2_BKP_MASK) >> S9_P2_BKP_SHIFT;
			p2[10] = (calib[5] & S10_P2_BKP_MASK) >> S10_P2_BKP_SHIFT;
			/* Fall through */
		case ONE_PT_CALIB:
		case ONE_PT_CALIB2:
			base1 = bkp[0] & BASE1_MASK;
			p1[0] = (bkp[0] & S0_P1_MASK) >> S0_P1_SHIFT;
			p1[1] = (bkp[0] & S1_P1_MASK) >> S1_P1_SHIFT;
			p1[2] = (bkp[0] & S2_P1_MASK) >> S2_P1_SHIFT;
			p1[3] = (bkp[0] & S3_P1_MASK) >> S3_P1_SHIFT;
			p1[4] = (bkp[1] & S4_P1_MASK);
			p1[5] = (bkp[1] & S5_P1_MASK) >> S5_P1_SHIFT;
			p1[6] = (bkp[1] & S6_P1_MASK) >> S6_P1_SHIFT;
			p1[7] = (bkp[1] & S7_P1_MASK) >> S7_P1_SHIFT;
			p1[8] = (bkp[2] & S8_P1_MASK_BKP) >> S8_P1_SHIFT;
			p1[9] = (bkp[2] & S9_P1_MASK_BKP) >> S9_P1_BKP_SHIFT;
			p1[10] = (bkp[2] & S10_P1_MASK_BKP) >> S10_P1_BKP_SHIFT;
			break;
		}
	} else {
		mode = (calib[1] & CAL_SEL_0_1) >> CAL_SEL_SHIFT;
		mode |= (calib[3] & CAL_SEL_2) >> CAL_SEL_SHIFT_2;

		switch (mode) {
		case TWO_PT_CALIB:
			base2 = (calib[2] & BASE2_MASK) >> BASE2_SHIFT;
			p2[0] = (calib[2] & S0_P2_MASK) >> S0_P2_SHIFT;
			p2[1] = (calib[2] & S1_P2_MASK) >> S1_P2_SHIFT;
			p2[2] = (calib[3] & S2_P2_MASK);
			p2[3] = (calib[3] & S3_P2_MASK) >> S3_P2_SHIFT;
			p2[4] = (calib[3] & S4_P2_MASK) >> S4_P2_SHIFT;
			p2[5] = (calib[3] & S5_P2_MASK) >> S5_P2_SHIFT;
			p2[6] = (calib[3] & S6_P2_MASK) >> S6_P2_SHIFT;
			p2[7] = (calib[4] & S7_P2_MASK);
			p2[8] = (calib[4] & S8_P2_MASK) >> S8_P2_SHIFT;
			p2[9] = (calib[4] & S9_P2_MASK) >> S9_P2_SHIFT;
			p2[10] = (calib[4] & S10_P2_MASK) >> S10_P2_SHIFT;
			/* Fall through */
		case ONE_PT_CALIB:
		case ONE_PT_CALIB2:
			base1 = calib[0] & BASE1_MASK;
			p1[0] = (calib[0] & S0_P1_MASK) >> S0_P1_SHIFT;
			p1[1] = (calib[0] & S1_P1_MASK) >> S1_P1_SHIFT;
			p1[2] = (calib[0] & S2_P1_MASK) >> S2_P1_SHIFT;
			p1[3] = (calib[0] & S3_P1_MASK) >> S3_P1_SHIFT;
			p1[4] = (calib[1] & S4_P1_MASK);
			p1[5] = (calib[1] & S5_P1_MASK) >> S5_P1_SHIFT;
			p1[6] = (calib[1] & S6_P1_MASK) >> S6_P1_SHIFT;
			p1[7] = (calib[1] & S7_P1_MASK) >> S7_P1_SHIFT;
			p1[8] = (calib[1] & S8_P1_MASK) >> S8_P1_SHIFT;
			p1[9] = (calib[2] & S9_P1_MASK);
			p1[10] = (calib[2] & S10_P1_MASK) >> S10_P1_SHIFT;
			break;
		}
	}

	switch (mode) {
	case ONE_PT_CALIB:
		for (i = 0; i < priv->num_sensors; i++)
			p1[i] += (base1 << 2) | BIT_APPEND;
		break;
	case TWO_PT_CALIB:
		for (i = 0; i < priv->num_sensors; i++) {
			p2[i] += base2;
			p2[i] <<= 2;
			p2[i] |= BIT_APPEND;
		}
		/* Fall through */
	case ONE_PT_CALIB2:
		for (i = 0; i < priv->num_sensors; i++) {
			p1[i] += base1;
			p1[i] <<= 2;
			p1[i] |= BIT_APPEND;
		}
		break;
	default:
		for (i = 0; i < priv->num_sensors; i++)
			p2[i] = 780;
		p1[0] = 502;
		p1[1] = 509;
		p1[2] = 503;
		p1[3] = 509;
		p1[4] = 505;
		p1[5] = 509;
		p1[6] = 507;
		p1[7] = 510;
		p1[8] = 508;
		p1[9] = 509;
		p1[10] = 508;
		break;
	}

	compute_intercept_slope(priv, p1, p2, mode);

	return 0;
}

static int calibrate_8976(struct tsens_priv *priv)
{
	int base0 = 0, base1 = 0, i;
	u32 p1[11], p2[11];
	int mode = 0, tmp = 0;
	u32 *qfprom_cdata;

	qfprom_cdata = (u32 *)qfprom_read(priv->dev, "calib");
	if (IS_ERR(qfprom_cdata))
		return PTR_ERR(qfprom_cdata);

	mode = (qfprom_cdata[4] & MSM8976_CAL_SEL_MASK);
	dev_dbg(priv->dev, "calibration mode is %d\n", mode);

	switch (mode) {
	case TWO_PT_CALIB:
		base1 = qfprom_cdata[2] & MSM8976_BASE1_MASK;
		base1 = base1 >> MSM8976_BASE1_SHIFT;
		p2[0] = qfprom_cdata[0] & MSM8976_S0_P2_MASK;
		p2[0] = p2[0] >> MSM8976_S0_P2_SHIFT;
		p2[1] = qfprom_cdata[0] & MSM8976_S1_P2_MASK;
		p2[1] = p2[1] >> MSM8976_S1_P2_SHIFT;
		p2[2] = qfprom_cdata[1] & MSM8976_S2_P2_MASK;
		p2[2] = p2[2] >> MSM8976_S2_P2_SHIFT;
		p2[3] = qfprom_cdata[1] & MSM8976_S3_P2_MASK;
		p2[3] = p2[3] >> MSM8976_S3_P2_SHIFT;
		p2[4] = qfprom_cdata[2] & MSM8976_S4_P2_MASK;
		p2[4] = p2[4] >> MSM8976_S4_P2_SHIFT;
		p2[5] = qfprom_cdata[2] & MSM8976_S5_P2_MASK;
		p2[5] = p2[5] >> MSM8976_S5_P2_SHIFT;
		p2[6] = qfprom_cdata[3] & MSM8976_S6_P2_MASK;
		p2[6] = p2[6] >> MSM8976_S6_P2_SHIFT;
		p2[7] = qfprom_cdata[3] & MSM8976_S7_P2_MASK;
		p2[7] = p2[7] >> MSM8976_S7_P2_SHIFT;
		p2[8] = qfprom_cdata[4] & MSM8976_S8_P2_MASK;
		p2[8] = p2[8] >> MSM8976_S8_P2_SHIFT;
		p2[9] = qfprom_cdata[4] & MSM8976_S9_P2_MASK;
		p2[9] = p2[9] >> MSM8976_S9_P2_SHIFT;
		p2[10] = qfprom_cdata[5] & MSM8976_S10_P2_MASK;
		p2[10] = p2[10] >> MSM8976_S10_P2_SHIFT;

		for (i = 0; i < priv->num_sensors; i++)
			p2[i] = ((base1 + p2[i]) << 2);
		/* Fall through */
	case ONE_PT_CALIB2:
		base0 = qfprom_cdata[0] & MSM8976_BASE0_MASK;
		p1[0] = qfprom_cdata[0] & MSM8976_S0_P1_MASK;
		p1[0] = p1[0] >> MSM8976_S0_P1_SHIFT;
		p1[1] = qfprom_cdata[0] & MSM8976_S1_P1_MASK;
		p1[1] = p1[1] >> MSM8976_S1_P1_SHIFT;
		p1[2] = qfprom_cdata[1] & MSM8976_S2_P1_MASK;
		p1[2] = p1[2] >> MSM8976_S2_P1_SHIFT;
		p1[3] = qfprom_cdata[1] & MSM8976_S3_P1_MASK;
		p1[3] = p1[3] >> MSM8976_S3_P1_SHIFT;
		p1[4] = qfprom_cdata[2] & MSM8976_S4_P1_MASK;
		p1[4] = p1[4] >> MSM8976_S4_P1_SHIFT;
		p1[5] = qfprom_cdata[2] & MSM8976_S5_P1_MASK;
		p1[5] = p1[5] >> MSM8976_S5_P1_SHIFT;
		p1[6] = qfprom_cdata[3] & MSM8976_S6_P1_MASK;
		p1[6] = p1[6] >> MSM8976_S6_P1_SHIFT;
		p1[7] = qfprom_cdata[3] & MSM8976_S7_P1_MASK;
		p1[7] = p1[7] >> MSM8976_S7_P1_SHIFT;
		p1[8] = qfprom_cdata[4] & MSM8976_S8_P1_MASK;
		p1[8] = p1[8] >> MSM8976_S8_P1_SHIFT;
		p1[9] = qfprom_cdata[4] & MSM8976_S9_P1_MASK;
		p1[9] = p1[9] >> MSM8976_S9_P1_SHIFT;
		p1[10] = qfprom_cdata[4] & MSM8976_S10_P1_MASK;
		p1[10] = p1[10] >> MSM8976_S10_P1_SHIFT;
		tmp = qfprom_cdata[5] & MSM8976_S10_P1_MASK_1;
		tmp = tmp << MSM8976_S10_P1_SHIFT_1;
		p1[10] |= tmp;

		for (i = 0; i < priv->num_sensors; i++)
			p1[i] = (((base0) + p1[i]) << 2);
		break;
	default:
		for (i = 0; i < priv->num_sensors; i++) {
			p1[i] = 500;
			p2[i] = 780;
		}
		break;
	}

	compute_intercept_slope_8976(priv, p1, p2, mode);

	return 0;
}

/* v0.1: 8916, 8974, 8976 */

static const struct tsens_features tsens_v0_1_feat = {
	.ver_major	= VER_0_1,
	.crit_int	= 0,
	.adc		= 1,
	.srot_split	= 1,
	.max_sensors	= 11,
};

static const struct reg_field tsens_v0_1_regfields[MAX_REGFIELDS] = {
	/* ----- SROT ------ */
	/* No VERSION information */

	/* CTRL_OFFSET */
	[TSENS_EN]     = REG_FIELD(SROT_CTRL_OFF, 0,  0),
	[TSENS_SW_RST] = REG_FIELD(SROT_CTRL_OFF, 1,  1),

	/* ----- TM ------ */
	/* INTERRUPT ENABLE */
	[INT_EN] = REG_FIELD(TM_INT_EN_OFF, 0, 0),

	/* Sn_STATUS */
	REG_FIELD_FOR_EACH_SENSOR11(LAST_TEMP,    TM_Sn_STATUS_OFF,  0,  9),
	/* No VALID field on v0.1 */
	REG_FIELD_FOR_EACH_SENSOR11(MIN_STATUS,   TM_Sn_STATUS_OFF, 10, 10),
	REG_FIELD_FOR_EACH_SENSOR11(LOWER_STATUS, TM_Sn_STATUS_OFF, 11, 11),
	REG_FIELD_FOR_EACH_SENSOR11(UPPER_STATUS, TM_Sn_STATUS_OFF, 12, 12),
	/* No CRITICAL field on v0.1 */
	REG_FIELD_FOR_EACH_SENSOR11(MAX_STATUS,   TM_Sn_STATUS_OFF, 13, 13),

	/* TRDY: 1=ready, 0=in progress */
	[TRDY] = REG_FIELD(TM_TRDY_OFF, 0, 0),
};

static const struct tsens_ops ops_8916 = {
	.init		= init_common,
	.calibrate	= calibrate_8916,
	.get_temp	= get_temp_common,
};

const struct tsens_plat_data data_8916 = {
	.num_sensors	= 5,
	.ops		= &ops_8916,
	.hw_ids		= (unsigned int []){0, 1, 2, 4, 5 },

	.feat		= &tsens_v0_1_feat,
	.fields	= tsens_v0_1_regfields,
};

static const struct tsens_ops ops_8974 = {
	.init		= init_common,
	.calibrate	= calibrate_8974,
	.get_temp	= get_temp_common,
};

const struct tsens_plat_data data_8974 = {
	.num_sensors	= 11,
	.ops		= &ops_8974,
	.feat		= &tsens_v0_1_feat,
	.fields	= tsens_v0_1_regfields,
};

static const struct tsens_ops ops_8976 = {
	.init		= init_common,
	.calibrate	= calibrate_8976,
	.get_temp	= get_temp_common,
};

const struct tsens_plat_data data_8976 = {
	.num_sensors	= 11,
	.ops		= &ops_8976,
	.hw_ids		= (unsigned int[]){0, 1, 2, 4, 5, 6, 7, 8, 9, 10},
	.feat		= &tsens_v0_1_feat,
	.fields		= tsens_v0_1_regfields,
};
