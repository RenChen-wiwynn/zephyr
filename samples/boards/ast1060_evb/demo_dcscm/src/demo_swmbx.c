/*
 * Copyright (c) 2022 ASPEED Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <sys/printk.h>
#include <drivers/i2c.h>
#include <drivers/i2c/pfr/swmbx.h>

#define swmbx_thread_size 256
#define SWMBX_DELAY 100
static struct k_thread zswmbx_n_t;
static struct k_thread zswmbx_f_t;
struct k_sem sem_0_0E, sem_0_0F;
struct k_sem sem_1_0E, sem_1_0F;
struct k_sem sem_fifo_0E, sem_fifo_0F;

K_THREAD_STACK_DEFINE(swmbx_thread_n_s, swmbx_thread_size);
K_THREAD_STACK_DEFINE(swmbx_thread_f_s, swmbx_thread_size);

/* dcscm thread demo for swmbx notify */
void dcscm_swmbx_notify(void *a, void *b, void *c)
{
#if CONFIG_PFR_SW_MAILBOX
	printk("dcscm swmbx_notify successful.");

	while (1) {
		if (k_sem_take(&sem_0_0E, K_MSEC(50)) == 0) {
			printk("dcscm : SEM_0_0E is taken!!");
		}

		if (k_sem_take(&sem_0_0F, K_MSEC(50)) == 0) {
			printk("dcscm : SEM_0_0F is taken!!");
		}

		if (k_sem_take(&sem_1_0E, K_MSEC(50)) == 0) {
			printk("dcscm : SEM_1_0E is taken!!");
		}

		if (k_sem_take(&sem_1_0F, K_MSEC(50)) == 0) {
			printk("dcscm : SEM_1_0F is taken!!");
		}

		k_sleep(K_MSEC(SWMBX_DELAY));
	}
#endif
}

/* i2c thread demo for dcscm swmbx fifo */
void dcscm_swmbx_fifo(void *a, void *b, void *c)
{
#if CONFIG_PFR_SW_MAILBOX
	printk("dcscm swmbx: swmbx_fifo successful.");

	while (1) {
		if (k_sem_take(&sem_fifo_0E, K_MSEC(50)) == 0) {
			printk("dcscm : SEM_FIFO_0E is taken!!");
		}

		if (k_sem_take(&sem_fifo_0F, K_MSEC(50)) == 0) {
			printk("dcscm : SEM_FIFO_0F is taken!!");
		}

		k_sleep(K_MSEC(SWMBX_DELAY));
	}
#endif
}

void aspeed_dcscm_swmbx_demo(void)
{
#if CONFIG_PFR_SW_MAILBOX
	const struct device *swmbx_ctrl = NULL;
	const struct device *dev = NULL;
	k_tid_t pidn, pidf;
	int ret;

	/* swmbx ctrl devices binding*/
	swmbx_ctrl = device_get_binding("SWMBX");
	if (!swmbx_ctrl) {
		printk("I2C: SWMBX Controller not be found.");
		return;
	}

	if (swmbx_ctrl) {
		/* apply function enable */
		ret = swmbx_enable_behavior(swmbx_ctrl,
			(SWMBX_NOTIFY | SWMBX_FIFO), true);
		if (ret) {
			printk("I2C: Enable SWMBX function failed.");
			return;
		}

		/* swmbx notify usage */
		k_sem_init(&sem_0_0E, 0, 1);
		k_sem_init(&sem_0_0F, 0, 1);
		k_sem_init(&sem_1_0E, 0, 1);
		k_sem_init(&sem_1_0F, 0, 1);

		ret = swmbx_update_notify(swmbx_ctrl, 0x0, &sem_0_0E,
		0x0, true);
		ret = swmbx_update_notify(swmbx_ctrl, 0x0, &sem_0_0F,
		0x40, true);
		ret = swmbx_update_notify(swmbx_ctrl, 0x1, &sem_1_0E,
		0x31, true);
		ret = swmbx_update_notify(swmbx_ctrl, 0x1, &sem_1_0F,
		0x41, true);

		/* create thread to receive mbx notify */
		if (IS_ENABLED(CONFIG_MULTITHREADING)) {
			pidn = k_thread_create(&zswmbx_n_t,
						swmbx_thread_n_s,
						swmbx_thread_size,
						(k_thread_entry_t) dcscm_swmbx_notify,
						NULL, NULL, NULL,
						-1,
						0, K_NO_WAIT);
		}

		/* assign mbx fifo setting */
		k_sem_init(&sem_fifo_0E, 0, 1);
		k_sem_init(&sem_fifo_0F, 0, 1);

		ret = swmbx_update_fifo(swmbx_ctrl, &sem_fifo_0E,
		0, 0x0e, 0x8, SWMBX_FIFO_NOTIFY_STOP, true);
		if (ret) {
			printk("I2C: Apply fifo 0e fail.");
			return;
		}

		ret = swmbx_update_fifo(swmbx_ctrl, &sem_fifo_0F,
		1, 0x0f, 0x8, SWMBX_FIFO_NOTIFY_STOP, true);
		if (ret) {
			printk("I2C: Apply fifo 0f fail.");
			return;
		}

		/* create thread to receive mbx fifo */
		if (IS_ENABLED(CONFIG_MULTITHREADING)) {
			pidf = k_thread_create(&zswmbx_f_t,
						  swmbx_thread_f_s,
						  swmbx_thread_size,
						  (k_thread_entry_t) dcscm_swmbx_fifo,
						  NULL, NULL, NULL,
						  -1,
						  0, K_NO_WAIT);
		}
	}

	/* register swmbx device 0/1*/
	dev = device_get_binding("SWMBX_SLAVE_0");
	if (!dev) {
		printk("I2C: SWMBX Slave 0 not be found.");
		return;
	}
	i2c_slave_driver_register(dev);

	dev = device_get_binding("SWMBX_SLAVE_1");
	if (!dev) {
		printk("I2C: SWMBX Slave 1 not be found.");
		return;
	}
	i2c_slave_driver_register(dev);

#endif
}