/*
 * Renesas System Solutions Asia Pte. Ltd - Migo-R
 *
 * Copyright (C) 2008 Magnus Damm
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/mtd/physmap.h>
#include <linux/mtd/nand.h>
#include <linux/i2c.h>
#include <linux/smc91x.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <media/soc_camera_platform.h>
#include <media/sh_mobile_ceu.h>
#include <video/sh_mobile_lcdc.h>
#include <asm/clock.h>
#include <asm/machvec.h>
#include <asm/io.h>
#include <asm/sh_keysc.h>
#include <mach/migor.h>
#include <cpu/sh7722.h>

/* Address     IRQ  Size  Bus  Description
 * 0x00000000       64MB  16   NOR Flash (SP29PL256N)
 * 0x0c000000       64MB  64   SDRAM (2xK4M563233G)
 * 0x10000000  IRQ0       16   Ethernet (SMC91C111)
 * 0x14000000  IRQ4       16   USB 2.0 Host Controller (M66596)
 * 0x18000000       8GB    8   NAND Flash (K9K8G08U0A)
 */

static struct smc91x_platdata smc91x_info = {
	.flags = SMC91X_USE_16BIT | SMC91X_NOWAIT,
};

static struct resource smc91x_eth_resources[] = {
	[0] = {
		.name   = "SMC91C111" ,
		.start  = 0x10000300,
		.end    = 0x1000030f,
		.flags  = IORESOURCE_MEM,
	},
	[1] = {
		.start  = 32, /* IRQ0 */
		.flags  = IORESOURCE_IRQ | IORESOURCE_IRQ_HIGHLEVEL,
	},
};

static struct platform_device smc91x_eth_device = {
	.name           = "smc91x",
	.num_resources  = ARRAY_SIZE(smc91x_eth_resources),
	.resource       = smc91x_eth_resources,
	.dev	= {
		.platform_data	= &smc91x_info,
	},
};

static struct sh_keysc_info sh_keysc_info = {
	.mode = SH_KEYSC_MODE_2, /* KEYOUT0->4, KEYIN1->5 */
	.scan_timing = 3,
	.delay = 5,
	.keycodes = {
		0, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER,
		0, KEY_F, KEY_C, KEY_D,	KEY_H, KEY_1,
		0, KEY_2, KEY_3, KEY_4,	KEY_5, KEY_6,
		0, KEY_7, KEY_8, KEY_9, KEY_S, KEY_0,
		0, KEY_P, KEY_STOP, KEY_REWIND, KEY_PLAY, KEY_FASTFORWARD,
	},
};

static struct resource sh_keysc_resources[] = {
	[0] = {
		.start  = 0x044b0000,
		.end    = 0x044b000f,
		.flags  = IORESOURCE_MEM,
	},
	[1] = {
		.start  = 79,
		.flags  = IORESOURCE_IRQ,
	},
};

static struct platform_device sh_keysc_device = {
	.name           = "sh_keysc",
	.id             = 0, /* "keysc0" clock */
	.num_resources  = ARRAY_SIZE(sh_keysc_resources),
	.resource       = sh_keysc_resources,
	.dev	= {
		.platform_data	= &sh_keysc_info,
	},
};

static struct mtd_partition migor_nor_flash_partitions[] =
{
	{
		.name = "uboot",
		.offset = 0,
		.size = (1 * 1024 * 1024),
		.mask_flags = MTD_WRITEABLE,	/* Read-only */
	},
	{
		.name = "rootfs",
		.offset = MTDPART_OFS_APPEND,
		.size = (15 * 1024 * 1024),
	},
	{
		.name = "other",
		.offset = MTDPART_OFS_APPEND,
		.size = MTDPART_SIZ_FULL,
	},
};

static struct physmap_flash_data migor_nor_flash_data = {
	.width		= 2,
	.parts		= migor_nor_flash_partitions,
	.nr_parts	= ARRAY_SIZE(migor_nor_flash_partitions),
};

static struct resource migor_nor_flash_resources[] = {
	[0] = {
		.name		= "NOR Flash",
		.start		= 0x00000000,
		.end		= 0x03ffffff,
		.flags		= IORESOURCE_MEM,
	}
};

static struct platform_device migor_nor_flash_device = {
	.name		= "physmap-flash",
	.resource	= migor_nor_flash_resources,
	.num_resources	= ARRAY_SIZE(migor_nor_flash_resources),
	.dev		= {
		.platform_data = &migor_nor_flash_data,
	},
};

static struct mtd_partition migor_nand_flash_partitions[] = {
	{
		.name		= "nanddata1",
		.offset		= 0x0,
		.size		= 512 * 1024 * 1024,
	},
	{
		.name		= "nanddata2",
		.offset		= MTDPART_OFS_APPEND,
		.size		= 512 * 1024 * 1024,
	},
};

static void migor_nand_flash_cmd_ctl(struct mtd_info *mtd, int cmd,
				     unsigned int ctrl)
{
	struct nand_chip *chip = mtd->priv;

	if (cmd == NAND_CMD_NONE)
		return;

	if (ctrl & NAND_CLE)
		writeb(cmd, chip->IO_ADDR_W + 0x00400000);
	else if (ctrl & NAND_ALE)
		writeb(cmd, chip->IO_ADDR_W + 0x00800000);
	else
		writeb(cmd, chip->IO_ADDR_W);
}

static int migor_nand_flash_ready(struct mtd_info *mtd)
{
	return gpio_get_value(GPIO_PTA1); /* NAND_RBn */
}

struct platform_nand_data migor_nand_flash_data = {
	.chip = {
		.nr_chips = 1,
		.partitions = migor_nand_flash_partitions,
		.nr_partitions = ARRAY_SIZE(migor_nand_flash_partitions),
		.chip_delay = 20,
		.part_probe_types = (const char *[]) { "cmdlinepart", NULL },
	},
	.ctrl = {
		.dev_ready = migor_nand_flash_ready,
		.cmd_ctrl = migor_nand_flash_cmd_ctl,
	},
};

static struct resource migor_nand_flash_resources[] = {
	[0] = {
		.name		= "NAND Flash",
		.start		= 0x18000000,
		.end		= 0x18ffffff,
		.flags		= IORESOURCE_MEM,
	},
};

static struct platform_device migor_nand_flash_device = {
	.name		= "gen_nand",
	.resource	= migor_nand_flash_resources,
	.num_resources	= ARRAY_SIZE(migor_nand_flash_resources),
	.dev		= {
		.platform_data = &migor_nand_flash_data,
	}
};

static struct sh_mobile_lcdc_info sh_mobile_lcdc_info = {
#ifdef CONFIG_SH_MIGOR_RTA_WVGA
	.clock_source = LCDC_CLK_BUS,
	.ch[0] = {
		.chan = LCDC_CHAN_MAINLCD,
		.bpp = 16,
		.interface_type = RGB16,
		.clock_divider = 2,
		.lcd_cfg = {
			.name = "LB070WV1",
			.xres = 800,
			.yres = 480,
			.left_margin = 64,
			.right_margin = 16,
			.hsync_len = 120,
			.upper_margin = 1,
			.lower_margin = 17,
			.vsync_len = 2,
			.sync = 0,
		},
		.lcd_size_cfg = { /* 7.0 inch */
			.width = 152,
			.height = 91,
		},
	}
#endif
#ifdef CONFIG_SH_MIGOR_QVGA
	.clock_source = LCDC_CLK_PERIPHERAL,
	.ch[0] = {
		.chan = LCDC_CHAN_MAINLCD,
		.bpp = 16,
		.interface_type = SYS16A,
		.clock_divider = 10,
		.lcd_cfg = {
			.name = "PH240320T",
			.xres = 320,
			.yres = 240,
			.left_margin = 0,
			.right_margin = 16,
			.hsync_len = 8,
			.upper_margin = 1,
			.lower_margin = 17,
			.vsync_len = 2,
			.sync = FB_SYNC_HOR_HIGH_ACT,
		},
		.lcd_size_cfg = { /* 2.4 inch */
			.width = 49,
			.height = 37,
		},
		.board_cfg = {
			.setup_sys = migor_lcd_qvga_setup,
		},
		.sys_bus_cfg = {
			.ldmt2r = 0x06000a09,
			.ldmt3r = 0x180e3418,
			/* set 1s delay to encourage fsync() */
			.deferred_io_msec = 1000,
		},
	}
#endif
};

static struct resource migor_lcdc_resources[] = {
	[0] = {
		.name	= "LCDC",
		.start	= 0xfe940000, /* P4-only space */
		.end	= 0xfe941fff,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start	= 28,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct platform_device migor_lcdc_device = {
	.name		= "sh_mobile_lcdc_fb",
	.num_resources	= ARRAY_SIZE(migor_lcdc_resources),
	.resource	= migor_lcdc_resources,
	.dev	= {
		.platform_data	= &sh_mobile_lcdc_info,
	},
};

static struct clk *camera_clk;

static void camera_power_on(void)
{
	/* Use 10 MHz VIO_CKO instead of 24 MHz to work
	 * around signal quality issues on Panel Board V2.1.
	 */
	camera_clk = clk_get(NULL, "video_clk");
	clk_set_rate(camera_clk, 10000000);
	clk_enable(camera_clk);	/* start VIO_CKO */

	/* use VIO_RST to take camera out of reset */
	mdelay(10);
	gpio_set_value(GPIO_PTT3, 0);
	mdelay(10);
	gpio_set_value(GPIO_PTT3, 1);
	mdelay(10); /* wait to let chip come out of reset */
}

static void camera_power_off(void)
{
	clk_disable(camera_clk); /* stop VIO_CKO */
	clk_put(camera_clk);

	gpio_set_value(GPIO_PTT3, 0);
}

static void camera_power(int mode)
{
	if (mode)
		camera_power_on();
	else
		camera_power_off();
}

#ifdef CONFIG_I2C
static unsigned char camera_ov772x_magic[] =
{
	0x09, 0x01, 0x0c, 0x20, 0x0d, 0x41, 0x0e, 0x01,
	0x12, 0x00, 0x13, 0x8F, 0x14, 0x4A, 0x15, 0x00,
	0x16, 0x00, 0x17, 0x23, 0x18, 0xa0, 0x19, 0x07,
	0x1a, 0xf0, 0x1b, 0x40, 0x1f, 0x00, 0x20, 0x10,
	0x22, 0xff, 0x23, 0x01, 0x28, 0x00, 0x29, 0xa0,
	0x2a, 0x00, 0x2b, 0x00, 0x2c, 0xf0, 0x2d, 0x00,
	0x2e, 0x00, 0x30, 0x80, 0x31, 0x60, 0x32, 0x00,
	0x33, 0x00, 0x34, 0x00, 0x3d, 0x80, 0x3e, 0xe2,
	0x3f, 0x1f, 0x42, 0x80, 0x43, 0x80, 0x44, 0x80,
	0x45, 0x80, 0x46, 0x00, 0x47, 0x00, 0x48, 0x00,
	0x49, 0x50, 0x4a, 0x30, 0x4b, 0x50, 0x4c, 0x50,
	0x4d, 0x00, 0x4e, 0xef, 0x4f, 0x10, 0x50, 0x60,
	0x51, 0x00, 0x52, 0x00, 0x53, 0x24, 0x54, 0x7a,
	0x55, 0xfc, 0x62, 0xff, 0x63, 0xf0, 0x64, 0x1f,
	0x65, 0x00, 0x66, 0x10, 0x67, 0x00, 0x68, 0x00,
	0x69, 0x5c, 0x6a, 0x11, 0x6b, 0xa2, 0x6c, 0x01,
	0x6d, 0x50, 0x6e, 0x80, 0x6f, 0x80, 0x70, 0x0f,
	0x71, 0x00, 0x72, 0x00, 0x73, 0x0f, 0x74, 0x0f,
	0x75, 0xff, 0x78, 0x10, 0x79, 0x70, 0x7a, 0x70,
	0x7b, 0xf0, 0x7c, 0xf0, 0x7d, 0xf0, 0x7e, 0x0e,
	0x7f, 0x1a, 0x80, 0x31, 0x81, 0x5a, 0x82, 0x69,
	0x83, 0x75, 0x84, 0x7e, 0x85, 0x88, 0x86, 0x8f,
	0x87, 0x96, 0x88, 0xa3, 0x89, 0xaf, 0x8a, 0xc4,
	0x8b, 0xd7, 0x8c, 0xe8, 0x8d, 0x20, 0x8e, 0x00,
	0x8f, 0x00, 0x90, 0x08, 0x91, 0x10, 0x92, 0x1f,
	0x93, 0x01, 0x94, 0x2c, 0x95, 0x24, 0x96, 0x08,
	0x97, 0x14, 0x98, 0x24, 0x99, 0x38, 0x9a, 0x9e,
	0x9b, 0x00, 0x9c, 0x40, 0x9e, 0x11, 0x9f, 0x02,
	0xa0, 0x00, 0xa1, 0x40, 0xa2, 0x40, 0xa3, 0x06,
	0xa4, 0x00, 0xa6, 0x00, 0xa7, 0x40, 0xa8, 0x40,
	0xa9, 0x80, 0xaa, 0x80, 0xab, 0x06, 0xac, 0xff,
	0x12, 0x06, 0x64, 0x3f, 0x12, 0x46, 0x17, 0x3f,
	0x18, 0x50, 0x19, 0x03, 0x1a, 0x78, 0x29, 0x50,
	0x2c, 0x78,
};

static int ov772x_set_capture(struct soc_camera_platform_info *info,
			      int enable)
{
	struct i2c_adapter *a = i2c_get_adapter(0);
	struct i2c_msg msg;
	int ret = 0;
	int i;

	if (!enable)
		return 0; /* camera_power_off() is enough */

	for (i = 0; i < ARRAY_SIZE(camera_ov772x_magic); i += 2) {
		u_int8_t buf[8];

		msg.addr = 0x21;
		msg.buf = buf;
		msg.len = 2;
		msg.flags = 0;

		buf[0] = camera_ov772x_magic[i];
		buf[1] = camera_ov772x_magic[i + 1];

		ret = (ret < 0) ? ret : i2c_transfer(a, &msg, 1);
	}

	return ret;
}

static struct soc_camera_platform_info ov772x_info = {
	.iface = 0,
	.format_name = "RGB565",
	.format_depth = 16,
	.format = {
		.pixelformat = V4L2_PIX_FMT_RGB565,
		.colorspace = V4L2_COLORSPACE_SRGB,
		.width = 320,
		.height = 240,
	},
	.bus_param =  SOCAM_PCLK_SAMPLE_RISING | SOCAM_HSYNC_ACTIVE_HIGH |
	SOCAM_VSYNC_ACTIVE_HIGH | SOCAM_MASTER | SOCAM_DATAWIDTH_8,
	.power = camera_power,
	.set_capture = ov772x_set_capture,
};

static struct platform_device migor_camera_device = {
	.name		= "soc_camera_platform",
	.dev	= {
		.platform_data	= &ov772x_info,
	},
};
#endif /* CONFIG_I2C */

static struct sh_mobile_ceu_info sh_mobile_ceu_info = {
	.flags = SOCAM_MASTER | SOCAM_DATAWIDTH_8 | SOCAM_PCLK_SAMPLE_RISING \
	| SOCAM_HSYNC_ACTIVE_HIGH | SOCAM_VSYNC_ACTIVE_HIGH,
};

static struct resource migor_ceu_resources[] = {
	[0] = {
		.name	= "CEU",
		.start	= 0xfe910000,
		.end	= 0xfe91009f,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start  = 52,
		.flags  = IORESOURCE_IRQ,
	},
	[2] = {
		/* place holder for contiguous memory */
	},
};

static struct platform_device migor_ceu_device = {
	.name		= "sh_mobile_ceu",
	.id             = 0, /* "ceu0" clock */
	.num_resources	= ARRAY_SIZE(migor_ceu_resources),
	.resource	= migor_ceu_resources,
	.dev	= {
		.platform_data	= &sh_mobile_ceu_info,
	},
};

static struct platform_device *migor_devices[] __initdata = {
	&smc91x_eth_device,
	&sh_keysc_device,
	&migor_lcdc_device,
	&migor_ceu_device,
#ifdef CONFIG_I2C
	&migor_camera_device,
#endif
	&migor_nor_flash_device,
	&migor_nand_flash_device,
};

static struct i2c_board_info migor_i2c_devices[] = {
	{
		I2C_BOARD_INFO("rs5c372b", 0x32),
	},
	{
		I2C_BOARD_INFO("migor_ts", 0x51),
		.irq = 38, /* IRQ6 */
	},
};

static int __init migor_devices_setup(void)
{
	/* Lit D11 LED */
	gpio_request(GPIO_PTJ7, NULL);
	gpio_direction_output(GPIO_PTJ7, 1);
	gpio_export(GPIO_PTJ7, 0);

	/* Lit D12 LED */
	gpio_request(GPIO_PTJ5, NULL);
	gpio_direction_output(GPIO_PTJ5, 1);
	gpio_export(GPIO_PTJ5, 0);

	/* SMC91C111 - Enable IRQ0, Setup CS4 for 16-bit fast access */
	gpio_request(GPIO_FN_IRQ0, NULL);
	ctrl_outl(0x00003400, BSC_CS4BCR);
	ctrl_outl(0x00110080, BSC_CS4WCR);

	/* KEYSC */
	gpio_request(GPIO_FN_KEYOUT0, NULL);
	gpio_request(GPIO_FN_KEYOUT1, NULL);
	gpio_request(GPIO_FN_KEYOUT2, NULL);
	gpio_request(GPIO_FN_KEYOUT3, NULL);
	gpio_request(GPIO_FN_KEYOUT4_IN6, NULL);
	gpio_request(GPIO_FN_KEYIN1, NULL);
	gpio_request(GPIO_FN_KEYIN2, NULL);
	gpio_request(GPIO_FN_KEYIN3, NULL);
	gpio_request(GPIO_FN_KEYIN4, NULL);
	gpio_request(GPIO_FN_KEYOUT5_IN5, NULL);

	/* NAND Flash */
	gpio_request(GPIO_FN_CS6A_CE2B, NULL);
	ctrl_outl((ctrl_inl(BSC_CS6ABCR) & ~0x0600) | 0x0200, BSC_CS6ABCR);
	gpio_request(GPIO_PTA1, NULL);
	gpio_direction_input(GPIO_PTA1);

	/* Touch Panel */
	gpio_request(GPIO_FN_IRQ6, NULL);

	/* LCD Panel */
#ifdef CONFIG_SH_MIGOR_QVGA /* LCDC - QVGA - Enable SYS Interface signals */
	gpio_request(GPIO_FN_LCDD17, NULL);
	gpio_request(GPIO_FN_LCDD16, NULL);
	gpio_request(GPIO_FN_LCDD15, NULL);
	gpio_request(GPIO_FN_LCDD14, NULL);
	gpio_request(GPIO_FN_LCDD13, NULL);
	gpio_request(GPIO_FN_LCDD12, NULL);
	gpio_request(GPIO_FN_LCDD11, NULL);
	gpio_request(GPIO_FN_LCDD10, NULL);
	gpio_request(GPIO_FN_LCDD8, NULL);
	gpio_request(GPIO_FN_LCDD7, NULL);
	gpio_request(GPIO_FN_LCDD6, NULL);
	gpio_request(GPIO_FN_LCDD5, NULL);
	gpio_request(GPIO_FN_LCDD4, NULL);
	gpio_request(GPIO_FN_LCDD3, NULL);
	gpio_request(GPIO_FN_LCDD2, NULL);
	gpio_request(GPIO_FN_LCDD1, NULL);
	gpio_request(GPIO_FN_LCDRS, NULL);
	gpio_request(GPIO_FN_LCDCS, NULL);
	gpio_request(GPIO_FN_LCDRD, NULL);
	gpio_request(GPIO_FN_LCDWR, NULL);
	gpio_request(GPIO_PTH2, NULL); /* LCD_DON */
	gpio_direction_output(GPIO_PTH2, 1);
#endif
#ifdef CONFIG_SH_MIGOR_RTA_WVGA /* LCDC - WVGA - Enable RGB Interface signals */
	gpio_request(GPIO_FN_LCDD15, NULL);
	gpio_request(GPIO_FN_LCDD14, NULL);
	gpio_request(GPIO_FN_LCDD13, NULL);
	gpio_request(GPIO_FN_LCDD12, NULL);
	gpio_request(GPIO_FN_LCDD11, NULL);
	gpio_request(GPIO_FN_LCDD10, NULL);
	gpio_request(GPIO_FN_LCDD9, NULL);
	gpio_request(GPIO_FN_LCDD8, NULL);
	gpio_request(GPIO_FN_LCDD7, NULL);
	gpio_request(GPIO_FN_LCDD6, NULL);
	gpio_request(GPIO_FN_LCDD5, NULL);
	gpio_request(GPIO_FN_LCDD4, NULL);
	gpio_request(GPIO_FN_LCDD3, NULL);
	gpio_request(GPIO_FN_LCDD2, NULL);
	gpio_request(GPIO_FN_LCDD1, NULL);
	gpio_request(GPIO_FN_LCDD0, NULL);
	gpio_request(GPIO_FN_LCDLCLK, NULL);
	gpio_request(GPIO_FN_LCDDCK, NULL);
	gpio_request(GPIO_FN_LCDVEPWC, NULL);
	gpio_request(GPIO_FN_LCDVCPWC, NULL);
	gpio_request(GPIO_FN_LCDVSYN, NULL);
	gpio_request(GPIO_FN_LCDHSYN, NULL);
	gpio_request(GPIO_FN_LCDDISP, NULL);
	gpio_request(GPIO_FN_LCDDON, NULL);
#endif

	/* CEU */
	gpio_request(GPIO_FN_VIO_CLK2, NULL);
	gpio_request(GPIO_FN_VIO_VD2, NULL);
	gpio_request(GPIO_FN_VIO_HD2, NULL);
	gpio_request(GPIO_FN_VIO_FLD, NULL);
	gpio_request(GPIO_FN_VIO_CKO, NULL);
	gpio_request(GPIO_FN_VIO_D15, NULL);
	gpio_request(GPIO_FN_VIO_D14, NULL);
	gpio_request(GPIO_FN_VIO_D13, NULL);
	gpio_request(GPIO_FN_VIO_D12, NULL);
	gpio_request(GPIO_FN_VIO_D11, NULL);
	gpio_request(GPIO_FN_VIO_D10, NULL);
	gpio_request(GPIO_FN_VIO_D9, NULL);
	gpio_request(GPIO_FN_VIO_D8, NULL);

	gpio_request(GPIO_PTT3, NULL); /* VIO_RST */
	gpio_direction_output(GPIO_PTT3, 0);
	gpio_request(GPIO_PTT2, NULL); /* TV_IN_EN */
	gpio_direction_output(GPIO_PTT2, 1);
	gpio_request(GPIO_PTT0, NULL); /* CAM_EN */
#ifdef CONFIG_SH_MIGOR_RTA_WVGA
	gpio_direction_output(GPIO_PTT0, 0);
#else
	gpio_direction_output(GPIO_PTT0, 1);
#endif
	ctrl_outw(ctrl_inw(PORT_MSELCRB) | 0x2000, PORT_MSELCRB); /* D15->D8 */

	platform_resource_setup_memory(&migor_ceu_device, "ceu", 4 << 20);

	i2c_register_board_info(0, migor_i2c_devices,
				ARRAY_SIZE(migor_i2c_devices));

	return platform_add_devices(migor_devices, ARRAY_SIZE(migor_devices));
}
__initcall(migor_devices_setup);
