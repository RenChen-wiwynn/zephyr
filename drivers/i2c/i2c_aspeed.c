/*
 * Copyright (c) ASPEED Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT aspeed_i2c

#include <drivers/clock_control.h>
#include "soc.h"

#include <errno.h>
#include <drivers/i2c.h>
#include <soc.h>

#include <sys/util.h>

#include <logging/log.h>
#define LOG_LEVEL CONFIG_I2C_LOG_LEVEL
LOG_MODULE_REGISTER(i2c_aspeed);

#include "i2c-priv.h"

#define I2C_SLAVE_BUF_SIZE		256

#define I2C_BUF_SIZE		0x20

/***************************************************************************/
#define ASPEED_I2CG_ISR				0x00
#define ASPEED_I2CG_SLAVE_ISR		0x04	/* ast2600 */
#define ASPEED_I2CG_OWNER			0x08
#define ASPEED_I2CG_CTRL			0x0C
#define ASPEED_I2CG_CLK_DIV_CTRL	0x10	/* ast2600 */

#define ASPEED_I2CG_MBX_FIFO_ADDR	0x14	/* ast1060 */

/* 0x0C : I2CG SRAM Buffer Enable  */
#define ASPEED_I2CG_SRAM_BUFFER_ENABLE		BIT(0)

/*ast2600 */
#define ASPEED_I2CG_SLAVE_PKT_NAK		BIT(4)
#define ASPEED_I2CG_M_S_SEPARATE_INTR	BIT(3)
#define ASPEED_I2CG_CTRL_NEW_REG		BIT(2)
#define ASPEED_I2CG_CTRL_NEW_CLK_DIV	BIT(1)

/* ASPEED_I2CG_MBX_FIFO_ADDR	0x14	ast1060 FIFO register */
#define AST_I2CG_FIFO_ADDR_MASK			0xffff
#define AST_I2CG_FIFO_SIZE(x)			(x << 16)

/***************************************************************************/

//AST2600 reg
#define AST_I2CC_FUN_CTRL			0x00 	/* 0x00 : I2CC Master/Slave Function Control Register  */
#define AST_I2CC_SLAVE_MAILBOX_EN 		BIT(24)
#define AST_I2CC_SLAVE_MAILBOX_SRAM_EN 	BIT(23)

#define AST_I2CC_SLAVE_ADDR_RX_EN 		BIT(20)
#define AST_I2CC_MASTER_RETRY_MASK		(0x3 << 18)
#define AST_I2CC_MASTER_RETRY(x) 		((x & 0x3) << 18)
#define AST_I2CC_BUS_AUTO_RELEASE		BIT(17)	
#define AST_I2CC_M_SDA_LOCK_EN			BIT(16)
#define AST_I2CC_MULTI_MASTER_DIS		BIT(15)
#define AST_I2CC_M_SCL_DRIVE_EN			BIT(14)
#define AST_I2CC_MSB_STS				BIT(9)
#define AST_I2CC_SDA_DRIVE_1T_EN		BIT(8)
#define AST_I2CC_M_SDA_DRIVE_1T_EN		BIT(7)
#define AST_I2CC_M_HIGH_SPEED_EN		BIT(6)
/* reserver 5 : 2 */
#define AST_I2CC_SLAVE_EN				BIT(1)
#define AST_I2CC_MASTER_EN				BIT(0)

#define AST_I2CC_AC_TIMING			0x04	/* 0x04 : I2CC Master/Slave Clock and AC Timing Control Register #1 */
#define AST_I2CC_tTIMEOUT(x)			((x & 0x1f) << 24)	// 0~7
#define AST_I2CC_tCKHIGHMin(x)			((x & 0xf) << 20)	// 0~f 
#define AST_I2CC_tCKHIGH(x)				((x & 0xf) << 16)	// 0~7 
#define AST_I2CC_tCKLOW(x)				((x & 0xf) << 12)	// 0~7 
#define AST_I2CC_tHDDAT(x)				((x & 0x3) << 10)	// 0~3 
#define AST_I2CC_toutBaseCLK(x)			((x & 0x3) << 8)	//0~3
#define AST_I2CC_tBaseCLK(x)			(x & 0xf)	// 0~0xf

#define AST_I2CC_STS_AND_BUFF		0x08	/* 0x08 : I2CC Master/Slave Transmit/Receive Byte Buffer Register */
#define AST_I2CC_TX_DIR_MASK			(0x7 << 29)
#define AST_I2CC_SDA_OE					BIT(28)
#define AST_I2CC_SDA_O					BIT(27)
#define AST_I2CC_SCL_OE					BIT(26)
#define AST_I2CC_SCL_O					BIT(25)

// Tx State Machine
#define AST_I2CM_IDLE	 				0x0
#define AST_I2CM_MACTIVE				0x8
#define AST_I2CM_MSTART					0x9
#define AST_I2CM_MSTARTR				0xa
#define AST_I2CM_MSTOP					0xb
#define AST_I2CM_MTXD					0xc  
#define AST_I2CM_MRXACK					0xd
#define AST_I2CM_MRXD 					0xe
#define AST_I2CM_MTXACK 				0xf
#define AST_I2CM_SWAIT					0x1
#define AST_I2CM_SRXD 					0x4
#define AST_I2CM_STXACK 				0x5
#define AST_I2CM_STXD					0x6
#define AST_I2CM_SRXACK 				0x7
#define AST_I2CM_RECOVER 				0x3

#define AST_I2CC_SCL_LINE_STS			BIT(18)
#define AST_I2CC_SDA_LINE_STS			BIT(17)
#define AST_I2CC_BUS_BUSY_STS			BIT(16)

#define AST_I2CC_GET_RX_BUFF(x)			((x >> 8) & 0xff)

#define AST_I2CC_BUFF_CTRL		0x0C	/* 0x0C : I2CC Master/Slave Pool Buffer Control Register  */
#define AST_I2CC_GET_RX_BUF_LEN(x)		((x >> 24) & 0x3f)
#define AST_I2CC_SET_RX_BUF_LEN(x)		(((x - 1) & 0x1f) << 16)
#define AST_I2CC_SET_TX_BUF_LEN(x)		(((x - 1) & 0x1f) << 8)
#define AST_I2CC_GET_TX_BUF_LEN(x)		(((x >> 8) & 0x1f) + 1)

#define AST_I2CM_IER			0x10	/* 0x10 : I2CM Master Interrupt Control Register */
#define AST_I2CM_ISR			0x14	/* 0x14 : I2CM Master Interrupt Status Register   : WC */

#define AST_I2CM_PKT_TIMEOUT			BIT(18)
#define AST_I2CM_PKT_ERROR				BIT(17)
#define AST_I2CM_PKT_DONE				BIT(16)

#define AST_I2CM_BUS_RECOVER_FAIL		BIT(15)
#define AST_I2CM_SDA_DL_TO				BIT(14)
#define AST_I2CM_BUS_RECOVER			BIT(13)
#define AST_I2CM_SMBUS_ALT				BIT(12)

#define AST_I2CM_SCL_LOW_TO				BIT(6)
#define AST_I2CM_ABNORMAL				BIT(5)
#define AST_I2CM_NORMAL_STOP			BIT(4)
#define AST_I2CM_ARBIT_LOSS				BIT(3)
#define AST_I2CM_RX_DONE				BIT(2)
#define AST_I2CM_TX_NAK					BIT(1)
#define AST_I2CM_TX_ACK					BIT(0)

#define AST_I2CM_CMD_STS		0x18	/* 0x18 : I2CM Master Command/Status Register   */
#define AST_I2CM_PKT_ADDR(x)			((x & 0x7f) << 24)	
#define AST_I2CM_PKT_EN					BIT(16)
#define AST_I2CM_SDA_OE_OUT_DIR			BIT(15)
#define AST_I2CM_SDA_O_OUT_DIR			BIT(14)
#define AST_I2CM_SCL_OE_OUT_DIR			BIT(13)
#define AST_I2CM_SCL_O_OUT_DIR			BIT(12)
#define AST_I2CM_RECOVER_CMD_EN			BIT(11)

#define AST_I2CM_RX_DMA_EN				BIT(9)
#define AST_I2CM_TX_DMA_EN				BIT(8)
#define AST_I2CM_RX_BUFF_EN				BIT(7)
#define AST_I2CM_TX_BUFF_EN				BIT(6)

/* Command Bit */
#define AST_I2CM_RX_BUFF_EN				BIT(7)
#define AST_I2CM_TX_BUFF_EN				BIT(6)
#define AST_I2CM_STOP_CMD				BIT(5)
#define AST_I2CM_RX_CMD_LAST			BIT(4)
#define AST_I2CM_RX_CMD					BIT(3)

#define AST_I2CM_TX_CMD					BIT(1)
#define AST_I2CM_START_CMD				BIT(0)

#define AST_I2CM_DMA_LEN		0x1C	/* 0x1C : I2CM Master DMA Transfer Length Register   */
#define AST_I2CM_SET_RX_DMA_LEN(x)		((((x) & 0xfff) << 16) | BIT(31))	/* 1 ~ 4096 */
#define AST_I2CM_SET_TX_DMA_LEN(x)		(((x) & 0xfff) | BIT(15))			/* 1 ~ 4096 */

#define AST_I2CS_IER			0x20	/* 0x20 : I2CS Slave Interrupt Control Register   */
#define AST_I2CS_ISR			0x24	/* 0x24 : I2CS Slave Interrupt Status Register   */

#define AST_I2CS_ADDR_INDICAT_MASK	(3 << 30)

#define AST_I2CS_Wait_TX_DMA		BIT(25)
#define AST_I2CS_Wait_RX_DMA		BIT(24)


#define AST_I2CS_ADDR3_NAK			BIT(22)
#define AST_I2CS_ADDR2_NAK			BIT(21)
#define AST_I2CS_ADDR1_NAK			BIT(20)

#define AST_I2CS_ADDR_MASK			(3 << 18)
#define AST_I2CS_PKT_ERROR			BIT(17)
#define AST_I2CS_PKT_DONE			BIT(16)
#define AST_I2CS_INACTIVE_TO		BIT(15)
//
#define AST_I2CS_SLAVE_MATCH		BIT(7)
//
#define AST_I2CS_ABNOR_STOP			BIT(5)
#define AST_I2CS_STOP				BIT(4)
#define AST_I2CS_RX_DONE_NAK		BIT(3)
#define AST_I2CS_RX_DONE			BIT(2)
#define AST_I2CS_TX_NAK				BIT(1)
#define AST_I2CS_TX_ACK				BIT(0)

#define AST_I2CS_CMD_STS		0x28	/* 0x28 : I2CS Slave CMD/Status Register   */
#define AST_I2CS_ACTIVE_ALL				(0x3 << 17)
#define AST_I2CS_PKT_MODE_EN			BIT(16)
#define AST_I2CS_AUTO_NAK_NOADDR		BIT(15)
#define AST_I2CS_AUTO_NAK_EN			BIT(14)

/* new for i2c snoop */
#define AST_I2CS_SNOOP_LOOP				BIT(12)
#define AST_I2CS_SNOOP_EN				BIT(11)

#define AST_I2CS_ALT_EN					BIT(10)
#define AST_I2CS_RX_DMA_EN				BIT(9)
#define AST_I2CS_TX_DMA_EN				BIT(8)
#define AST_I2CS_RX_BUFF_EN				BIT(7)
#define AST_I2CS_TX_BUFF_EN				BIT(6)
#define AST_I2CS_RX_CMD_LAST			BIT(4)

#define AST_I2CS_TX_CMD					BIT(2)

#define AST_I2CS_DMA_LEN		0x2C
#define AST_I2CS_SET_RX_DMA_LEN(x)		((((x - 1) & 0xfff) << 16) | BIT(31))
#define AST_I2CS_RX_DMA_LEN_MASK		(0xfff << 16)

#define AST_I2CS_SET_TX_DMA_LEN(x)		(((x - 1) & 0xfff) | BIT(15))
#define AST_I2CS_TX_DMA_LEN_MASK		0xfff

#define AST_I2CM_TX_DMA			0x30 	/* I2CM Master DMA Tx Buffer Register   */
#define AST_I2CM_RX_DMA			0x34	/* I2CM Master DMA Rx Buffer Register   */
#define AST_I2CS_TX_DMA			0x38 	/* I2CS Slave DMA Tx Buffer Register   */
#define AST_I2CS_RX_DMA			0x3C	/* I2CS Slave DMA Rx Buffer Register   */

/* 0x40 : Slave Device Address Register */
#define AST_I2CS_ADDR_CTRL		0x40

#define AST_I2CS_ADDR3_MBX_TYPE(x)		(x << 28)
#define AST_I2CS_ADDR2_MBX_TYPE(x)		(x << 26)
#define AST_I2CS_ADDR1_MBX_TYPE(x)		(x << 24)
#define AST_I2CS_ADDR3_ENABLE			BIT(23)
#define AST_I2CS_ADDR3(x)				((x & 0x7f) << 16)
#define AST_I2CS_ADDR2_ENABLE			BIT(15)
#define AST_I2CS_ADDR2(x)				((x & 0x7f) << 8)
#define AST_I2CS_ADDR1_ENABLE			BIT(7)
#define AST_I2CS_ADDR1(x)				(x & 0x7f)

#define	AST_I2CS_ADDR3_MASK		(0x7f << 16)
#define	AST_I2CS_ADDR2_MASK		(0x7f << 8)
#define	AST_I2CS_ADDR1_MASK		0x7f

#define AST_I2CM_DMA_LEN_STS	0x48
#define AST_I2CS_DMA_LEN_STS	0x4C

#define AST_I2C_GET_TX_DMA_LEN(x)		(x & 0x1fff)
#define AST_I2C_GET_RX_DMA_LEN(x)		((x >> 16) & 0x1fff)

/* new for i2c snoop */
#define AST_I2CS_SNOOP_DMA_WPT	0x50
#define AST_I2CS_SNOOP_DMA_RPT	0x58
/***************************************************************************/
/* Use platform_data instead of module parameters */
/* Fast Mode = 400 kHz, Standard = 100 kHz */
//static int clock = 100; /* Default: 100 kHz */
/***************************************************************************/
#define AST_LOCKUP_DETECTED 	BIT(15)
#define AST_I2C_LOW_TIMEOUT 	0x07
/***************************************************************************/
#define ASPEED_I2C_DMA_SIZE		4096
/***************************************************************************/

#define DEV_CFG(dev) \
	((struct i2c_aspeed_config *)(dev)->config)
#define DEV_DATA(dev) \
	((struct i2c_aspeed_data *)(dev)->data)
#define DEV_BASE(dev) \
	((DEV_CFG(dev))->base)

enum i2c_xfer_mode {
	DMA_MODE = 0,
	BUFF_MODE,
	BYTE_MODE,
};

struct i2c_aspeed_config {
	uint32_t global_reg;
	uintptr_t base;
	/* Buffer mode */
	uintptr_t	buf_base;
	size_t		buf_size;
	uint32_t bitrate;
	int multi_master;
	int smbus_alert;
	const clock_control_subsys_t clk_id;
	uint32_t ac_timing;
	void (*irq_config_func)(const struct device *dev);
	uint32_t clk_src;
	int	clk_div_mode;	//0: old mode, 1: new mode
	enum i2c_xfer_mode mode;	
};

struct i2c_aspeed_data {
	struct k_sem 		sync_sem;

	int					alert_enable;

	uint32_t			bus_recover;
	
	struct i2c_msg		*msgs;	//cur xfer msgs
	uint16_t 			addr;
	int					buf_index;	//buffer mode idx	
	int 				msgs_index; //cur xfer msgs index
	int 				msgs_count; //total msgs

	int 				master_xfer_cnt;	//total xfer count


	bool slave_attached;	
///



	int 		xfer_complete;

	uint32_t	bus_frequency;	
	/* master configuration */
	bool multi_master;
	int			cmd_err;
	uint16_t	flags;
	uint8_t 	*buf;
	int			len;	//master xfer count
	/* snoop mode */
#ifdef I2C_SNOOP_MODE	
	uint8_t		*snoop_buf;
	size_t		snoop_size;
#endif
	/* slave configuration */
	uint8_t 	slave_addr;
	uint32_t	slave_xfer_len;
	uint32_t	slave_xfer_cnt;

	/* slave mqueue */
	int			fifo_fetch_idx;
	int			fifo_rx_idx;
	int			fifo_full;

#ifdef CONFIG_I2C_SLAVE
	unsigned char		slave_dma_buf[I2C_SLAVE_BUF_SIZE];
	struct i2c_slave_config *slave_cfg;
#endif

};

struct ast_i2c_timing_table {
	uint32_t divisor;
	uint32_t timing;
};

static struct ast_i2c_timing_table aspeed_old_i2c_timing_table[] = {
	/* Divisor : Base Clock : tCKHighMin : tCK High : tCK Low  */
	/* Divisor :	  [3:0] : [23: 20]   :   [19:16]:   [15:12] */
	{6,	0x00000300 | (0x0) | (0x2 << 20) | (0x2 << 16) | (0x2 << 12) },
	{7,	0x00000300 | (0x0) | (0x3 << 20) | (0x3 << 16) | (0x2 << 12) },
	{8,	0x00000300 | (0x0) | (0x3 << 20) | (0x3 << 16) | (0x3 << 12) },
	{9,	0x00000300 | (0x0) | (0x4 << 20) | (0x4 << 16) | (0x3 << 12) },
	{10, 	0x00000300 | (0x0) | (0x4 << 20) | (0x4 << 16) | (0x4 << 12) },
	{11, 	0x00000300 | (0x0) | (0x5 << 20) | (0x5 << 16) | (0x4 << 12) },
	{12, 	0x00000300 | (0x0) | (0x5 << 20) | (0x5 << 16) | (0x5 << 12) },
	{13, 	0x00000300 | (0x0) | (0x6 << 20) | (0x6 << 16) | (0x5 << 12) },
	{14, 	0x00000300 | (0x0) | (0x6 << 20) | (0x6 << 16) | (0x6 << 12) },
	{15, 	0x00000300 | (0x0) | (0x7 << 20) | (0x7 << 16) | (0x6 << 12) },
	{16, 	0x00000300 | (0x0) | (0x7 << 20) | (0x7 << 16) | (0x7 << 12) },
	{17, 	0x00000300 | (0x0) | (0x8 << 20) | (0x8 << 16) | (0x7 << 12) },
	{18, 	0x00000300 | (0x0) | (0x8 << 20) | (0x8 << 16) | (0x8 << 12) },
	{19, 	0x00000300 | (0x0) | (0x9 << 20) | (0x9 << 16) | (0x8 << 12) },
	{20, 	0x00000300 | (0x0) | (0x9 << 20) | (0x9 << 16) | (0x9 << 12) },
	{21, 	0x00000300 | (0x0) | (0xa << 20) | (0xa << 16) | (0x9 << 12) },
	{22, 	0x00000300 | (0x0) | (0xa << 20) | (0xa << 16) | (0xa << 12) },
	{23, 	0x00000300 | (0x0) | (0xb << 20) | (0xb << 16) | (0xa << 12) },
	{24, 	0x00000300 | (0x0) | (0xb << 20) | (0xb << 16) | (0xb << 12) },
	{25, 	0x00000300 | (0x0) | (0xc << 20) | (0xc << 16) | (0xb << 12) },
	{26, 	0x00000300 | (0x0) | (0xc << 20) | (0xc << 16) | (0xc << 12) },
	{27, 	0x00000300 | (0x0) | (0xd << 20) | (0xd << 16) | (0xc << 12) },
	{28, 	0x00000300 | (0x0) | (0xd << 20) | (0xd << 16) | (0xd << 12) },
	{29, 	0x00000300 | (0x0) | (0xe << 20) | (0xe << 16) | (0xd << 12) },
	{30, 	0x00000300 | (0x0) | (0xe << 20) | (0xe << 16) | (0xe << 12) },
	{31, 	0x00000300 | (0x0) | (0xf << 20) | (0xf << 16) | (0xe << 12) },
	{32, 	0x00000300 | (0x0) | (0xf << 20) | (0xf << 16) | (0xf << 12) },

	{34, 	0x00000300 | (0x1) | (0x8 << 20) | (0x8 << 16) | (0x7 << 12) },
	{36, 	0x00000300 | (0x1) | (0x8 << 20) | (0x8 << 16) | (0x8 << 12) },
	{38, 	0x00000300 | (0x1) | (0x9 << 20) | (0x9 << 16) | (0x8 << 12) },
	{40, 	0x00000300 | (0x1) | (0x9 << 20) | (0x9 << 16) | (0x9 << 12) },
	{42, 	0x00000300 | (0x1) | (0xa << 20) | (0xa << 16) | (0x9 << 12) },
	{44, 	0x00000300 | (0x1) | (0xa << 20) | (0xa << 16) | (0xa << 12) },
	{46, 	0x00000300 | (0x1) | (0xb << 20) | (0xb << 16) | (0xa << 12) },
	{48, 	0x00000300 | (0x1) | (0xb << 20) | (0xb << 16) | (0xb << 12) },
	{50, 	0x00000300 | (0x1) | (0xc << 20) | (0xc << 16) | (0xb << 12) },
	{52, 	0x00000300 | (0x1) | (0xc << 20) | (0xc << 16) | (0xc << 12) },
	{54, 	0x00000300 | (0x1) | (0xd << 20) | (0xd << 16) | (0xc << 12) },
	{56, 	0x00000300 | (0x1) | (0xd << 20) | (0xd << 16) | (0xd << 12) },
	{58, 	0x00000300 | (0x1) | (0xe << 20) | (0xe << 16) | (0xd << 12) },
	{60, 	0x00000300 | (0x1) | (0xe << 20) | (0xe << 16) | (0xe << 12) },
	{62, 	0x00000300 | (0x1) | (0xf << 20) | (0xf << 16) | (0xe << 12) },
	{64, 	0x00000300 | (0x1) | (0xf << 20) | (0xf << 16) | (0xf << 12) },

	{68, 	0x00000300 | (0x2) | (0x8 << 20) | (0x8 << 16) | (0x7 << 12) },
	{72, 	0x00000300 | (0x2) | (0x8 << 20) | (0x8 << 16) | (0x8 << 12) },
	{76, 	0x00000300 | (0x2) | (0x9 << 20) | (0x9 << 16) | (0x8 << 12) },
	{80, 	0x00000300 | (0x2) | (0x9 << 20) | (0x9 << 16) | (0x9 << 12) },
	{84, 	0x00000300 | (0x2) | (0xa << 20) | (0xa << 16) | (0x9 << 12) },
	{88, 	0x00000300 | (0x2) | (0xa << 20) | (0xa << 16) | (0xa << 12) },
	{92, 	0x00000300 | (0x2) | (0xb << 20) | (0xb << 16) | (0xa << 12) },
	{96, 	0x00000300 | (0x2) | (0xb << 20) | (0xb << 16) | (0xb << 12) },
	{100, 	0x00000300 | (0x2) | (0xc << 20) | (0xc << 16) | (0xb << 12) },
	{104, 	0x00000300 | (0x2) | (0xc << 20) | (0xc << 16) | (0xc << 12) },
	{108, 	0x00000300 | (0x2) | (0xd << 20) | (0xd << 16) | (0xc << 12) },
	{112, 	0x00000300 | (0x2) | (0xd << 20) | (0xd << 16) | (0xd << 12) },
	{116, 	0x00000300 | (0x2) | (0xe << 20) | (0xe << 16) | (0xd << 12) },
	{120, 	0x00000300 | (0x2) | (0xe << 20) | (0xe << 16) | (0xe << 12) },
	{124, 	0x00000300 | (0x2) | (0xf << 20) | (0xf << 16) | (0xe << 12) },
	{128, 	0x00000300 | (0x2) | (0xf << 20) | (0xf << 16) | (0xf << 12) },

	{136, 	0x00000300 | (0x3) | (0x8 << 20) | (0x8 << 16) | (0x7 << 12) },
	{144, 	0x00000300 | (0x3) | (0x8 << 20) | (0x8 << 16) | (0x8 << 12) },
	{152, 	0x00000300 | (0x3) | (0x9 << 20) | (0x9 << 16) | (0x8 << 12) },
	{160, 	0x00000300 | (0x3) | (0x9 << 20) | (0x9 << 16) | (0x9 << 12) },
	{168, 	0x00000300 | (0x3) | (0xa << 20) | (0xa << 16) | (0x9 << 12) },
	{176, 	0x00000300 | (0x3) | (0xa << 20) | (0xa << 16) | (0xa << 12) },
	{184, 	0x00000300 | (0x3) | (0xb << 20) | (0xb << 16) | (0xa << 12) },
	{192, 	0x00000300 | (0x3) | (0xb << 20) | (0xb << 16) | (0xb << 12) },
	{200, 	0x00000300 | (0x3) | (0xc << 20) | (0xc << 16) | (0xb << 12) },
	{208, 	0x00000300 | (0x3) | (0xc << 20) | (0xc << 16) | (0xc << 12) },
	{216, 	0x00000300 | (0x3) | (0xd << 20) | (0xd << 16) | (0xc << 12) },
	{224, 	0x00000300 | (0x3) | (0xd << 20) | (0xd << 16) | (0xd << 12) },
	{232, 	0x00000300 | (0x3) | (0xe << 20) | (0xe << 16) | (0xd << 12) },
	{240, 	0x00000300 | (0x3) | (0xe << 20) | (0xe << 16) | (0xe << 12) },
	{248, 	0x00000300 | (0x3) | (0xf << 20) | (0xf << 16) | (0xe << 12) },
	{256, 	0x00000300 | (0x3) | (0xf << 20) | (0xf << 16) | (0xf << 12) },

	{272, 	0x00000300 | (0x4) | (0x8 << 20) | (0x8 << 16) | (0x7 << 12) },
	{288, 	0x00000300 | (0x4) | (0x8 << 20) | (0x8 << 16) | (0x8 << 12) },
	{304, 	0x00000300 | (0x4) | (0x9 << 20) | (0x9 << 16) | (0x8 << 12) },
	{320, 	0x00000300 | (0x4) | (0x9 << 20) | (0x9 << 16) | (0x9 << 12) },
	{336, 	0x00000300 | (0x4) | (0xa << 20) | (0xa << 16) | (0x9 << 12) },
	{352, 	0x00000300 | (0x4) | (0xa << 20) | (0xa << 16) | (0xa << 12) },
	{368, 	0x00000300 | (0x4) | (0xb << 20) | (0xb << 16) | (0xa << 12) },
	{384, 	0x00000300 | (0x4) | (0xb << 20) | (0xb << 16) | (0xb << 12) },
	{400, 	0x00000300 | (0x4) | (0xc << 20) | (0xc << 16) | (0xb << 12) },
	{416, 	0x00000300 | (0x4) | (0xc << 20) | (0xc << 16) | (0xc << 12) },
	{432, 	0x00000300 | (0x4) | (0xd << 20) | (0xd << 16) | (0xc << 12) },
	{448, 	0x00000300 | (0x4) | (0xd << 20) | (0xd << 16) | (0xd << 12) },
	{464, 	0x00000300 | (0x4) | (0xe << 20) | (0xe << 16) | (0xd << 12) },
	{480, 	0x00000300 | (0x4) | (0xe << 20) | (0xe << 16) | (0xe << 12) },
	{496, 	0x00000300 | (0x4) | (0xf << 20) | (0xf << 16) | (0xe << 12) },
	{512, 	0x00000300 | (0x4) | (0xf << 20) | (0xf << 16) | (0xf << 12) },

	{544, 	0x00000300 | (0x5) | (0x8 << 20) | (0x8 << 16) | (0x7 << 12) },
	{576, 	0x00000300 | (0x5) | (0x8 << 20) | (0x8 << 16) | (0x8 << 12) },
	{608, 	0x00000300 | (0x5) | (0x9 << 20) | (0x9 << 16) | (0x8 << 12) },
	{640, 	0x00000300 | (0x5) | (0x9 << 20) | (0x9 << 16) | (0x9 << 12) },
	{672, 	0x00000300 | (0x5) | (0xa << 20) | (0xa << 16) | (0x9 << 12) },
	{704, 	0x00000300 | (0x5) | (0xa << 20) | (0xa << 16) | (0xa << 12) },
	{736, 	0x00000300 | (0x5) | (0xb << 20) | (0xb << 16) | (0xa << 12) },
	{768, 	0x00000300 | (0x5) | (0xb << 20) | (0xb << 16) | (0xb << 12) },
	{800, 	0x00000300 | (0x5) | (0xc << 20) | (0xc << 16) | (0xb << 12) },
	{832, 	0x00000300 | (0x5) | (0xc << 20) | (0xc << 16) | (0xc << 12) },
	{864, 	0x00000300 | (0x5) | (0xd << 20) | (0xd << 16) | (0xc << 12) },
	{896, 	0x00000300 | (0x5) | (0xd << 20) | (0xd << 16) | (0xd << 12) },
	{928, 	0x00000300 | (0x5) | (0xe << 20) | (0xe << 16) | (0xd << 12) },
	{960, 	0x00000300 | (0x5) | (0xe << 20) | (0xe << 16) | (0xe << 12) },
	{992, 	0x00000300 | (0x5) | (0xf << 20) | (0xf << 16) | (0xe << 12) },
	{1024, 	0x00000300 | (0x5) | (0xf << 20) | (0xf << 16) | (0xf << 12) },

	{1088, 	0x00000300 | (0x6) | (0x8 << 20) | (0x8 << 16) | (0x7 << 12) },
	{1152, 	0x00000300 | (0x6) | (0x8 << 20) | (0x8 << 16) | (0x8 << 12) },
	{1216, 	0x00000300 | (0x6) | (0x9 << 20) | (0x9 << 16) | (0x8 << 12) },
	{1280, 	0x00000300 | (0x6) | (0x9 << 20) | (0x9 << 16) | (0x9 << 12) },
	{1344, 	0x00000300 | (0x6) | (0xa << 20) | (0xa << 16) | (0x9 << 12) },
	{1408, 	0x00000300 | (0x6) | (0xa << 20) | (0xa << 16) | (0xa << 12) },
	{1472, 	0x00000300 | (0x6) | (0xb << 20) | (0xb << 16) | (0xa << 12) },
	{1536, 	0x00000300 | (0x6) | (0xb << 20) | (0xb << 16) | (0xb << 12) },
	{1600, 	0x00000300 | (0x6) | (0xc << 20) | (0xc << 16) | (0xb << 12) },
	{1664, 	0x00000300 | (0x6) | (0xc << 20) | (0xc << 16) | (0xc << 12) },
	{1728, 	0x00000300 | (0x6) | (0xd << 20) | (0xd << 16) | (0xc << 12) },
	{1792, 	0x00000300 | (0x6) | (0xd << 20) | (0xd << 16) | (0xd << 12) },
	{1856, 	0x00000300 | (0x6) | (0xe << 20) | (0xe << 16) | (0xd << 12) },
	{1920, 	0x00000300 | (0x6) | (0xe << 20) | (0xe << 16) | (0xe << 12) },
	{1984, 	0x00000300 | (0x6) | (0xf << 20) | (0xf << 16) | (0xe << 12) },
	{2048, 	0x00000300 | (0x6) | (0xf << 20) | (0xf << 16) | (0xf << 12) },

	{2176, 	0x00000300 | (0x7) | (0x8 << 20) | (0x8 << 16) | (0x7 << 12) },
	{2304, 	0x00000300 | (0x7) | (0x8 << 20) | (0x8 << 16) | (0x8 << 12) },
	{2432, 	0x00000300 | (0x7) | (0x9 << 20) | (0x9 << 16) | (0x8 << 12) },
	{2560, 	0x00000300 | (0x7) | (0x9 << 20) | (0x9 << 16) | (0x9 << 12) },
	{2688, 	0x00000300 | (0x7) | (0xa << 20) | (0xa << 16) | (0x9 << 12) },
	{2816, 	0x00000300 | (0x7) | (0xa << 20) | (0xa << 16) | (0xa << 12) },
	{2944, 	0x00000300 | (0x7) | (0xb << 20) | (0xb << 16) | (0xa << 12) },
	{3072, 	0x00000300 | (0x7) | (0xb << 20) | (0xb << 16) | (0xb << 12) },
};

/* cache size = 32B * 128 = 4KB */
#define CACHE_LINE_SIZE_LOG2	5
#define CACHE_LINE_SIZE		(1 << CACHE_LINE_SIZE_LOG2)
#define N_CACHE_LINE		128
#define CACHE_ALIGNED_ADDR(addr)                                               \
	((addr >> CACHE_LINE_SIZE_LOG2) << CACHE_LINE_SIZE_LOG2)
	
#define SCU_BASE 0x7e6e2000

#define CACHE_AREA_CTRL_REG		0xa50
#define CACHE_AREA_SIZE_LOG2	15
#define CACHE_AREA_SIZE			(1 << CACHE_AREA_SIZE_LOG2)

#define CACHE_INVALID_REG		0xa54
#define DCACHE_INVALID(addr)	(BIT(31) | ((addr & GENMASK(10, 0)) << 16))
#define ICACHE_INVALID(addr)	(BIT(15) | ((addr & GENMASK(10, 0)) << 0))

#define CACHE_FUNC_CTRL_REG		0xa58
#define ICACHE_CLEAN			BIT(2)
#define DCACHE_CLEAN			BIT(1)
#define CACHE_EANABLE			BIT(0)
#if 0

static uint32_t get_n_cacheline(uint32_t addr, uint32_t size, uint32_t *p_head)
{
	uint32_t n = 0;
	uint32_t tail;

	/* head */
	*p_head = CACHE_ALIGNED_ADDR(addr); 

	/* roundup the tail address */
	tail = addr + size + (CACHE_LINE_SIZE - 1);
	tail = CACHE_ALIGNED_ADDR(tail);

	n = (tail - *p_head) >> CACHE_LINE_SIZE_LOG2;

	return n;
}
#endif
void aspeed_cache_invalid_data(uint32_t addr, uint32_t size)
{
#if 0
	uint32_t aligned_addr, i, n;

	n = get_n_cacheline(addr, size, &aligned_addr);

	LOG_DBG("addr %x, size %d n: %d\n", addr, size, n);
	sys_read32((uint32_t)(0x70000));

	for (i = 0; i < n; i++) {
		sys_write32(DCACHE_INVALID(aligned_addr), SCU_BASE + CACHE_INVALID_REG);
		sys_write32(0, SCU_BASE + CACHE_INVALID_REG);
		aligned_addr += CACHE_LINE_SIZE;
	}

	/* issue a non-cached data access to flush the prefetch buffer */
	sys_read32((uint32_t)(0x70000));
	sys_read32((uint32_t)(addr | 0x10000));
#else
	sys_write32(sys_read32(SCU_BASE + 0xa58) & ~0x2, SCU_BASE + 0xa58);
	sys_write32(sys_read32(SCU_BASE + 0xa58) | 0x2, SCU_BASE + 0xa58);
#endif

}

static uint32_t i2c_aspeed_select_clock(const struct device *dev)
{
	const struct i2c_aspeed_config *config = DEV_CFG(dev);	
	struct i2c_aspeed_data *data = DEV_DATA(dev);
	uint32_t ac_timing;
	int i;
	int div = 0;
	int divider_ratio = 0;
	uint32_t clk_div_reg;
	int inc = 0;
	unsigned long base_clk1, base_clk2, base_clk3, base_clk4;
	uint32_t scl_low, scl_high;

//	LOG_DBG("clk src %d, targe %d \n", config->clk_src, data->bus_frequency);
	
	if(config->clk_div_mode) {
		clk_div_reg = sys_read32(config->global_reg + ASPEED_I2CG_CLK_DIV_CTRL);
		base_clk1 = config->clk_src / (((clk_div_reg & 0xff) + 2) / 2);
		base_clk2 = config->clk_src / ((((clk_div_reg >> 8) & 0xff) + 2) / 2);
		base_clk3 = config->clk_src / ((((clk_div_reg >> 16) & 0xff) + 2) / 2);
		base_clk4 = config->clk_src / ((((clk_div_reg >> 24) & 0xff) + 2) / 2);
//		printf("base_clk1 %ld, base_clk2 %ld, base_clk3 %ld, base_clk4 %ld \n", base_clk1, base_clk2, base_clk3, base_clk4);
		if((config->clk_src / data->bus_frequency) <= 32) {
			div = 0;
			divider_ratio = config->clk_src / data->bus_frequency;
		} else if ((base_clk1 / data->bus_frequency) <= 32) {
			div = 1;
			divider_ratio = base_clk1 / data->bus_frequency;
		} else if ((base_clk2 / data->bus_frequency) <= 32) {
			div = 2;
			divider_ratio = base_clk2 / data->bus_frequency;
		} else if ((base_clk3 / data->bus_frequency) <= 32) {
			div = 3;
			divider_ratio = base_clk3 / data->bus_frequency;
		} else {
			div = 4;
			divider_ratio = base_clk4 / data->bus_frequency;
			inc = 0;
			while((divider_ratio + inc) > 32) {
				inc |= divider_ratio & 0x1;
				divider_ratio >>= 1;
				div++;
			}
			divider_ratio += inc;
		}
		div &= 0xf;
		scl_low = ((divider_ratio >> 1) - 1) & 0xf;
		scl_high = (divider_ratio - scl_low - 2) & 0xf;
		/* Divisor : Base Clock : tCKHighMin : tCK High : tCK Low  */		
		ac_timing = (scl_high << 20) | (scl_high << 16) | (scl_low << 12) | (div);
	} else {
		for (i = 0; i < ARRAY_SIZE(aspeed_old_i2c_timing_table); i++) {
			if ((config->clk_src / aspeed_old_i2c_timing_table[i].divisor) <
			    data->bus_frequency) {
				break;
			}
		}
		i--;
		ac_timing = aspeed_old_i2c_timing_table[i].timing;
//		LOG_DBG("divisor [%d], timing [%x] \n",aspeed_old_i2c_timing_table[i].divisor, aspeed_old_i2c_timing_table[i].timing);
	} 

	return ac_timing;
}

/* Default maximum time we allow for an I2C transfer (unit:ms) */
#define I2C_TRANS_TIMEOUT K_MSEC(100)

static int i2c_wait_completion(const struct device *dev)
{
	struct i2c_aspeed_data *data = DEV_DATA(dev);

	if (k_sem_take(&data->sync_sem, I2C_TRANS_TIMEOUT) == 0) {
		return 0;
	} else {
		return -ETIMEDOUT;
	}
}

static uint8_t
aspeed_new_i2c_recover_bus(const struct device *dev)
{
	struct i2c_aspeed_data *data = DEV_DATA(dev);
	uint8_t i2c_base = DEV_BASE(dev);

	uint32_t ctrl, state;
	int r;
	int ret = 0;

	ctrl = sys_read32(i2c_base + AST_I2CC_FUN_CTRL);

	sys_write32(ctrl & ~(AST_I2CC_MASTER_EN | AST_I2CC_SLAVE_EN),
				i2c_base + AST_I2CC_FUN_CTRL);

	sys_write32(sys_read32(i2c_base + AST_I2CC_FUN_CTRL) | AST_I2CC_MASTER_EN,
				i2c_base + AST_I2CC_FUN_CTRL);

	//Let's retry 10 times
	k_sem_reset(&data->sync_sem);
	data->bus_recover = 1;
	data->cmd_err = 0;

	//Check 0x14's SDA and SCL status
	state = sys_read32(i2c_base + AST_I2CC_STS_AND_BUFF);
	if (!(state & AST_I2CC_SDA_LINE_STS) && (state & AST_I2CC_SCL_LINE_STS)) {
		sys_write32(AST_I2CM_RECOVER_CMD_EN, i2c_base + AST_I2CM_CMD_STS);
		r = i2c_wait_completion(dev);
		if (r == 0) {
			LOG_DBG("recovery timed out\n");
			ret = -ETIMEDOUT;
		} else {
			if (data->cmd_err) {
				LOG_DBG("recovery error \n");
				ret = -EPROTO;
			} 
		}
	} else {
		LOG_DBG("can't recovery this situation\n");
		ret = -EPROTO;
	}
	LOG_DBG("Recovery done [%x]\n", sys_read32(i2c_base + AST_I2CC_STS_AND_BUFF));

	return ret;
}

static int i2c_aspeed_configure(const struct device *dev,
			     uint32_t dev_config_raw)
{
	const struct i2c_aspeed_config *config = DEV_CFG(dev);
	struct i2c_aspeed_data *data = DEV_DATA(dev);	
	uint32_t i2c_base = DEV_BASE(dev);
	uint32_t fun_ctrl = AST_I2CC_BUS_AUTO_RELEASE;
	
	if (I2C_ADDR_10_BITS & dev_config_raw) {
		return -EINVAL;
	}

	if (I2C_MODE_MASTER & dev_config_raw)
		fun_ctrl |= AST_I2CC_MASTER_EN;

	switch (I2C_SPEED_GET(dev_config_raw)) {
	case I2C_SPEED_STANDARD:
		data->bus_frequency = KHZ(100);
		break;
	case I2C_SPEED_FAST:
		data->bus_frequency = KHZ(400);
		break;
	case I2C_SPEED_FAST_PLUS:
		data->bus_frequency = MHZ(1);
		break;
	default:
		return -EINVAL;
	}

	//I2C Reset
	sys_write32(0, i2c_base + AST_I2CC_FUN_CTRL);

	if(!config->multi_master)
		fun_ctrl |= AST_I2CC_MULTI_MASTER_DIS;

	/* Enable Master Mode */
	sys_write32(fun_ctrl, i2c_base + AST_I2CC_FUN_CTRL);

	/* Set AC Timing */
	sys_write32(i2c_aspeed_select_clock(dev), i2c_base + AST_I2CC_AC_TIMING);

	//Clear Interrupt
	sys_write32(0xfffffff, i2c_base + AST_I2CM_ISR);

	/* Set interrupt generation of I2C master controller */
	if(config->smbus_alert)
		sys_write32(AST_I2CM_PKT_DONE | AST_I2CM_BUS_RECOVER |
						AST_I2CM_SMBUS_ALT, 
						i2c_base + AST_I2CM_IER);
	else
		sys_write32(AST_I2CM_PKT_DONE | AST_I2CM_BUS_RECOVER,
						i2c_base + AST_I2CM_IER);

#ifdef CONFIG_I2C_SLAVE
	if (config->mode == DMA_MODE)
		memset(data->slave_dma_buf, 0, I2C_SLAVE_BUF_SIZE);

	sys_write32(0xfffffff, i2c_base + AST_I2CS_ISR);
	
	if (config->mode == BYTE_MODE)
		sys_write32(0xffff, i2c_base + AST_I2CS_IER);
	else {
		/* Set interrupt generation of I2C slave controller */
		sys_write32(AST_I2CS_PKT_DONE, i2c_base + AST_I2CS_IER);
	}
#endif
	return 0;
}

static void aspeed_new_i2c_do_start(const struct device *dev)
{
	struct i2c_aspeed_config *config = DEV_CFG(dev);
	struct i2c_aspeed_data *data = DEV_DATA(dev);
	uint32_t i2c_base = DEV_BASE(dev);

	int i = 0;
	int	xfer_len;
	struct i2c_msg *msg = &data->msgs[data->msgs_index];
	uint32_t cmd = AST_I2CM_PKT_EN | AST_I2CM_PKT_ADDR(data->addr) | AST_I2CM_START_CMD;

	//send start
	LOG_DBG("[%s]: [%d/%d] %sing %d byte%s %s 0x%02x\n",
		dev->name, data->msgs_index, data->msgs_count,
		msg->flags & I2C_MSG_READ ? "read" : "write",
		msg->len, msg->len > 1 ? "s" : "",
		msg->flags & I2C_MSG_READ ? "from" : "to",
		data->addr);

	data->master_xfer_cnt = 0;
	data->buf_index = 0;

	if (msg->flags & I2C_MSG_READ) {
		cmd |= AST_I2CM_RX_CMD;
		if(config->mode == DMA_MODE) {
			//dma mode 
			cmd |= AST_I2CM_RX_DMA_EN;

			if(msg->flags & I2C_MSG_RECV_LEN) {
				LOG_DBG("smbus read \n");
				xfer_len = 1;
			} else {
	 			if (msg->len > ASPEED_I2C_DMA_SIZE) {
					xfer_len = ASPEED_I2C_DMA_SIZE;
				} else {
					xfer_len = msg->len;
					if(data->msgs_index + 1 == data->msgs_count) {
						LOG_DBG("last stop \n");
						cmd |= AST_I2CM_RX_CMD_LAST | AST_I2CM_STOP_CMD;
					}
				}
			}
			sys_write32(AST_I2CM_SET_RX_DMA_LEN(xfer_len - 1), i2c_base + AST_I2CM_DMA_LEN);
			sys_write32((uint32_t)msg->buf, i2c_base + AST_I2CM_RX_DMA);
		} else if (config->mode == BUFF_MODE) {
			//buff mode
			cmd |= AST_I2CM_RX_BUFF_EN;
			if(msg->flags & I2C_MSG_RECV_LEN) {
				LOG_DBG("smbus read \n");
				xfer_len = 1;
			} else {
				if (msg->len > config->buf_size) {
					xfer_len = config->buf_size;
				} else {
					xfer_len = msg->len;
					if(data->msgs_index + 1 == data->msgs_count) {
						LOG_DBG("last stop \n");
						cmd |= AST_I2CM_RX_CMD_LAST | AST_I2CM_STOP_CMD;
					}
				}
			}
			sys_write32(AST_I2CC_SET_RX_BUF_LEN(xfer_len), i2c_base + AST_I2CC_BUFF_CTRL);
		} else {
			//byte mode
			xfer_len = 1;
			if(msg->flags & I2C_MSG_RECV_LEN) {
				LOG_DBG("smbus read \n");
			} else {
				if((data->msgs_index + 1 == data->msgs_count) && (msg->len == 1)) {
					LOG_DBG("last stop \n");
					cmd |= AST_I2CM_RX_CMD_LAST | AST_I2CM_STOP_CMD;
				} 
			}
		}
	} else {
		if(config->mode == DMA_MODE) {
			//dma mode
			if (msg->len > ASPEED_I2C_DMA_SIZE) {
				xfer_len = ASPEED_I2C_DMA_SIZE;
			} else {
				if(data->msgs_index + 1 == data->msgs_count) {
					LOG_DBG("with P \n");
					cmd |= AST_I2CM_STOP_CMD;
				}
				xfer_len = msg->len;
			}

			if(xfer_len) {
				cmd |= AST_I2CM_TX_DMA_EN | AST_I2CM_TX_CMD;
				sys_write32(AST_I2CM_SET_TX_DMA_LEN(xfer_len - 1), i2c_base + AST_I2CM_DMA_LEN);
				sys_write32((uint32_t)msg->buf, i2c_base + AST_I2CM_TX_DMA);
			}
		} else if (config->mode == BUFF_MODE) {
			uint8_t wbuf[4];
			//buff mode
			if (msg->len > config->buf_size) {
				xfer_len = config->buf_size;
			} else {
				if(data->msgs_index + 1 == data->msgs_count) {
					LOG_DBG("with stop \n");
					cmd |= AST_I2CM_STOP_CMD;
				}
				xfer_len = msg->len;
			}
			if(xfer_len) {
				cmd |= AST_I2CM_TX_BUFF_EN | AST_I2CM_TX_CMD;
				sys_write32(AST_I2CC_SET_TX_BUF_LEN(xfer_len), i2c_base + AST_I2CC_BUFF_CTRL);
				for(i = 0; i < xfer_len; i++) {
					wbuf[i % 4] = msg->buf[i];
					if (i % 4 == 3)
						sys_write32(*(uint32_t *)wbuf,
							   config->buf_base + i - 3);
					LOG_DBG("[%02x] \n", msg->buf[i]);
				}
				if (--i % 4 != 3)
					sys_write32(*(uint32_t *)wbuf,
						   config->buf_base + i - (i % 4));
				
			}
		} else {
			//byte mode
			if((data->msgs_index + 1 == data->msgs_count) && (msg->len <= 1)) {
				LOG_DBG("with stop \n");
				cmd |= AST_I2CM_STOP_CMD;
			}

			if(msg->len) {
				cmd |= AST_I2CM_TX_CMD;
				xfer_len = 1;
				LOG_DBG("w [0] : %02x \n", msg->buf[0]);
				sys_write32(msg->buf[0], i2c_base + AST_I2CC_STS_AND_BUFF);
			} else
				xfer_len = 0;
		}
	}
	LOG_DBG("len %d , cmd %x \n", xfer_len, cmd);
	sys_write32(cmd, i2c_base + AST_I2CM_CMD_STS);

}

static int i2c_aspeed_transfer(const struct device *dev, struct i2c_msg *msgs,
			    uint8_t num_msgs, uint16_t addr)
{
	struct i2c_aspeed_config *config = DEV_CFG(dev);
	struct i2c_aspeed_data *data = DEV_DATA(dev);
	uint8_t i2c_base = DEV_BASE(dev);
	if (!num_msgs) {
		return 0;
	}

	/* If bus is busy in a single master environment, attempt recovery. */
	if (!config->multi_master && 
		(sys_read32(i2c_base + AST_I2CC_STS_AND_BUFF) & AST_I2CC_BUS_BUSY_STS)) {
		int ret;
		ret = aspeed_new_i2c_recover_bus(dev);
		if (ret)
			return ret;
	}

	data->addr = addr;
	data->cmd_err = 0;
	data->msgs = msgs;
	data->msgs_index = 0;
	data->msgs_count = num_msgs;
	k_sem_reset(&data->sync_sem);

	aspeed_new_i2c_do_start(dev);
	if (i2c_wait_completion(dev)) {
		/*
		 * If timed out and bus is still busy in a multi master
		 * environment, attempt recovery at here.
		 */
		if (data->multi_master &&
			(sys_read32(i2c_base + AST_I2CC_STS_AND_BUFF) & AST_I2CC_BUS_BUSY_STS))
				aspeed_new_i2c_recover_bus(dev);
		return -ETIMEDOUT;
	}

	LOG_DBG(" end %d \n", data->cmd_err);

	return data->cmd_err;

}

static int aspeed_i2c_is_irq_error(uint32_t irq_status)
{
        if (irq_status & AST_I2CM_ARBIT_LOSS)
                return -EAGAIN;
        if (irq_status & (AST_I2CM_SDA_DL_TO |
                          AST_I2CM_SCL_LOW_TO))
                return -EBUSY;
        if (irq_status & (AST_I2CM_ABNORMAL))
                return -EPROTO;

        return 0;
}

int aspeed_i2c_master_irq(const struct device *dev)
{
	struct i2c_aspeed_config *config = DEV_CFG(dev);
	struct i2c_aspeed_data *data = DEV_DATA(dev);
	uint32_t i2c_base = DEV_BASE(dev);
	uint32_t sts = sys_read32(i2c_base + AST_I2CM_ISR);
	struct i2c_msg *msg = &data->msgs[data->msgs_index];
	uint32_t cmd = AST_I2CM_PKT_EN;
	int xfer_len;
	int i;

	LOG_DBG("M sts %x\n", sts);

	if(!data->alert_enable)
		sts &= ~AST_I2CM_SMBUS_ALT;

	if (AST_I2CM_BUS_RECOVER_FAIL & sts) {
		LOG_DBG("AST_I2CM_BUS_RECOVER_FAIL \n");
		LOG_DBG("M clear isr: AST_I2CM_BUS_RECOVER_FAIL= %x\n", sts);
		sys_write32(AST_I2CM_BUS_RECOVER_FAIL, i2c_base + AST_I2CM_ISR);
		if (data->bus_recover) {
			data->cmd_err = -EPROTO;
			data->bus_recover = 0;
			k_sem_give(&data->sync_sem);
		} else {
			LOG_DBG("Error !! Bus revover\n");
		}
		return 1;
	}
	
	if (AST_I2CM_BUS_RECOVER & sts) {
		LOG_DBG("M clear isr: AST_I2CM_BUS_RECOVER= %x\n", sts);
		sys_write32(AST_I2CM_BUS_RECOVER, i2c_base + AST_I2CM_ISR);
		data->cmd_err = 0;
		if (data->bus_recover) {
			data->bus_recover = 0;
			k_sem_give(&data->sync_sem);
		} else {
			LOG_DBG("Error !! Bus revover\n");
		}
		return 1;
	}

	if (AST_I2CM_SMBUS_ALT & sts) {
		if (sys_read32(i2c_base + AST_I2CM_IER) & AST_I2CM_SMBUS_ALT) {
			LOG_DBG("AST_I2CM_SMBUS_ALT 0x%02x\n", sts);
			LOG_DBG("M clear isr: AST_I2CM_SMBUS_ALT= %x\n", sts);
			//Disable ALT INT
			sys_write32(sys_read32(i2c_base + i2c_base + AST_I2CM_IER) &
					  ~AST_I2CM_SMBUS_ALT,
					  AST_I2CM_IER);
//			i2c_handle_smbus_alert(data->ara);
			sys_write32(AST_I2CM_SMBUS_ALT, i2c_base + AST_I2CM_ISR);
			LOG_DBG("TODO aspeed_master_alert_recv bus id Disable Alt, Please Imple \n");
			return 1;
		} else 
			sts &= ~AST_I2CM_SMBUS_ALT;
	}

	data->cmd_err = aspeed_i2c_is_irq_error(sts);
	if(data->cmd_err) {
		LOG_DBG("received error interrupt: 0x%02x\n",
				sts);
		sys_write32(AST_I2CM_PKT_DONE, i2c_base + AST_I2CM_ISR);
		k_sem_give(&data->sync_sem);
		return 1;
	}

	if (AST_I2CM_PKT_DONE & sts) {
		sts &= ~AST_I2CM_PKT_DONE;
		sys_write32( AST_I2CM_PKT_DONE, i2c_base + AST_I2CM_ISR);
		switch (sts) {
			case AST_I2CM_PKT_ERROR | AST_I2CM_TX_NAK:	//a0 fix for issue
//				LOG_DBG("a0 workaround for M TX NAK [%x]\n", sys_read32(i2c_base + AST_I2CC_STS_AND_BUFF));
			case AST_I2CM_PKT_ERROR | AST_I2CM_TX_NAK | AST_I2CM_NORMAL_STOP:
				LOG_DBG("M : TX NAK | NORMAL STOP \n");
				data->cmd_err = -ENXIO;
				k_sem_give(&data->sync_sem);
				break;
			case AST_I2CM_NORMAL_STOP:
				//write 0 byte only have stop isr
				LOG_DBG("M clear isr: AST_I2CM_NORMAL_STOP = %x\n", sts);
//				sys_write32(AST_I2CM_NORMAL_STOP, i2c_base + AST_I2CM_ISR);
				data->msgs_index++;
				k_sem_give(&data->sync_sem);
				break;
			case AST_I2CM_TX_ACK:
//				LOG_DBG("M : AST_I2CM_TX_ACK = %x\n", sts);
			case AST_I2CM_TX_ACK | AST_I2CM_NORMAL_STOP:
				LOG_DBG("M : AST_I2CM_TX_ACK | AST_I2CM_NORMAL_STOP = %x\n", sts);
				if (config->mode == DMA_MODE) {
					xfer_len = AST_I2C_GET_TX_DMA_LEN(sys_read32(i2c_base + AST_I2CM_DMA_LEN_STS));
				} else if (config->mode == BUFF_MODE) {
					xfer_len = AST_I2CC_GET_TX_BUF_LEN(sys_read32(i2c_base + AST_I2CC_BUFF_CTRL));
				} else {
					xfer_len = 1;
				}
				data->master_xfer_cnt += xfer_len;

				if (data->master_xfer_cnt == msg->len) {
					data->msgs_index++;
					if(data->msgs_index == data->msgs_count) {
						k_sem_give(&data->sync_sem);
					} else
						aspeed_new_i2c_do_start(dev);
				} else {
					//do next tx
					cmd |= AST_I2CM_TX_CMD;
					if (config->mode == DMA_MODE) {
						cmd |= AST_I2CS_TX_DMA_EN;
						xfer_len = msg->len - data->master_xfer_cnt;
						if(xfer_len > ASPEED_I2C_DMA_SIZE) {
							xfer_len = ASPEED_I2C_DMA_SIZE;
						} else {
							if(data->msgs_index + 1 == data->msgs_count) {
								LOG_DBG("M: STOP \n");
								cmd |= AST_I2CM_STOP_CMD;
							}
						}
						sys_write32(AST_I2CM_SET_TX_DMA_LEN(xfer_len - 1), i2c_base + AST_I2CM_DMA_LEN);
						LOG_DBG("next tx xfer_len: %d, offset %d \n", xfer_len, data->master_xfer_cnt);
						sys_write32((uint32_t)(data->msgs->buf + data->master_xfer_cnt), i2c_base + AST_I2CM_TX_DMA);
					} else if (config->mode == BUFF_MODE) {
						uint8_t wbuf[4];
						cmd |= AST_I2CS_RX_BUFF_EN;
						xfer_len = msg->len - data->master_xfer_cnt;
						if(xfer_len > config->buf_size) {
							xfer_len = config->buf_size;
						} else {
							if(data->msgs_index + 1 == data->msgs_count) {
								LOG_DBG("M: STOP \n");
								cmd |= AST_I2CM_STOP_CMD;
							}
						}
						for(i = 0; i < xfer_len; i++) {
							wbuf[i % 4] = msg->buf[data->master_xfer_cnt + i];
							if (i % 4 == 3)
								sys_write32(*(uint32_t *)wbuf,
									   config->buf_base + i - 3);
							LOG_DBG("[%02x] \n", msg->buf[data->master_xfer_cnt + i]);
						}
						if (--i % 4 != 3)
							sys_write32(*(uint32_t *)wbuf,
								   config->buf_base + i - (i % 4));
						
						sys_write32(AST_I2CC_SET_TX_BUF_LEN(xfer_len), i2c_base + AST_I2CC_BUFF_CTRL);
					} else {
						//byte
						if((data->msgs_index + 1 == data->msgs_count) && ((data->master_xfer_cnt + 1) == msg->len)) {
							LOG_DBG("M: STOP \n");
							cmd |= AST_I2CM_STOP_CMD;
						}
						LOG_DBG("tx buff[%x] \n", msg->buf[data->master_xfer_cnt]);
						sys_write32(msg->buf[data->master_xfer_cnt], i2c_base + AST_I2CC_STS_AND_BUFF);
					}
					LOG_DBG("next tx cmd: %x\n", cmd);
					sys_write32(cmd, i2c_base + AST_I2CM_CMD_STS);
				}
				break;
			case AST_I2CM_RX_DONE:
//				LOG_DBG("M : AST_I2CM_RX_DONE = %x\n", sts);
			case AST_I2CM_RX_DONE | AST_I2CM_NORMAL_STOP:
				LOG_DBG("M : AST_I2CM_RX_DONE | AST_I2CM_NORMAL_STOP = %x\n", sts);
				//do next rx
				if (config->mode == DMA_MODE) {
					xfer_len = AST_I2C_GET_RX_DMA_LEN(sys_read32(i2c_base + AST_I2CM_DMA_LEN_STS));
				} else if (config->mode == BUFF_MODE) {
					xfer_len = AST_I2CC_GET_RX_BUF_LEN(sys_read32(i2c_base + AST_I2CC_BUFF_CTRL));
					for(i = 0; i < xfer_len; i++)
						msg->buf[data->master_xfer_cnt + i] = sys_read8(config->buf_base + i);
				} else {
					xfer_len = 1;
					msg->buf[data->master_xfer_cnt] = AST_I2CC_GET_RX_BUFF(sys_read32(i2c_base + AST_I2CC_STS_AND_BUFF));
				}

				if (msg->flags & I2C_MSG_RECV_LEN) {
					LOG_DBG("smbus first len = %x\n", msg->buf[0]);
					msg->flags &= ~I2C_MSG_RECV_LEN;
				}
				data->master_xfer_cnt += xfer_len;
				LOG_DBG("master_xfer_cnt [%d/%d] \n", data->master_xfer_cnt, msg->len);

				if (data->master_xfer_cnt == msg->len) {
					//TODO dma unmap
					/* Assure cache coherency after DMA write operation */
					aspeed_cache_invalid_data((uint32_t)msg->buf, (uint32_t)msg->len);
					for (i = 0; i < msg->len; i++) {
							 LOG_DBG("M: r %d:[%x] \n", i, msg->buf[i]);
					}
					data->msgs_index++;
					if(data->msgs_index == data->msgs_count) {
						k_sem_give(&data->sync_sem);
					} else
						aspeed_new_i2c_do_start(dev);
				} else {
					//next rx
					cmd |= AST_I2CM_RX_CMD;
					if (config->mode == DMA_MODE) {
						cmd |= AST_I2CM_RX_DMA_EN;
						xfer_len = msg->len - data->master_xfer_cnt;
						if(xfer_len > ASPEED_I2C_DMA_SIZE) {
							xfer_len = ASPEED_I2C_DMA_SIZE;
						} else {
							if(data->msgs_index + 1 == data->msgs_count) {
								LOG_DBG("last stop \n");
								cmd |= AST_I2CM_RX_CMD_LAST | AST_I2CM_STOP_CMD;
							}
						}
						LOG_DBG("M: next rx len [%d/%d] , cmd %x	\n", xfer_len, msg->len, cmd);
						sys_write32(AST_I2CM_SET_RX_DMA_LEN(xfer_len - 1), i2c_base + AST_I2CM_DMA_LEN);
						LOG_DBG("TODO check addr dma addr %x \n", (uint32_t)msg->buf);
						sys_write32((uint32_t) (msg->buf + data->master_xfer_cnt), i2c_base + AST_I2CM_RX_DMA);
					} else if (config->mode == BUFF_MODE) {
						cmd |= AST_I2CM_RX_BUFF_EN;
						xfer_len = msg->len - data->master_xfer_cnt;
						if(xfer_len > config->buf_size) {
							xfer_len = config->buf_size;
						} else {
							if(data->msgs_index + 1 == data->msgs_count) {
								LOG_DBG("last stop \n");
								cmd |= AST_I2CM_RX_CMD_LAST | AST_I2CM_STOP_CMD;
							}
						}
						sys_write32(AST_I2CC_SET_RX_BUF_LEN(xfer_len), i2c_base + AST_I2CC_BUFF_CTRL);
					} else {
						if((data->msgs_index + 1 == data->msgs_count) && 
							((data->master_xfer_cnt + 1) == msg->len)) {
							LOG_DBG("last stop \n");
							cmd |= AST_I2CM_RX_CMD_LAST | AST_I2CM_STOP_CMD;
						}
					}
					LOG_DBG("M: next rx len %d, cmd %x \n", xfer_len, cmd);
					sys_write32(cmd, i2c_base + AST_I2CM_CMD_STS);
				}
				break;
			default:
				LOG_DBG("TODO care -- > sts %x \n", sts);
//				sys_write32(sys_read32(i2c_base + AST_I2CM_ISR), i2c_base + AST_I2CM_ISR);
				break;
		}

		return 1;
	}

	if(sys_read32(i2c_base + AST_I2CM_ISR)) {
		LOG_DBG("TODO care -- > sts %x \n", sys_read32(i2c_base + AST_I2CM_ISR));
		sys_write32(sys_read32(i2c_base + AST_I2CM_ISR), i2c_base + AST_I2CM_ISR);
	}

	return 0;

}

#ifdef CONFIG_I2C_SLAVE
int aspeed_i2c_slave_irq(const struct device *dev)
{
	struct i2c_aspeed_config *config = DEV_CFG(dev);
	uint32_t i2c_base = DEV_BASE(dev);
	struct i2c_aspeed_data *data = DEV_DATA(dev);
	const struct i2c_slave_callbacks *slave_cb = data->slave_cfg->callbacks;
	uint32_t cmd = 0;
	int ret = 0;
	int i = 0;
	uint32_t sts = 0;
	uint8_t byte_data = 0;
	int slave_rx_len = 0;
	uint8_t value;

	LOG_DBG("aspeed_i2c_slave_irq \n");

	if((config->mode == DMA_MODE) || (config->mode == BUFF_MODE)) {
		if(!(sys_read32(i2c_base + AST_I2CS_ISR) & AST_I2CS_PKT_DONE))
			return 0;
	} 

	sts = sys_read32(i2c_base + AST_I2CS_ISR);
	if (!sts) return 0;

	sts &= ~AST_I2CS_ADDR_INDICAT_MASK;

	if (AST_I2CS_ADDR1_NAK & sts)
		sts &= ~AST_I2CS_ADDR1_NAK;

	if (AST_I2CS_ADDR2_NAK & sts)
		sts &= ~AST_I2CS_ADDR2_NAK;

	if (AST_I2CS_ADDR3_NAK & sts)
		sts &= ~AST_I2CS_ADDR3_NAK;

	if (AST_I2CS_ADDR_MASK & sts)
		sts &= ~AST_I2CS_ADDR_MASK;

	if (AST_I2CS_PKT_DONE & sts) {
		sts &= ~(AST_I2CS_PKT_DONE | AST_I2CS_PKT_ERROR);
		switch (sts) {
			case AST_I2CS_SLAVE_MATCH | AST_I2CS_STOP:
				LOG_DBG("S : Sw | P \n");
                if (slave_cb->stop) {
                        slave_cb->stop(data->slave_cfg);
                }
				cmd = AST_I2CS_ACTIVE_ALL | AST_I2CS_PKT_MODE_EN;
				if (config->mode == DMA_MODE) {
					cmd |= AST_I2CS_RX_DMA_EN;
					sys_write32(AST_I2CS_SET_RX_DMA_LEN(I2C_SLAVE_BUF_SIZE), i2c_base + AST_I2CS_DMA_LEN);
				} else if (config->mode == BUFF_MODE) {
					cmd |= AST_I2CS_RX_BUFF_EN;
					sys_write32(AST_I2CC_SET_RX_BUF_LEN(I2C_BUF_SIZE), i2c_base + AST_I2CC_BUFF_CTRL);
				} else {
					cmd &= ~AST_I2CS_PKT_MODE_EN;
				}
				sys_write32(cmd, i2c_base + AST_I2CS_CMD_STS);
				break;
			case AST_I2CS_SLAVE_MATCH | AST_I2CS_RX_DONE | AST_I2CS_Wait_RX_DMA | AST_I2CS_STOP:
//				LOG_DBG(data->dev, "S : Sw|D|P wait rx dma workaround \n");
			case AST_I2CS_RX_DONE | AST_I2CS_Wait_RX_DMA | AST_I2CS_STOP:
//				LOG_DBG(data->dev, "S : D|P wait rx dma workaround \n");
			case AST_I2CS_RX_DONE | AST_I2CS_STOP:
			case AST_I2CS_SLAVE_MATCH | AST_I2CS_RX_DONE | AST_I2CS_Wait_RX_DMA:
			case AST_I2CS_SLAVE_MATCH | AST_I2CS_RX_DONE | AST_I2CS_STOP:
				if(sts & AST_I2CS_STOP) {
					if(sts & AST_I2CS_SLAVE_MATCH)
						LOG_DBG("S : Sw|D|P \n");
					else
						LOG_DBG("S : D|P \n");
				} else
					LOG_DBG("S : Sw|D \n");

				if(sts & AST_I2CS_SLAVE_MATCH) {
	                if (slave_cb->write_requested) {
	                        slave_cb->write_requested(data->slave_cfg);
	                }
				}

				cmd = AST_I2CS_ACTIVE_ALL | AST_I2CS_PKT_MODE_EN;
				if (config->mode == DMA_MODE) {
					cmd |= AST_I2CS_RX_DMA_EN;
					slave_rx_len = AST_I2C_GET_RX_DMA_LEN(sys_read32(i2c_base + AST_I2CS_DMA_LEN_STS));
					for (i = 0; i < slave_rx_len; i++) {
//						LOG_DBG(data->dev, "[%02x]", data->slave_dma_buf[i]);
						if (slave_cb->write_received) {
								slave_cb->write_received(data->slave_cfg, data->slave_dma_buf[i]);
						}
					}
					sys_write32(AST_I2CS_SET_RX_DMA_LEN(I2C_SLAVE_BUF_SIZE), i2c_base + AST_I2CS_DMA_LEN);
				} else if (config->mode == BUFF_MODE) {
					cmd |= AST_I2CS_RX_BUFF_EN;
					slave_rx_len = AST_I2CC_GET_RX_BUF_LEN(sys_read32(i2c_base + AST_I2CC_BUFF_CTRL));
					for (i = 0; i < slave_rx_len; i++) {
//						LOG_DBG("[%02x]", value);
						if (slave_cb->write_received) {
								slave_cb->write_received(data->slave_cfg, sys_read8(config->buf_base + i));
						}
					}
					sys_write32(AST_I2CC_SET_RX_BUF_LEN(config->buf_size), i2c_base + AST_I2CC_BUFF_CTRL);
				} else {
					cmd &= ~AST_I2CS_PKT_MODE_EN;
					byte_data = AST_I2CC_GET_RX_BUFF(sys_read32(i2c_base + AST_I2CC_STS_AND_BUFF));
					LOG_DBG("[%02x]", byte_data);
					if (slave_cb->write_received) {
							slave_cb->write_received(data->slave_cfg, byte_data);
					}
				}
				if(sts & AST_I2CS_STOP)
					if (slave_cb->stop) {
							slave_cb->stop(data->slave_cfg);
					}

				sys_write32(cmd, i2c_base + AST_I2CS_CMD_STS);
				break;
			//it is Mw data Mr coming -> it need send tx
			case AST_I2CS_RX_DONE | AST_I2CS_Wait_TX_DMA:
			case AST_I2CS_SLAVE_MATCH | AST_I2CS_RX_DONE | AST_I2CS_Wait_TX_DMA:
				//it should be repeat start read 
				if(sts & AST_I2CS_SLAVE_MATCH)
					LOG_DBG("S: AST_I2CS_Wait_TX_DMA | AST_I2CS_SLAVE_MATCH | AST_I2CS_RX_DONE\n");
				else
					LOG_DBG("S: AST_I2CS_Wait_TX_DMA | AST_I2CS_RX_DONE\n");

				if(sts & AST_I2CS_SLAVE_MATCH)
	                if (slave_cb->write_requested)
						slave_cb->write_requested(data->slave_cfg);

				cmd = AST_I2CS_ACTIVE_ALL | AST_I2CS_PKT_MODE_EN;
				if (config->mode == DMA_MODE) {
					cmd |= AST_I2CS_TX_DMA_EN;
					slave_rx_len = AST_I2C_GET_RX_DMA_LEN(sys_read32(i2c_base + AST_I2CS_DMA_LEN_STS));
					for (i = 0; i < slave_rx_len; i++) {
						LOG_DBG("rx [%02x]", data->slave_dma_buf[i]);
						if (slave_cb->write_received) {
								slave_cb->write_received(data->slave_cfg, data->slave_dma_buf[i]);
						}
					}
					if (slave_cb->read_requested) {
							slave_cb->read_requested(data->slave_cfg, &data->slave_dma_buf[i]);
					}
					LOG_DBG("tx : [%02x]", data->slave_dma_buf[0]);
					sys_write32(0, i2c_base + AST_I2CS_DMA_LEN_STS);
					sys_write32((uint32_t)data->slave_dma_buf, i2c_base + AST_I2CS_TX_DMA);
					sys_write32(AST_I2CS_SET_TX_DMA_LEN(1), i2c_base + AST_I2CS_DMA_LEN);
				} else if (config->mode == BUFF_MODE) {
					cmd |= AST_I2CS_TX_BUFF_EN;
					slave_rx_len = AST_I2CC_GET_RX_BUF_LEN(sys_read32(i2c_base + AST_I2CC_BUFF_CTRL));
					for (i = 0; i < slave_rx_len; i++) {
						if (slave_cb->write_received) {
								slave_cb->write_received(data->slave_cfg, sys_read8(config->buf_base + i));
						}
					}
					if (slave_cb->read_requested) {
							slave_cb->read_requested(data->slave_cfg, &value);
					}
					LOG_DBG("tx : [%02x]", value);
					sys_write8(value, config->buf_base);
					sys_write32(AST_I2CC_SET_TX_BUF_LEN(1), i2c_base + AST_I2CC_BUFF_CTRL);
				} else {
					cmd &= ~AST_I2CS_PKT_MODE_EN;
					cmd |= AST_I2CS_TX_CMD;
					byte_data = AST_I2CC_GET_RX_BUFF(sys_read32(i2c_base + AST_I2CC_STS_AND_BUFF));
					LOG_DBG("rx : [%02x]", byte_data);
					if (slave_cb->write_received) {
							slave_cb->write_received(data->slave_cfg, byte_data);
					}
					if (slave_cb->read_requested) {
							slave_cb->read_requested(data->slave_cfg, &byte_data);
					}
					LOG_DBG("tx : [%02x]", byte_data);
					sys_write32(byte_data, i2c_base + AST_I2CC_STS_AND_BUFF);
				}
				LOG_DBG("slave cmd %x \n", cmd);
				sys_write32(cmd, i2c_base + AST_I2CS_CMD_STS);
				break;
				case AST_I2CS_SLAVE_MATCH | AST_I2CS_Wait_TX_DMA:
					//First Start read
					LOG_DBG("S: AST_I2CS_SLAVE_MATCH | AST_I2CS_Wait_TX_DMA\n");
					cmd = AST_I2CS_ACTIVE_ALL | AST_I2CS_PKT_MODE_EN;
					if (config->mode == DMA_MODE) {
						cmd |= AST_I2CS_TX_DMA_EN;
						if (slave_cb->read_requested) {
								slave_cb->read_requested(data->slave_cfg, &data->slave_dma_buf[0]);
						}
#if 1	//currently i2c slave framework only support one byte request. 
						LOG_DBG("tx: [%x]\n", data->slave_dma_buf[0]);
						sys_write32(AST_I2CS_SET_TX_DMA_LEN(1), i2c_base + AST_I2CS_DMA_LEN);
#else
						LOG_DBG("ssif tx len: [%x]\n", data->slave_dma_buf[0]);
						for( i = 1; i < data->slave_dma_buf[0] + 1; i++) {
							i2c_slave_event(data->slave, I2C_SLAVE_READ_PROCESSED, &data->slave_dma_buf[i]);
						}
						sys_write32(AST_I2CS_SET_TX_DMA_LEN(data->slave_dma_buf[0]), i2c_base + AST_I2CS_DMA_LEN);
#endif
					} else if (config->mode == BUFF_MODE) {
						cmd |= AST_I2CS_TX_BUFF_EN;
						if (slave_cb->read_requested) {
								slave_cb->read_requested(data->slave_cfg, &byte_data);
						}
#if 1				//currently i2c slave framework only support one byte request. 
						LOG_DBG("tx : [%02x]", byte_data);
						sys_write8(byte_data, config->buf_base);
						sys_write32(AST_I2CC_SET_TX_BUF_LEN(1), i2c_base + AST_I2CC_BUFF_CTRL);
#else
						uint8_t wbuf[4];
						LOG_DBG("ssif tx len: [%x]\n", byte_data);
						for (i = 1; i < byte_data + 1; i++) {
							i2c_slave_event(data->slave, I2C_SLAVE_READ_PROCESSED, &value);
							wbuf[i % 4] = data[i];
							if (i % 4 == 3)
								sys_write32(*(uint32_t *)wbuf,
									   data->buf_base + i - 3);
						}
						if (--i % 4 != 3)
							sys_write32(*(uint32_t *)wbuf,
								   data->buf_base + i - (i % 4));
						sys_write32(AST_I2CC_SET_TX_BUF_LEN(byte_data), AST_I2CC_BUFF_CTRL);
#endif
					} else {
						cmd &= ~AST_I2CS_PKT_MODE_EN;
						cmd |= AST_I2CS_TX_CMD;
						if (slave_cb->read_requested) {
								slave_cb->read_requested(data->slave_cfg, &byte_data);
						}
						sys_write32(byte_data, i2c_base + AST_I2CC_STS_AND_BUFF);
					}
					sys_write32(cmd, i2c_base + AST_I2CS_CMD_STS);
					break;	
				case AST_I2CS_Wait_TX_DMA:
					//it should be next start read 
					LOG_DBG("S: AST_I2CS_Wait_TX_DMA \n");
					cmd = AST_I2CS_ACTIVE_ALL | AST_I2CS_PKT_MODE_EN;
					if (config->mode == DMA_MODE) {
						cmd |= AST_I2CS_TX_DMA_EN;
						if (slave_cb->read_processed) {
								slave_cb->read_processed(data->slave_cfg, &data->slave_dma_buf[0]);
						}
						LOG_DBG("tx : [%02x]", data->slave_dma_buf[0]);
						sys_write32(0, i2c_base + AST_I2CS_DMA_LEN_STS);
						sys_write32(AST_I2CS_SET_TX_DMA_LEN(1), i2c_base + AST_I2CS_DMA_LEN);
					} else if (config->mode == BUFF_MODE) {	
						cmd |= AST_I2CS_TX_BUFF_EN;
						if (slave_cb->read_processed) {
								slave_cb->read_processed(data->slave_cfg, &value);
						}
						LOG_DBG("tx: [%02x]\n", value);
						sys_write8(value, config->buf_base);
						sys_write32(AST_I2CC_SET_TX_BUF_LEN(1), i2c_base + AST_I2CC_BUFF_CTRL);
					} else {
						cmd &= ~AST_I2CS_PKT_MODE_EN;
						cmd |= AST_I2CS_TX_CMD;
						if (slave_cb->read_processed) {
								slave_cb->read_processed(data->slave_cfg, &byte_data);
						}
						LOG_DBG("tx: [%02x]\n", byte_data);
						sys_write32(byte_data, i2c_base + AST_I2CC_STS_AND_BUFF);					
					}
					sys_write32(cmd, i2c_base + AST_I2CS_CMD_STS);
					break;
				case AST_I2CS_TX_NAK | AST_I2CS_STOP:
					//it just tx complete
					LOG_DBG("S: AST_I2CS_TX_NAK | AST_I2CS_STOP \n");
					cmd = AST_I2CS_ACTIVE_ALL | AST_I2CS_PKT_MODE_EN;
					if (slave_cb->stop) {
							slave_cb->stop(data->slave_cfg);
					}
					if (config->mode == DMA_MODE) {
						cmd |= AST_I2CS_RX_DMA_EN;
						sys_write32(0, i2c_base + AST_I2CS_DMA_LEN_STS);
						sys_write32(AST_I2CS_SET_RX_DMA_LEN(I2C_SLAVE_BUF_SIZE), i2c_base + AST_I2CS_DMA_LEN);
					} else if (config->mode == BUFF_MODE) {
						cmd |= AST_I2CS_RX_BUFF_EN;
						sys_write32(AST_I2CC_SET_RX_BUF_LEN(config->buf_size), i2c_base + AST_I2CC_BUFF_CTRL);
					} else {
						cmd &= ~AST_I2CS_PKT_MODE_EN;
					}
					sys_write32(cmd, i2c_base + AST_I2CS_CMD_STS);
					break;
#if 0
				// it is repeat start write coming
				case AST_I2CS_RX_DONE | AST_I2CS_Wait_TX_DMA:
					LOG_DBG("S: AST_I2CS_RX_DONE | AST_I2CS_Wait_TX_DMA TODO\n");
					slave_rx_len = AST_I2C_GET_RX_DMA_LEN(sys_read32(i2c_base + AST_I2CS_DMA_LEN_STS));
					LOG_DBG("S: rx len %d \n", slave_rx_len);
					for (i = 0; i < slave_rx_len; i++) {
						LOG_DBG("[%x]", data->slave_dma_buf[i]);
						i2c_slave_event(data->slave, I2C_SLAVE_WRITE_RECEIVED, &data->slave_dma_buf[i]);
					}
					sys_write32(AST_I2CS_SET_TX_DMA_LEN(1), i2c_base + AST_I2CS_DMA_LEN);
					sys_write32(sys_read32(i2c_base + AST_I2CS_CMD_STS) | AST_I2CS_RX_DMA_EN | AST_I2CS_TX_BUFF_EN, i2c_base + AST_I2CS_CMD_STS);
					break;
#endif
				default:
					LOG_DBG("TODO slave sts case %x, now %x \n", sts, sys_read32(i2c_base + AST_I2CS_ISR));
					break;

		}
		sys_write32(AST_I2CS_PKT_DONE, i2c_base + AST_I2CS_ISR);
		ret = 1;
	} else {
		LOG_DBG("byte mode todo check \n");
		//only coming for byte mode 
		cmd = AST_I2CS_ACTIVE_ALL;
		switch (sts) {
			case AST_I2CS_SLAVE_MATCH | AST_I2CS_RX_DONE | AST_I2CS_Wait_RX_DMA:
				LOG_DBG("S : Sw|D \n");
				//first address match is address 
				byte_data = AST_I2CC_GET_RX_BUFF(sys_read32(i2c_base + AST_I2CC_STS_AND_BUFF));
				LOG_DBG("addr [%x]", byte_data);
				break;
			case AST_I2CS_RX_DONE | AST_I2CS_Wait_RX_DMA:
				LOG_DBG("S : D \n");
				byte_data = AST_I2CC_GET_RX_BUFF(sys_read32(i2c_base + AST_I2CC_STS_AND_BUFF));
				LOG_DBG("rx [%x]", byte_data);

				break;
			case AST_I2CS_SLAVE_MATCH | AST_I2CS_RX_DONE | AST_I2CS_Wait_TX_DMA:
				cmd |= AST_I2CS_TX_CMD;
				LOG_DBG("S : Sr|D \n");
				byte_data = AST_I2CC_GET_RX_BUFF(sys_read32(i2c_base + AST_I2CC_STS_AND_BUFF));
				LOG_DBG("addr : [%02x]", byte_data);
//				i2c_slave_event(obj, I2C_SLAVE_READ_REQUESTED);
				LOG_DBG("tx: [%02x]\n", byte_data);
				sys_write32(byte_data, i2c_base + AST_I2CC_STS_AND_BUFF);
				break;
			case AST_I2CS_TX_ACK | AST_I2CS_Wait_TX_DMA:
				cmd |= AST_I2CS_TX_CMD;
				LOG_DBG("S : D \n");
//				i2c_slave_event(obj, I2C_SLAVE_READ_PROCESSED);
				LOG_DBG("tx: [%02x]\n", byte_data);
				sys_write32(byte_data, i2c_base + AST_I2CC_STS_AND_BUFF);
				break;
			case AST_I2CS_STOP:
			case AST_I2CS_STOP | AST_I2CS_TX_NAK:
				LOG_DBG("S : P \n");
//				i2c_slave_event(obj, I2C_SLAVE_STOP);
				break;
			default:
				LOG_DBG("TODO no pkt_done intr ~~~ ***** sts %x \n", sts);
				break;
		}
		sys_write32(cmd, i2c_base + AST_I2CS_CMD_STS);
		sys_write32(sts, i2c_base + AST_I2CS_ISR);
		ret = 1;
	}

	return ret;

}
#endif

static void i2c_aspeed_isr(const struct device *dev)
{
	uint32_t i2c_base = DEV_BASE(dev);

	if(sys_read32(i2c_base + AST_I2CC_FUN_CTRL) & AST_I2CC_SLAVE_EN) {
		if(aspeed_i2c_slave_irq(dev)) {
			return;
		}
	}

	aspeed_i2c_master_irq(dev);
}

static int i2c_aspeed_init(const struct device *dev)
{
	struct i2c_aspeed_config *config = DEV_CFG(dev);
	struct i2c_aspeed_data *data = DEV_DATA(dev);
	const struct device *clock_dev = device_get_binding(ASPEED_CLK_CTRL_NAME);
	uint32_t i2c_base = DEV_BASE(dev);
	uint32_t bitrate_cfg;
	int error;

	k_sem_init(&data->sync_sem, 0, UINT_MAX);

	config->global_reg = i2c_base & 0xfffff000;

	//get global control register
	if (sys_read32(config->global_reg + ASPEED_I2CG_CTRL) & ASPEED_I2CG_CTRL_NEW_CLK_DIV)
		config->clk_div_mode = 1;
	
	config->multi_master = 0;
	config->mode = DMA_MODE;
	clock_control_get_rate(clock_dev, config->clk_id, &config->clk_src);
printk("================================== \n");
	LOG_DBG("clk src %d, div mode %d, multi-master %d, xfer mode %d\n", 
			config->clk_src, config->clk_div_mode, config->multi_master, config->mode);

	bitrate_cfg = i2c_map_dt_bitrate(config->bitrate);

	error = i2c_aspeed_configure(dev, I2C_MODE_MASTER | bitrate_cfg);
	if (error) {
		return error;
	}

	config->irq_config_func(dev);

	return 0;
}

#ifdef CONFIG_I2C_SLAVE
static int i2c_aspeed_slave_register(const struct device *dev,
				  struct i2c_slave_config *config)
{
	struct i2c_aspeed_config *i2c_config = DEV_CFG(dev);
	struct i2c_aspeed_data *data = dev->data;
	uint32_t i2c_base = DEV_BASE(dev);	
	uint32_t cmd = AST_I2CS_ACTIVE_ALL | AST_I2CS_PKT_MODE_EN;

	if (!config) {
		return -EINVAL;
	}

	data->slave_cfg = config;

	LOG_DBG(" [%x] \n", config->address);

	/* Set slave addr. */
	sys_write32(config->address | 
			(sys_read32(i2c_base + AST_I2CS_ADDR_CTRL) & ~AST_I2CS_ADDR1_MASK), 
			i2c_base + AST_I2CS_ADDR_CTRL);

	//trigger rx buffer
	if(i2c_config->mode == DMA_MODE) {
		cmd |= AST_I2CS_RX_DMA_EN;
		sys_write32((uint32_t)data->slave_dma_buf, i2c_base + AST_I2CS_RX_DMA);
		sys_write32(AST_I2CS_SET_RX_DMA_LEN(I2C_SLAVE_BUF_SIZE), i2c_base + AST_I2CS_DMA_LEN);
	} else if (i2c_config->mode == BUFF_MODE) {
		cmd |= AST_I2CS_RX_BUFF_EN;
		sys_write32(AST_I2CC_SET_RX_BUF_LEN(i2c_config->buf_size), i2c_base + AST_I2CC_BUFF_CTRL);
	} else {
		cmd &= ~AST_I2CS_PKT_MODE_EN;
	}

	sys_write32(AST_I2CS_AUTO_NAK_EN, i2c_base + AST_I2CS_CMD_STS);
	sys_write32(AST_I2CC_SLAVE_EN | sys_read32(i2c_base + AST_I2CC_FUN_CTRL), i2c_base + AST_I2CC_FUN_CTRL);
	sys_write32(cmd, i2c_base + AST_I2CS_CMD_STS);

	data->slave_attached = true;

	return 0;
}

static int i2c_aspeed_slave_unregister(const struct device *dev,
				    struct i2c_slave_config *config)
{
	struct i2c_aspeed_data *data = dev->data;
	uint32_t i2c_base = DEV_BASE(dev);	

	if (!data->slave_attached) {
		return -EINVAL;
	}

	LOG_DBG(" [%x]\n", config->address);

	/* Turn off slave mode. */
	sys_write32(~AST_I2CC_SLAVE_EN & sys_read32(i2c_base + AST_I2CC_FUN_CTRL), i2c_base + AST_I2CC_FUN_CTRL);
	sys_write32(sys_read32(i2c_base + AST_I2CS_ADDR_CTRL) & ~AST_I2CS_ADDR1_MASK, i2c_base + AST_I2CS_ADDR_CTRL);

	data->slave_attached = false;

	return 0;
}
#endif

static const struct i2c_driver_api i2c_aspeed_driver_api = {
	.configure = i2c_aspeed_configure,
	.transfer = i2c_aspeed_transfer,
#ifdef CONFIG_I2C_SLAVE
	.slave_register = i2c_aspeed_slave_register,
	.slave_unregister = i2c_aspeed_slave_unregister,
#endif
	
};

#define I2C_ASPEED_INIT(n)							\
	static void i2c_aspeed_config_func_##n(const struct device *dev);	\
									\
	static const struct i2c_aspeed_config i2c_aspeed_config_##n = {	\
		.base = DT_INST_REG_ADDR(n),		\
		.irq_config_func = i2c_aspeed_config_func_##n,		\
		.bitrate = DT_INST_PROP(n, clock_frequency),		\
		.clk_id = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, clk_id), \
	};								\
									\
	static struct i2c_aspeed_data i2c_aspeed_data_##n;			\
									\
	DEVICE_DT_INST_DEFINE(n,					\
				&i2c_aspeed_init,				\
				device_pm_control_nop,			\
				&i2c_aspeed_data_##n, &i2c_aspeed_config_##n,	\
				POST_KERNEL,				\
				CONFIG_KERNEL_INIT_PRIORITY_DEVICE,	\
				&i2c_aspeed_driver_api);			\
									\
	static void i2c_aspeed_config_func_##n(const struct device *dev)	\
	{								\
		ARG_UNUSED(dev);					\
									\
		IRQ_CONNECT(DT_INST_IRQN(n),				\
			    DT_INST_IRQ(n, priority),			\
			    i2c_aspeed_isr, DEVICE_DT_INST_GET(n), 0);	\
									\
		irq_enable(DT_INST_IRQN(n));				\
	}

DT_INST_FOREACH_STATUS_OKAY(I2C_ASPEED_INIT)