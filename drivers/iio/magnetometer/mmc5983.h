#ifndef _MMC5983_H_
#define _MMC5983_H_

#include <linux/iio/iio.h>
#include <linux/regmap.h>

#define MMC5983_REG_DATA             0x00
#define MMC5983_REG_STATUS           0x08
#define MMC5983_REG_CTRL0            0x09
#define MMC5983_REG_CTRL1            0x0A
#define MMC5983_REG_PRODUCTID        0x2F
#define MMC5983_CHIP_ID              0x30

#define MMC5983_MEAS_DONE_BIT        BIT(0)
#define MMC5983_MEASURE_CMD          0x01
#define MMC5983_RESET_CMD            0x10

struct mmc5983_data {
    struct device *dev;
    struct regmap *regmap;
    struct mutex lock;  // Protects sensor registers
    struct iio_mount_matrix orientation;
    //
    //TODO: Cached data, if necessary

};

int mmc5983_common_probe(struct device *dev, struct regmap *regmap, const char *name);
int mmc5983_common_remove(struct device *dev);

#endif // _MMC5983_H_
