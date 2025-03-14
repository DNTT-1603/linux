// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include "mmc5983.h"

static const struct regmap_config mmc5983_regmap_config = {
    .reg_bits = 8,
    .val_bits = 8,
};

static int mmc5983_i2c_probe(struct i2c_client *client)
{
    struct regmap *regmap;

    regmap = devm_regmap_init_i2c(client, &mmc5983_regmap_config);
    if (IS_ERR(regmap))
        return PTR_ERR(regmap);

    return mmc5983_common_probe(&client->dev, regmap, client->name);
}
static int mmc5983_i2c_remove(struct i2c_client *client)
{
	return mmc5983_common_remove(&client->dev);
}

static const struct of_device_id mmc5983_of_match[] = {
    { .compatible = "memsic,mmc5983" },
    { },
};

MODULE_DEVICE_TABLE(of, mmc5983_of_match);

static struct i2c_driver mmc5983_driver = {
    .driver = {
        .name = "mmc5983",
        .of_match_table = mmc5983_of_match,
    },
    //TODO: add power management
    .probe_new = mmc5983_i2c_probe,
    .remove = mmc5983_i2c_remove,
};

module_i2c_driver(mmc5983_driver);

MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("MMC5983 IIO I2C driver");
MODULE_LICENSE("GPL");
