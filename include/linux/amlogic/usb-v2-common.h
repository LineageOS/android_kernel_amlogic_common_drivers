/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef __USB_V2_COMMON_HEADER_
#define __USB_V2_COMMON_HEADER_

#include <linux/usb/phy.h>
#include <linux/usb/role.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/clk.h>
#include <linux/amlogic/usb-v2.h>

enum aml_usb_phy_mode {
	AML_USB_PHY_MODE_INVALID,
	AML_USB_PHY_MODE_USB_HOST,
	AML_USB_PHY_MODE_USB_DEVICE,
	AML_USB_PHY_MODE_USB_LS,
	AML_USB_PHY_MODE_USB_FS,
	AML_USB_PHY_MODE_USB_HS,
	AML_USB_PHY_MODE_USB_HSP,
	AML_USB_PHY_MODE_USB_SS,
	AML_USB_PHY_MODE_USB_OTG,
};

#define USB_PHY_MAX_NUMBER  0x8
#define AML_USB_PHY_MAX_CLK_NUMBER  0x3

struct amlogic_usb_v2 {
	struct usb_phy		phy;
	struct device		*dev;
	const void			*pdata;
	void __iomem	*regs;
	void __iomem	*reset_regs;
	void __iomem	*phy_cfg[4];
	void __iomem	*phy3_cfg;
	void __iomem	*phy3_cfg_r1;
	void __iomem	*phy3_cfg_r2;
	void __iomem	*phy3_cfg_r4;
	void __iomem	*phy3_cfg_r5;
	void __iomem	*phy31_cfg;
	void __iomem	*phy31_cfg_r1;
	void __iomem	*phy31_cfg_r2;
	void __iomem	*phy31_cfg_r4;
	void __iomem	*phy31_cfg_r5;
	void __iomem	*usb2_phy_cfg;
	void __iomem	*xhci_port_a_addr;
	struct u2p_aml_regs_m_v2 __iomem *u2p_aml_regs[USB_PHY_MAX_NUMBER];
	u32 pll_setting[8];
	u32 pll_ver;
	u32 ic_ver;
	u32 pll_dis_thred_enhance;
	int phy_cfg_state[4];
	int phy_trim_initvalue[8];
	int phy_0xc_initvalue[8];
	int phy_trim_state[4];
	/* Set VBus Power though GPIO */
	int vbus_power_pin;
	int vbus_power_pin_work_mask;
	int otg;
	u32 version;
	int portspeed;
	struct delayed_work	work;
	struct delayed_work	id_gpio_work;
	struct delayed_work	id_override_work;
	struct gpio_desc *usb_gpio_desc;
	struct gpio_desc *idgpiodesc;
	struct gpio_desc *id_override_gpio;
	struct usb_role_switch *role_sw;
	enum usb_role current_role;
	u32 id_override_delay_ms;
	bool id_override_auto;
	bool upstream_connected;

	int portnum;
	int suspend_flag;
	int phy_version;
	u32 phy_reset_level_bit[USB_PHY_MAX_NUMBER];
	u32 phy_reg_reset_level_bit[USB_PHY_MAX_NUMBER];
	u32 usb_reset_bit;
	u32 usb_comb_reset_bit;
	u32 pm_controller:1;
	u32 sw_hsp:1;
	u32 otg_phy_index;
	u32 reset_level;
	struct clk_bulk_data clks[AML_USB_PHY_MAX_CLK_NUMBER];
	int clk_num;
	u32	clk_mux;
	struct clk		*clk;
	struct clk		*usb_clk;
	struct clk		*gate0_clk;
	struct clk		*gate1_clk;
	struct clk		*hcsl_clk;
	struct clk		*pcie_bgp;
	u32 portconfig_31;
	u32 portconfig_30;
	void __iomem	*usb_phy_trim_reg;
	u32 phy_id;
	struct clk		*general_clk;
	u32 usb3_apb_reset_bit;
	u32 usb3_phy_reset_bit;
	u32 usb3_reset_shift;
	void	(*resume_xhci_p_a)(void);
	void	(*disable_port_a)(void);
	void	(*usb2_phy_init)(void);
	int	(*usb2_get_mode)(void);
	void (*phy_trim_tuning)(struct usb_phy *x,
		int port, int default_val);
	void (*set_usb_pll)(struct amlogic_usb_v2 *phy,
				void __iomem *reg);
};

static inline void
usb_phy_trim_tuning(struct usb_phy *x, int port, int default_val)
{
	struct amlogic_usb_v2	*aml_phy;

	if (x) {
		if (x->label)
			if (!strncmp(x->label, "amlogic-usbm31phy3", 18) ||
				!strncmp(x->label, "amlogic-usb2-m31-phy", 20))
				return;
		aml_phy = container_of(x, struct amlogic_usb_v2, phy);

		if (aml_phy->phy_trim_tuning)
			aml_phy->phy_trim_tuning(x, port, default_val);
	}
}

#endif
