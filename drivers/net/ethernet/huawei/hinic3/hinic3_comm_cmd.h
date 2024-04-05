/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2019-2022. All rights reserved.
 * File Name     : hinic3_comm_cmd.h
 * Version       : Initial Draft
 * Created       : 2019/4/25
 * Last Modified :
 * Description   : COMM Commands between Driver and MPU
 * Function List :
 */

#ifndef HINIC3_COMMON_CMD_H
#define HINIC3_COMMON_CMD_H

/* COMM Commands between Driver to MPU */
enum hinic3_mgmt_cmd {
	/* flr及资源清理相关命令 */
	COMM_MGMT_CMD_FUNC_RESET = 0,
	COMM_MGMT_CMD_FEATURE_NEGO,
	COMM_MGMT_CMD_FLUSH_DOORBELL,
	COMM_MGMT_CMD_START_FLUSH,
	COMM_MGMT_CMD_SET_FUNC_FLR,
	COMM_MGMT_CMD_GET_GLOBAL_ATTR,
	COMM_MGMT_CMD_SET_PPF_FLR_TYPE,
	COMM_MGMT_CMD_SET_FUNC_SVC_USED_STATE,

	/* 分配msi-x中断资源 */
	COMM_MGMT_CMD_CFG_MSIX_NUM = 10,

	/* 驱动相关配置命令 */
	COMM_MGMT_CMD_SET_CMDQ_CTXT = 20,
	COMM_MGMT_CMD_SET_VAT,
	COMM_MGMT_CMD_CFG_PAGESIZE,
	COMM_MGMT_CMD_CFG_MSIX_CTRL_REG,
	COMM_MGMT_CMD_SET_CEQ_CTRL_REG,
	COMM_MGMT_CMD_SET_DMA_ATTR,

	/* INFRA配置相关命令字 */
	COMM_MGMT_CMD_GET_MQM_FIX_INFO = 40,
	COMM_MGMT_CMD_SET_MQM_CFG_INFO,
	COMM_MGMT_CMD_SET_MQM_SRCH_GPA,
	COMM_MGMT_CMD_SET_PPF_TMR,
	COMM_MGMT_CMD_SET_PPF_HT_GPA,
	COMM_MGMT_CMD_SET_FUNC_TMR_BITMAT,
	COMM_MGMT_CMD_SET_MBX_CRDT,
	COMM_MGMT_CMD_CFG_TEMPLATE,
	COMM_MGMT_CMD_SET_MQM_LIMIT,

	/* 信息获取相关命令字 */
	COMM_MGMT_CMD_GET_FW_VERSION = 60,
	COMM_MGMT_CMD_GET_BOARD_INFO,
	COMM_MGMT_CMD_SYNC_TIME,
	COMM_MGMT_CMD_GET_HW_PF_INFOS,
	COMM_MGMT_CMD_SEND_BDF_INFO,
	COMM_MGMT_CMD_GET_VIRTIO_BDF_INFO,
	COMM_MGMT_CMD_GET_SML_TABLE_INFO,
	COMM_MGMT_CMD_GET_SDI_INFO,

	/* 升级相关命令字 */
	COMM_MGMT_CMD_UPDATE_FW = 80,
	COMM_MGMT_CMD_ACTIVE_FW,
	COMM_MGMT_CMD_HOT_ACTIVE_FW,
	COMM_MGMT_CMD_HOT_ACTIVE_DONE_NOTICE,
	COMM_MGMT_CMD_SWITCH_CFG,
	COMM_MGMT_CMD_CHECK_FLASH,
	COMM_MGMT_CMD_CHECK_FLASH_RW,
	COMM_MGMT_CMD_RESOURCE_CFG,
	COMM_MGMT_CMD_UPDATE_BIOS,    /* TODO: merge to COMM_MGMT_CMD_UPDATE_FW */
	COMM_MGMT_CMD_MPU_GIT_CODE,

	/* chip reset相关 */
	COMM_MGMT_CMD_FAULT_REPORT = 100,
	COMM_MGMT_CMD_WATCHDOG_INFO,
	COMM_MGMT_CMD_MGMT_RESET,
	COMM_MGMT_CMD_FFM_SET,    /* TODO: check if needed */

	/* chip info/log 相关 */
	COMM_MGMT_CMD_GET_LOG = 120,
	COMM_MGMT_CMD_TEMP_OP,
	COMM_MGMT_CMD_EN_AUTO_RST_CHIP,
	COMM_MGMT_CMD_CFG_REG,
	COMM_MGMT_CMD_GET_CHIP_ID,
	COMM_MGMT_CMD_SYSINFO_DFX,
	COMM_MGMT_CMD_PCIE_DFX_NTC,
	COMM_MGMT_CMD_DICT_LOG_STATUS, /* LOG STATUS 127 */
	COMM_MGMT_CMD_MSIX_INFO,
	COMM_MGMT_CMD_CHANNEL_DETECT,
	COMM_MGMT_CMD_DICT_COUNTER_STATUS,

	/* switch workmode 相关 */
	COMM_MGMT_CMD_CHECK_IF_SWITCH_WORKMODE = 140,
	COMM_MGMT_CMD_SWITCH_WORKMODE,

	/* mpu 相关 */
	COMM_MGMT_CMD_MIGRATE_DFX_HPA = 150,
	COMM_MGMT_CMD_BDF_INFO,
	COMM_MGMT_CMD_NCSI_CFG_INFO_GET_PROC,

	/* rsvd0 section */
	COMM_MGMT_CMD_SECTION_RSVD_0 = 160,

	/* rsvd1 section */
	COMM_MGMT_CMD_SECTION_RSVD_1 = 170,

	/* rsvd2 section */
	COMM_MGMT_CMD_SECTION_RSVD_2 = 180,

	/* rsvd3 section */
	COMM_MGMT_CMD_SECTION_RSVD_3 = 190,

	/* TODO: move to DFT mode */
	COMM_MGMT_CMD_GET_DIE_ID = 200,
	COMM_MGMT_CMD_GET_EFUSE_TEST,
	COMM_MGMT_CMD_EFUSE_INFO_CFG,
	COMM_MGMT_CMD_GPIO_CTL,
	COMM_MGMT_CMD_HI30_SERLOOP_START, /* TODO: DFT or hilink */
	COMM_MGMT_CMD_HI30_SERLOOP_STOP, /* TODO: DFT or hilink */
	COMM_MGMT_CMD_HI30_MBIST_SET_FLAG, /* TODO: DFT or hilink */
	COMM_MGMT_CMD_HI30_MBIST_GET_RESULT, /* TODO: DFT or hilink */
	COMM_MGMT_CMD_ECC_TEST,
	COMM_MGMT_CMD_FUNC_BIST_TEST, /* 209 */

	COMM_MGMT_CMD_VPD_SET = 210,
	COMM_MGMT_CMD_VPD_GET,

	COMM_MGMT_CMD_ERASE_FLASH,
	COMM_MGMT_CMD_QUERY_FW_INFO,
	COMM_MGMT_CMD_GET_CFG_INFO,
	COMM_MGMT_CMD_GET_UART_LOG,
	COMM_MGMT_CMD_SET_UART_CMD,
	COMM_MGMT_CMD_SPI_TEST,

	/* TODO: ALL reg read/write merge to COMM_MGMT_CMD_CFG_REG */
	COMM_MGMT_CMD_UP_REG_GET,
	COMM_MGMT_CMD_UP_REG_SET, /* 219 */

	COMM_MGMT_CMD_REG_READ = 220,
	COMM_MGMT_CMD_REG_WRITE,
	COMM_MGMT_CMD_MAG_REG_WRITE,
	COMM_MGMT_CMD_ANLT_REG_WRITE,

	COMM_MGMT_CMD_HEART_EVENT,  /* TODO: delete */
	COMM_MGMT_CMD_NCSI_OEM_GET_DRV_INFO, /* TODO: delete */
	COMM_MGMT_CMD_LASTWORD_GET,
	COMM_MGMT_CMD_READ_BIN_DATA, /* TODO: delete */
	/* COMM_MGMT_CMD_WWPN_GET, TODO: move to FC? */
	/* COMM_MGMT_CMD_WWPN_SET, TODO: move to FC? */ /* 229 */

	/* TODO: check if needed */
	COMM_MGMT_CMD_SET_VIRTIO_DEV = 230,
	COMM_MGMT_CMD_SET_MAC,
	/* MPU patch cmd */
	COMM_MGMT_CMD_LOAD_PATCH,
	COMM_MGMT_CMD_REMOVE_PATCH,
	COMM_MGMT_CMD_PATCH_ACTIVE,
	COMM_MGMT_CMD_PATCH_DEACTIVE,
	COMM_MGMT_CMD_PATCH_SRAM_OPTIMIZE,
	/* container host process */
	COMM_MGMT_CMD_CONTAINER_HOST_PROC,
	/* nsci counter */
	COMM_MGMT_CMD_NCSI_COUNTER_PROC,
	COMM_MGMT_CMD_CHANNEL_STATUS_CHECK, /* 239 */

	/* hot patch rsvd cmd */
	COMM_MGMT_CMD_RSVD_0 = 240,
	COMM_MGMT_CMD_RSVD_1,
	COMM_MGMT_CMD_RSVD_2,
	COMM_MGMT_CMD_RSVD_3,
	COMM_MGMT_CMD_RSVD_4,
	/* 无效字段，版本收编删除，编译使用 */
	COMM_MGMT_CMD_SEND_API_ACK_BY_UP,

	/* 注：添加cmd，不能修改已有命令字的值，请在前方rsvd
	 * section中添加；原则上所有分支cmd表完全一致
	 */
	COMM_MGMT_CMD_MAX = 255,
};

/* CmdQ Common subtype */
enum comm_cmdq_cmd {
	COMM_CMD_UCODE_ARM_BIT_SET = 2,
	COMM_CMD_SEND_NPU_DFT_CMD,
};

#endif /* HINIC3_COMMON_CMD_H */