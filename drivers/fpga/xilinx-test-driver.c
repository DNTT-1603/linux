// SPDX-License-Identifier: GPL-2.0
/*
 * xilinx-test-driver.c
 *
 * A minimal, fpga-mgr-free test helper that programs a Xilinx FPGA
 * using raw GPIOs (PROGRAM_B, INIT_B, DONE) and the SPI core directly,
 * inspired by SPRD_OTF_V0.1_250922_SPark.ino.
 *
 * - Looks up the SPI device by DT node path or compatible
 *   (default: "xlnx,fpga-slave-serial").
 * - Toggles PROGRAM_B low->high, waits for INIT_B high.
 * - Streams /lib/firmware/design.bin over SPI in chunks.
 * - Sends extra CCLK pulses (dummy 0xFF bytes).
 * - Checks DONE and reports status.
 *
 * Exposes a simple sysfs interface:
 *   echo start > /sys/class/fpga_test/loader0/firmware
 * which triggers loading /lib/firmware/design.bin
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

#define DRV_NAME            "xlnx-spi-test"
#define DEFAULT_FW_NAME     "xilinx-fpga-fw.bin"

struct xlnx_test {
    struct spi_device *spi;
    struct gpio_desc *prog_b; /* PROGRAM_B (active low) */
    struct gpio_desc *init_b; /* INIT_B     (active high) */
    struct gpio_desc *done;   /* DONE       (active high) */

    /* sysfs */
    struct class *class;
    struct device *dev;
    struct mutex lock; /* serialize programming */
    int last_status;
    size_t last_bytes;
};

static struct xlnx_test g;

/* Parameters to locate the SPI device and control transfer */
static char node_path[256];
module_param_string(node_path, node_path, sizeof(node_path), 0444);
MODULE_PARM_DESC(node_path, "OF full path of SPI child node (e.g. /soc/spi@.../fpga@1)");

static char compat[64] = "xlnx,fpga-slave-serial";
module_param_string(compat, compat, sizeof(compat), 0444);
MODULE_PARM_DESC(compat, "compatible to search if node_path empty (default: xlnx,fpga-slave-serial)");

static char firmware_name[128] = DEFAULT_FW_NAME;
module_param_string(firmware_name, firmware_name, sizeof(firmware_name), 0644);
MODULE_PARM_DESC(firmware_name, "Firmware file under /lib/firmware to stream over SPI");

static unsigned int spi_hz = 10000000; /* 10 MHz default */
module_param(spi_hz, uint, 0644);
MODULE_PARM_DESC(spi_hz, "SPI clock frequency in Hz (default 10MHz)");

static unsigned int init_timeout_ms = 1000; /* wait for INIT_B high */
module_param(init_timeout_ms, uint, 0644);
MODULE_PARM_DESC(init_timeout_ms, "Timeout waiting for INIT_B high (ms)");

static unsigned int done_timeout_ms = 2000; /* overall DONE check window */
module_param(done_timeout_ms, uint, 0644);
MODULE_PARM_DESC(done_timeout_ms, "Timeout window to see DONE go high (ms)");

static unsigned int extra_cclk_bytes = 4; /* 32 extra CCLKs (4 bytes of 0xFF) */
module_param(extra_cclk_bytes, uint, 0644);
MODULE_PARM_DESC(extra_cclk_bytes, "Number of 0xFF bytes to clock after payload");

static struct spi_device *xlnx_find_spi(void)
{
    struct device_node *np;
    struct spi_device *spi;

    if (node_path[0])
        np = of_find_node_by_path(node_path);
    else
        np = of_find_compatible_node(NULL, NULL, compat);

    if (!np)
        return ERR_PTR(-ENODEV);

    spi = of_find_spi_device_by_node(np);
    of_node_put(np);

    if (!spi)
        return ERR_PTR(-EPROBE_DEFER);

    return spi; /* holds a device ref; caller must put_device(&spi->dev) */
}

static int xlnx_get_gpios(struct xlnx_test *t)
{
    struct device *dev = &t->spi->dev;

    /* Use raw values to avoid surprises with GPIO_ACTIVE_LOW/ACTIVE_HIGH */
    t->prog_b = gpiod_get(dev, "prog_b", GPIOD_OUT_HIGH);
    if (IS_ERR(t->prog_b))
        return dev_err_probe(dev, PTR_ERR(t->prog_b), "prog_b gpio\n");

    t->init_b = gpiod_get_optional(dev, "init-b", GPIOD_IN);
    if (IS_ERR(t->init_b))
        return dev_err_probe(dev, PTR_ERR(t->init_b), "init-b gpio\n");
    
    t->done = gpiod_get(dev, "done", GPIOD_IN);
    if (IS_ERR(t->done))
        return dev_err_probe(dev, PTR_ERR(t->done), "done gpio\n");
    dev_info(dev, "[DEBUG] (Prog_B: %d), (Init_B: %d), (DONE pin: %d)\n", desc_to_gpio(t->prog_b), \
            desc_to_gpio(t->init_b),\
            desc_to_gpio(t->done));
    return 0;
}

static void xlnx_put_gpios(struct xlnx_test *t)
{
    if (!IS_ERR_OR_NULL(t->done))
        gpiod_put(t->done);
    if (!IS_ERR_OR_NULL(t->init_b))
        gpiod_put(t->init_b);
    if (!IS_ERR_OR_NULL(t->prog_b))
        gpiod_put(t->prog_b);
}

static int xlnx_wait_gpio_high(struct gpio_desc *gpio, unsigned int timeout_ms)
{
    unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);

    if (!gpio)
        return 0; /* treat missing as success */

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

static int xlnx_spi_apply_cclk(struct xlnx_test *t)
{
    u8 ff[32];
    size_t n = min_t(unsigned int, extra_cclk_bytes, sizeof(ff));

    if (!n)
        return 0;

    memset(ff, 0xff, n);
    // dev_info(&t->spi->dev, "Sending %zu extra CCLK cycles\n", n * 8);
    return spi_write(t->spi, ff, n);
}

static int xlnx_program_fpga(struct xlnx_test *t, const char *fw_name)
{
    const struct firmware *fw = NULL;
    size_t off = 0;
    int ret;

    if (!t->spi)
        return -ENODEV;

    /* Configure SPI: MODE0, 8 bits, target freq */
    t->spi->mode = SPI_MODE_0;
    t->spi->bits_per_word = 8;
    if (spi_hz)
        t->spi->max_speed_hz = spi_hz;
    ret = spi_setup(t->spi);
    if (ret)
        return ret;

    /* PROGRAM_B: Low -> small delay -> High */
    gpiod_set_raw_value_cansleep(t->prog_b, 0);
    dev_info(&t->spi->dev, "[DEBUG] [ Start Init Write] (Prog_B: %d), (Init_B: %d), (DONE pin: %d)\n", gpiod_get_raw_value_cansleep(t->prog_b), \
            gpiod_get_raw_value_cansleep(t->init_b),\
            gpiod_get_raw_value_cansleep(t->done));

    /* Xilinx spec requires min 500 ns; sleep 2ms for safety */
    msleep(2);
    gpiod_set_raw_value_cansleep(t->prog_b, 1);

    /* Wait INIT_B = High (ready) */
    ret = xlnx_wait_gpio_high(t->init_b, init_timeout_ms);
    if (ret) {
        dev_err(&t->spi->dev, "INIT_B did not go high (%d)\n", ret);
        return ret;
    }
    dev_info(&t->spi->dev, "[DEBUG] [ Finish Init Write] (Prog_B: %d), (Init_B: %d), (DONE pin: %d)\n", gpiod_get_raw_value_cansleep(t->prog_b), \
            gpiod_get_raw_value_cansleep(t->init_b),\
            gpiod_get_raw_value_cansleep(t->done));
    dev_info(&t->spi->dev, "[DEBUG] starting SPI transfer\n");

    /* Fetch firmware from /lib/firmware */
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
        if (!(off & ((128 * 1024) - 1))) /* log every 64KiB */
            dev_info(&t->spi->dev, ".. %zu/%zu\n", off, fw->size);
    }

    /* Extra CCLK pulses */
    ret = xlnx_spi_apply_cclk(t);
    if (ret) {
        dev_err(&t->spi->dev, "extra CCLK failed: %d\n", ret);
        goto out_fw;
    }

    /* Wait brief and check DONE */
    msleep(min(done_timeout_ms, 100u));
    ret = gpiod_get_raw_value_cansleep(t->done);
    if (ret < 0)
        goto out_fw;
    if (!ret) {
        /* Keep clocking within timeout window while checking DONE */
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
        dev_err(&t->spi->dev, "[DEBUG] Fail: DONE low after transfer (INIT_B=%d)\n", initv);
        dev_err(&t->spi->dev, "[DEBUG] (Prog_B: %d), (Init_B: %d), (DONE pin: %d)\n", \
                gpiod_get_raw_value_cansleep(t->prog_b), \
                gpiod_get_raw_value_cansleep(t->init_b),\
                gpiod_get_raw_value_cansleep(t->done));
        ret = -ETIMEDOUT;
        goto out_fw;
    }

    dev_info(&t->spi->dev, "[DEBUG] DONE is high: configuration SUCCESS\n");
    dev_info(&t->spi->dev, "[DEBUG] [ Finish Configuration] (Prog_B: %d), (Init_B: %d), (DONE pin: %d)\n", \
            gpiod_get_raw_value_cansleep(t->prog_b), \
            gpiod_get_raw_value_cansleep(t->init_b),\
            gpiod_get_raw_value_cansleep(t->done));

    g.last_bytes = fw->size;
    ret = 0;

out_fw:
    release_firmware(fw);
    return ret;
}

/* sysfs: echo start > firmware */
static ssize_t firmware_store(struct device *dev, struct device_attribute *attr,
                              const char *buf, size_t count)
{
    char *kbuf;
    int ret;

    kbuf = kstrndup(buf, count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
    strim(kbuf);

    if (!sysfs_streq(kbuf, "start")) {
        kfree(kbuf);
        return -EINVAL;
    }
    kfree(kbuf);

    mutex_lock(&g.lock);
    ret = xlnx_program_fpga(&g, firmware_name);
    g.last_status = ret;
    mutex_unlock(&g.lock);

    return ret ? ret : count;
}
static DEVICE_ATTR_WO(firmware);

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
                           char *buf)
{
    int done = -1, initv = -1;
    if (g.done)
        done = gpiod_get_raw_value_cansleep(g.done);
    if (g.init_b)
        initv = gpiod_get_raw_value_cansleep(g.init_b);
    return scnprintf(buf, PAGE_SIZE,
                     "last-status: %d\nlast-bytes: %zu\ndone: %d\ninit_b: %d\n",
                     g.last_status, g.last_bytes, done, initv);
}
static DEVICE_ATTR_RO(status);

static struct attribute *test_attrs[] = {
    &dev_attr_firmware.attr,
    &dev_attr_status.attr,
    NULL,
};
ATTRIBUTE_GROUPS(test);

static int __init xlnx_test_init(void)
{
    int ret;

    memset(&g, 0, sizeof(g));
    mutex_init(&g.lock);
    g.last_status = -EAGAIN;

    g.spi = xlnx_find_spi();
    if (IS_ERR(g.spi))
        return PTR_ERR(g.spi);

    /* Acquire GPIOs from the SPI child's DT node */
    ret = xlnx_get_gpios(&g);
    if (ret)
        goto err_put_spi;

    /* Prepare a class + device for sysfs knob */
    g.class = class_create(THIS_MODULE, "fpga_test");
    if (IS_ERR(g.class)) {
        ret = PTR_ERR(g.class);
        g.class = NULL;
        goto err_put_gpios;
    }

    g.dev = device_create_with_groups(g.class, NULL, MKDEV(0, 0), NULL,
                                      test_groups, "loader0");
    if (IS_ERR(g.dev)) {
        ret = PTR_ERR(g.dev);
        g.dev = NULL;
        goto err_destroy_class;
    }

    dev_info(g.dev, "ready: echo start > %s/firmware to load %s\n",
             dev_name(g.dev), firmware_name);
    return 0;

err_destroy_class:
    class_destroy(g.class);
err_put_gpios:
    xlnx_put_gpios(&g);
err_put_spi:
    put_device(&g.spi->dev);
    g.spi = NULL;
    return ret;
}

static void __exit xlnx_test_exit(void)
{
    if (g.dev)
        device_unregister(g.dev);
    if (g.class)
        class_destroy(g.class);
    xlnx_put_gpios(&g);
    if (g.spi) {
        put_device(&g.spi->dev);
        g.spi = NULL;
    }
}

module_init(xlnx_test_init);
module_exit(xlnx_test_exit);

MODULE_DESCRIPTION("Bare-metal SPI/GPIO test loader for Xilinx FPGAs");
MODULE_AUTHOR("TuDo <thanhtu.do@nucaremed.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
