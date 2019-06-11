/*
## Cypress USB 3.0 Platform source file
## ===========================
##
##  Copyright Cypress Semiconductor Corporation, 2010-2011,
##  All Rights Reserved
##  UNPUBLISHED, LICENSED SOFTWARE.
##
##  CONFIDENTIAL AND PROPRIETARY INFORMATION
##  WHICH IS THE PROPERTY OF CYPRESS.
##
##  Use of this file is governed
##  by the license agreement included in the file
##
##     <install>/license/license.txt
##
##  where <install> is the Cypress software
##  installation root directory path.
##
## ===========================
*/

#include <cyu3usb.h>
#include <cyu3error.h>
#include <cyu3spi.h>
#include "include/debug.h"
#include "include/extension_unit.h"
#include "include/sensor_v034_raw.h"
#include "include/fx3_bsp.h"
#include "include/uvc.h"
#include "include/xp_sensor_firmware_version.h"
#include "include/inv_icm20608.h"
#include "include/tlc59116.h"
#include "include/tlc59108.h"
#include "include/sensor_ar0141.h"

  /* FLASH sector Memory Map
  --------------------------------------
  | Sector |    Start   |     End      |
  --------------------------------------
  |   0    | 0000 0000h | 0000 FFFFh   |
  --------------------------------------
  |   1    | 0001 0000h | 0001 FFFFh   |
  --------------------------------------
  |   2    | 0002 0000h | 0002 FFFFh   |
  --------------------------------------
  |   3    | 0003 0000h | 0003 FFFFh   |
  --------------------------------------
  |   4    | 0004 0000h | 0004 FFFFh   |
  --------------------------------------
  |   5    | 0005 0000h | 0005 FFFFh   |
  --------------------------------------
  |   6    | 0006 0000h | 0006 FFFFh   |
  --------------------------------------
  |   7    | 0007 0000h | 0007 FFFFh   |
  --------------------------------------
  Every sector size = 64KB, All sector is 512KB
  Sector 0 - 3 [0 - 256KB] is boot flash.
  Secotr 4 is Device message, include: Device ID
  Sector 5 is Device calib file, 
  Secotr 6 - 7 is not used now.
  */
uint16_t glSpiPageSize = 0x100;  /* SPI Page size to be used for transfers. */

/* Give a timeout value of 5s for any flash programming. */
#define CY_FX_FLASH_PROG_TIMEOUT                (5000)
CyU3PDmaChannel glSpiTxHandle;   /* SPI Tx channel handle */
CyU3PDmaChannel glSpiRxHandle;   /* SPI Rx channel handle */
static CyU3PReturnStatus_t CyFxFlashProgSpiWaitForStatus(void);

/* Array to hold sensor data */
volatile char last_imu[IMU_BURST_LEN] = {' '};

/**
 *  @brief      handle single imu read/write of extension unit request.
 *  @param[out] bRequest    bRequst value of uvc.
 *  @return     0 if successful.
 */
void EU_Rqts_imu_rw(uint8_t bRequest) {
  #define CMD_IMU_RW_LEN 5
  #define CMD_IMU_RW_R 0
  #define CMD_IMU_RW_W 1

  static uint8_t regval;
  uint8_t Ep0Buffer[32];
  uint16_t readCount;
  uint16_t regaddr;
  CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;

  switch (bRequest) {
  case CY_FX_USB_UVC_GET_CUR_REQ:
    Ep0Buffer[0] = 0;
    Ep0Buffer[1] = 0;
    Ep0Buffer[2] = 0;
    Ep0Buffer[3] = 0;
    Ep0Buffer[4] = regval;
    CyU3PUsbSendEP0Data(CMD_IMU_RW_LEN, (uint8_t *)Ep0Buffer);
    // sensor_dbg("EU request IMU read reg val: 0x%x\r\n", regval);
    break;
  case CY_FX_USB_UVC_SET_CUR_REQ:
    apiRetStatus = CyU3PUsbGetEP0Data(CMD_IMU_RW_LEN, Ep0Buffer, &readCount);
    if (apiRetStatus != CY_U3P_SUCCESS) {
      sensor_err("CyU3 get Ep0 data failed\r\n");
      CyFxAppErrorHandler(apiRetStatus);
    }
    regaddr = (Ep0Buffer[1] << 8) + Ep0Buffer[2];
    // sensor_dbg("regaddr: 0x%x\r\n", regaddr);
    if (Ep0Buffer[0] == CMD_IMU_RW_R) {
      icm_read_reg(regaddr, &regval);
    } else if (Ep0Buffer[0] == CMD_IMU_RW_W) {
      regval = (Ep0Buffer[3] << 8) + Ep0Buffer[4];
      icm_write_reg(regaddr, regval);
      sensor_info("warning: icm reg set write addr: 0x%x, val: 0x%x\r\n", regaddr, regval);
    }
    break;
  case CY_FX_USB_UVC_GET_LEN_REQ:
    Ep0Buffer[0] = CMD_IMU_RW_LEN;
    Ep0Buffer[1] = 0;
    CyU3PUsbSendEP0Data(2, (uint8_t *)Ep0Buffer);
    // sensor_dbg("EU request IMU_RW get len: 0x%x\r\n", Ep0Buffer[0]);
    break;
  case CY_FX_USB_UVC_GET_INFO_REQ:
    // sensor_dbg("EU request IMU_RW get info\r\n");
    Ep0Buffer[0] = 3;
    CyU3PUsbSendEP0Data(1, (uint8_t *)Ep0Buffer);
    break;
  default:
    sensor_dbg("unknown reg rw cmd: 0x%x\r\n", bRequest);
    CyU3PUsbStall(0, CyTrue, CyFalse);
    break;
  }
}

/**
 *  @brief      handle burst imu read of extension unit request.
 *  @param[out] bRequest    bRequst value of uvc.
 *  @return     0 if successful.
 */
void EU_Rqts_imu_burst(uint8_t bRequest) {
  uint8_t Ep0Buffer[32];
  #ifndef IMU_LOOP_SAMPLE
  int status;
  uint8_t raw_IMU_data[14];
  #endif

  switch (bRequest) {
  case CY_FX_USB_UVC_GET_CUR_REQ:
    // sensor_dbg("EU request IMU burst get cur xu\r\n");
    #ifndef IMU_LOOP_SAMPLE
    status = icm_get_sensor_reg(raw_IMU_data, 0);
    if (status != CY_U3P_SUCCESS) {
        sensor_err("get icm data err\r\n");
    }
    int i = 0;
    for (i = 0; i < 6; ++i) {
      last_imu[i + 0] = (char)(raw_IMU_data[i]);
      last_imu[i + 6] = (char)(raw_IMU_data[i + 8]);
    }
    uint32_t t = CyU3PGetTime();
    last_imu[12] = t >> 24;
    last_imu[13] = t >> 16;
    last_imu[14] = t >> 8;
    last_imu[15] = t >> 0;
    #endif
    CyU3PUsbSendEP0Data(IMU_BURST_LEN, (uint8_t *)last_imu);
    break;
  case CY_FX_USB_UVC_SET_CUR_REQ:
    sensor_dbg("EU request IMU burst set cur xu\r\n");
    break;
  case CY_FX_USB_UVC_GET_LEN_REQ:
    Ep0Buffer[0] = IMU_BURST_LEN;
    Ep0Buffer[1] = 0;
    CyU3PUsbSendEP0Data(2, (uint8_t *)Ep0Buffer);
    sensor_dbg("EU request IMU burst get len: 0x%x\r\n", Ep0Buffer[0]);
    break;
  case CY_FX_USB_UVC_GET_INFO_REQ:
    sensor_dbg("EU request IMU burst get info\r\n");
    Ep0Buffer[0] = 3;
    CyU3PUsbSendEP0Data(1, (uint8_t *)Ep0Buffer);
    break;
  default:
    sensor_dbg("unknown reg burst cmd: 0x%x\r\n", bRequest);
    CyU3PUsbStall(0, CyTrue, CyFalse);
    break;
  }
}

/**
 *  @brief      handle spi flash action of extension unit request.
 *  @param[out] bRequest    bRequst value of uvc.
 *  @return     0 if successful.
 */
void EU_Rqts_spi_flash(uint8_t bRequest) {
  uint8_t Ep0Buffer[32] = {0};
  uint16_t readCount;
  CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;

  switch (bRequest) {
  case CY_FX_USB_UVC_GET_CUR_REQ:
    CyU3PUsbSendEP0Data(1, (uint8_t *)Ep0Buffer);
    break;
  case CY_FX_USB_UVC_SET_CUR_REQ:
    apiRetStatus = CyU3PUsbGetEP0Data(1, Ep0Buffer, &readCount);
    if (apiRetStatus != CY_U3P_SUCCESS) {
      sensor_err("CyU3 get Ep0 data failed\r\n");
      CyFxAppErrorHandler(apiRetStatus);
    }
    if (Ep0Buffer[0] == 'E') {
      sensor_dbg("erase flash\r\n");
      CyFxFlashProgEraseSector(CyTrue, 0, Ep0Buffer);
    } else if (Ep0Buffer[0] == 'D') {
      sensor_dbg("Device ID erase\r\n");
      CyFxFlashProgEraseSector(CyTrue, 4, Ep0Buffer);
    }
    break;
  case CY_FX_USB_UVC_GET_LEN_REQ:
    Ep0Buffer[0] = 1;
    Ep0Buffer[1] = 0;
    CyU3PUsbSendEP0Data(2, (uint8_t *)Ep0Buffer);
    break;
  case CY_FX_USB_UVC_GET_INFO_REQ:
    Ep0Buffer[0] = 3;
    CyU3PUsbSendEP0Data(1, (uint8_t *)Ep0Buffer);
    break;
  default:
    sensor_err("unknown flash cmd: 0x%x\r\n", bRequest);
    CyU3PUsbStall(0, CyTrue, CyFalse);
    break;
  }
}

/**
 *  @brief      handle camera chip register of extension unit request.
 *  @param[out] bRequest    bRequst value of uvc.
 *  @return     0 if successful.
 */
void EU_Rqts_cam_reg(uint8_t bRequest) {
  static uint16_t read_reg_addr = 0;
  uint8_t Ep0Buffer[32] = {0};
  uint8_t HighAddr;
  uint8_t LowAddr;
  uint8_t HighData;
  uint8_t LowData;
  uint16_t readval;
  uint16_t readCount;
  CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;

  switch (bRequest) {
  case CY_FX_USB_UVC_GET_CUR_REQ:
    // sensor_dbg("EU request camera reg get cur\r\n");
    Ep0Buffer[0] = read_reg_addr >> 8;
    Ep0Buffer[1] = read_reg_addr & 0xFF;
    if (sensor_type == XPIRL2 || sensor_type == XPIRL3 || sensor_type == XPIRL3_A)
      readval = AR0141_RegisterRead(Ep0Buffer[0], Ep0Buffer[1]);
    else
      readval = V034_RegisterRead(Ep0Buffer[0], Ep0Buffer[1]);

    Ep0Buffer[2] = readval >> 8;
    Ep0Buffer[3] = readval & 0xFF;
    CyU3PUsbSendEP0Data(4, (uint8_t *)Ep0Buffer);
    break;
  case CY_FX_USB_UVC_SET_CUR_REQ:
    // sensor_dbg("EU request camera reg set cur\r\n");
    apiRetStatus = CyU3PUsbGetEP0Data(1, Ep0Buffer, &readCount);
    if (apiRetStatus != CY_U3P_SUCCESS) {
      sensor_err("CyU3 get Ep0 data failed\r\n");
      CyFxAppErrorHandler(apiRetStatus);
    }
   /*
    * We use the MSB of Ep0Buffer[0] to mark what Ep0Buffer really stores:
    * 1) The reg_addr and reg_val for a write operation
    * 2) 0nly the reg_addr for the following read operation
    */
    if (!(Ep0Buffer[0] & 0x80)) {
      HighAddr = Ep0Buffer[0];
      LowAddr  = Ep0Buffer[1];
      HighData = Ep0Buffer[2];
      LowData  = Ep0Buffer[3];
      if (sensor_type == XPIRL2 || sensor_type == XPIRL3 || sensor_type == XPIRL3_A)
        AR0141_RegisterWrite(HighAddr, LowAddr, HighData, LowData);
      else
        V034_RegisterWrite(HighAddr, LowAddr, HighData, LowData);
      CyU3PThreadSleep(1);
    } else {
      read_reg_addr = Ep0Buffer[0] << 8| Ep0Buffer[1];
    }
    break;
  case CY_FX_USB_UVC_GET_LEN_REQ:
    sensor_dbg("EU request camera reg get len\r\n");
    Ep0Buffer[0] = 4;
    Ep0Buffer[1] = 0;
    CyU3PUsbSendEP0Data(2, (uint8_t *)Ep0Buffer);
    break;
  case CY_FX_USB_UVC_GET_INFO_REQ:
    sensor_dbg("EU request camera reg get info\r\n");
    Ep0Buffer[0] = 3;
    CyU3PUsbSendEP0Data(1, (uint8_t *)Ep0Buffer);
    break;
  default:
    sensor_err("unknown cam reg cmd: 0x%x\r\n", bRequest);
    CyU3PUsbStall(0, CyTrue, CyFalse);
    break;
  }
}

/**
 *  @brief      handle soft version information read of extension unit request.
 *  @param[out] bRequest    bRequst value of uvc.
 *  @return     0 if successful.
 */
void EU_Rqts_soft_version(uint8_t bRequest) {
  uint8_t Ep0Buffer[32];
  uint16_t readCount;
  char *version_info = FIRMWARE_VERSION;
  CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;

  switch (bRequest) {
  case CY_FX_USB_UVC_GET_CUR_REQ:
    CyU3PUsbSendEP0Data(strlen(version_info), (uint8_t *)version_info);
    sensor_dbg("EU software version get cur request\r\n");
    break;
  case CY_FX_USB_UVC_SET_CUR_REQ:
    apiRetStatus = CyU3PUsbGetEP0Data(1, Ep0Buffer, &readCount);
    if (apiRetStatus != CY_U3P_SUCCESS) {
      sensor_err("CyU3 get Ep0 data failed\r\n");
      CyFxAppErrorHandler(apiRetStatus);
    }
    sensor_dbg("EU software version set cur request\r\n");
    break;
  case CY_FX_USB_UVC_GET_LEN_REQ:
    Ep0Buffer[0] = strlen(version_info);
    Ep0Buffer[1] = 0;
    CyU3PUsbSendEP0Data(2, (uint8_t *)Ep0Buffer);
    sensor_dbg("EU software version get len:%d request\r\n", Ep0Buffer[0]);
    break;
  case CY_FX_USB_UVC_GET_INFO_REQ:
    Ep0Buffer[0] = 3;
    CyU3PUsbSendEP0Data(1, (uint8_t *)Ep0Buffer);
    sensor_dbg("EU software version get info request\r\n");
    break;
  default:
    sensor_err("unknown software version cmd: 0x%x\r\n", bRequest);
    CyU3PUsbStall(0, CyTrue, CyFalse);
    break;
  }
}

/**
 *  @brief      handle hard version information read of extension unit request.
 *  @param[out] bRequest    bRequst value of uvc.
 *  @return     0 if successful.
 */
void EU_Rqts_hard_version(uint8_t bRequest) {
  uint8_t Ep0Buffer[32];
  uint16_t readCount;
  CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;

  switch (bRequest) {
  case CY_FX_USB_UVC_GET_CUR_REQ:
    Ep0Buffer[0] = hardware_version_num >> 24;
    Ep0Buffer[1] = hardware_version_num >> 16;
    Ep0Buffer[2] = hardware_version_num >> 8;
    Ep0Buffer[3] = hardware_version_num >> 0;
    CyU3PUsbSendEP0Data(4, Ep0Buffer);
    sensor_dbg("EU hardware version get cur request\r\n");
    break;
  case CY_FX_USB_UVC_SET_CUR_REQ:
    apiRetStatus = CyU3PUsbGetEP0Data(1, Ep0Buffer, &readCount);
    if (apiRetStatus != CY_U3P_SUCCESS) {
      sensor_err("CyU3 get Ep0 data failed\r\n");
      CyFxAppErrorHandler(apiRetStatus);
    }
    sensor_dbg("EU hardware version set cur request\r\n");
    break;
  case CY_FX_USB_UVC_GET_LEN_REQ:
    Ep0Buffer[0] = 4;
    Ep0Buffer[1] = 0;
    CyU3PUsbSendEP0Data(2, (uint8_t *)Ep0Buffer);
    sensor_dbg("EU hardware version get len:%d request\r\n", Ep0Buffer[0]);
    break;
  case CY_FX_USB_UVC_GET_INFO_REQ:
    Ep0Buffer[0] = 3;
    CyU3PUsbSendEP0Data(1, (uint8_t *)Ep0Buffer);
    sensor_dbg("EU hardware version get info request\r\n");
    break;
  default:
    sensor_err("unknown hardware version cmd: 0x%x\r\n", bRequest);
    CyU3PUsbStall(0, CyTrue, CyFalse);
    break;
  }
}
/**
 *  @brief      control firmware flag.
 *  @param[out] bRequest    bRequst value of uvc.
 *  @return     0 if successful.
 */
void EU_Rqts_firmware_flag(uint8_t bRequest) {
  uint8_t Ep0Buffer[32];
  uint16_t readCount;
  CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;

  switch (bRequest) {
  case CY_FX_USB_UVC_GET_CUR_REQ:
    sensor_dbg("EU firmware flag get firmware_ctrl_flag: 0x%x\r\n", firmware_ctrl_flag);
    Ep0Buffer[0] = *((uint32_t *)(&firmware_ctrl_flag)) >> 24;
    Ep0Buffer[1] = *((uint32_t *)(&firmware_ctrl_flag)) >> 16;
    Ep0Buffer[2] = *((uint32_t *)(&firmware_ctrl_flag)) >> 8;
    Ep0Buffer[3] = *((uint32_t *)(&firmware_ctrl_flag)) >> 0;
    CyU3PUsbSendEP0Data(4, Ep0Buffer);
    break;
  case CY_FX_USB_UVC_SET_CUR_REQ:
    apiRetStatus = CyU3PUsbGetEP0Data(4, Ep0Buffer, &readCount);
    if (apiRetStatus != CY_U3P_SUCCESS) {
      sensor_err("CyU3 get Ep0 data failed\r\n");
      CyFxAppErrorHandler(apiRetStatus);
    }
    uint32_t firmware_ctrl_flag_tmp = Ep0Buffer[0] << 24 | \
                                      Ep0Buffer[1] << 16 | \
                                      Ep0Buffer[2] << 8  | \
                                      Ep0Buffer[3];
    CyU3PMemCopy((uint8_t *)(&firmware_ctrl_flag), (uint8_t *)(&firmware_ctrl_flag_tmp),
                  sizeof(firmware_ctrl_flag));
    debug_level = firmware_ctrl_flag.log_dbg | firmware_ctrl_flag.log_info << 1 \
                | firmware_ctrl_flag.log_dump << 2;
    sensor_dbg("EU firmware flag set firmware_ctrl_flag: 0x%x\r\n", firmware_ctrl_flag);
    break;
  case CY_FX_USB_UVC_GET_LEN_REQ:
    Ep0Buffer[0] = 4;
    Ep0Buffer[1] = 0;
    CyU3PUsbSendEP0Data(2, (uint8_t *)Ep0Buffer);
    sensor_dbg("EU firmware flag get len:%d request\r\n", Ep0Buffer[0]);
    break;
  case CY_FX_USB_UVC_GET_INFO_REQ:
    Ep0Buffer[0] = 3;
    CyU3PUsbSendEP0Data(1, (uint8_t *)Ep0Buffer);
    sensor_dbg("EU firmware flag get info request\r\n");
    break;
  default:
    sensor_err("unknown firmware_flag cmd: 0x%x\r\n", bRequest);
    CyU3PUsbStall(0, CyTrue, CyFalse);
    break;
  }
}

/**
 *  @brief      control firmware flag.
 *  @param[out] bRequest    bRequst value of uvc.
 *  @return     0 if successful.
 */
void EU_Rqts_IR_control(uint8_t bRequest) {
  uint8_t Ep0Buffer[32] = {0};
  uint16_t readCount;
  CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;

  switch (bRequest) {
  case CY_FX_USB_UVC_GET_CUR_REQ:
    if (sensor_type == XPIRL2  || sensor_type == XPIRL3) {
      Ep0Buffer[0] = *((uint32_t *)(&XPIRLx_IR_ctrl)) >> 24;
      Ep0Buffer[1] = *((uint32_t *)(&XPIRLx_IR_ctrl)) >> 16;
      Ep0Buffer[2] = *((uint32_t *)(&XPIRLx_IR_ctrl)) >> 8;
      Ep0Buffer[3] = *((uint32_t *)(&XPIRLx_IR_ctrl)) >> 0;
      sensor_dbg("EU IR control read : 0x%x\r\n", XPIRLx_IR_ctrl);
    } else {
      sensor_err("This sensor type don't support infrared light read control.\r\n");
    }
    CyU3PUsbSendEP0Data(4, Ep0Buffer);
    break;
  case CY_FX_USB_UVC_SET_CUR_REQ:
    apiRetStatus = CyU3PUsbGetEP0Data(4, Ep0Buffer, &readCount);
    if (apiRetStatus != CY_U3P_SUCCESS) {
      sensor_err("CyU3 get Ep0 data failed\r\n");
      CyFxAppErrorHandler(apiRetStatus);
    }
    if (sensor_type == XPIRL2 || sensor_type == XPIRL3) {
      if ((Ep0Buffer[3] & 0x80) == 0)
        Ep0Buffer[3] = Ep0Buffer[3] | 0x80;
      uint32_t XPIRL2_IR_ctrl_tmp = Ep0Buffer[0] << 24 | \
                                    Ep0Buffer[1] << 16 | \
                                    Ep0Buffer[2] << 8  | \
                                    Ep0Buffer[3];
      CyU3PMemCopy((uint8_t *)(&XPIRLx_IR_ctrl), (uint8_t *)(&XPIRL2_IR_ctrl_tmp),
                    sizeof(XPIRLx_IR_ctrl));
      sensor_dbg("EU IR control set : 0x%x\r\n", XPIRLx_IR_ctrl);
      if (sensor_type == XPIRL2)
        xpril2_proc_ir_ctl(&XPIRLx_IR_ctrl);
      else if (sensor_type == XPIRL3)
        xpril3_proc_ir_ctl(&XPIRLx_IR_ctrl);
      else
        sensor_err("This type sensor don't support IR control\r\n");
    } else {
      sensor_err("This sensor type don't support infrared light set control.\r\n");
    }
    break;
  case CY_FX_USB_UVC_GET_LEN_REQ:
    Ep0Buffer[0] = 4;
    Ep0Buffer[1] = 0;
    CyU3PUsbSendEP0Data(2, (uint8_t *)Ep0Buffer);
    sensor_dbg("EU IR control get len:%d request\r\n", Ep0Buffer[0]);
    break;
  case CY_FX_USB_UVC_GET_INFO_REQ:
    Ep0Buffer[0] = 3;
    CyU3PUsbSendEP0Data(1, (uint8_t *)Ep0Buffer);
    sensor_dbg("EU IR control get info request\r\n");
    break;
  default:
    sensor_err("unknown IR control cmd: 0x%x\r\n", bRequest);
    CyU3PUsbStall(0, CyTrue, CyFalse);
  //   break;
  }
}

/**
 *  @brief      handle flash Read/Write action of extension unit request.
 *  @param[out] bRequest    bRequst value of uvc.
 *  @return     NULL.
 */
void EU_Rqts_flash_RW(uint8_t bRequest) {
  uint8_t Ep0Buffer[32] = {0};
  struct flash_struct_t flash_store;
  uint16_t flash_len = sizeof (struct flash_struct_t);
  uint16_t readCount;
  CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;

  switch (bRequest) {
  case CY_FX_USB_UVC_GET_CUR_REQ:
    sensor_dbg("read flash\r\n");
    CyU3PMemSet ((uint8_t *)(&flash_store), 0, flash_len);
    apiRetStatus = CyFxFlashProgSpiTransfer(DEVICE_MSG_ADDR, flash_len, (uint8_t *)(&flash_store),
                                            CyTrue);
    sensor_info("read device ID: %s\n", flash_store.Sensor_ID);
    CyU3PUsbSendEP0Data(flash_len, (uint8_t *)(&flash_store));
    break;
  case CY_FX_USB_UVC_SET_CUR_REQ:
    sensor_dbg("write flash\r\n");
    apiRetStatus = CyU3PUsbGetEP0Data(flash_len, (uint8_t *)(&flash_store), &readCount);
    if (apiRetStatus != CY_U3P_SUCCESS) {
      sensor_err("CyU3 get Ep0 data failed\r\n");
      CyFxAppErrorHandler(apiRetStatus);
      break;
    }
    // write flash
    sensor_info("write device ID: %s\n", flash_store.Sensor_ID);
    CyFxFlashProgEraseSector(CyTrue, 4, Ep0Buffer);
    apiRetStatus = CyFxFlashProgSpiTransfer(DEVICE_MSG_ADDR, flash_len, (uint8_t *)(&flash_store),
                                            CyFalse);
    if (apiRetStatus != CY_U3P_SUCCESS) {
      sensor_err("Write Flash error\r\n");
    }
    break;
  case CY_FX_USB_UVC_GET_LEN_REQ:
    Ep0Buffer[0] = 255;
    Ep0Buffer[1] = 0;
    CyU3PUsbSendEP0Data(2, (uint8_t *)Ep0Buffer);
    break;
  case CY_FX_USB_UVC_GET_INFO_REQ:
    Ep0Buffer[0] = 3;
    CyU3PUsbSendEP0Data(1, (uint8_t *)Ep0Buffer);
    break;
  default:
    sensor_err("unknown flash cmd: 0x%x\r\n", bRequest);
    CyU3PUsbStall(0, CyTrue, CyFalse);
    break;
  }
}

/**
 *  @brief      handle debug some variable value from driver.
 *  @param[out] bRequest    bRequst value of uvc.
 *  @return     NULL.
 */
uint32_t debug_variable1 = 1;
uint32_t debug_variable2 = 1;
void EU_Rqts_debug_RW(uint8_t bRequest) {
  uint8_t Ep0Buffer[32] = {0};
  uint16_t readCount;
  uint8_t debug_tmp[255];
  CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;
  uint32_t* debug_ptr = (uint32_t*)debug_tmp;

  switch (bRequest) {
  case CY_FX_USB_UVC_GET_CUR_REQ:
    *debug_ptr = debug_variable1;
    *(debug_ptr + 1) = debug_variable2;
    sensor_dbg("debug read value\r\n");
    CyU3PUsbSendEP0Data(255, debug_tmp);
    break;
  case CY_FX_USB_UVC_SET_CUR_REQ:
    sensor_dbg("debug write value\r\n");
    apiRetStatus = CyU3PUsbGetEP0Data(255, debug_tmp, &readCount);
    if (apiRetStatus != CY_U3P_SUCCESS) {
      sensor_err("CyU3 get Ep0 data failed\r\n");
      CyFxAppErrorHandler(apiRetStatus);
      break;
    }
    debug_variable1 = *debug_ptr;
    debug_variable2 = *(debug_ptr + 1);
    break;
  case CY_FX_USB_UVC_GET_LEN_REQ:
    Ep0Buffer[0] = 255;
    Ep0Buffer[1] = 0;
    CyU3PUsbSendEP0Data(2, (uint8_t *)Ep0Buffer);
    break;
  case CY_FX_USB_UVC_GET_INFO_REQ:
    Ep0Buffer[0] = 3;
    CyU3PUsbSendEP0Data(1, (uint8_t *)Ep0Buffer);
    break;
  default:
    sensor_err("unknown flash cmd: 0x%x\r\n", bRequest);
    CyU3PUsbStall(0, CyTrue, CyFalse);
    break;
  }
}

/**
 *  @brief      read or write calibration file from/to spi flash.
 *  @param[out] bRequest    bRequst value of uvc.
 *  @return     NULL.
 */
void EU_Rqts_calib_RW(uint8_t bRequest) {
  uint8_t Ep0Buffer[32] = {0};
  calib_struct_t calib_store;
  uint16_t calib_len = sizeof (calib_struct_t);
  uint16_t readCount;
  CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;
  static uint8_t read_loop = 0;
  static uint8_t write_loop = 0;

  switch (bRequest) {
  case CY_FX_USB_UVC_GET_CUR_REQ:
    sensor_dbg("read calib file\r\n");
    CyU3PMemSet ((uint8_t *)(&calib_store), 0, calib_len);
    apiRetStatus = CyFxFlashProgSpiTransfer(DEVICE_CALIB_ADDR + read_loop, calib_len, (uint8_t *)(&calib_store),
                                            CyTrue);
    sensor_info("header: 0x%x 0x%x, total: 0x%x, id: 0x%x, readCount:%d\r\n", 
                calib_store.header[0], calib_store.header[1], calib_store.packet_total, calib_store.id, readCount);
    if (read_loop == calib_store.id) {
      sensor_info("read device calib info: 0x%x 0x%x %d %d\r\n", 
                  calib_store.header[0], 
                  calib_store.header[1],
                  calib_store.packet_total,
                  calib_store.id);
      CyU3PUsbSendEP0Data(calib_len, (uint8_t *)(&calib_store));
      sensor_info("read done!\r\n");
      if (++read_loop >= calib_store.packet_total) {
        read_loop = 0;
      }
    } else {
      // go to original loop status
      read_loop = 0;
    } 
    break;
  case CY_FX_USB_UVC_SET_CUR_REQ:
    sensor_dbg("write calib file\r\n");
    CyU3PMemSet ((uint8_t *)(&calib_store), 0, calib_len);
    apiRetStatus = CyU3PUsbGetEP0Data(calib_len, (uint8_t *)(&calib_store), &readCount);
    if (apiRetStatus != CY_U3P_SUCCESS) {
      sensor_err("CyU3 get Ep0 data failed\r\n");
      CyFxAppErrorHandler(apiRetStatus);
      break;
    }
    sensor_info("header: 0x%x 0x%x, total: 0x%x, id: 0x%x, readCount:%d\r\n", 
                calib_store.header[0], calib_store.header[1], calib_store.packet_total, calib_store.id, readCount);
    // write flash
    //sensor_info("write device ID: %s\n", flash_store.Sensor_ID);
    // Only erase sector the first time in loop
    if (write_loop == 0) {
      CyFxFlashProgEraseSector(CyTrue, 5, Ep0Buffer);
      sensor_info("Erase the %dth sector(begin from 0)\r\n", 5);
    }
    if (write_loop == calib_store.id) {
      apiRetStatus = CyFxFlashProgSpiTransfer(DEVICE_CALIB_ADDR + write_loop, 
                                              calib_len, 
                                              (uint8_t *)(&calib_store),
                                              CyFalse);
      sensor_info("write flash addr:0x%x len:%d\r\n", 
                  (DEVICE_CALIB_ADDR + write_loop)*glSpiPageSize,
                  calib_len);
      if (apiRetStatus != CY_U3P_SUCCESS) {
        sensor_err("Write Flash error\r\n");
      }
      if (++write_loop >= calib_store.packet_total) {
        write_loop = 0;
      }
    } else {
      // go to original loop status
      write_loop = 0;
    }
    break;
  case CY_FX_USB_UVC_GET_LEN_REQ:
    Ep0Buffer[0] = 255;
    Ep0Buffer[1] = 0;
    CyU3PUsbSendEP0Data(2, (uint8_t *)Ep0Buffer);
    break;
  case CY_FX_USB_UVC_GET_INFO_REQ:
    Ep0Buffer[0] = 3;
    CyU3PUsbSendEP0Data(1, (uint8_t *)Ep0Buffer);
    break;
  default:
    sensor_err("unknown flash cmd: 0x%x\r\n", bRequest);
    CyU3PUsbStall(0, CyTrue, CyFalse);
    break;
  }
}

/* SPI initialization for flash programmer application. */
CyU3PReturnStatus_t CyFxFlashProgSpiInit(uint16_t pageLen) {
  CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
  CyU3PSpiConfig_t spiConfig;
  CyU3PDmaChannelConfig_t dmaConfig;

  /* Start the SPI module and configure the master. */
  status = CyU3PSpiInit();
  if (status != CY_U3P_SUCCESS) {
    sensor_err("CyU3 spi module int failed!\r\n");
    return status;
  }

  /* Start the SPI master block. Run the SPI clock at 8MHz
   * and configure the word length to 8 bits. Also configure
   * the slave select using FW. */
  CyU3PMemSet ((uint8_t *)&spiConfig, 0, sizeof(spiConfig));
  spiConfig.isLsbFirst = CyFalse;
  spiConfig.cpol       = CyTrue;
  spiConfig.ssnPol     = CyFalse;
  spiConfig.cpha       = CyTrue;
  spiConfig.leadTime   = CY_U3P_SPI_SSN_LAG_LEAD_HALF_CLK;
  spiConfig.lagTime    = CY_U3P_SPI_SSN_LAG_LEAD_HALF_CLK;
  spiConfig.ssnCtrl    = CY_U3P_SPI_SSN_CTRL_FW;
  spiConfig.clock      = 8000000;
  spiConfig.wordLen    = 8;

  status = CyU3PSpiSetConfig(&spiConfig, NULL);
  if (status != CY_U3P_SUCCESS) {
    return status;
  }

  /* Create the DMA channels for SPI write and read. */
  CyU3PMemSet ((uint8_t *)&dmaConfig, 0, sizeof(dmaConfig));
  dmaConfig.size           = pageLen;
  /* No buffers need to be allocated as this channel
   * will be used only in override mode. */
  dmaConfig.count          = 0;
  dmaConfig.prodAvailCount = 0;
  dmaConfig.dmaMode        = CY_U3P_DMA_MODE_BYTE;
  dmaConfig.prodHeader     = 0;
  dmaConfig.prodFooter     = 0;
  dmaConfig.consHeader     = 0;
  dmaConfig.notification   = 0;
  dmaConfig.cb             = NULL;

  /* Channel to write to SPI flash. */
  dmaConfig.prodSckId = CY_U3P_CPU_SOCKET_PROD;
  dmaConfig.consSckId = CY_U3P_LPP_SOCKET_SPI_CONS;
  status = CyU3PDmaChannelCreate(&glSpiTxHandle, CY_U3P_DMA_TYPE_MANUAL_OUT, &dmaConfig);
  if (status != CY_U3P_SUCCESS) {
    return status;
  }

  /* Channel to read from SPI flash. */
  dmaConfig.prodSckId = CY_U3P_LPP_SOCKET_SPI_PROD;
  dmaConfig.consSckId = CY_U3P_CPU_SOCKET_CONS;
  status = CyU3PDmaChannelCreate(&glSpiRxHandle, CY_U3P_DMA_TYPE_MANUAL_IN, &dmaConfig);
  if (status == CY_U3P_SUCCESS) {
    glSpiPageSize = pageLen;
  }

  return status;
}

/* Wait for the status response from the SPI flash. */
static CyU3PReturnStatus_t CyFxFlashProgSpiWaitForStatus(void) {
  uint8_t buf[2], rd_buf[2];
  CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

  /* Wait for status response from SPI flash device. */
  do {
    buf[0] = 0x06;  /* Write enable command. */

    CyU3PSpiSetSsnLine(CyFalse);
    status = CyU3PSpiTransmitWords(buf, 1);
    CyU3PSpiSetSsnLine(CyTrue);
    if (status != CY_U3P_SUCCESS) {
      sensor_err("SPI WR_ENABLE command failed\n\r");
      return status;
    }

    buf[0] = 0x05;  /* Read status command */

    CyU3PSpiSetSsnLine(CyFalse);
    status = CyU3PSpiTransmitWords(buf, 1);
    if (status != CY_U3P_SUCCESS) {
      sensor_err("SPI READ_STATUS command failed\n\r");
      CyU3PSpiSetSsnLine(CyTrue);
      return status;
    }

    status = CyU3PSpiReceiveWords(rd_buf, 2);
    CyU3PSpiSetSsnLine(CyTrue);
    if (status != CY_U3P_SUCCESS) {
      sensor_err("SPI status read failed\n\r");
      return status;
    }
  } while ((rd_buf[0] & 1) || (!(rd_buf[0] & 0x2)));

  return CY_U3P_SUCCESS;
}

/* SPI read / write for programmer application. */
CyU3PReturnStatus_t CyFxFlashProgSpiTransfer(uint16_t pageAddress, uint16_t  byteCount,
                                             uint8_t  *buffer, CyBool_t  isRead) {
  CyU3PDmaBuffer_t buf_p;
  uint8_t location[4];
  uint32_t byteAddress = 0;
  uint16_t pageCount = (byteCount / glSpiPageSize);
  CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

  if (byteCount == 0) {
    return CY_U3P_SUCCESS;
  }
  if ((byteCount % glSpiPageSize) != 0) {
    pageCount++;
  }

  buf_p.buffer = buffer;
  buf_p.status = 0;

  byteAddress  = pageAddress * glSpiPageSize;
  sensor_dbg("SPI access - addr: 0x%x, size: 0x%x, pages: 0x%x.\r\n",
            byteAddress, byteCount, pageCount);

  while (pageCount != 0) {
    location[1] = (byteAddress >> 16) & 0xFF;       /* MS byte */
    location[2] = (byteAddress >> 8) & 0xFF;
    location[3] = byteAddress & 0xFF;               /* LS byte */

    if (isRead) {
      location[0] = 0x03; /* Read command. */

      buf_p.size  = glSpiPageSize;
      buf_p.count = glSpiPageSize;

      status = CyFxFlashProgSpiWaitForStatus();
      if (status != CY_U3P_SUCCESS)
        return status;

      CyU3PSpiSetSsnLine(CyFalse);
      status = CyU3PSpiTransmitWords(location, 4);
      if (status != CY_U3P_SUCCESS) {
        sensor_err("SPI READ command failed\r\n");
        CyU3PSpiSetSsnLine(CyTrue);
        return status;
      }

      CyU3PSpiSetBlockXfer(0, glSpiPageSize);

      status = CyU3PDmaChannelSetupRecvBuffer(&glSpiRxHandle, &buf_p);
      if (status != CY_U3P_SUCCESS) {
        CyU3PSpiSetSsnLine(CyTrue);
        return status;
      }
      status = CyU3PDmaChannelWaitForCompletion(&glSpiRxHandle, CY_FX_FLASH_PROG_TIMEOUT);
      if (status != CY_U3P_SUCCESS) {
        CyU3PSpiSetSsnLine(CyTrue);
        return status;
      }

      CyU3PSpiSetSsnLine(CyTrue);
      CyU3PSpiDisableBlockXfer(CyFalse, CyTrue);
    } else { /* Write */
      location[0] = 0x02; /* Write command */

      buf_p.size  = glSpiPageSize;
      buf_p.count = glSpiPageSize;

      status = CyFxFlashProgSpiWaitForStatus();
      if (status != CY_U3P_SUCCESS)
        return status;

      CyU3PSpiSetSsnLine(CyFalse);
      status = CyU3PSpiTransmitWords(location, 4);
      if (status != CY_U3P_SUCCESS) {
        sensor_err("SPI WRITE command failed\r\n");
        CyU3PSpiSetSsnLine(CyTrue);
        return status;
      }

      CyU3PSpiSetBlockXfer(glSpiPageSize, 0);

      status = CyU3PDmaChannelSetupSendBuffer(&glSpiTxHandle, &buf_p);
      if (status != CY_U3P_SUCCESS) {
        CyU3PSpiSetSsnLine(CyTrue);
        return status;
      }
      status = CyU3PDmaChannelWaitForCompletion(&glSpiTxHandle,
               CY_FX_FLASH_PROG_TIMEOUT);
      if (status != CY_U3P_SUCCESS) {
        CyU3PSpiSetSsnLine(CyTrue);
        return status;
      }

      CyU3PSpiSetSsnLine(CyTrue);
      CyU3PSpiDisableBlockXfer(CyTrue, CyFalse);
    }

    /* Update the parameters */
    byteAddress  += glSpiPageSize;
    buf_p.buffer += glSpiPageSize;
    pageCount--;

    CyU3PThreadSleep(10);
  }
  return CY_U3P_SUCCESS;
}

/* Function to erase SPI flash sectors. */
CyU3PReturnStatus_t CyFxFlashProgEraseSector(CyBool_t isErase, uint8_t sector, uint8_t *wip) {
  uint32_t temp = 0;
  uint8_t  location[4], rdBuf[2];
  CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

  if ((!isErase) && (wip == NULL)) {
    return CY_U3P_ERROR_BAD_ARGUMENT;
  }

  location[0] = 0x06;  /* Write enable. */

  CyU3PSpiSetSsnLine(CyFalse);
  status = CyU3PSpiTransmitWords(location, 1);
  CyU3PSpiSetSsnLine(CyTrue);
  if (status != CY_U3P_SUCCESS)
    return status;

  if (isErase) {
    location[0] = 0xD8; /* Sector erase. */
    temp        = sector * 0x10000;
    location[1] = (temp >> 16) & 0xFF;
    location[2] = (temp >> 8) & 0xFF;
    location[3] = temp & 0xFF;

    CyU3PSpiSetSsnLine(CyFalse);
    status = CyU3PSpiTransmitWords(location, 4);
    CyU3PSpiSetSsnLine(CyTrue);
  } else {
    location[0] = 0x05; /* Read status */

    CyU3PSpiSetSsnLine(CyFalse);
    status = CyU3PSpiTransmitWords(location, 1);
    if (status != CY_U3P_SUCCESS) {
      CyU3PSpiSetSsnLine(CyTrue);
      return status;
    }

    status = CyU3PSpiReceiveWords(rdBuf, 2);
    CyU3PSpiSetSsnLine(CyTrue);
    *wip = rdBuf[0] & 0x1;
  }

  return status;
}
