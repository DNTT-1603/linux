// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/delay.h>
#include <linux/unaligned.h>

#include "mmc5983.h"

static const struct iio_mount_matrix *
mmc5983_get_mount_matrix(const struct iio_dev *indio_dev,
                         const struct iio_chan_spec *chan)
{
    struct mmc5983_data *data = iio_priv(indio_dev);
    return &data->orientation;
}

static const struct iio_chan_spec_ext_info mmc5983_ext_info[] = {
    IIO_MOUNT_MATRIX(IIO_SHARED_BY_DIR, mmc5983_get_mount_matrix),
	{ }
};

static const struct iio_chan_spec mmc5983_channels[] = {
    {
        .type = IIO_MAGN,
        .modified = 1,
        .channel2 = IIO_MOD_X,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
        .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
                                    BIT(IIO_CHAN_INFO_OFFSET),
        .scan_index = 0,
        .scan_type = {
            .sign = 'u', .realbits = 16, .storagebits = 16, .endianness = IIO_BE,
        },
        .ext_info = mmc5983_ext_info,
    },
    {
        .type = IIO_MAGN,
        .modified = 1,
        .channel2 = IIO_MOD_Y,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
        .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
                                    BIT(IIO_CHAN_INFO_OFFSET),
        .scan_index = 1,
        .scan_type = {
            .sign = 'u', .realbits = 16, .storagebits = 16, .endianness = IIO_BE,
        },
        .ext_info = mmc5983_ext_info,
    },
    {
        .type = IIO_MAGN,
        .modified = 1,
        .channel2 = IIO_MOD_Z,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
        .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
                                    BIT(IIO_CHAN_INFO_OFFSET),
        .scan_index = 2,
        .scan_type = {
            .sign = 'u', .realbits = 16, .storagebits = 16, .endianness = IIO_BE,
        },
        .ext_info = mmc5983_ext_info,
    },
    {
        .type = IIO_TEMP,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
                              BIT(IIO_CHAN_INFO_SCALE),
    },
    IIO_CHAN_SOFT_TIMESTAMP(3),
};

static int mmc5983_wait_for_data_ready(struct mmc5983_data *data, bool temp)
{
	int tries = 50;
	unsigned int val;
	int ret;
	unsigned int mask = temp ? MMC5983_STATUS_MEAS_T_DONE : MMC5983_STATUS_MEAS_M_DONE;

	while (tries-- > 0) {
		ret = regmap_read(data->regmap, MMC5983_REG_STATUS, &val);
		if (ret < 0)
			return ret;
		if (val & mask)
			break;
		usleep_range(1000, 2000);
	}

	if (tries < 0) {
		dev_err(data->dev, "data not ready\n");
		return -EIO;
	}

	return 0;
}

static int mmc5983_read_measurement(struct mmc5983_data *data, int idx, int *val)
{
    u8 values[7];
    int ret;

    mutex_lock(&data->lock);
    ret = regmap_write(data->regmap, MMC5983_REG_CTRL0, MMC5983_CTRL0_TM_M);
    if (ret)
        goto unlock;

    ret = mmc5983_wait_for_data_ready(data, false);
    if (ret < 0) {
        goto unlock;
    }

    ret = regmap_bulk_read(data->regmap, MMC5983_REG_DATA, values, sizeof(values));
    if (ret)
        goto unlock;

    *val = (int)get_unaligned_be16(&values[idx * 2]);

    ret = IIO_VAL_INT;

unlock:
    mutex_unlock(&data->lock);
    return ret;
}

static int mmc5983_read_temperature(struct mmc5983_data *data, int *val)
{
    unsigned int raw;
    int ret;

    mutex_lock(&data->lock);
    ret = regmap_write(data->regmap, MMC5983_REG_CTRL0, MMC5983_CTRL0_TM_T);
    if (ret)
        goto unlock;

    ret = mmc5983_wait_for_data_ready(data, true);
    if (ret < 0)
        goto unlock;

    ret = regmap_read(data->regmap, MMC5983_REG_TOUT, &raw);
    if (ret)
        goto unlock;

    /*
     * Temperature range is -75 to 125 C, 8-bit unsigned.
     * 0x00 is -75 C, resolution ~0.8 C/LSB.
     * T = 0.8 * raw - 75
     */
    *val = (int)raw * 800 - 75000;
    ret = IIO_VAL_INT;

unlock:
    mutex_unlock(&data->lock);
    return ret;
}

static int mmc5983_read_raw(struct iio_dev *indio_dev,
                            struct iio_chan_spec const *chan,
                            int *val, int *val2, long mask)
{
    struct mmc5983_data *data = iio_priv(indio_dev);

    switch (mask) {
    case IIO_CHAN_INFO_RAW:
        if (chan->type == IIO_MAGN)
            return mmc5983_read_measurement(data, chan->scan_index, val);
        else if (chan->type == IIO_TEMP)
            return mmc5983_read_temperature(data, val);
        return -EINVAL;
    case IIO_CHAN_INFO_SCALE:
        if (chan->type == IIO_MAGN) {
            *val = 1;
            *val2 = 4096;
            return IIO_VAL_FRACTIONAL;
        } else if (chan->type == IIO_TEMP) {
            *val = 1;
            *val2 = 1000;
            return IIO_VAL_FRACTIONAL;
        }
        return -EINVAL;
    case IIO_CHAN_INFO_OFFSET:
        if (chan->type == IIO_MAGN) {
            *val = -32768;
            return IIO_VAL_INT;
        }
        return -EINVAL;
    default:
        return -EINVAL;
    }
}

static const struct iio_info mmc5983_info = {
    .read_raw = &mmc5983_read_raw,
};

int mmc5983_common_probe(struct device *dev, struct regmap *regmap, const char *name)
{
    static const struct iio_mount_matrix identity = {
        .rotation = { "1", "0", "0", "0", "1", "0", "0", "0", "1" }
    };
    struct iio_dev *indio_dev;
    struct mmc5983_data *data;
    unsigned int chip_id;
    int ret;

    ret = regmap_read(regmap, MMC5983_REG_PRODUCTID, &chip_id);
    if (ret)
        return ret;

    if (chip_id != MMC5983_CHIP_ID) {
        dev_err(dev, "Invalid chip ID: 0x%02x\n", chip_id);
        return -ENODEV;
    }

    indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
    if (!indio_dev)
        return -ENOMEM;

    dev_set_drvdata(dev, indio_dev);
    data = iio_priv(indio_dev);
    data->dev = dev;
    data->regmap = regmap;
    mutex_init(&data->lock);

    /* Software reset */
    ret = regmap_write(regmap, MMC5983_REG_CTRL1, MMC5983_CTRL1_SW_RST);
    if (ret)
        return ret;

    /* Mandatory 10ms wait for power-on */
    msleep(10);

    /* Enable Automatic Set/Reset */
    ret = regmap_write(regmap, MMC5983_REG_CTRL0, MMC5983_CTRL0_AUTO_SR_EN);
    if (ret)
        return ret;

    ret = iio_read_mount_matrix(dev, &data->orientation);
    if (ret) {
        dev_dbg(dev, "Failed to read mount matrix: %d, using default identity\n", ret);
        data->orientation = identity;
    }
    indio_dev->name = name;
    indio_dev->info = &mmc5983_info;
    indio_dev->modes = INDIO_DIRECT_MODE;
    indio_dev->channels = mmc5983_channels;
    indio_dev->num_channels = ARRAY_SIZE(mmc5983_channels);

    return devm_iio_device_register(dev, indio_dev);
}

EXPORT_SYMBOL(mmc5983_common_probe);

int mmc5983_common_remove(struct device *dev)
{
    struct iio_dev *indio_dev = dev_get_drvdata(dev);

    iio_device_unregister(indio_dev);
    //TODO: set sleep mode?

    return 0;
}
EXPORT_SYMBOL(mmc5983_common_remove);

MODULE_AUTHOR("Tu Do <dongocthanhtuwork@gmail.com>");
MODULE_DESCRIPTION("MMC5983 IIO Core Driver");
MODULE_LICENSE("GPL");
