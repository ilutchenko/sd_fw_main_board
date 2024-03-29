/*
    ChibiOS - Copyright (C) 2006..2018 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <string.h>

#include "ch.h"
#include "hal.h"
#include "shell.h"
#include "config.h"
#include "json_output.h"
#ifdef USE_UBLOX_GPS_MODULE
#include "neo-m8.h"
extern ubx_nav_pvt_t *pvt_box;
#endif
#ifdef USE_SD_SHELL
#include "sd_shell_cmds.h"
extern output_t *output;
#endif
#ifdef USE_XBEE_MODULE
#include "xbee.h"
#endif
#ifdef USE_BNO055_MODULE
#include "bno055_i2c.h"
extern bno055_t *bno055;
#endif
#ifdef USE_MICROSD_MODULE
#include "microsd.h"
extern microsd_t *microsd;
extern microsd_fsm_t *microsd_fsm;
#endif
#ifdef USE_WINDSENSOR_MODULE
#include "windsensor.h"
extern windsensor_t *wind;
#endif
#ifdef USE_BLE_MODULE
#include "nina-b3.h"

#endif
#include "eeprom.h"
#ifdef USE_MATH_MODULE
#include "sd_math.h"
#endif
#ifdef USE_BMX160_MODULE
#include "bmx160_i2c.h"
#endif
#ifdef USE_HMC5883_MODULE
#include "hmc5883_i2c.h"
extern hmc5883_t *hmc5883;
#endif

#ifdef USE_HMC6343_MODULE
#include "hmc6343_i2c.h"
extern hmc6343_t *hmc6343;
#endif

#ifdef USE_MCU_MCU_MODULE
#include "mcu-mcu_i2c.h"
#endif

#include "adc.h"
extern dots_t *r_rudder_dots;
extern coefs_t *r_rudder_coefs;
extern rudder_t *r_rudder;
extern uint8_t need_calibration;
const I2CConfig bmx160_i2c_cfg1 = {
  0x30420F13,
  0,
  0
};
struct ch_semaphore usart1_semaph;
struct ch_semaphore spi2_semaph;
extern uint32_t __ram0_end__;
static const WDGConfig wdgcfg = {
  STM32_IWDG_PR_64,
  STM32_IWDG_RL(1000),
  0xFFF					//Windowed watchdog workaround
};

/*===========================================================================*/
/* Application code.                                                         */
/*===========================================================================*/

void fill_memory(void){
#ifdef USE_BNO055_MODULE
	bno055 = calloc(1, sizeof(bno055_t));
#endif
#ifdef USE_UBLOX_GPS_MODULE
	pvt_box = calloc(1, sizeof(ubx_nav_pvt_t));
#endif
#ifdef USE_WINDSENSOR_MODULE
	wind = calloc(1, sizeof(windsensor_t));
#endif
#ifdef USE_BLE_MODULE
	//ble = calloc(1, sizeof(ble_t));
	r_rudder_coefs = calloc(1, sizeof(coefs_t));
	r_rudder_dots = calloc(1, sizeof(dots_t));
#endif
#ifdef USE_SD_SHELL
	output = calloc(1, sizeof(output_t));
#endif
#ifdef USE_MICROSD_MODULE
	microsd = calloc(1, sizeof(microsd_t));
	microsd_fsm = calloc(1, sizeof(microsd_fsm_t));
#endif
#ifdef USE_HMC6343_MODULE
	hmc6343 = calloc(1, sizeof(hmc6343_t));
#endif
#ifdef USE_HMC5883_MODULE
	hmc5883 = calloc(1, sizeof(hmc5883_t));
#endif
}
/*
 * Application entry point.
 */
int main(void) {
	uint32_t *ACTLR = (uint32_t *) 0xE000E008;
	*ACTLR |= 2;
	thread_t *sh = NULL;

	/*
	 * System initializations.
	 * - HAL initialization, this also initializes the configured device drivers
	 *   and performs the board-specific initializations.
	 * - Kernel initialization, the main() function becomes a thread and the
	 *   RTOS is active.
	 */

	//wdgReset(&WDGD1);
	halInit();
	chSysInit();

	fill_memory();
#if (__CORTEX_M == 0x03 || __CORTEX_M == 0x04)
	chSysLock();
	// enable UsageFault, BusFault, MemManageFault
	SCB->SHCSR |= SCB_SHCSR_USGFAULTENA_Msk |
	SCB_SHCSR_BUSFAULTENA_Msk |
	SCB_SHCSR_MEMFAULTENA_Msk;
	// enable fault on division by zero
	SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk;
	chSysUnlock();
#endif

	chSemObjectInit(&usart1_semaph, 1);
	chSemObjectInit(&spi2_semaph, 1);
	palClearLine(LINE_RF_868_RST);
	palClearLine(LINE_NINA_CTS);
uint32_t hz = RTC->BKP0R;
	//check_first_reboot
	if (RTC->BKP0R != 1)
	{
		RTC->BKP0R = 1;
		chThdSleepMilliseconds(1500);
		hz = RTC->BKP0R;
		NVIC_SystemReset();
	}

	start_eeprom_module();
#ifdef USE_SD_SHELL
	sdStart(&SD1, NULL);
	shellInit();
	chSemWait(&usart1_semaph);
	sh = cmd_init();
	chSemSignal(&usart1_semaph);
	//wdgReset(&WDGD1);
#else
	sdStart(&SD1, NULL);
#endif


#ifdef USE_MATH_MODULE
//	start_math_module();
	calib_init_params();
#endif
	chThdSleepMilliseconds(30);

	//wdgReset(&WDGD1);
#ifdef USE_WINDSENSOR_MODULE
	start_windsensor_module();
	chThdSleepMilliseconds(15);
#endif
	//wdgReset(&WDGD1);
#ifdef USE_UBLOX_GPS_MODULE
	start_gps_module();
	//chThdSleepMilliseconds(1500);
#endif
	//wdgReset(&WDGD1);
#ifdef USE_BNO055_MODULE
	//start_bno055_module();
	chThdSleepMilliseconds(100);
#endif

#ifdef USE_HMC5883_MODULE
	start_hmc5883_module();
#endif

#ifdef USE_BMX160_MODULE
	start_bmx160_module();
#endif

#ifdef USE_HMC6343_MODULE
	chThdSleepMilliseconds(500);
	start_hmc6343_module();
#endif

#ifdef USE_BLE_MODULE
	start_ble_module();
	r_rudder->min_native = r_rudder_dots->x1;
	r_rudder->center_native = r_rudder_dots->x2;
	r_rudder->max_native = r_rudder_dots->x3;
	r_rudder->min_degrees = r_rudder_dots->y1;
	r_rudder->center_degrees = r_rudder_dots->y2;
	r_rudder->max_degrees = r_rudder_dots->y3;
#endif
#ifdef USE_MCU_MCU_MODULE
	start_mcu_mcu_module();
#endif

#ifdef USE_SD_SHELL
	start_json_module();
	chThdSleepMilliseconds(15);
#endif



#ifdef USE_XBEE_MODULE
	start_xbee_module();
	chThdSleepMilliseconds(15);
#endif
#ifdef USE_MICROSD_MODULE
	start_microsd_module();
	chThdSleepMilliseconds(15);
#endif
	//wdgStart(&WDGD1, &wdgcfg);

	toggle_test_output();

	while (true) {
#ifdef USE_SD_SHELL
		if (!sh)
			sh = cmd_init();
		else if (chThdTerminatedX(sh)) {
			chThdRelease(sh);
			sh = NULL;
		}
#endif
		chThdSleepMilliseconds(500);
	}
}
