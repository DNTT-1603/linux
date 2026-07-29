// SPDX-License-Identifier: GPL-2.0+
/*
 * DRM driver for Ilitek ILI9486 panels
 *
 * Copyright 2020 Kamlesh Gurudasani <kamlesh.gurudasani@gmail.com>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>	/* DEBUG hardcode: gpio_device_find_by_fwnode / gpiochip_request_own_desc */
#include <linux/gpio/machine.h>	/* DEBUG hardcode: enum gpio_lookup_flags, GPIO_ACTIVE_LOW/HIGH */
#include <linux/of.h>		/* DEBUG hardcode: of_find_node_by_path / of_fwnode_handle */
#include <linux/module.h>
#include <linux/property.h>
#include <linux/sizes.h>
#include <linux/spi/spi.h>

#include <video/mipi_display.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modeset_helper.h>

#define ILI9486_ITFCTR1         0xb0
#define ILI9486_PWCTRL1         0xc2
#define ILI9486_VMCTRL1         0xc5
#define ILI9486_PGAMCTRL        0xe0
#define ILI9486_NGAMCTRL        0xe1
#define ILI9486_DGAMCTRL        0xe2
#define ILI9486_MADCTL_BGR      BIT(3)
#define ILI9486_MADCTL_MV       BIT(5)
#define ILI9486_MADCTL_MX       BIT(6)
#define ILI9486_MADCTL_MY       BIT(7)

/*
 * The PiScreen/waveshare rpi-lcd-35 has a SPI to 16-bit parallel bus converter
 * in front of the  display controller. This means that 8-bit values have to be
 * transferred as 16-bit.
 */
static int waveshare_command(struct mipi_dbi *mipi, u8 *cmd, u8 *par,
			     size_t num)
{
	struct spi_device *spi = mipi->spi;
	u32 speed_hz;
	int ret;

	spi_bus_lock(spi->controller);

	gpiod_set_value_cansleep(mipi->dc, 0);
	speed_hz = mipi_dbi_spi_cmd_max_speed(spi, 1);
	ret = mipi_dbi_spi_transfer(spi, speed_hz, 8, cmd, 1);
	if (ret || !num)
		goto out_unlock;

	if (*cmd == MIPI_DCS_WRITE_MEMORY_START) {
		/*
		 * RGB565 -> 18-bit (3 bytes/pixel, inverted) conversion for
		 * the panel's parallel bus converter. Convert and transfer in
		 * fixed-size chunks through mipi->tx_buf9 (unused by this
		 * bus type otherwise) instead of kmalloc'ing a buffer sized
		 * for the whole transfer: a full-frame update needs up to
		 * ~450KB, and repeating a contiguous allocation that size on
		 * every redraw eventually fails once physical memory
		 * fragments (order-4..7 kmalloc failures under load).
		 */
		size_t max_chunk_px = mipi->tx_buf9_len / 3;
		u8 *buf = mipi->tx_buf9;

		gpiod_set_value_cansleep(mipi->dc, 1);

		while (num >= 2) {
			size_t chunk_px = min(num / 2, max_chunk_px);
			size_t i;

			for (i = 0; i < chunk_px; i++) {
				u16 color = (par[2 * i] << 8) | par[2 * i + 1];

				buf[3 * i]     = ~((color & 0xF800) >> 8);
				buf[3 * i + 1] = ~((color & 0x07E0) >> 3);
				buf[3 * i + 2] = ~((color & 0x1F) << 3);
			}

			speed_hz = mipi_dbi_spi_cmd_max_speed(spi, chunk_px * 3);
			ret = mipi_dbi_spi_transfer(spi, speed_hz, 8, buf,
						    chunk_px * 3);
			if (ret)
				goto out_unlock;

			par += chunk_px * 2;
			num -= chunk_px * 2;
		}
	} else {
		/*
		 * Configuration parameters are always 8-bit here; the 16-bit
		 * bpw case in the generic helper only applies to pixel data,
		 * which is handled above.
		 */
		gpiod_set_value_cansleep(mipi->dc, 1);
		speed_hz = mipi_dbi_spi_cmd_max_speed(spi, num);
		ret = mipi_dbi_spi_transfer(spi, speed_hz, 8, par, num);
	}

 out_unlock:
	spi_bus_unlock(spi->controller);

	return ret;
}

static void waveshare_enable(struct drm_simple_display_pipe *pipe,
			     struct drm_crtc_state *crtc_state,
			     struct drm_plane_state *plane_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct mipi_dbi *dbi = &dbidev->dbi;
	u8 addr_mode;
	int ret, idx;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	DRM_DEBUG_KMS("\n");

	ret = mipi_dbi_poweron_conditional_reset(dbidev);
	if (ret < 0)
		goto out_exit;
	if (ret == 1)
		goto out_enable;

	mipi_dbi_command(dbi, ILI9486_ITFCTR1);
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(250);

	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, 0x66);

	mipi_dbi_command(dbi, ILI9486_PWCTRL1, 0x44);

	mipi_dbi_command(dbi, ILI9486_VMCTRL1, 0x00, 0x23, 0x00, 0x23);

	// mipi_dbi_command(dbi, ILI9486_PGAMCTRL,
	// 		 0x0F, 0x1F, 0x1C, 0x0C, 0x0F, 0x08, 0x48, 0x98,
	// 		 0x37, 0x0A, 0x13, 0x04, 0x11, 0x0D, 0x0);
	// mipi_dbi_command(dbi, ILI9486_NGAMCTRL,
	// 		 0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
	// 		 0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00);
	// mipi_dbi_command(dbi, ILI9486_DGAMCTRL,
	// 		 0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
	// 		 0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00);
	mipi_dbi_command(dbi, MIPI_DCS_ENTER_NORMAL_MODE);

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	msleep(100);

 out_enable:
	switch (dbidev->rotation) {
	case 90:
		addr_mode = ILI9486_MADCTL_MY;
		break;
	case 180:
		addr_mode = ILI9486_MADCTL_MV;
		break;
	case 270:
		addr_mode = ILI9486_MADCTL_MX;
		break;
	default:
		addr_mode = ILI9486_MADCTL_MV | ILI9486_MADCTL_MY |
			ILI9486_MADCTL_MX;
		break;
	}
	addr_mode |= ILI9486_MADCTL_BGR;
	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);
	mipi_dbi_enable_flush(dbidev, crtc_state, plane_state);
 out_exit:
	drm_dev_exit(idx);
}

static const struct drm_simple_display_pipe_funcs waveshare_pipe_funcs = {
	DRM_MIPI_DBI_SIMPLE_DISPLAY_PIPE_FUNCS(waveshare_enable),
};

static const struct drm_display_mode waveshare_mode = {
	DRM_SIMPLE_MODE(480, 320, 73, 49),
};

DEFINE_DRM_GEM_DMA_FOPS(ili9486_fops);

static const struct drm_driver ili9486_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &ili9486_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "ili9486",
	.desc			= "Ilitek ILI9486",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id ili9486_of_match[] = {
	{ .compatible = "waveshare,rpi-lcd-35" },
	{ .compatible = "ozzmaker,piscreen" },
	{},
};
MODULE_DEVICE_TABLE(of, ili9486_of_match);

static const struct spi_device_id ili9486_id[] = {
	{ "ili9486", 0 },
	{ "rpi-lcd-35", 0 },
	{ "piscreen", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, ili9486_id);

/*
 * DEBUG HARDCODE (Nucare SPRD SAM9X75, temporary)
 * ------------------------------------------------
 * The runtime DT on this board is corrupted (FDT relocation collision),
 * so devm_gpiod_get(dev, "reset"/"dc", ...) fails with -ENOENT even though
 * reset-gpios / dc-gpios are present in the .dts source.
 *
 * This helper bypasses the DT lookup: it finds the pioA gpiochip by its
 * device-tree node path and requests a hardcoded line number on it. The
 * board mapping baked in below is:
 *
 *     reset  =  pioA pin 18  (active-low)   LCD_RESX
 *     dc     =  pioA pin 15  (active-high)  LCD_RS_DCX
 *
 * REVERT THIS once the FDT collision is fixed (set fdt_high=0xffffffff in
 * U-Boot env, or move fdt_addr_r out of the kernel's decompress range).
 */
static struct gpio_desc *ili9486_dbg_request_pioA(struct device *dev,
						  unsigned int line,
						  const char *label,
						  enum gpio_lookup_flags lflags,
						  enum gpiod_flags dflags)
{
	struct device_node *np;
	struct gpio_device *gdev;
	struct gpio_chip *gc;
	struct gpio_desc *desc;

	np = of_find_node_by_path("/apb/pinctrl@fffff400/gpio@fffff400");
	if (!np) {
		dev_err(dev, "DEBUG: pioA node not found\n");
		return ERR_PTR(-ENODEV);
	}

	gdev = gpio_device_find_by_fwnode(of_fwnode_handle(np));
	of_node_put(np);
	if (!gdev) {
		dev_warn(dev, "DEBUG: pioA gpio_device not ready, deferring\n");
		return ERR_PTR(-EPROBE_DEFER);
	}

	gc = gpio_device_get_chip(gdev);
	if (!gc) {
		gpio_device_put(gdev);
		dev_err(dev, "DEBUG: pioA gpio_chip is NULL\n");
		return ERR_PTR(-ENODEV);
	}

	desc = gpiochip_request_own_desc(gc, line, label, lflags, dflags);
	gpio_device_put(gdev);
	if (IS_ERR(desc))
		dev_err(dev, "DEBUG: gpiochip_request_own_desc(pioA[%u]) failed: %ld\n",
			line, PTR_ERR(desc));
	return desc;
}

static int ili9486_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct mipi_dbi_dev *dbidev;
	struct drm_device *drm;
	struct mipi_dbi *dbi;
	struct gpio_desc *dc;
	u32 rotation = 0;
	int ret;

	dbidev = devm_drm_dev_alloc(dev, &ili9486_driver,
				    struct mipi_dbi_dev, drm);
	if (IS_ERR(dbidev))
		return PTR_ERR(dbidev);

	dbi = &dbidev->dbi;
	drm = &dbidev->drm;

	/* === DEBUG HARDCODE BEGIN: bypass DT, force PA18/PA15 === */
	dbi->reset = ili9486_dbg_request_pioA(dev, 18, "lcd-reset",
					      GPIO_ACTIVE_HIGH, GPIOD_OUT_HIGH);
	if (IS_ERR(dbi->reset))
		return dev_err_probe(dev, PTR_ERR(dbi->reset),
				     "DEBUG: hardcoded reset gpio failed\n");

	dc = ili9486_dbg_request_pioA(dev, 15, "lcd-dc",
				      GPIO_ACTIVE_HIGH, GPIOD_OUT_LOW);
	if (IS_ERR(dc))
		return dev_err_probe(dev, PTR_ERR(dc),
				     "DEBUG: hardcoded dc gpio failed\n");
	/* === DEBUG HARDCODE END ===
	 * Original DT-based lookups (restore when FDT is fixed):
	 *   dbi->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	 *   if (IS_ERR(dbi->reset))
	 *           return dev_err_probe(dev, PTR_ERR(dbi->reset),
	 *                                "Failed to get GPIO 'reset'\n");
	 *   dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
	 *   if (IS_ERR(dc))
	 *           return dev_err_probe(dev, PTR_ERR(dc),
	 *                                "Failed to get GPIO 'dc'\n");
	 */

	dbidev->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(dbidev->backlight))
		return PTR_ERR(dbidev->backlight);

	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_spi_init(spi, dbi, dc);
	if (ret)
		return ret;

	/*
	 * dbi->tx_buf9 is unused for Type C Option 3 (dc gpio present), so
	 * mipi_dbi_spi_init() left it NULL. Repurpose it as a fixed-size,
	 * DMA-safe scratch buffer for waveshare_command()'s RGB565 -> 18-bit
	 * conversion, allocated once here rather than kmalloc'd per screen
	 * update. SZ_16K matches what the core uses for its own conversion
	 * buffer and is a low enough order to allocate reliably at probe.
	 */
	dbi->tx_buf9_len = SZ_16K;
	dbi->tx_buf9 = devm_kmalloc(dev, dbi->tx_buf9_len, GFP_KERNEL);
	if (!dbi->tx_buf9)
		return -ENOMEM;

	dbi->command = waveshare_command;
	// dbi->read_commands = NULL;

	ret = mipi_dbi_dev_init(dbidev, &waveshare_pipe_funcs,
				&waveshare_mode, rotation);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	spi_set_drvdata(spi, drm);

	drm_client_setup(drm, NULL);

	return 0;
}

static void ili9486_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void ili9486_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver ili9486_spi_driver = {
	.driver = {
		.name = "ili9486",
		.of_match_table = ili9486_of_match,
	},
	.id_table = ili9486_id,
	.probe = ili9486_probe,
	.remove = ili9486_remove,
	.shutdown = ili9486_shutdown,
};
module_spi_driver(ili9486_spi_driver);

MODULE_DESCRIPTION("Ilitek ILI9486 DRM driver");
MODULE_AUTHOR("Kamlesh Gurudasani <kamlesh.gurudasani@gmail.com>");
MODULE_LICENSE("GPL");
