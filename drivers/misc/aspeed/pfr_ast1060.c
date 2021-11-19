/*
 * Copyright (c) 2021 ASPEED
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <device.h>
#include <kernel.h>
#include <soc.h>
#include <errno.h>
#include <string.h>
#include <logging/log.h>
#include <drivers/clock_control.h>
#include <drivers/gpio.h>
#include <drivers/misc/aspeed/pfr_aspeed.h>

#define PFR_SCU_CTRL_REG	0x7e6e20f0

void pfr_bmc_rst_enable_ctrl(bool enable)
{
	const struct device *gpio_dev = NULL;

	/* GPIOM5 */
	gpio_dev = device_get_binding("GPIO0_M_P");
	if (gpio_dev == NULL) {
		printk("[%d]Fail to get GPIO0_M_P", __LINE__);
		return;
	}
	gpio_pin_configure(gpio_dev, 5, GPIO_OUTPUT);
	k_busy_wait(10000); /* 10ms */

	if (enable)
		gpio_pin_set(gpio_dev, 5, 0);
	else
		gpio_pin_set(gpio_dev, 5, 1);

	k_busy_wait(50000); /* 50ms */
}

void pfr_bmc_rst_flash(uint32_t flash_idx)
{
	uint32_t val;
	uint32_t bit_off = 1 << (flash_idx - 1);
	const struct device *spim_common_dev = NULL;

#ifdef CONFIG_SPI_MONITOR_ASPEED
	spim_common_dev = device_get_binding("spi_m_common");
	if (!spim_common_dev) {
		printk("spi_m_common device cannot be found!\n");
		return;
	}
	/* Using SCU0F0 to enable flash rst
	 * SCU0F0[23:20]: Reset source selection
	 * SCU0F0[27:24]: Enable reset signal output
	 */
	val = (bit_off << 20) | (bit_off << 24);
	spim_scu_ctrl_set(spim_common_dev, val, val);

	/* SCU0F0[19:16]: output value */
	/* reset flash */
	val = bit_off << 16;
	spim_scu_ctrl_clear(spim_common_dev, val);

	k_busy_wait(500000); /* 500ms */

	/* release reset */
	spim_scu_ctrl_set(spim_common_dev, val, val);

	k_busy_wait(50000); /* 50ms */
#endif
}
