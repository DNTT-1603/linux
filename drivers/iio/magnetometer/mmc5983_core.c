// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/delay.h>

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
        .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
        .scan_index = 0,
        .scan_type = {
            .sign = 's', .realbits = 16, .storagebits = 16, .endianness = IIO_BE,
        },
        .ext_info = mmc5983_ext_info,
    },
    {
        .type = IIO_MAGN,
        .modified = 1,
        .channel2 = IIO_MOD_Y,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
        .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
        .scan_index = 1,
        .scan_type = {
            .sign = 's', .realbits = 16, .storagebits = 16, .endianness = IIO_BE,
        },
        .ext_info = mmc5983_ext_info,
    },
    {
        .type = IIO_MAGN,
        .modified = 1,
        .channel2 = IIO_MOD_Z,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
        .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
        .scan_index = 2,
        .scan_type = {
            .sign = 's', .realbits = 16, .storagebits = 16, .endianness = IIO_BE,
        },
        .ext_info = mmc5983_ext_info,
    },
    IIO_CHAN_SOFT_TIMESTAMP(3),
};

static int mmc5983_wait_for_data_ready(struct mmc5983_data *data)
{
	int tries = 150;
	unsigned int val;
	int ret;

	while (tries-- > 0) {
		ret = regmap_read(data->regmap, MMC5983_REG_STATUS, &val);
		if (ret < 0)
			return ret;
		if (val & MMC5983_MEAS_DONE_BIT)
			break;
		msleep(20);
	}

	if (tries < 0) {
		dev_err(data->dev, "data not ready\n");
		return -EIO;
	}

	return 0;
}

static int mmc5983_read_measurement(struct mmc5983_data *data, int idx, int *val)
{
    __be16 values[3];
    int ret;

    mutex_lock(&data->lock);
    ret = regmap_write(data->regmap, MMC5983_REG_CTRL0, MMC5983_MEASURE_CMD);
    if (ret)
        goto unlock;

    ret = mmc5983_wait_for_data_ready(data);
    if (ret < 0) {
        goto unlock;
    }

    ret = regmap_bulk_read(data->regmap, MMC5983_REG_DATA, values, sizeof(values));
    if (ret)
        goto unlock;

    *val = sign_extend32(be16_to_cpu(values[idx]), 15);

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
        return mmc5983_read_measurement(data, chan->scan_index, val);
    case IIO_CHAN_INFO_SCALE:
        //TODO: Implement scale factor
        *val = 0;
        *val2 = 250; // example: unit scale factor
        return IIO_VAL_INT_PLUS_MICRO;
    default:
        return -EINVAL;
    }
}

static const struct iio_info mmc5983_info = {
    .read_raw = &mmc5983_read_raw,
};

int mmc5983_common_probe(struct device *dev, struct regmap *regmap, const char *name)
{
    struct iio_dev *indio_dev;
    struct mmc5983_data *data;
    int ret;

    indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
    if (!indio_dev)
        return -ENOMEM;

    dev_set_drvdata(dev, indio_dev);
    data = iio_priv(indio_dev);
    data->dev = dev;
    data->regmap = regmap;
    mutex_init(&data->lock);

    ret = iio_read_mount_matrix(dev, &data->orientation);
    if (ret) {
        dev_err(dev, "Failed to read mount matrix: %d\n", ret);
        return ret;
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

MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("MMC5983 IIO Core Driver");
MODULE_LICENSE("GPL");
