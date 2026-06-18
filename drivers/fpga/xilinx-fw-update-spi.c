// SPDX-License-Identifier: GPL-2.0
/*
 * xilinx-fw-update-spi.c - bare-metal Xilinx slave-serial bitstream loader.
 *
 * Test/bring-up helper: binds to an "xlnx,fpga-slave-serial" SPI node, then
 * streams a bitstream over the SPI core while toggling PROGRAM_B / INIT_B /
 * DONE GPIOs directly (no FPGA manager). Triggered from sysfs:
 *
 *   echo <fw-name> > /sys/bus/spi/devices/<spiX.Y>/firmware
 *   echo start     > /sys/bus/spi/devices/<spiX.Y>/firmware   (uses default)
 *
 * For bring-up only - not a production configuration path.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/sizes.h>

#define DRIVER_NAME "xilinx-fw-update-spi"
#define DEFAULT_FW_NAME "xilinx-fpga-fw.bin"

struct xlnx_spi {
	struct spi_device *spi;
	struct gpio_desc *prog_b;
	struct gpio_desc *init_b;
	struct gpio_desc *done;

	struct mutex lock;
	int last_status;
	size_t last_bytes;
};

static char firmware_name[128] = DEFAULT_FW_NAME;
module_param_string(firmware_name, firmware_name, sizeof(firmware_name), 0644);
MODULE_PARM_DESC(firmware_name, "Default firmware under /lib/firmware to stream over SPI");

static unsigned int spi_hz = 50000000; /* 50 MHz default */
module_param(spi_hz, uint, 0644);
MODULE_PARM_DESC(spi_hz, "SPI clock frequency in Hz (default 50MHz)");

static unsigned int init_timeout_ms = 1000; /* wait for INIT_B high */
module_param(init_timeout_ms, uint, 0644);
MODULE_PARM_DESC(init_timeout_ms, "Timeout waiting for INIT_B high (ms)");

static unsigned int done_timeout_ms = 2000; /* overall DONE check window */
module_param(done_timeout_ms, uint, 0644);
MODULE_PARM_DESC(done_timeout_ms, "Timeout window to see DONE go high (ms)");

static unsigned int extra_cclk_bytes = 4; /* 32 extra CCLKs (4 bytes of 0xFF) */
module_param(extra_cclk_bytes, uint, 0644);
MODULE_PARM_DESC(extra_cclk_bytes, "Number of 0xFF bytes to clock after payload");

static int xlnx_wait_gpio_high(struct gpio_desc *gpio, unsigned int timeout_ms)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);

	if (!gpio)
		return 0;

	while (time_before(jiffies, timeout)) {
		int v = gpiod_get_raw_value_cansleep(gpio);

		if (v < 0)
			return v;
		if (v)
			return 0;
		usleep_range(100, 300);
	}
	return -ETIMEDOUT;
}

static int xlnx_spi_apply_cclk(struct xlnx_spi *t)
{
	u8 ff[32];
	size_t n = min_t(unsigned int, extra_cclk_bytes, sizeof(ff));

	if (!n)
		return 0;

	memset(ff, 0xff, n);
	return spi_write(t->spi, ff, n);
}

static int xlnx_program_fpga(struct xlnx_spi *t, const char *fw_name)
{
	const struct firmware *fw = NULL;
	size_t off = 0;
	int ret;

	t->spi->mode = SPI_MODE_0;
	t->spi->bits_per_word = 8;
	if (spi_hz)
		t->spi->max_speed_hz = spi_hz;
	ret = spi_setup(t->spi);
	if (ret) {
		dev_err(&t->spi->dev, "Failed to setup SPI: %d\n", ret);
		return ret;
	}

	/* PROGRAM_B: Low -> small delay -> High (raw, independent of DT flags) */
	gpiod_set_raw_value_cansleep(t->prog_b, 0);
	msleep(2);
	gpiod_set_raw_value_cansleep(t->prog_b, 1);

	/* Wait INIT_B = High (ready) */
	ret = xlnx_wait_gpio_high(t->init_b, init_timeout_ms);
	if (ret) {
		dev_err(&t->spi->dev, "INIT_B did not go high (%d)\n", ret);
		return ret;
	}

	ret = request_firmware(&fw, fw_name, &t->spi->dev);
	if (ret) {
		dev_err(&t->spi->dev, "request_firmware('%s') failed: %d\n", fw_name, ret);
		return ret;
	}

	dev_info(&t->spi->dev, "Streaming %zu bytes over SPI at %u Hz\n",
		 fw->size, t->spi->max_speed_hz);

	/* Stream payload in chunks (4 KiB) */
	while (off < fw->size) {
		size_t stride = min_t(size_t, fw->size - off, SZ_4K);

		ret = spi_write(t->spi, fw->data + off, stride);
		if (ret) {
			dev_err(&t->spi->dev, "spi_write error at %zu: %d\n", off, ret);
			goto out_fw;
		}
		off += stride;
		if (!(off & ((128 * 1024) - 1))) /* log every 128KiB */
			dev_info(&t->spi->dev, ".. %zu/%zu\n", off, fw->size);
	}

	/* Extra CCLK pulses */
	ret = xlnx_spi_apply_cclk(t);
	if (ret) {
		dev_err(&t->spi->dev, "extra CCLK failed: %d\n", ret);
		goto out_fw;
	}

	/* Wait briefly and check DONE */
	msleep(min(done_timeout_ms, 100u));
	ret = gpiod_get_raw_value_cansleep(t->done);
	if (ret < 0)
		goto out_fw;

	if (!ret) {
		/* Keep clocking within the timeout window while checking DONE */
		unsigned long timeout = jiffies + msecs_to_jiffies(done_timeout_ms);

		do {
			xlnx_spi_apply_cclk(t);
			ret = gpiod_get_raw_value_cansleep(t->done);
			if (ret < 0)
				goto out_fw;
			if (ret)
				break;
			usleep_range(200, 400);
		} while (time_before(jiffies, timeout));
	}

	if (!ret) {
		int initv = t->init_b ? gpiod_get_raw_value_cansleep(t->init_b) : 1;

		dev_err(&t->spi->dev, "Fail: DONE low after transfer (INIT_B=%d)\n", initv);
		ret = -ETIMEDOUT;
		goto out_fw;
	}

	dev_info(&t->spi->dev, "DONE is high: configuration SUCCESS\n");
	t->last_bytes = fw->size;
	ret = 0;

out_fw:
	release_firmware(fw);
	return ret;
}

/* sysfs: echo <fw-name> (or "start"/empty for the default) > firmware */
static ssize_t firmware_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct xlnx_spi *t = dev_get_drvdata(dev);
	const char *name;
	char *kbuf;
	int ret;

	kbuf = kstrndup(buf, count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	strim(kbuf);

	/* empty or the literal "start" => use the configured default */
	name = (!kbuf[0] || !strcmp(kbuf, "start")) ? firmware_name : kbuf;

	mutex_lock(&t->lock);
	ret = xlnx_program_fpga(t, name);
	t->last_status = ret;
	mutex_unlock(&t->lock);

	kfree(kbuf);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(firmware);

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct xlnx_spi *t = dev_get_drvdata(dev);
	int done = -1, initv = -1;

	if (t->done)
		done = gpiod_get_raw_value_cansleep(t->done);
	if (t->init_b)
		initv = gpiod_get_raw_value_cansleep(t->init_b);

	return scnprintf(buf, PAGE_SIZE,
			 "last-status: %d\nlast-bytes: %zu\ndone: %d\ninit_b: %d\n",
			 t->last_status, t->last_bytes, done, initv);
}
static DEVICE_ATTR_RO(status);

static struct attribute *xlnx_attrs[] = {
	&dev_attr_firmware.attr,
	&dev_attr_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(xlnx);

static int xlnx_fpga_probe(struct spi_device *spi)
{
	struct xlnx_spi *t;
	int ret;

	t = devm_kzalloc(&spi->dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;

	t->spi = spi;
	mutex_init(&t->lock);
	t->last_status = -EAGAIN;

	/* PROGRAM_B / INIT_B / DONE; values are driven/read raw (see program). */
	t->prog_b = devm_gpiod_get(&spi->dev, "prog_b", GPIOD_OUT_HIGH);
	if (IS_ERR(t->prog_b))
		return dev_err_probe(&spi->dev, PTR_ERR(t->prog_b), "prog_b gpio\n");

	t->init_b = devm_gpiod_get_optional(&spi->dev, "init-b", GPIOD_IN);
	if (IS_ERR(t->init_b))
		return dev_err_probe(&spi->dev, PTR_ERR(t->init_b), "init-b gpio\n");

	t->done = devm_gpiod_get(&spi->dev, "done", GPIOD_IN);
	if (IS_ERR(t->done))
		return dev_err_probe(&spi->dev, PTR_ERR(t->done), "done gpio\n");

	spi_set_drvdata(spi, t);

	ret = devm_device_add_group(&spi->dev, &xlnx_group);
	if (ret)
		return ret;

	dev_info(&spi->dev,
		 "ready: echo <fw>|start > /sys/bus/spi/devices/%s/firmware (default %s)\n",
		 dev_name(&spi->dev), firmware_name);
	return 0;
}

static const struct of_device_id xlnx_fpga_of_match[] = {
	{ .compatible = "xlnx,fpga-slave-serial" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, xlnx_fpga_of_match);

static struct spi_driver xlnx_fpga_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = xlnx_fpga_of_match,
	},
	.probe = xlnx_fpga_probe,
};
module_spi_driver(xlnx_fpga_driver);

MODULE_DESCRIPTION("Bare-metal SPI/GPIO test loader for Xilinx FPGAs");
MODULE_AUTHOR("TuDo <dongocthanhtuwork@gmail.com>");
MODULE_LICENSE("GPL");
