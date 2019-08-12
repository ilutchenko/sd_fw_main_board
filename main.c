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
#include <stdlib.h>
#include "ch.h"
#include "hal.h"
#include "rt_test_root.h"
#include "oslib_test_root.h"
#include "shell.h"

#include "MPU9250.h"
#include "sd_shell_cmds.h"
#include "xbee.h"
#include "ff.h"

#include "chprintf.h"
#include "neo-m8.h"
#include "bno055.h"
#include "bno055_i2c.h"
#include "windsensor.h"

//#define TRAINER_MODULE
/**
 * Executes the BKPT instruction that causes the debugger to stop.
 * If no debugger is attached, this will be ignored
 */
#define bkpt() __asm volatile("BKPT #0\n")

void NMI_Handler(void) {
    //TODO
    while(1);
}

//See http://infocenter.arm.com/help/topic/com.arm.doc.dui0552a/BABBGBEC.html
typedef enum  {
    Reset = 1,
    NMI = 2,
    HardFault = 3,
    MemManage = 4,
    BusFault = 5,
    UsageFault = 6,
} FaultType;
uint8_t payload[256];
uint8_t read_pvt = 1;
extern ubx_nav_pvt_t *pvt_box;
extern ubx_cfg_rate_t *rate_box;
extern ubx_cfg_nav5_t *nav5_box;
extern ubx_cfg_odo_t *cfg_odo_box;
extern ubx_nav_odo_t *odo_box;
extern xbee_struct_t *xbee;
extern neo_struct_t *neo;
extern mpu_struct_t *mpu;
extern tx_box_t *tx_box;
extern windsensor_t *wind;
extern output_struct_t *output;
struct ch_semaphore usart1_semaph;
struct ch_semaphore spi2_semaph;

bno055_t bno055_struct;
bno055_t *bno055 = &bno055_struct;

extern float calib[];
extern const I2CConfig i2ccfg;
#define MAX_FILLER 11
#define FLOAT_PRECISION 9

void send_data(uint8_t stream);
static void gpt9cb(GPTDriver *gptp);
static void gpt11cb(GPTDriver *gptp);
static void gpt12cb(GPTDriver *gptp);
static void gpt14cb(GPTDriver *gptp);
float mag_offset[3];
void insert_dot(char *str){
	uint8_t str2[20];
	str2[0] = str[0];
	str2[1] = str[1];
	str2[2] = '.';
	memcpy(&str2[3], &str[2], 7);
	memcpy(str, str2, 8);
}

/*
 * Watchdog deadline set to more than one second (LSI=40000 / (64 * 1000)).
 */
/*static const WDGConfig wdgcfg = {
  STM32_IWDG_PR_64,
  STM32_IWDG_RL(512),
  STM32_IWDG_WIN_DISABLED
};
*/

/* Maximum speed SPI configuration (18MHz, CPHA=0, CPOL=0, MSb first).*/
static const SPIConfig hs_spicfg = {false, NULL, GPIOC, GPIOC_SD_CS, 0, 0};

/* Low speed SPI configuration (281.250kHz, CPHA=0, CPOL=0, MSb first).*/
static const SPIConfig ls_spicfg = {false, NULL, GPIOC, GPIOC_SD_CS,
		SPI_CR1_BR_2,
                                    0};

/*===========================================================================*/
/* Module exported variables.                                                */
/*===========================================================================*/

/* MMC/SD over SPI driver configuration.*/
//MMCConfig const portab_mmccfg = {&SPID3, &ls_spicfg, &hs_spicfg};
//MMCDriver MMCD1;


const I2CConfig i2c1cfg = {
  0x20E7112A,
  0,
  0
};

static SerialConfig sd7cfg =
{
		115200
};

static GPTConfig gpt14cfg =
{
		20000,      // Timer clock
		gpt14cb,        // Callback function
		0,
		0
};

static GPTConfig gpt12cfg =
{
		20000,      // Timer clock
		gpt12cb,        // Callback function
		0,
		0
};

static GPTConfig gpt11cfg =
{
		20000,      // Timer clock
		gpt11cb,        // Callback function
		0,
		0
};

static GPTConfig gpt9cfg =
{
		20000,      // Timer clock
		gpt9cb,        // Callback function
		0,
		0
};

/*
 * Maximum speed SPI configuration (3.3MHz, CPHA=0, CPOL=0, MSb first).
 */
static const SPIConfig spi1_cfg = {
		false,
		NULL,
		GPIOA,
		GPIOA_RF_868_CS,
		SPI_CR1_BR_2 | SPI_CR1_BR_1 | SPI_CR1_BR_0,	//FPCLK1 is 54 MHZ. XBEE support 3.5 max, so divide it by 16
		//  0,
		0
};

const SPIConfig neo_spi_cfg = {
		false,
		NULL,
		GPIOC,
		GPIOC_MCU_CS,
		SPI_CR1_BR_1 | SPI_CR1_BR_0,
		//0,
		0
};

const SPIConfig mpu_spi_cfg = {
		false,
		NULL,
		GPIOC,
		GPIOC_MCU_CS,
		SPI_CR1_BR_2 | SPI_CR1_BR_1 | SPI_CR1_BR_0 | SPI_CR1_CPOL,
		//0,
		0
};




thread_reference_t xbee_poll_trp = NULL;

static THD_WORKING_AREA(xbee_poll_thread_wa, 1024);
static THD_FUNCTION(xbee_poll_thread, p){
	(void)p;
	msg_t msg;
	uint8_t i;
	uint8_t txbuff[20];
	chRegSetThreadName("XBee polling thd");
	gptStop(&GPTD9);
	gptStart(&GPTD9, &gpt9cfg);
//	gptStartContinuous(&GPTD9, 2000);
	while (true){
		chSysLock();
		if (xbee->poll_suspend_state) {
			msg = chThdSuspendS(&xbee_poll_trp);
		}
		chSysUnlock();
		//palToggleLine(LINE_RED_LED);
		while(!palReadLine(LINE_RF_868_SPI_ATTN)){
			xbee_polling();
		}
	}
}

thread_reference_t xbee_trp = NULL;

static THD_WORKING_AREA(xbee_thread_wa, 1024);
static THD_FUNCTION(xbee_thread, p){
	(void)p;
	msg_t msg;
	uint8_t at[] = {'S', 'L'};
	uint8_t rxbuf[15];
	chRegSetThreadName("XBee Thread");
	while (true) {
		chSysLock();
		if (xbee->suspend_state) {
			msg = chThdSuspendS(&xbee_trp);
		}
		chSysUnlock();

		/* Perform processing here.*/
		switch (msg){
		case XBEE_GET_OWN_ADDR:
			xbee_read_own_addr(xbee);
			break;
		case XBEE_GET_RSSI:
			xbee->rssi = xbee_read_last_rssi(xbee);
			chSemWait(&usart1_semaph);
			chprintf((BaseSequentialStream*)&SD1, "RSSI: %d\r\n", xbee->rssi);
			chSemSignal(&usart1_semaph);
			break;
		case XBEE_GET_PACKET_PAYLOAD:
			xbee->packet_payload = xbee_get_packet_payload(xbee);
			chSemWait(&usart1_semaph);
			chprintf((BaseSequentialStream*)&SD1, "Packet payload: %d\r\n", xbee->packet_payload);
			chSemSignal(&usart1_semaph);
			break;
		case XBEE_GET_STAT:
			xbee->bytes_transmitted = xbee_get_bytes_transmitted(xbee);
			xbee->good_packs_res = xbee_get_good_packets_res(xbee);
			xbee->rec_err_count = xbee_get_received_err_count(xbee);
			xbee->trans_errs = xbee_get_transceived_err_count(xbee);
			xbee->unicast_trans_count = xbee_get_unicast_trans_count(xbee);
			xbee->rssi = xbee_read_last_rssi(xbee);
			chSemWait(&usart1_semaph);
			chprintf((BaseSequentialStream*)&SD1, "Bytes transmitted:     %d\r\n", xbee->bytes_transmitted);
			chprintf((BaseSequentialStream*)&SD1, "Good packets received: %d\r\n", xbee->good_packs_res);
			chprintf((BaseSequentialStream*)&SD1, "Received errors count: %d\r\n", xbee->rec_err_count);
			chprintf((BaseSequentialStream*)&SD1, "Transceiver errors:    %d\r\n", xbee->trans_errs);
			chprintf((BaseSequentialStream*)&SD1, "Unicast transmittions: %d\r\n", xbee->unicast_trans_count);
			chprintf((BaseSequentialStream*)&SD1, "RSSI:                  %d\r\n", xbee->rssi);
			chSemSignal(&usart1_semaph);
			break;
		case XBEE_GET_PING:
			chSemWait(&usart1_semaph);
			chprintf((BaseSequentialStream*)&SD1, "Ping hello message\r\n");
			chSemSignal(&usart1_semaph);
			xbee_send_ping_message(xbee);
			break;
		case XBEE_GET_CHANNELS:
			xbee->channels = xbee_read_channels(xbee);
			chSemWait(&usart1_semaph);
			chprintf((BaseSequentialStream*)&SD1, "Channels settings: %x\r\n", xbee->channels);
			chSemSignal(&usart1_semaph);
			break;
		}


		xbee->suspend_state = 1;
	}
}

/*
 * Thread to process data collection and filtering from NEO-M8P
 */
thread_reference_t coords_trp = NULL;
static THD_WORKING_AREA(coords_thread_wa, 4096);
static THD_FUNCTION(coords_thread, arg) {

	(void)arg;
	msg_t msg;
	chRegSetThreadName("GPS Parse");
	gptStop(&GPTD12);
#ifndef TRAINER_MODULE
	gptStart(&GPTD12, &gpt12cfg);
	gptStartContinuous(&GPTD12, 5000);
#endif
	while (true) {
		chSysLock();
		if (neo->suspend_state) {
			msg = chThdSuspendS(&coords_trp);
		}
		chSysUnlock();
		//if (read_pvt == 1){

			chSemWait(&spi2_semaph);
			neo_create_poll_request(UBX_NAV_CLASS, UBX_NAV_PVT_ID);
					chThdSleepMilliseconds(5);
					neo_poll();
					chSemSignal(&spi2_semaph);
					read_pvt = 0;
		/*}else{
			chSemWait(&spi2_semaph);
			neo_create_poll_request(UBX_NAV_CLASS, UBX_NAV_ODO_ID);
					chThdSleepMilliseconds(5);
					neo_poll();
					chSemSignal(&spi2_semaph);
					read_pvt = 1;
		}*/
		chThdSleepMilliseconds(25);

		palToggleLine(LINE_RED_LED);
		neo->suspend_state = 1;

	}
}

/*
 * Thread to process data collection and filtering from MPU9250
 */
thread_reference_t mpu_trp = NULL;
static THD_WORKING_AREA(mpu_thread_wa, 4096);
static THD_FUNCTION(mpu_thread, arg) {

	(void)arg;
	msg_t msg;
	chRegSetThreadName("MPU9250 Thread");
	gptStop(&GPTD11);
#ifndef TRAINER_MODULE
	gptStart(&GPTD11, &gpt11cfg);
	gptStartContinuous(&GPTD11, 2000);
#endif
	while (true) {
		chSysLock();
		if (mpu->suspend_state) {
			msg = chThdSuspendS(&mpu_trp);
		}
		chSysUnlock();
		switch(msg){
		case MPU_GET_GYRO_DATA:
			bno055_read_euler(bno055);
			send_json(pvt_box, bno055);
			//mpu_get_gyro_data();
			break;
		}
	}
}
/*
 * This is a periodic thread that does absolutely nothing except flashing
 * a LED.
 */msg_t msg;
thread_reference_t shell_trp = NULL;
static THD_WORKING_AREA(shell_thread_wa, 512);
static THD_FUNCTION(shell_thread, arg) {

	(void)arg;
	msg_t msg;
	chRegSetThreadName("Shell Thread");
	while (true) {
		chSysLock();
		msg = chThdSuspendS(&shell_trp);
		chSysUnlock();

		/* Perform processing here.*/
		switch (msg){
		case SHELL_UBX_COG_STATUS:
			neo_create_poll_request(UBX_CFG_CLASS, UBX_CFG_NAV5_ID);
			chThdSleepMilliseconds(100);
			neo_poll();
			chThdSleepMilliseconds(50);
			neo_poll();
			chThdSleepMilliseconds(50);
			break;
		case SHELL_UBX_RATE_STATUS:
			neo_create_poll_request(UBX_CFG_CLASS, UBX_CFG_RATE_ID);
			chThdSleepMilliseconds(50);
			neo_poll();
			chThdSleepMilliseconds(50);
			neo_poll();
			break;
		case SHELL_UBX_RATE_SET:
			neo_create_poll_request(UBX_CFG_CLASS, UBX_CFG_RATE_ID);
			chThdSleepMilliseconds(50);
			neo_poll();
			rate_box->measRate = 250;
			chThdSleepMilliseconds(50);
			neo_write_struct((uint8_t *)rate_box, UBX_CFG_CLASS, UBX_CFG_RATE_ID, sizeof(ubx_cfg_rate_t));
			chThdSleepMilliseconds(50);
			neo_poll();
			chThdSleepMilliseconds(50);
			break;
		default:
			break;
		}
	}
}

/*
 * Thread that works with UBLOX NINA Bluetooth
 */
/*
thread_reference_t bt_trp = NULL;
static THD_WORKING_AREA(bt_thread_wa, 256);
static THD_FUNCTION(bt_thread, arg){
	(void)arg;
	msg_t msg;
	chRegSetThreadName("BT Thd");
	while (true) {
		chSysLock();
		if (output->suspend_state) {
			msg = chThdSuspendS(&bt_trp);
		}
		chSysUnlock();
		switch (msg){
		case NINA_GET_DISCOVERABLE:
			nina_get_discoverable_status();
			break;

			event_listener_t elSerData;
			eventmask_t flags;
			chEvtRegisterMask((EventSource *)chnGetEventSource(&SD7), &elSerData, EVENT_MASK(1));

			while (TRUE)
			{
				chEvtWaitOneTimeout(EVENT_MASK(1), MS2ST(10));
				chSysLock();
				flags = chEvtGetAndClearFlags(&elSerData);
				chSysUnlock();
				if (flags & CHN_INPUT_AVAILABLE)
				{
					msg_t charbuf;
					do
					{
						charbuf = chnGetTimeout(&SD1, TIME_IMMEDIATE);
						if ( charbuf != Q_TIMEOUT )
						{
							chSequentialStreamPut(&SD1, charbuf);
						}
					}
					while (charbuf != Q_TIMEOUT);
				}
			}

		}
	}
}
*/
/*
 * Thread that outputs debug data which is needed
 */
thread_reference_t output_trp = NULL;
static THD_WORKING_AREA(output_thread_wa, 1024*2);
static THD_FUNCTION(output_thread, arg) {
	(void)arg;
	int32_t spdi = 0;
	char lon[20];
	char lat[20];
	double spd;
	msg_t msg;
	chRegSetThreadName("Data output");
	gptStop(&GPTD14);
	gptStart(&GPTD14, &gpt14cfg);
	gptStartContinuous(&GPTD14, 2000);
	while (true) {
		chSysLock();
		if (output->suspend_state) {
			msg = chThdSuspendS(&output_trp);
		}
		chSysUnlock();
		palToggleLine(LINE_GREEN_LED);
		//chprintf((BaseSequentialStream*)&SD1, "output is %d\n\r", output->test);
		if (output->test){
			//chSemWait(&usart1_semaph);
			//chprintf((BaseSequentialStream*)&SD1, "Test output\n\r");
			//chSemSignal(&usart1_semaph);
			send_data(OUTPUT_XBEE);
		}else{
			if (output->gps){

				itoa(pvt_box->lat, lat, 10);
				itoa(pvt_box->lon, lon, 10);
				insert_dot(lat);
				insert_dot(lon);
				spd = (float)(pvt_box->gSpeed * 0.0036);
				spdi = (int32_t)(spd);
				chSemWait(&usart1_semaph);
				//chprintf((BaseSequentialStream*)&SD1, "GPS output\n\r");
				chprintf((BaseSequentialStream*)&SD1, "%s,", lat);
				chprintf((BaseSequentialStream*)&SD1, "%s,", lon);
				chprintf((BaseSequentialStream*)&SD1, "%d,", pvt_box->hour);
				chprintf((BaseSequentialStream*)&SD1, "%d,", pvt_box->min);
				chprintf((BaseSequentialStream*)&SD1, "%d,", pvt_box->sec);
				chprintf((BaseSequentialStream*)&SD1, "%d,", pvt_box->numSV);
				chprintf((BaseSequentialStream*)&SD1, "%d",  spdi);
				chprintf((BaseSequentialStream*)&SD1, "\r\n");
				chSemSignal(&usart1_semaph);
			}else if (output->ypr){
				chSemWait(&usart1_semaph);
				chprintf((BaseSequentialStream*)&SD1, "Yaw: %f, Pitch: %f, Roll: %f\n\r",
						mpu->yaw, mpu->pitch, mpu->roll);
				//chprintf((BaseSequentialStream*)&SD1, "%f;%f;%f\n\r", mpu->mx, mpu->my, mpu->mz);
				chSemSignal(&usart1_semaph);
			}else if (output->gyro){
				chSemWait(&usart1_semaph);
				/*chprintf((BaseSequentialStream*)&SD1, "AX: %d, AY: %d, AZ: %d  GX: %d, GY: %d, GZ: %d  MX: %d, MY: %d, MZ: %d\r\n",
						mpu->accelCount[0], mpu->accelCount[1], mpu->accelCount[2],
						mpu->gyroCount[0], mpu->gyroCount[1], mpu->gyroCount[2],
						mpu->magCount[0], mpu->magCount[1], mpu->magCount[2]);*/
				chprintf((BaseSequentialStream*)&SD1, "AX: %f, AY: %f, AZ: %f  GX: %f, GY: %f, GZ: %f  MX: %f, MY: %f, MZ: %f\r\n",
										mpu->ax, mpu->ay, mpu->az,
										mpu->gx, mpu->gy, mpu->gz,
										mpu->mx, mpu->my, mpu->mz);
				chSemSignal(&usart1_semaph);
			}else if(output->xbee){
				send_data(OUTPUT_XBEE);
			}
		}
		output->suspend_state = 1;

	}
}

void send_data(uint8_t stream){
	uint8_t databuff[34];
	int32_t spdi = 0;
	double spd;
	double dlat, dlon;
	spd = (float)(pvt_box->gSpeed * 0.0036);
	spdi = (int32_t)(spd);
	//tx_box->lat_cel = (int16_t)pvt_box->lat;
	//modf(pvt_box->lat, &dlat);
	tx_box->lat = pvt_box->lat;
	//tx_box->lat_drob = (uint16_t)((pvt_box->lat - tx_box->lat_cel)*10000000);
	tx_box->lon = pvt_box->lon;
	//modf(pvt_box->lon, &dlon);
	//tx_box->lon_cel = (int16_t)dlon;
	//tx_box->lon_drob = (uint16_t)((pvt_box->lon - tx_box->lon_cel)*10000000);
	tx_box->hour = pvt_box->hour;
	tx_box->min = pvt_box->min;
	tx_box->sec = pvt_box->sec;
	tx_box->dist = (uint16_t)odo_box->distance;
	tx_box->sat = pvt_box->numSV;
	tx_box->speed = spd;
	tx_box->headMot = pvt_box->headMot;
	tx_box->headVeh = pvt_box->headVeh;
	//if(stream == OUTPUT_USART){
		//chSemWait(&usart1_semaph);
	/*	chprintf((BaseSequentialStream*)&SD1, "%d.%d;", tx_box->lat_cel, tx_box->lat_drob);
		    chprintf((BaseSequentialStream*)&SD1, "%d.%d;", tx_box->lon_cel, tx_box->lon_drob);
		    chprintf((BaseSequentialStream*)&SD1, "%d:", tx_box->hour);
		    chprintf((BaseSequentialStream*)&SD1, "%d:", tx_box->min);
		    chprintf((BaseSequentialStream*)&SD1, "%d;", tx_box->sec);
		    chprintf((BaseSequentialStream*)&SD1, "%d;", tx_box->sat);
		    chprintf((BaseSequentialStream*)&SD1, "%d",  tx_box->dist);
		    chprintf((BaseSequentialStream*)&SD1, "%d",  tx_box->speed);
		    chprintf((BaseSequentialStream*)&SD1, "\r\n");
		*/
	//	chprintf((BaseSequentialStream*)&SD1, "%d;%d;%d:%d:%d:%d:%d:%d:\r\n",
		//		tx_box->lat, tx_box->lon, tx_box->hour,
		//		tx_box->min, tx_box->sec, tx_box->sat, tx_box->dist, tx_box->speed);
		//chSemSignal(&usart1_semaph);
//	}else if (stream == OUTPUT_XBEE){
	//	wdgReset(&WDGD1);
		//memcpy(&tx_box->lat, &databuff[0], sizeof(float));
		databuff[0] = RF_GPS_PACKET;
		databuff[1] = (uint8_t)(tx_box->lat >> 24);
		databuff[2] = (uint8_t)(tx_box->lat >> 16 );
		databuff[3] = (uint8_t)(tx_box->lat >> 8);
		databuff[4] = (uint8_t)(tx_box->lat);
		//memcpy(&tx_box->lon, &databuff[4], sizeof(float));
		databuff[5] = (uint8_t)(tx_box->lon >> 24);
		databuff[6] = (uint8_t)(tx_box->lon >> 16);
		databuff[7] = (uint8_t)(tx_box->lon >> 8);
		databuff[8] = (uint8_t)(tx_box->lon);
		databuff[9] = tx_box->hour;
		databuff[10] = tx_box->min;
		databuff[11] = tx_box->sec;
		databuff[12] = tx_box->sat;
		databuff[13] = (uint8_t)(tx_box->dist >> 8);
		databuff[14] = (uint8_t)(tx_box->dist);

		memcpy(&databuff[15], &tx_box->speed, sizeof(tx_box->speed));
		//memcpy(&databuff[15], &mag_offset[0], sizeof(mag_offset[0]));

		//databuff[15] = (uint8_t)(tx_box->speed >> 24);
		//databuff[16] = (uint8_t)(tx_box->speed >> 16);
		//databuff[17] = (uint8_t)(tx_box->speed >> 8);
		//databuff[18] = (uint8_t)(tx_box->speed);

		databuff[19] = (uint8_t)(tx_box->yaw >> 8);
		databuff[20] = (uint8_t)(tx_box->yaw);

		memcpy(&databuff[21], &tx_box->pitch, sizeof(tx_box->pitch));
		//memcpy(&databuff[21], &mag_offset[1], sizeof(mag_offset[1]));
		//databuff[21] = (int16_t)(tx_box->pitch);
		//databuff[22] = (int16_t)(tx_box->pitch * 10 % 10);
		//databuff[21] = (uint8_t)(tx_box->pitch >> 24);
		//databuff[22] = (uint8_t)(tx_box->pitch >> 16);
		//databuff[23] = (uint8_t)(tx_box->pitch >> 8);
		//databuff[24] = (uint8_t)(tx_box->pitch);

		//memcpy(&databuff[25], &mag_offset[2], sizeof(mag_offset[2]));
		memcpy(&databuff[25], &tx_box->roll, sizeof(tx_box->roll));
		//databuff[25] = (uint8_t)(tx_box->roll >> 24);
		//databuff[26] = (uint8_t)(tx_box->roll >> 16);
		//databuff[27] = (uint8_t)(tx_box->roll >> 8);
		//databuff[28] = (uint8_t)(tx_box->roll);
		databuff[29] = tx_box->bat;

		databuff[30] = (uint8_t)(tx_box->headMot >> 24);
		databuff[31] = (uint8_t)(tx_box->headMot >> 16);
		databuff[32] = (uint8_t)(tx_box->headMot >> 8);
		databuff[33] = (uint8_t)(tx_box->headMot);
/*
		databuff[27] = (uint8_t)(tx_box->headVeh >> 24);
		databuff[28] = (uint8_t)(tx_box->headVeh >> 16);
		databuff[29] = (uint8_t)(tx_box->headVeh >> 8);
		databuff[30] = (uint8_t)(tx_box->headVeh);
*/
		xbee_send_rf_message(xbee, databuff, 34);
	//}
}

void send_json(ubx_nav_pvt_t *pvt_box, bno055_t *bno055)
{
	chprintf((BaseSequentialStream*)&SD1, "\r\n{\"msg_type\":\"boats_data\",\r\n\t\t\"boat_1\":{\r\n\t\t\t");
		chprintf((BaseSequentialStream*)&SD1, "\"hour\":%d,\r\n\t\t\t", pvt_box->hour);
		chprintf((BaseSequentialStream*)&SD1, "\"min\":%d,\r\n\t\t\t", pvt_box->min);
		chprintf((BaseSequentialStream*)&SD1, "\"sec\":%d,\r\n\t\t\t", pvt_box->sec);
		chprintf((BaseSequentialStream*)&SD1, "\"lat\":%f,\r\n\t\t\t", pvt_box->lat / 10000000.0f);
		chprintf((BaseSequentialStream*)&SD1, "\"lon\":%f,\r\n\t\t\t", pvt_box->lon / 10000000.0f);
		chprintf((BaseSequentialStream*)&SD1, "\"speed\":%f,\r\n\t\t\t", (float)(pvt_box->gSpeed * 0.0036));
		chprintf((BaseSequentialStream*)&SD1, "\"dist\":%d,\r\n\t\t\t", (uint16_t)odo_box->distance);
		chprintf((BaseSequentialStream*)&SD1, "\"yaw\":%d,\r\n\t\t\t", (uint16_t)bno055->d_euler_hpr.h);
		chprintf((BaseSequentialStream*)&SD1, "\"pitch\":%f,\r\n\t\t\t", bno055->d_euler_hpr.p);
		chprintf((BaseSequentialStream*)&SD1, "\"roll\":%f,\r\n\t\t\t", bno055->d_euler_hpr.r);
		chprintf((BaseSequentialStream*)&SD1, "\"headMot\":%d,\r\n\t\t\t", (uint16_t)(pvt_box->headMot / 100000));
		chprintf((BaseSequentialStream*)&SD1, "\"sat\":%d,\r\n\t\t\t", pvt_box->numSV);
		//chprintf((BaseSequentialStream*)&SD1, "\"mag_decl\":%f,\r\n\t\t\t", pvt_box->magDec / 100.0f);
		chprintf((BaseSequentialStream*)&SD1, "\"rssi\":%d,\r\n\t\t\t", xbee->rssi);
		chprintf((BaseSequentialStream*)&SD1, "\"wind_dir\":%d,\r\n\t\t\t", wind->direction);
		chprintf((BaseSequentialStream*)&SD1, "\"wind_spd\":%f,\r\n\t\t\t", wind->speed);
	//	chprintf((BaseSequentialStream*)&SD1, "\"accel_raw\":%d; %d; %d,\r\n\t\t\t", bno055->accel_raw.x, bno055->accel_raw.y, bno055->accel_raw.z);
	//	chprintf((BaseSequentialStream*)&SD1, "\"gyro_raw\":%d; %d; %d,\r\n\t\t\t", bno055->gyro_raw.x, bno055->gyro_raw.y, bno055->gyro_raw.z);
	//	chprintf((BaseSequentialStream*)&SD1, "\"magn_raw\":%d; %d; %d,\r\n\t\t\t", bno055->mag_raw.x, bno055->mag_raw.y, bno055->mag_raw.z);
	//	chprintf((BaseSequentialStream*)&SD1, "\"magn_cal\":%d,\r\n\t\t\t", bno055->magn_cal);
	//	chprintf((BaseSequentialStream*)&SD1, "\"accel_cal\":%d,\r\n\t\t\t", bno055->accel_cal);
	//	chprintf((BaseSequentialStream*)&SD1, "\"gyro_cal\":%d,\r\n\t\t\t", bno055->gyro_cal);
		chprintf((BaseSequentialStream*)&SD1, "\"bat\":0\r\n\t\t\t");
		chprintf((BaseSequentialStream*)&SD1, "}\r\n\t}");
}

static void gpt9cb(GPTDriver *gptp){
	(void)gptp;
	chSysLockFromISR();
	chThdResumeI(&xbee_poll_trp, (msg_t)MPU_GET_GYRO_DATA);  /* Resuming the thread with message.*/
	chSysUnlockFromISR();

}

static void gpt11cb(GPTDriver *gptp){
	(void)gptp;

	chSysLockFromISR();
	chThdResumeI(&mpu_trp, (msg_t)MPU_GET_GYRO_DATA);  /* Resuming the thread with message.*/
	chSysUnlockFromISR();

}

static void gpt12cb(GPTDriver *gptp){
	(void)gptp;

	chSysLockFromISR();
	chThdResumeI(&coords_trp, (msg_t)0x01);  /* Resuming the thread with message.*/
	chSysUnlockFromISR();

}

/*
 * GPT14  callback.
 */
static void gpt14cb(GPTDriver *gptp)
{
	(void)gptp;

	chSysLockFromISR();
	chThdResumeI(&output_trp, (msg_t)0x01);  /* Resuming the thread with message.*/
	chSysUnlockFromISR();
}

void init_modules(void){
	chSysLock();
	neo_switch_to_ubx();
	chThdSleepMilliseconds(50);
	neo_poll();


	chThdSleepMilliseconds(50);
	//neo_set_pvt_1hz();
	chThdSleepMilliseconds(50);
	neo_poll();
	chThdSleepMilliseconds(50);
	neo_create_poll_request(UBX_CFG_CLASS, UBX_CFG_RATE_ID);
	chThdSleepMilliseconds(50);
	neo_poll();
	rate_box->measRate = 250;
	chThdSleepMilliseconds(50);
	neo_write_struct((uint8_t *)rate_box, UBX_CFG_CLASS, UBX_CFG_RATE_ID, sizeof(ubx_cfg_rate_t));
	chThdSleepMilliseconds(50);
	neo_poll();
	chThdSleepMilliseconds(50);
	neo_create_poll_request(UBX_CFG_CLASS, UBX_CFG_RATE_ID);
	chThdSleepMilliseconds(50);
	neo_poll();
	chThdSleepMilliseconds(100);

	neo_create_poll_request(UBX_CFG_CLASS, UBX_CFG_NAV5_ID);
	chThdSleepMilliseconds(100);
	neo_poll();
	chThdSleepMilliseconds(50);
	neo_poll();
	chThdSleepMilliseconds(50);
	//neo_write_struct((uint8_t *)nav5_box, UBX_CFG_CLASS, UBX_CFG_NAV5_ID, sizeof(ubx_cfg_nav5_t));
	//chThdSleepMilliseconds(50);
	//neo_poll();
	chSysUnlock();
}

extern void chSysHalt(const char*);
void _unhandled_exception(void) {
	//Copy to local variables (not pointers) to allow GDB "i loc" to directly show the info
	    //Get thread context. Contains main registers including PC and LR
	    struct port_extctx ctx;
	    memcpy(&ctx, (void*)__get_PSP(), sizeof(struct port_extctx));
	    (void)ctx;
	    //Interrupt status register: Which interrupt have we encountered, e.g. HardFault?
	    FaultType faultType = (FaultType)__get_IPSR();
	    (void)faultType;
	    //For HardFault/BusFault this is the address that was accessed causing the error
	    uint32_t faultAddress = SCB->BFAR;
	    (void)faultAddress;
	    //Flags about hardfault / busfault
	    //See http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0552a/Cihdjcfc.html for reference
	    bool isFaultPrecise = ((SCB->CFSR >> SCB_CFSR_BUSFAULTSR_Pos) & (1 << 1) ? true : false);
	    bool isFaultImprecise = ((SCB->CFSR >> SCB_CFSR_BUSFAULTSR_Pos) & (1 << 2) ? true : false);
	    bool isFaultOnUnstacking = ((SCB->CFSR >> SCB_CFSR_BUSFAULTSR_Pos) & (1 << 3) ? true : false);
	    bool isFaultOnStacking = ((SCB->CFSR >> SCB_CFSR_BUSFAULTSR_Pos) & (1 << 4) ? true : false);
	    bool isFaultAddressValid = ((SCB->CFSR >> SCB_CFSR_BUSFAULTSR_Pos) & (1 << 7) ? true : false);
	    (void)isFaultPrecise;
	    (void)isFaultImprecise;
	    (void)isFaultOnUnstacking;
	    (void)isFaultOnStacking;
	    (void)isFaultAddressValid;
	    //Cause debugger to stop. Ignored if no debugger is attached
	    bkpt();
	    NVIC_SystemReset();
}

/*
 * Application entry point.
 */
int main(void) {
	uint32_t *ACTLR = (uint32_t *)0xE000E008;
	*ACTLR |= 2;
	thread_t *sh = NULL;
	float mag_scaling[3];

	/*
	 * System initializations.
	 * - HAL initialization, this also initializes the configured device drivers
	 *   and performs the board-specific initializations.
	 * - Kernel initialization, the main() function becomes a thread and the
	 *   RTOS is active.
	 */
	halInit();
	chSysInit();
	chSemObjectInit(&usart1_semaph, 1);
	chSemObjectInit(&spi2_semaph, 1);
	/*
	 * Activates the serial driver 1 using the driver default configuration.
	 */

	spiStart(&SPID1, &spi1_cfg);
	spiStart(&SPID2, &neo_spi_cfg);
	i2cStart(&I2CD1, &i2c1cfg);
	//i2cStart(&I2CD1, &i2ccfg);
	xbee->suspend_state = 1;

	/*
     B* Shell manager initialization.
	 */
#ifdef USE_SD_SHELL
	sdStart(&SD1, NULL);
	shellInit();
	chSemWait(&usart1_semaph);
	sh = cmd_init();
	chSemSignal(&usart1_semaph);
#else
	sdStart(&SD1, NULL);
#endif
	//sdStart(&SD7, &sd7cfg);
	//chprintf((BaseSequentialStream*)&SD7, "AT+CPWROFF\r");
	mpu9250_init();

	chThdSleepMilliseconds(100);

	mpu->mx = 0.0f;
	mpu->my = 0.0f;
	mpu->mz = 0.0f;
	initAK8963(&mpu->magCalibration[0]);
	chThdSleepMilliseconds(300);
	mpu_get_gyro_data();
	chThdSleepMilliseconds(300);
	mpu_get_gyro_data();

	while((mpu->mx == 0.0f) && (mpu->my == 0.0f) && (mpu->mz == 0.0f)){
	chSemWait(&usart1_semaph);
	chprintf((BaseSequentialStream*)&SD1, "Looks like magn failed to startup, trying to recover...\r\n");
	chSemSignal(&usart1_semaph);
	initAK8963(&mpu->magCalibration[0]);
	chThdSleepMilliseconds(300);
	mpu_get_gyro_data();
	chThdSleepMilliseconds(300);
	mpu_get_gyro_data();
	chThdSleepMilliseconds(300);
	mpu_get_gyro_data();
	}
	chSemWait(&usart1_semaph);
	chprintf((BaseSequentialStream*)&SD1, "Looks like magn starts successfully\r\n");
	chSemSignal(&usart1_semaph);


	output->suspend_state = 1;
	xbee->suspend_state = 1;
	xbee->poll_suspend_state = 1;
	xbee->tx_ready = 1;
	neo->suspend_state = 1;
	mpu->suspend_state = 1;

	palClearLine(LINE_RF_868_RST);
	chThdSleepMilliseconds(100);
	palSetLine(LINE_RF_868_RST);

	uint8_t chip_id;
	bno055_full_init(bno055);

//	mmcObjectInit(&MMCD1);
//	  mmcStart(&MMCD1, &portab_mmccfg);
//	  InsertSD();

	  /*
	   * Starting the watchdog driver.
	   */
	//wdgStart(&WDGD1, &wdgcfg);

	// set up the timer




	//wdgReset(&WDGD1);

	/*
	 * Creates threads.
	 */
//	chSemWait(&usart1_semaph);
//		chprintf((BaseSequentialStream*)&SD1, "THD\r\n");
//		chSemSignal(&usart1_semaph);

		neo_switch_to_ubx();
				chThdSleepMilliseconds(50);
			//	neo_set_pvt_1hz();
					chThdSleepMilliseconds(50);
				neo_create_poll_request(UBX_CFG_CLASS, UBX_CFG_RATE_ID);
					chThdSleepMilliseconds(50);
					neo_poll();
					rate_box->measRate = 250;
					chThdSleepMilliseconds(50);
					neo_write_struct((uint8_t *)rate_box, UBX_CFG_CLASS, UBX_CFG_RATE_ID, sizeof(ubx_cfg_rate_t));
					chThdSleepMilliseconds(50);
					neo_poll();
					chThdSleepMilliseconds(50);

					neo_create_poll_request(UBX_CFG_CLASS, UBX_CFG_ODO_ID);
					chThdSleepMilliseconds(50);
					neo_poll();
					cfg_odo_box->flags = 1 << 0;
					chThdSleepMilliseconds(50);
					neo_write_struct((uint8_t *)cfg_odo_box, UBX_CFG_CLASS, UBX_CFG_ODO_ID, sizeof(ubx_cfg_odo_t));
					chThdSleepMilliseconds(50);
					neo_poll();

	chThdCreateStatic(xbee_thread_wa, sizeof(xbee_thread_wa), NORMALPRIO, xbee_thread, NULL);
	chThdCreateStatic(xbee_poll_thread_wa, sizeof(xbee_poll_thread_wa), NORMALPRIO, xbee_poll_thread, NULL);
	chThdCreateStatic(shell_thread_wa, sizeof(shell_thread_wa), NORMALPRIO, shell_thread, NULL);
	chThdCreateStatic(output_thread_wa, sizeof(output_thread_wa), NORMALPRIO, output_thread, NULL);
	chThdCreateStatic(coords_thread_wa, sizeof(coords_thread_wa), NORMALPRIO, coords_thread, NULL);
	chThdCreateStatic(mpu_thread_wa, sizeof(mpu_thread_wa), NORMALPRIO, mpu_thread, NULL);

	start_windsensor_module();
	palEnableLineEventI(LINE_RF_868_SPI_ATTN, PAL_EVENT_MODE_FALLING_EDGE);
	palSetLineCallbackI(LINE_RF_868_SPI_ATTN, xbee_attn_event, NULL);

//	chSemWait(&usart1_semaph);
//		chprintf((BaseSequentialStream*)&SD1, "MAG_Calibration: %f, %f, %f\r\n", mpu->magCalibration[0], mpu->magCalibration[1], mpu->magCalibration[2]);
//		chSemSignal(&usart1_semaph);
/*
	neo_switch_to_ubx();
		chThdSleepMilliseconds(50);
		neo_create_poll_request(UBX_CFG_CLASS, UBX_CFG_RATE_ID);
			chThdSleepMilliseconds(50);
			neo_poll();
			rate_box->measRate = 250;
			chThdSleepMilliseconds(50);
			neo_write_struct((uint8_t *)rate_box, UBX_CFG_CLASS, UBX_CFG_RATE_ID, sizeof(ubx_cfg_rate_t));
			chThdSleepMilliseconds(50);
			neo_poll();
			chThdSleepMilliseconds(50);
*/

	//	xbee_get_attn_pin_cfg(xbee);

	//	chThdSleepMilliseconds(300);
	//	chprintf((BaseSequentialStream*)&SD7, "AT+UBTDM?\r");

		xbee_set_10kbs_rate();
		//eeprom_write_hw_version();
		chThdSleepMilliseconds(100);
		//eeprom_read_hw_version();

		//xbee_read_baudrate(xbee);
		//chThdSleepMilliseconds(100);
	//	xbee_read_channels(xbee);
//	chThdSleepMilliseconds(3000);
		// configure the timer to fire after 25 timer clock tics
	//   The clock is running at 200,000Hz, so each tick is 50uS,
	//   so 200,000 / 25 = 8,000Hz
		//chThdSleepMilliseconds(1000);


		//mag_calibration(&mag_offset[0], &mag_scaling[0]);
		mpu->magbias[0] = mag_offset[0];  // User environmental x-axis correction in milliGauss, should be automatically calculated
			mpu->magbias[1] = mag_offset[1];  // User environmental x-axis correction in milliGauss
			mpu->magbias[2] = mag_offset[2];  // User environmental x-axis correction in milliGauss
		//toggle_test_output();
		//toggle_ypr_output();
		//toggle_gyro_output();
	/*
	 * Normal main() thread activity, in this demo it does nothing except
	 * sleeping in a loop and check the button state.
	 */
	while (true) {
#ifdef USE_SD_SHELL
		if (!sh)
			sh = cmd_init();
		else if (chThdTerminatedX(sh)) {
			chThdRelease(sh);
			sh = NULL;
		}
#endif
		//chThdWait(shelltp);               /* Waiting termination.*/
		chThdSleepMilliseconds(1000);

	}
}
