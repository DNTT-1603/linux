// SPDX-License-Identifier: GPL-2.0
/*
   * Simple helper that programs a Xilinx FPGA over the xilinx-spi FPGA manager.
   * Drop this in drivers/fpga/xilinx_spi_loader.c (or your own module directory).
   */
#include <linux/device.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kdev_t.h>

#define DEFAULT_COMPAT "xlnx,fpga-slave-serial"
#define MAX_FW_NAME 256


struct xspi_loader {
	struct class *class;
    struct device *dev;
    struct mutex lock;
    char last_fw[256];
    int last_status;
};

static struct xspi_loader loader;

static char manager_path[256];
module_param_string(manager_path, manager_path, sizeof(manager_path), 0444);
MODULE_PARM_DESC(manager_path, "OF full path to the xilinx-spi node (optional, e.g. /soc/spi@ff0c0000/slave-serial@0)");

static char manager_compat[64] = DEFAULT_COMPAT;
module_param_string(manager_compat, manager_compat, sizeof(manager_compat),0444);
MODULE_PARM_DESC(manager_compat, "compatible string to search for if manager_path is empty (default: \"xlnx,fpga-slave-serial\")");

static unsigned int config_timeout_us = 100000;
module_param(config_timeout_us, uint, 0644);
MODULE_PARM_DESC(config_timeout_us,"DONE timeout supplied to fpga_mgr_load() (microseconds)");

static struct fpga_manager *xspi_loader_get_mgr(void)
{
	struct device_node *np;
	struct fpga_manager *mgr;

	if (manager_path[0])
		np = of_find_node_by_path(manager_path);
	else
		np = of_find_compatible_node(NULL, NULL, manager_compat);

	if (!np)
		return ERR_PTR(-ENODEV);

	mgr = of_fpga_mgr_get(np);
	of_node_put(np);

	return mgr;
}

static int xspi_loader_program(const char *fw_name)
{
	struct fpga_manager *mgr;
	struct fpga_image_info *info;
	char *dup;
	int ret;

	mgr = xspi_loader_get_mgr();
	if (IS_ERR(mgr))
		return PTR_ERR(mgr);

	info = fpga_image_info_alloc(&mgr->dev);
	if (!info) {
		ret = -ENOMEM;
		goto out_put_mgr;
	}

	dup = kstrdup(fw_name, GFP_KERNEL);
	if (!dup) {
		ret = -ENOMEM;
		goto out_free_info;
	}

	info->firmware_name = dup;
	info->config_complete_timeout_us = config_timeout_us;
	info->flags &=
		~FPGA_MGR_PARTIAL_RECONFIG; /* xilinx-spi only supports full reloads */

	ret = fpga_mgr_lock(mgr);
	if (ret)
		goto out_free_info;

	ret = fpga_mgr_load(mgr, info);
	fpga_mgr_unlock(mgr);

out_free_info:
	fpga_image_info_free(info);
out_put_mgr:
	fpga_mgr_put(mgr);
	return ret;
}

static ssize_t firmware_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	char *kbuf;
	int ret;

	if (count >= MAX_FW_NAME)
		return -EINVAL;

	kbuf = kstrndup(buf, count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	strim(kbuf);
	if (!kbuf[0]) {
		kfree(kbuf);
		return -EINVAL;
	}

	mutex_lock(&loader.lock);
	ret = xspi_loader_program(kbuf);
	if (!ret)
		strscpy(loader.last_fw, kbuf, sizeof(loader.last_fw));
	loader.last_status = ret;
	mutex_unlock(&loader.lock);

	kfree(kbuf);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(firmware);

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	ssize_t len;

	mutex_lock(&loader.lock);
	len = scnprintf(buf, PAGE_SIZE, "last-firmware: %s\nlast-status: %d\n",
			loader.last_fw[0] ? loader.last_fw : "<none>",
			loader.last_status);
	mutex_unlock(&loader.lock);

	return len;
}
static DEVICE_ATTR_RO(status);

static struct attribute *loader_attrs[] = {
	&dev_attr_firmware.attr,
	&dev_attr_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(loader);

static int __init xspi_loader_init(void)
{
	int ret;

	mutex_init(&loader.lock);
	loader.last_status = -EAGAIN;

	loader.class = class_create(THIS_MODULE, "fpga_loader");
	if (IS_ERR(loader.class))
		return PTR_ERR(loader.class);

	loader.dev = device_create_with_groups(loader.class, NULL, MKDEV(0, 0),
					       NULL, loader_groups, "loader0");
	if (IS_ERR(loader.dev)) {
		ret = PTR_ERR(loader.dev);
		class_destroy(loader.class);
		return ret;
	}

	dev_info(loader.dev,"ready write a firmware name to firmware to configure the FPGA\n");
	return 0;
}

static void __exit xspi_loader_exit(void)
{
	device_unregister(loader.dev);
	class_destroy(loader.class);
}

module_init(xspi_loader_init);
module_exit(xspi_loader_exit);

MODULE_DESCRIPTION("Firmware loader for xilinx-spi FPGA manager");
MODULE_AUTHOR("TuDo<thanhtu.do@nucaremed.com>");
MODULE_LICENSE("GPL");
