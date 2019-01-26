// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 * Copyright (c) 2017, AngeloGioacchino Del Regno <kholk11@gmail.com>
 */

#include <linux/platform_device.h>
#include "tsens.h"

/* eeprom layout data for 8976 */
#define BASE0_MASK		0xff
#define BASE1_MASK		0xff
#define BASE1_SHIFT		8

#define S0_P1_MASK		0x3f00
#define S1_P1_MASK		0x3f00000
#define S2_P1_MASK		0x3f
#define S3_P1_MASK		0x3f000
#define S4_P1_MASK		0x3f00
#define S5_P1_MASK		0x3f00000
#define S6_P1_MASK		0x3f
#define S7_P1_MASK		0x3f000
#define S8_P1_MASK		0x1f8
#define S9_P1_MASK		0x1f8000
#define S10_P1_MASK		0xf8000000
#define S10_P1_MASK_1		0x1

#define S0_P2_MASK		0xfc000
#define S1_P2_MASK		0xfc000000
#define S2_P2_MASK		0xfc0
#define S3_P2_MASK		0xfc0000
#define S4_P2_MASK		0xfc000
#define S5_P2_MASK		0xfc000000
#define S6_P2_MASK		0xfc0
#define S7_P2_MASK		0xfc0000
#define S8_P2_MASK		0x7e00
#define S9_P2_MASK		0x7e00000
#define S10_P2_MASK		0x7e

#define S0_P1_SHIFT		0x8
#define S1_P1_SHIFT		0x14
#define S2_P1_SHIFT		0x0
#define S3_P1_SHIFT		0xc
#define S4_P1_SHIFT		0x8
#define S5_P1_SHIFT		0x14
#define S6_P1_SHIFT		0x0
#define S7_P1_SHIFT		0xc
#define S8_P1_SHIFT		0x3
#define S9_P1_SHIFT		0xf
#define S10_P1_SHIFT		0x1b
#define S10_P1_SHIFT_1		0

#define S0_P2_SHIFT		0xe
#define S1_P2_SHIFT		0x1a
#define S2_P2_SHIFT		0x6
#define S3_P2_SHIFT		0x12
#define S4_P2_SHIFT		0xe
#define S5_P2_SHIFT		0x1a
#define S6_P2_SHIFT		0x6
#define S7_P2_SHIFT		0x12
#define S8_P2_SHIFT		0x9
#define S9_P2_SHIFT		0x15
#define S10_P2_SHIFT		0x1

#define CAL_SEL_MASK		0x3

#define CAL_DEGC_PT1		30
#define CAL_DEGC_PT2		120
#define SLOPE_FACTOR		1000
#define SLOPE_DEFAULT		3200

static void compute_intercept_slope_8976(struct tsens_device *tmdev,
			      u32 *p1, u32 *p2, u32 mode)
{
	int i;

	tmdev->sensor[0].slope = 3313;
	tmdev->sensor[1].slope = 3275;
	tmdev->sensor[2].slope = 3320;
	tmdev->sensor[3].slope = 3246;
	tmdev->sensor[4].slope = 3279;
	tmdev->sensor[5].slope = 3257;
	tmdev->sensor[6].slope = 3234;
	tmdev->sensor[7].slope = 3269;
	tmdev->sensor[8].slope = 3255;
	tmdev->sensor[9].slope = 3239;
	tmdev->sensor[10].slope = 3286;

	for (i = 0; i < tmdev->num_sensors; i++) {
                dev_dbg(tmdev->dev,
                        "sensor%d - data_point1:%#x data_point2:%#x\n",
                        i, p1[i], p2[i]);

		tmdev->sensor[i].offset = (p1[i] * SLOPE_FACTOR) -
				(CAL_DEGC_PT1 *
				tmdev->sensor[i].slope);
		dev_dbg(tmdev->dev, "offset:%d\n", tmdev->sensor[i].offset);
	}
}

static int calibrate_8976(struct tsens_device *tmdev)
{
	int base0 = 0, base1 = 0, i;
	u32 p1[11], p2[11];
	int mode = 0, tmp = 0;
	u32 *qfprom_cdata;

	qfprom_cdata = (u32 *)qfprom_read(tmdev->dev, "calib");
	if (IS_ERR(qfprom_cdata))
		return PTR_ERR(qfprom_cdata);

	mode = (qfprom_cdata[4] & CAL_SEL_MASK);
	dev_dbg(tmdev->dev, "calibration mode is %d\n", mode);

	switch (mode) {
	case TWO_PT_CALIB:
		base1 = (qfprom_cdata[2] & BASE1_MASK) >> BASE1_SHIFT;
		p2[0] = (qfprom_cdata[0] & S0_P2_MASK) >> S0_P2_SHIFT;
		p2[1] = (qfprom_cdata[0] & S1_P2_MASK) >> S1_P2_SHIFT;
		p2[2] = (qfprom_cdata[1] & S2_P2_MASK) >> S2_P2_SHIFT;
		p2[3] = (qfprom_cdata[1] & S3_P2_MASK) >> S3_P2_SHIFT;
		p2[4] = (qfprom_cdata[2] & S4_P2_MASK) >> S4_P2_SHIFT;
		p2[5] = (qfprom_cdata[2] & S5_P2_MASK) >> S5_P2_SHIFT;
		p2[6] = (qfprom_cdata[3] & S6_P2_MASK) >> S6_P2_SHIFT;
		p2[7] = (qfprom_cdata[3] & S7_P2_MASK) >> S7_P2_SHIFT;
		p2[8] = (qfprom_cdata[4] & S8_P2_MASK) >> S8_P2_SHIFT;
		p2[9] = (qfprom_cdata[4] & S9_P2_MASK) >> S9_P2_SHIFT;
		p2[10] = (qfprom_cdata[5] & S10_P2_MASK) >> S10_P2_SHIFT;

		for (i = 0; i < tmdev->num_sensors; i++)
			p2[i] = ((base1 + p2[i]) << 2);
		/* Fall through */
	case ONE_PT_CALIB2:
		base0 = (qfprom_cdata[0] & BASE0_MASK);
		p1[0] = (qfprom_cdata[0] & S0_P1_MASK) >> S0_P1_SHIFT;
		p1[1] = (qfprom_cdata[0] & S1_P1_MASK) >> S1_P1_SHIFT;
		p1[2] = (qfprom_cdata[1] & S2_P1_MASK) >> S2_P1_SHIFT;
		p1[3] = (qfprom_cdata[1] & S3_P1_MASK) >> S3_P1_SHIFT;
		p1[4] = (qfprom_cdata[2] & S4_P1_MASK) >> S4_P1_SHIFT;
		p1[5] = (qfprom_cdata[2] & S5_P1_MASK) >> S5_P1_SHIFT;
		p1[6] = (qfprom_cdata[3] & S6_P1_MASK) >> S6_P1_SHIFT;
		p1[7] = (qfprom_cdata[3] & S7_P1_MASK) >> S7_P1_SHIFT;
		p1[8] = (qfprom_cdata[4] & S8_P1_MASK) >> S8_P1_SHIFT;
		p1[9] = (qfprom_cdata[4] & S9_P1_MASK) >> S9_P1_SHIFT;
		p1[10] = (qfprom_cdata[4] & S10_P1_MASK) >> S10_P1_SHIFT;
		tmp = (qfprom_cdata[5] & S10_P1_MASK_1) << S10_P1_SHIFT_1;
		p1[10] |= tmp;

		for (i = 0; i < tmdev->num_sensors; i++)
			p1[i] = (((base0) + p1[i]) << 2);
		break;
	default:
		for (i = 0; i < tmdev->num_sensors; i++) {
			p1[i] = 500;
			p2[i] = 780;
		}
		break;
	}

	compute_intercept_slope_8976(tmdev, p1, p2, mode);

	return 0;
}

static const struct tsens_ops ops_8976 = {
	.init		= init_common,
	.calibrate	= calibrate_8976,
	.get_temp	= get_temp_common,
};

const struct tsens_data data_8976 = {
	.num_sensors	= 11,
	.ops		= &ops_8976,
	.reg_offsets	= { [SROT_CTRL_OFFSET] = 0x0 },
	.hw_ids		= (unsigned int[]){0, 1, 2, 4, 5, 6, 7, 8, 9, 10},
};
