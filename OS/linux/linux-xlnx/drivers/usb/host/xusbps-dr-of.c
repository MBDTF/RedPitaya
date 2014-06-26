/*
 * Xilinx PS USB Driver for device tree support.
 *
 * Copyright (C) 2011 Xilinx, Inc.
 *
 * This file is based on fsl-mph-dr-of.c file with few minor modifications
 * to support Xilinx PS USB controller.
 *
 * Setup platform devices needed by the dual-role USB controller modules
 * based on the description in flat device tree.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/xilinx_devices.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/clk.h>
#include <linux/usb/ulpi.h>

#include "ehci-xilinx-usbps.h"

static u64 dma_mask = 0xFFFFFFF0;

struct xusbps_dev_data {
	char *dr_mode;		/* controller mode */
	char *drivers[3];	/* drivers to instantiate for this mode */
	enum xusbps_usb2_operating_modes op_mode;	/* operating mode */
};

struct xusbps_host_data {
	struct clk *clk;
};

static struct xusbps_dev_data dr_mode_data[] = {
	{
		.dr_mode = "host",
		.drivers = { "xusbps-ehci", NULL, NULL, },
		.op_mode = XUSBPS_USB2_DR_HOST,
	},
	{
		.dr_mode = "otg",
		.drivers = { "xusbps-otg", "xusbps-ehci", "xusbps-udc", },
		.op_mode = XUSBPS_USB2_DR_OTG,
	},
	{
		.dr_mode = "peripheral",
		.drivers = { "xusbps-udc", NULL, NULL, },
		.op_mode = XUSBPS_USB2_DR_DEVICE,
	},
};

static struct xusbps_dev_data * get_dr_mode_data(
		struct device_node *np)
{
	const unsigned char *prop;
	int i;

	prop = of_get_property(np, "dr_mode", NULL);
	if (prop) {
		for (i = 0; i < ARRAY_SIZE(dr_mode_data); i++) {
			if (!strcmp(prop, dr_mode_data[i].dr_mode))
				return &dr_mode_data[i];
		}
	}
	pr_warn("%s: Invalid 'dr_mode' property, fallback to host mode\n",
		np->full_name);
	return &dr_mode_data[0]; /* mode not specified, use host */
}

static enum xusbps_usb2_phy_modes determine_usb_phy(const char *phy_type)
{
	if (!phy_type)
		return XUSBPS_USB2_PHY_NONE;
	if (!strcasecmp(phy_type, "ulpi"))
		return XUSBPS_USB2_PHY_ULPI;
	if (!strcasecmp(phy_type, "utmi"))
		return XUSBPS_USB2_PHY_UTMI;
	if (!strcasecmp(phy_type, "utmi_wide"))
		return XUSBPS_USB2_PHY_UTMI_WIDE;
	if (!strcasecmp(phy_type, "serial"))
		return XUSBPS_USB2_PHY_SERIAL;

	return XUSBPS_USB2_PHY_NONE;
}

static struct platform_device * xusbps_device_register(
					struct platform_device *ofdev,
					struct xusbps_usb2_platform_data *pdata,
					const char *name, int id)
{
	struct platform_device *pdev;
	const struct resource *res = ofdev->resource;
	unsigned int num = ofdev->num_resources;
	struct xusbps_usb2_platform_data *pdata1;
	int retval;

	pdev = platform_device_alloc(name, id);
	if (!pdev) {
		retval = -ENOMEM;
		goto error;
	}

	pdev->dev.parent = &ofdev->dev;

	pdev->dev.coherent_dma_mask = ofdev->dev.coherent_dma_mask;
	pdev->dev.dma_mask = &dma_mask;
	pdev->dev.of_node = ofdev->dev.of_node;

	retval = platform_device_add_data(pdev, pdata, sizeof(*pdata));
	if (retval)
		goto error;

	if (num) {
		retval = platform_device_add_resources(pdev, res, num);
		if (retval)
			goto error;
	}

	retval = platform_device_add(pdev);
	if (retval)
		goto error;

	pdata1 = pdev->dev.platform_data;
	/* Copy the otg transceiver pointer into host/device platform data */
	if (pdata1->otg)
		pdata->otg = pdata1->otg;

	return pdev;

error:
	platform_device_put(pdev);
	return ERR_PTR(retval);
}

static int xusbps_dr_of_probe(struct platform_device *ofdev)
{
	struct device_node *np = ofdev->dev.of_node;
	struct platform_device *usb_dev;
	struct xusbps_usb2_platform_data data, *pdata;
	struct xusbps_dev_data *dev_data;
	struct xusbps_host_data *hdata;
	const unsigned char *prop;
	static unsigned int idx;
	struct resource *res;
	int i, phy_init;
	int ret;

	pdata = &data;
	memset(pdata, 0, sizeof(data));

	res = platform_get_resource(ofdev, IORESOURCE_IRQ, 0);
	if (!res) {
		dev_err(&ofdev->dev,
			"IRQ not found\n");
		return -ENODEV;
	}
	pdata->irq = res->start;

	res = platform_get_resource(ofdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&ofdev->dev,
			"Register base not found");
		return -ENODEV;
	}

	if (!request_mem_region(res->start, res->end - res->start + 1,
						ofdev->name)) {
		dev_err(&ofdev->dev, "Controller already in use\n");
		return -EBUSY;
	}

	pdata->regs = ioremap(res->start, res->end - res->start + 1);
	if (!pdata->regs) {
		dev_err(&ofdev->dev, "unable to iomap registers\n");
		release_mem_region(res->start, resource_size(res));
		return -EFAULT;
	}

	dev_data = get_dr_mode_data(np);
	pdata->operating_mode = dev_data->op_mode;

	prop = of_get_property(np, "phy_type", NULL);
	pdata->phy_mode = determine_usb_phy(prop);

	hdata = kmalloc(sizeof(*hdata), GFP_KERNEL);
	if (!hdata) {
		dev_err(&ofdev->dev, "cannot allocate memory\n");
		return -ENOMEM;
	}
	platform_set_drvdata(ofdev, hdata);

	if (pdata->irq == 53)
		hdata->clk = clk_get_sys("USB0_APER", NULL);
	else
		hdata->clk = clk_get_sys("USB1_APER", NULL);
	if (IS_ERR(hdata->clk)) {
		dev_err(&ofdev->dev, "APER clock not found.\n");
		ret = PTR_ERR(hdata->clk);
		goto err_free;
	}

	ret = clk_prepare_enable(hdata->clk);
	if (ret) {
		dev_err(&ofdev->dev, "Unable to enable APER clock.\n");
		goto err_out_clk_put;
	}

	/* If ULPI phy type, set it up */
	if (pdata->phy_mode == XUSBPS_USB2_PHY_ULPI) {
		pdata->ulpi = otg_ulpi_create(&ulpi_viewport_access_ops,
			ULPI_OTG_DRVVBUS | ULPI_OTG_DRVVBUS_EXT);
		if (pdata->ulpi) {
			pdata->ulpi->io_priv = pdata->regs +
							XUSBPS_SOC_USB_ULPIVP;

			phy_init = usb_phy_init(pdata->ulpi);
			if (phy_init) {
				dev_err(&ofdev->dev,
					"Unable to init USB phy, missing?\n");
				ret = -ENODEV;
				goto err_out_clk_disable;
			}
		} else {
			dev_err(&ofdev->dev,
				"Unable to create ULPI transceiver\n");
		}
	}

	for (i = 0; i < ARRAY_SIZE(dev_data->drivers); i++) {
		if (!dev_data->drivers[i])
			continue;
		usb_dev = xusbps_device_register(ofdev, pdata,
					dev_data->drivers[i], idx);
		if (IS_ERR(usb_dev)) {
			dev_err(&ofdev->dev, "Can't register usb device\n");
			ret = PTR_ERR(usb_dev);
			goto err_out_clk_disable;
		}
	}
	idx++;
	return 0;

err_out_clk_disable:
	clk_disable_unprepare(hdata->clk);
err_out_clk_put:
	clk_put(hdata->clk);
err_free:
	kfree(hdata);

	return ret;
}

static int __unregister_subdev(struct device *dev, void *d)
{
	platform_device_unregister(to_platform_device(dev));
	return 0;
}

static int xusbps_dr_of_remove(struct platform_device *ofdev)
{
	struct resource *res;
	struct xusbps_host_data *hdata = platform_get_drvdata(ofdev);

	res = platform_get_resource(ofdev, IORESOURCE_MEM, 0);
	release_mem_region(res->start, resource_size(res));

	device_for_each_child(&ofdev->dev, NULL, __unregister_subdev);
	clk_disable_unprepare(hdata->clk);
	clk_put(hdata->clk);
	kfree(hdata);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int xusbps_dr_of_suspend(struct device *dev)
{
	struct xusbps_host_data *hdata = dev_get_drvdata(dev);

	clk_disable(hdata->clk);

	return 0;
}

static int xusbps_dr_of_resume(struct device *dev)
{
	struct xusbps_host_data *hdata = dev_get_drvdata(dev);
	int ret;

	ret = clk_enable(hdata->clk);
	if (ret) {
		dev_err(dev, "cannot enable clock. resume failed\n");
		return ret;
	}

	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(xusbps_pm_ops, xusbps_dr_of_suspend,
		xusbps_dr_of_resume);

static const struct of_device_id xusbps_dr_of_match[] = {
	{ .compatible = "xlnx,ps7-usb-1.00.a" },
	{},
};
MODULE_DEVICE_TABLE(of, xusbps_dr_of_match);

static struct platform_driver xusbps_dr_driver = {
	.driver = {
		.name = "xusbps-dr",
		.owner = THIS_MODULE,
		.of_match_table = xusbps_dr_of_match,
		.pm = &xusbps_pm_ops,
	},
	.probe	= xusbps_dr_of_probe,
	.remove	= xusbps_dr_of_remove,
};

module_platform_driver(xusbps_dr_driver);

MODULE_DESCRIPTION("XUSBPS DR OF devices driver");
MODULE_AUTHOR("Xilinx");
MODULE_LICENSE("GPL");
