// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#include <linux/printk.h>
#include "common.h"
#include "mach_reg.h"

static void set_phy_6g_sm1(void)
{
	unsigned char info[12] = {0};
	int idx = 1;
	struct hdmitx_dev *hdev = get_hdmitx_device();
	int ret;

	if (!IS_ERR_OR_NULL(hdev->mbox_chan)) {
		ret = aml_mbox_transfer_data(hdev->mbox_chan, MBOX_CMD_OSCRING_VALUE,
				NULL, 0, info, sizeof(info), MBOX_SYNC);
		if (ret < 0) {
			memset(info, 0, sizeof(info));
			pr_info("Fail to send mbox message\n");
		}
	}
	pr_info("value: %d\n", info[3]);
	if (info[3] == 0)
		idx = 1;
	else if (info[3] <= 127)
		idx = 0;
	else if (info[3] <= 138)
		idx = 1;
	else
		idx = 2;

	switch (idx) {
	case 0:
		hd_write_reg(P_HHI_HDMI_PHY_CNTL0, 0x07EB65F3);
		break;
	case 1:
		hd_write_reg(P_HHI_HDMI_PHY_CNTL0, 0x37EB65C4);
		break;
	case 2:
		hd_write_reg(P_HHI_HDMI_PHY_CNTL0, 0x37EB65A4);
		break;
	default:
		hd_write_reg(P_HHI_HDMI_PHY_CNTL0, 0x37EB65C4);
		break;
	}
}

void set_phy_by_mode_sm1(unsigned int mode)
{
	switch (mode) {
	case HDMI_PHYPARA_6G: /* 5.94Gbps */
		hd_write_reg(P_HHI_HDMI_PHY_CNTL5, 0x0000080b);
		set_phy_6g_sm1();
		hd_write_reg(P_HHI_HDMI_PHY_CNTL3, 0x2ab0ff3b);
		break;
	case HDMI_PHYPARA_4p5G: /* 4.5Gbps*/
	case HDMI_PHYPARA_3p7G: /* 3.7Gbps */
		hd_write_reg(P_HHI_HDMI_PHY_CNTL5, 0x0000080b);
		hd_write_reg(P_HHI_HDMI_PHY_CNTL0, 0x37eb65c4);
		hd_write_reg(P_HHI_HDMI_PHY_CNTL3, 0x2ab0ff3b);
		break;
	case HDMI_PHYPARA_3G: /* 2.97Gbps */
		hd_write_reg(P_HHI_HDMI_PHY_CNTL5, 0x00000003);
		hd_write_reg(P_HHI_HDMI_PHY_CNTL0, 0x33eb42a2);
		hd_write_reg(P_HHI_HDMI_PHY_CNTL3, 0x2ab0ff3b);
		break;
	case HDMI_PHYPARA_270M: /* SD format, 480p/576p, 270Mbps */
	case HDMI_PHYPARA_DEF: /* less than 2.97G */
	default:
		hd_write_reg(P_HHI_HDMI_PHY_CNTL5, 0x00000003);
		hd_write_reg(P_HHI_HDMI_PHY_CNTL0, 0x33eb4252);
		hd_write_reg(P_HHI_HDMI_PHY_CNTL3, 0x2ab0ff3b);
		break;
	}
}

