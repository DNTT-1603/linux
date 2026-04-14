#ifndef _MMC5983_H_
#define _MMC5983_H_

#include <linux/iio/iio.h>
#include <linux/regmap.h>

#define MMC5983_REG_DATA             0x00
#define MMC5983_REG_TOUT             0x07
#define MMC5983_REG_STATUS           0x08
#define MMC5983_REG_CTRL0            0x09
#define MMC5983_REG_CTRL1            0x0A
#define MMC5983_REG_CTRL2            0x0B
#define MMC5983_REG_PRODUCTID        0x2F
#define MMC5983_CHIP_ID              0x30

/* CTRL0 bits */
#define MMC5983_CTRL0_TM_M           BIT(0)
#define MMC5983_CTRL0_TM_T           BIT(1)
#define MMC5983_CTRL0_INT_M_DONE_EN  BIT(2)
#define MMC5983_CTRL0_SET            BIT(3)
#define MMC5983_CTRL0_RESET          BIT(4)
#define MMC5983_CTRL0_AUTO_SR_EN     BIT(5)

/* CTRL1 bits */
#define MMC5983_CTRL1_SW_RST         BIT(7)

/* STATUS bits */
#define MMC5983_STATUS_MEAS_M_DONE   BIT(0)
#define MMC5983_STATUS_MEAS_T_DONE   BIT(1)

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
