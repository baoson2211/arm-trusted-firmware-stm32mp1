/*
 * Copyright (c) 2015-2020, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <platform_def.h>

#include <arch_helpers.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <common/desc_image_load.h>
#include <drivers/clk.h>
#include <drivers/delay_timer.h>
#include <drivers/generic_delay_timer.h>
#include <drivers/st/bsec.h>
#include <drivers/st/stm32_console.h>
#include <drivers/st/stm32_iwdg.h>
#include <drivers/st/stm32_uart.h>
#include <drivers/st/stm32mp_clkfunc.h>
#include <drivers/st/stm32mp_pmic.h>
#include <drivers/st/stm32mp_reset.h>
#include <drivers/st/stm32mp1_clk.h>
#include <drivers/st/stm32mp1_pwr.h>
#include <drivers/st/stm32mp1_ram.h>
#include <drivers/st/stpmic1.h>
#include <lib/fconf/fconf.h>
#include <lib/fconf/fconf_dyn_cfg_getter.h>
#include <lib/mmio.h>
#include <lib/optee_utils.h>
#include <lib/xlat_tables/xlat_tables_v2.h>
#include <plat/common/platform.h>

#include <boot_api.h>
#include <stm32mp1_context.h>
#include <stm32mp1_critic_power.h>
#include <stm32mp1_dbgmcu.h>

#define PWRLP_TEMPO_5_HSI	5

#define RESET_TIMEOUT_US_1MS		1000U

static const char debug_msg[626] = {
	"***************************************************\n"
	"** NOTICE   NOTICE   NOTICE   NOTICE   NOTICE    **\n"
	"**                                               **\n"
	"** DEBUG ACCESS PORT IS OPEN!                    **\n"
	"** This boot image is only for debugging purpose **\n"
	"** and is unsafe for production use.             **\n"
	"**                                               **\n"
	"** If you see this message and you are not       **\n"
	"** debugging report this immediately to your     **\n"
	"** vendor!                                       **\n"
	"**                                               **\n"
	"***************************************************\n"
};

static console_t console;
static enum boot_device_e boot_device = BOOT_DEVICE_BOARD;

static void print_reset_reason(void)
{
	uint32_t rstsr = mmio_read_32(stm32mp_rcc_base() + RCC_MP_RSTSCLRR);

	if (rstsr == 0U) {
		WARN("Reset reason unknown\n");
		return;
	}

	INFO("Reset reason (0x%x):\n", rstsr);

	if ((rstsr & RCC_MP_RSTSCLRR_PADRSTF) == 0U) {
		if ((rstsr & RCC_MP_RSTSCLRR_STDBYRSTF) != 0U) {
			INFO("System exits from STANDBY\n");
			return;
		}

		if ((rstsr & RCC_MP_RSTSCLRR_CSTDBYRSTF) != 0U) {
			INFO("MPU exits from CSTANDBY\n");
			return;
		}
	}

	if ((rstsr & RCC_MP_RSTSCLRR_PORRSTF) != 0U) {
		INFO("  Power-on Reset (rst_por)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_BORRSTF) != 0U) {
		INFO("  Brownout Reset (rst_bor)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_MCSYSRSTF) != 0U) {
		if ((rstsr & RCC_MP_RSTSCLRR_PADRSTF) != 0U) {
			INFO("  System reset generated by MCU (MCSYSRST)\n");
		} else {
			INFO("  Local reset generated by MCU (MCSYSRST)\n");
		}
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_MPSYSRSTF) != 0U) {
		INFO("  System reset generated by MPU (MPSYSRST)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_HCSSRSTF) != 0U) {
		INFO("  Reset due to a clock failure on HSE\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_IWDG1RSTF) != 0U) {
		INFO("  IWDG1 Reset (rst_iwdg1)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_IWDG2RSTF) != 0U) {
		INFO("  IWDG2 Reset (rst_iwdg2)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_MPUP0RSTF) != 0U) {
		INFO("  MPU Processor 0 Reset\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_MPUP1RSTF) != 0U) {
		INFO("  MPU Processor 1 Reset\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_PADRSTF) != 0U) {
		INFO("  Pad Reset from NRST\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_VCORERSTF) != 0U) {
		INFO("  Reset due to a failure of VDD_CORE\n");
		return;
	}

	ERROR("  Unidentified reset reason\n");
}

enum boot_device_e get_boot_device(void)
{
	return boot_device;
}

void bl2_el3_early_platform_setup(u_register_t arg0,
				  u_register_t arg1 __unused,
				  u_register_t arg2 __unused,
				  u_register_t arg3 __unused)
{
	stm32mp_save_boot_ctx_address(arg0);
}

static int ddr_mapping_and_security(void)
{
	uint32_t ddr_size = dt_get_ddr_size();

	if (ddr_size == 0U) {
		return -EINVAL;
	}

	/* Map DDR for binary load, now with cacheable attribute */
	return mmap_add_dynamic_region(STM32MP_DDR_BASE, STM32MP_DDR_BASE,
				       ddr_size, MT_MEMORY | MT_RW | MT_SECURE);
}

void bl2_platform_setup(void)
{
	int ret;

	ret = stm32mp1_ddr_probe();
	if (ret < 0) {
		ERROR("Invalid DDR init: error %d\n", ret);
		panic();
	}

#if STM32MP_USE_STM32IMAGE
#ifdef AARCH32_SP_OPTEE
	INFO("BL2 runs OP-TEE setup\n");
#else
	INFO("BL2 runs SP_MIN setup\n");
#endif
#endif

	if (!stm32mp1_ddr_is_restored()) {
		uint32_t bkpr_core1_magic =
			tamp_bkpr(BOOT_API_CORE1_MAGIC_NUMBER_TAMP_BCK_REG_IDX);
		uint32_t bkpr_core1_addr =
			tamp_bkpr(BOOT_API_CORE1_BRANCH_ADDRESS_TAMP_BCK_REG_IDX);

		/* Clear backup register */
		mmio_write_32(bkpr_core1_addr, 0);
		/* Clear backup register magic */
		mmio_write_32(bkpr_core1_magic, 0);

		/* Clear the context in BKPSRAM */
		stm32_clean_context();

		if (dt_pmic_status() > 0) {
			configure_pmic();
		}
	}

	ret = ddr_mapping_and_security();
	if (ret < 0) {
		ERROR("DDR mapping: error %d\n", ret);
		panic();
	}
}

static void update_monotonic_counter(void)
{
	uint32_t version;
	uint32_t otp;

	CASSERT(STM32_TF_VERSION <= MAX_MONOTONIC_VALUE,
		assert_stm32mp1_monotonic_counter_reach_max);

	/* Check if monotonic counter needs to be incremented */
	if (stm32_get_otp_index(MONOTONIC_OTP, &otp, NULL) != 0) {
		panic();
	}

	if (stm32_get_otp_value(MONOTONIC_OTP, &version) != 0) {
		panic();
	}

	if ((version + 1U) < BIT(STM32_TF_VERSION)) {
		uint32_t result;

		/* Need to increment the monotonic counter. */
		version = BIT(STM32_TF_VERSION) - 1U;

		result = bsec_program_otp(version, otp);
		if (result != BSEC_OK) {
			ERROR("BSEC: MONOTONIC_OTP program Error %i\n",
			      result);
			panic();
		}
		INFO("Monotonic counter has been incremented (value 0x%x)\n",
		     version);
	}
}

static void initialize_clock(void)
{
	uint32_t voltage_mv = 0U;
	uint32_t freq_khz = 0U;
	int ret = 0;

	if (stm32mp1_is_wakeup_from_standby()) {
		ret = stm32_get_pll1_settings_from_context();
	}

	/*
	 * If no pre-defined PLL1 settings in DT, find the highest frequency
	 * in the OPP table (in DT, compatible with plaform capabilities, or
	 * in structure restored in RAM), and set related CPU supply voltage.
	 * If PLL1 settings found in DT, we consider CPU supply voltage in DT
	 * is consistent with it.
	 */
	if ((ret == 0) && !fdt_is_pll1_predefined()) {
		if (stm32mp1_is_wakeup_from_standby()) {
			ret = stm32mp1_clk_get_maxfreq_opp(&freq_khz,
							   &voltage_mv);
		} else {
			ret = dt_get_max_opp_freqvolt(&freq_khz, &voltage_mv);
		}

		if (ret != 0) {
			panic();
		}

		if (dt_pmic_status() > 0) {
			int read_voltage;
			const char *name;

			name = stm32mp_get_cpu_supply_name();
			if (name == NULL) {
				panic();
			}

			read_voltage = stpmic1_regulator_voltage_get(name);
			if (read_voltage < 0) {
				panic();
			}

			if (voltage_mv != (uint32_t)read_voltage) {
				if (stpmic1_regulator_voltage_set(name,
						(uint16_t)voltage_mv) != 0) {
					panic();
				}
			}
		}
	}

	if (stm32mp1_clk_init(freq_khz) < 0) {
		panic();
	}
}

static void reset_uart(uint32_t reset)
{
	int ret;

	ret = stm32mp_reset_assert(reset, RESET_TIMEOUT_US_1MS);
	if (ret != 0) {
		panic();
	}

	udelay(2);

	ret = stm32mp_reset_deassert(reset, RESET_TIMEOUT_US_1MS);
	if (ret != 0) {
		panic();
	}

	mdelay(1);
}

void bl2_el3_plat_arch_setup(void)
{
	int32_t result;
	struct dt_node_info dt_uart_info;
	const char *board_model;
	boot_api_context_t *boot_context =
		(boot_api_context_t *)stm32mp_get_boot_ctx_address();
	uint32_t clk_rate;
	uintptr_t pwr_base;
	uintptr_t rcc_base;
	bool serial_uart_interface __unused =
				(boot_context->boot_interface_selected ==
				 BOOT_API_CTX_BOOT_INTERFACE_SEL_SERIAL_UART);
	uintptr_t uart_prog_addr __unused;

	if (bsec_probe() != 0) {
		panic();
	}

	mmap_add_region(BL_CODE_BASE, BL_CODE_BASE,
			BL_CODE_END - BL_CODE_BASE,
			MT_CODE | MT_SECURE);

#if STM32MP_USE_STM32IMAGE && !defined(AARCH32_SP_OPTEE)
	/* Prevent corruption of preloaded BL32 */
	mmap_add_region(BL32_BASE, BL32_BASE,
			BL32_LIMIT - BL32_BASE,
			MT_RO_DATA | MT_SECURE);
#endif
	/* Prevent corruption of preloaded Device Tree */
	mmap_add_region(DTB_BASE, DTB_BASE,
			DTB_LIMIT - DTB_BASE,
			MT_RO_DATA | MT_SECURE);

	configure_mmu();

	if (dt_open_and_check(STM32MP_DTB_BASE) < 0) {
		panic();
	}

	pwr_base = stm32mp_pwr_base();
	rcc_base = stm32mp_rcc_base();

	/* Clear Stop Request bits to correctly manage low-power exit */
	mmio_write_32(rcc_base + RCC_MP_SREQCLRR,
		      (uint32_t)(RCC_MP_SREQCLRR_STPREQ_P0 |
				 RCC_MP_SREQCLRR_STPREQ_P1));

	/*
	 * Disable the backup domain write protection.
	 * The protection is enable at each reset by hardware
	 * and must be disabled by software.
	 */
	mmio_setbits_32(pwr_base + PWR_CR1, PWR_CR1_DBP);

	while ((mmio_read_32(pwr_base + PWR_CR1) & PWR_CR1_DBP) == 0U) {
		;
	}

	/*
	 * Configure Standby mode available for MCU by default
	 * and allow to switch in standby SoC in all case
	 */
	mmio_setbits_32(pwr_base + PWR_MCUCR, PWR_MCUCR_PDDS);

	/* Reset backup domain on cold boot cases */
	if ((mmio_read_32(rcc_base + RCC_BDCR) & RCC_BDCR_RTCSRC_MASK) == 0U) {
		mmio_setbits_32(rcc_base + RCC_BDCR, RCC_BDCR_VSWRST);

		while ((mmio_read_32(rcc_base + RCC_BDCR) & RCC_BDCR_VSWRST) ==
		       0U) {
			;
		}

		mmio_clrbits_32(rcc_base + RCC_BDCR, RCC_BDCR_VSWRST);
	}

	/* Wait 5 HSI periods before re-enabling PLLs after STOP modes */
	mmio_clrsetbits_32(rcc_base + RCC_PWRLPDLYCR,
			   RCC_PWRLPDLYCR_PWRLP_DLY_MASK,
			   PWRLP_TEMPO_5_HSI);

	/* Disable retention and backup RAM content after standby */
	mmio_clrbits_32(pwr_base + PWR_CR2, PWR_CR2_BREN | PWR_CR2_RREN);

	/* Disable MCKPROT */
	mmio_clrbits_32(rcc_base + RCC_TZCR, RCC_TZCR_MCKPROT);

	/* Enable BKP Register protection */
	mmio_write_32(TAMP_SMCR,
		      TAMP_BKP_SEC_NUMBER << TAMP_BKP_SEC_WDPROT_SHIFT |
		      TAMP_BKP_SEC_NUMBER << TAMP_BKP_SEC_RWDPROT_SHIFT);

	generic_delay_timer_init();

#if STM32MP_UART_PROGRAMMER
	uart_prog_addr = get_uart_address(boot_context->boot_interface_instance);

	/* Disable programmer UART before changing clock tree */
	if (serial_uart_interface) {
		stm32_uart_stop(uart_prog_addr);
	}
#endif
#if STM32MP_USB_PROGRAMMER
	if (boot_context->boot_interface_selected ==
	    BOOT_API_CTX_BOOT_INTERFACE_SEL_SERIAL_USB) {
		boot_device = BOOT_DEVICE_USB;
	}
#endif
	if (stm32mp1_clk_probe() < 0) {
		panic();
	}

	if (dt_pmic_status() > 0) {
		initialize_pmic();
	}

	initialize_clock();

	result = dt_get_stdout_uart_info(&dt_uart_info);

	if ((result <= 0) ||
	    (dt_uart_info.status == DT_DISABLED) ||
#if STM32MP_UART_PROGRAMMER
	    (serial_uart_interface &&
	     (uart_prog_addr == dt_uart_info.base)) ||
#endif
	    (dt_uart_info.clock < 0) ||
	    (dt_uart_info.reset < 0)) {
		goto skip_console_init;
	}

	if (dt_set_stdout_pinctrl() != 0) {
		goto skip_console_init;
	}

	if (dt_uart_info.status == DT_DISABLED) {
		panic();
	}

	clk_enable((unsigned long)dt_uart_info.clock);

	reset_uart((uint32_t)dt_uart_info.reset);

	clk_rate = clk_get_rate((unsigned long)dt_uart_info.clock);

	if (console_stm32_register(dt_uart_info.base, clk_rate,
				   STM32MP_UART_BAUDRATE, &console) == 0) {
		panic();
	}

	console_set_scope(&console, CONSOLE_FLAG_BOOT |
			  CONSOLE_FLAG_CRASH | CONSOLE_FLAG_TRANSLATE_CRLF);

	stm32mp_print_cpuinfo();

	board_model = dt_get_board_model();
	if (board_model != NULL) {
		NOTICE("Model: %s\n", board_model);
	}

	stm32mp_print_boardinfo();

#if TRUSTED_BOARD_BOOT
	if (boot_context->auth_status != BOOT_API_CTX_AUTH_NO) {
		NOTICE("Bootrom authentication %s\n",
		       (boot_context->auth_status == BOOT_API_CTX_AUTH_FAILED) ?
		       "failed" : "succeeded");
	}
#endif

skip_console_init:
#if !TRUSTED_BOARD_BOOT
	if (stm32mp_is_closed_device()) {
		/* Closed chip required authentication */
		ERROR("Secured chip must enabled TRUSTED_BOARD_BOOT\n");
		panic();
	}
#endif

	stm32mp1_syscfg_init();

	if (stm32_iwdg_init() < 0) {
		panic();
	}

	stm32_iwdg_refresh();

	if (bsec_read_debug_conf() != 0U) {
		result = stm32mp1_dbgmcu_freeze_iwdg2();
		if (result != 0) {
			INFO("IWDG2 freeze error : %i\n", result);
		}

		if (stm32mp_is_closed_device()) {
			NOTICE("\n%s", debug_msg);
		}
	}

	if (stm32_save_boot_interface(boot_context->boot_interface_selected,
				      boot_context->boot_interface_instance) !=
	    0) {
		ERROR("Cannot save boot interface\n");
	}

	stm32mp1_arch_security_setup();

	print_reset_reason();

	update_monotonic_counter();

	if (dt_pmic_status() > 0) {
		initialize_pmic();
		print_pmic_info_and_debug();
	}

#if STM32MP_USE_STM32IMAGE
	if (!stm32mp1_ddr_is_restored()) {
		stm32mp_io_setup();
	}
#else
	fconf_populate("TB_FW", STM32MP_DTB_BASE);

	stm32mp_io_setup();
#endif
}

#if defined(AARCH32_SP_OPTEE) && STM32MP_USE_STM32IMAGE
static void set_mem_params_info(entry_point_info_t *ep_info,
				image_info_t *unpaged, image_info_t *paged)
{
	uintptr_t bl32_ep = 0;

	/* Use the default dram setup if no valid ep found */
	if (get_optee_header_ep(ep_info, &bl32_ep) &&
	    (bl32_ep >= STM32MP_OPTEE_BASE) &&
	    (bl32_ep < (STM32MP_OPTEE_BASE + STM32MP_OPTEE_SIZE))) {
		assert((STM32MP_OPTEE_BASE >= BL2_LIMIT) ||
		       ((STM32MP_OPTEE_BASE + STM32MP_OPTEE_SIZE) <= BL2_BASE));

		unpaged->image_base = STM32MP_OPTEE_BASE;
		unpaged->image_max_size = STM32MP_OPTEE_SIZE;
	} else {
		unpaged->image_base = STM32MP_DDR_BASE + dt_get_ddr_size() -
				      STM32MP_DDR_S_SIZE -
				      STM32MP_DDR_SHMEM_SIZE;
		unpaged->image_max_size = STM32MP_DDR_S_SIZE;
	}
	paged->image_base = STM32MP_DDR_BASE + dt_get_ddr_size() -
			    STM32MP_DDR_S_SIZE - STM32MP_DDR_SHMEM_SIZE;
	paged->image_max_size = STM32MP_DDR_S_SIZE;
}
#endif

/*******************************************************************************
 * This function can be used by the platforms to update/use image
 * information for given `image_id`.
 ******************************************************************************/
int bl2_plat_handle_post_image_load(unsigned int image_id)
{
	int err = 0;
	bl_mem_params_node_t *bl_mem_params = get_bl_mem_params_node(image_id);
	bl_mem_params_node_t *bl32_mem_params;
	bl_mem_params_node_t *pager_mem_params __unused;
	bl_mem_params_node_t *paged_mem_params __unused;
#if !STM32MP_USE_STM32IMAGE
	const struct dyn_cfg_dtb_info_t *config_info;
	bl_mem_params_node_t *tos_fw_mem_params;
	unsigned int i;
	unsigned long long ddr_top __unused;
	bool wakeup_ddr_sr = stm32mp1_ddr_is_restored();
	const unsigned int image_ids[] = {
		BL32_IMAGE_ID,
		BL33_IMAGE_ID,
		HW_CONFIG_ID,
		TOS_FW_CONFIG_ID,
	};
#endif

	assert(bl_mem_params != NULL);

#if TRUSTED_BOARD_BOOT && STM32MP_USE_STM32IMAGE
	/* Clean header to avoid loaded header reused */
	stm32mp_delete_loaded_header();
#endif

	switch (image_id) {
#if !STM32MP_USE_STM32IMAGE
	case FW_CONFIG_ID:
		/* Set global DTB info for fixed fw_config information */
		set_config_info(STM32MP_FW_CONFIG_BASE, STM32MP_FW_CONFIG_MAX_SIZE, FW_CONFIG_ID);
		fconf_populate("FW_CONFIG", STM32MP_FW_CONFIG_BASE);

		/* Iterate through all the fw config IDs */
		for (i = 0; i < ARRAY_SIZE(image_ids); i++) {
			bl_mem_params = get_bl_mem_params_node(image_ids[i]);
			assert(bl_mem_params != NULL);

			config_info = FCONF_GET_PROPERTY(dyn_cfg, dtb, image_ids[i]);
			if (config_info == NULL) {
				continue;
			}

			bl_mem_params->image_info.image_base = config_info->config_addr;
			bl_mem_params->image_info.image_max_size = config_info->config_max_size;

			/*
			 * If going back from CSTANDBY / STANDBY and DDR was in Self-Refresh,
			 * DDR partitions must not be reloaded.
			 */
			if (!(wakeup_ddr_sr && (config_info->config_addr >= STM32MP_DDR_BASE))) {
				bl_mem_params->image_info.h.attr &= ~IMAGE_ATTRIB_SKIP_LOADING;
			}

			switch (image_ids[i]) {
			case BL32_IMAGE_ID:
				bl_mem_params->ep_info.pc = config_info->config_addr;

				/* In case of OPTEE, initialize address space with tos_fw addr */
				pager_mem_params = get_bl_mem_params_node(BL32_EXTRA1_IMAGE_ID);
				pager_mem_params->image_info.image_base = config_info->config_addr;
				pager_mem_params->image_info.image_max_size =
					config_info->config_max_size;

				/* Init base and size for pager if exist */
				paged_mem_params = get_bl_mem_params_node(BL32_EXTRA2_IMAGE_ID);
				paged_mem_params->image_info.image_base = STM32MP_DDR_BASE +
					(dt_get_ddr_size() - STM32MP_DDR_S_SIZE);
				paged_mem_params->image_info.image_max_size = STM32MP_DDR_S_SIZE;
				break;

			case BL33_IMAGE_ID:
				if (wakeup_ddr_sr) {
					/*
					 * Set ep_info PC to 0, to inform BL32 it is a reset
					 * after STANDBY
					 */
					bl_mem_params->ep_info.pc = 0U;
				} else {
					bl_mem_params->ep_info.pc = config_info->config_addr;
				}
				break;

			case HW_CONFIG_ID:
			case TOS_FW_CONFIG_ID:
				break;

			default:
				return -EINVAL;
			}
		}

		break;
#endif /* !STM32MP_USE_STM32IMAGE */

	case BL32_IMAGE_ID:
#if defined(AARCH32_SP_OPTEE) || !STM32MP_USE_STM32IMAGE
		bl_mem_params->ep_info.pc = bl_mem_params->image_info.image_base;
		if (get_optee_header_ep(&bl_mem_params->ep_info, &bl_mem_params->ep_info.pc) == 1) {
			/* BL32 is OP-TEE header */
#if !STM32MP_USE_STM32IMAGE
			if (wakeup_ddr_sr) {
				bl_mem_params->ep_info.pc = stm32_pm_get_optee_ep();
				if (stm32mp1_addr_inside_backupsram(bl_mem_params->ep_info.pc)) {
					clk_enable(BKPSRAM);
				}

				break;
			}
#endif
			pager_mem_params = get_bl_mem_params_node(BL32_EXTRA1_IMAGE_ID);
			paged_mem_params = get_bl_mem_params_node(BL32_EXTRA2_IMAGE_ID);
			assert((pager_mem_params != NULL) && (paged_mem_params != NULL));

#if STM32MP_USE_STM32IMAGE
			set_mem_params_info(&bl_mem_params->ep_info, &pager_mem_params->image_info,
					    &paged_mem_params->image_info);
#endif

			err = parse_optee_header(&bl_mem_params->ep_info,
						 &pager_mem_params->image_info,
						 &paged_mem_params->image_info);
			if (err) {
				ERROR("OPTEE header parse error.\n");
				panic();
			}

			/* Set optee boot info from parsed header data */
			bl_mem_params->ep_info.args.arg0 = paged_mem_params->image_info.image_base;
			bl_mem_params->ep_info.args.arg1 = 0; /* Unused */
			bl_mem_params->ep_info.args.arg2 = 0; /* No DT supported */
		} else {
#if STM32MP_USE_STM32IMAGE
			bl_mem_params->ep_info.pc = STM32MP_BL32_BASE;
#else
			tos_fw_mem_params = get_bl_mem_params_node(TOS_FW_CONFIG_ID);
			bl_mem_params->image_info.image_max_size +=
				tos_fw_mem_params->image_info.image_max_size;
#endif
			bl_mem_params->ep_info.args.arg0 = 0;
		}

		if (bl_mem_params->ep_info.pc >= STM32MP_DDR_BASE) {
			stm32_context_save_bl2_param();
		}
#endif
		break;

	case BL33_IMAGE_ID:
		bl32_mem_params = get_bl_mem_params_node(BL32_IMAGE_ID);
		assert(bl32_mem_params != NULL);
		bl32_mem_params->ep_info.lr_svc = bl_mem_params->ep_info.pc;

		flush_dcache_range(bl_mem_params->image_info.image_base,
				   bl_mem_params->image_info.image_max_size);
		break;

	default:
		/* Do nothing in default case */
		break;
	}

	return err;
}

void bl2_el3_plat_prepare_exit(void)
{
	stm32mp1_security_setup();
}
