/*
 * Copyright (c) 2021 ASPEED
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief JTAG shell commands.
 */

#include <shell/shell.h>
#include <drivers/jtag.h>
#include <stdlib.h>

static uint8_t tdi_buffer[512];

static int cmd_ir_scan(const struct shell *shell, size_t argc, char **argv)
{
	const struct device *dev;
	uint32_t bit_len, byte_len;
	uint32_t value;
	int err, index;

	dev = device_get_binding(argv[1]);
	if (!dev) {
		shell_error(shell, "JTAG device not found");
		return -EINVAL;
	}

	bit_len = strtoul(argv[2], NULL, 0);
	value = strtoul(argv[3], NULL, 16);

	err = jtag_ir_scan(dev, bit_len, (uint8_t *)&value, tdi_buffer,
			   TAP_IDLE);
	if (err) {
		shell_error(shell, "failed to IR scan (err %d)", err);
		return err;
	}
	byte_len = (bit_len + 7) >> 3;
	for (index = byte_len - 1; index >= 0; index--) {
		shell_print(shell, "%x", tdi_buffer[index]);
	}
	return 0;
}

static int cmd_dr_scan(const struct shell *shell, size_t argc, char **argv)
{
	const struct device *dev;
	uint32_t bit_len, byte_len;
	uint32_t value;
	int err, index;

	dev = device_get_binding(argv[1]);
	if (!dev) {
		shell_error(shell, "JTAG device not found");
		return -EINVAL;
	}

	bit_len = strtoul(argv[2], NULL, 0);
	value = strtoul(argv[3], NULL, 16);

	err = jtag_dr_scan(dev, bit_len, (uint8_t *)&value, tdi_buffer,
			   TAP_IDLE);
	if (err) {
		shell_error(shell, "failed to DR scan (err %d)", err);
		return err;
	}
	byte_len = (bit_len + 7) >> 3;
	for (index = byte_len - 1; index >= 0; index--) {
		shell_print(shell, "%x", tdi_buffer[index]);
	}
	return 0;
}

static int cmd_frequency(const struct shell *shell, size_t argc, char **argv)
{
	const struct device *dev;
	uint32_t freq;
	int err;

	dev = device_get_binding(argv[1]);
	if (!dev) {
		shell_error(shell, "JTAG device not found");
		return -EINVAL;
	}

	freq = strtoul(argv[2], NULL, 0);

	err = jtag_freq_set(dev, freq);
	if (err) {
		shell_error(shell, "failed to setup JTAG frequency(err %d)",
			    err);
		return err;
	}
	err = jtag_freq_get(dev, &freq);
	if (err) {
		shell_error(shell, "failed to get JTAG frequency (err %d)",
			    err);
		return err;
	}
	shell_print(shell, "%d\n", freq);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	jtag_cmds,
	SHELL_CMD_ARG(frequency, NULL, "<device> <frequency>", cmd_frequency, 2,
		      1),
	SHELL_CMD_ARG(ir_scan, NULL, "<device> <len> <value>", cmd_ir_scan, 3,
		      1),
	SHELL_CMD_ARG(dr_scan, NULL, "<device> <len> <value>", cmd_dr_scan, 3,
		      1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(jtag, &jtag_cmds, "JTAG shell commands", NULL);