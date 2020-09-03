/*----------------------------------------------------------------------------
* Copyright (c) <2016-2017>, <Huawei Technologies Co., Ltd>
* All rights reserved.
* Redistribution and use in source and binary forms, with or without modification,
* are permitted provided that the following conditions are met:
* 1. Redistributions of source code must retain the above copyright notice, this list of
* conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright notice, this list
* of conditions and the following disclaimer in the documentation and/or other materials
* provided with the distribution.
* 3. Neither the name of the copyright holder nor the names of its contributors may be used
* to endorse or promote products derived from this software without specific prior written
* permission.
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
* THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
* PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
* CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
* EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
* PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
* OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
* OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
* ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*---------------------------------------------------------------------------*/

#ifndef __EP_COMMON_H_
#define __EP_COMMON_H_
#define LINK_RATE_REG 0xa0200080
#define INVALID_BACK_ID (0xFFFFFFFF)

enum ep_warning_type{
    LINK_DOWN_TIMEOUT,
    HEART_BEAT_INT,
    SYNC_DISK_FAILED,
    LINK_RATE_ERR,
    RM_x86_DRIVER = 15
};

enum ep_sync_type{
    EP_SYNC_SUCCESS,
    EP_SYNC_FAILED
};

enum ep_state_type{
    EP_STATE_FRONT_UNKOWN,
    EP_STATE_FRONT_INIT,
    EP_STATE_FRONT_CONNECTED,
    EP_STATE_FRONT_LINKDOWN,
    EP_STATE_FRONT_RMMOD
};

typedef enum {
    EP_NOTIFY_RESET_BACK_STATE,
    EP_NOTIFY_CHECK_BACK_STATE,
    EP_NOTIFY_SYNC_DISK,
    EP_NOTIFY_SYNC_CONFIG,
    EP_NOTIFY_SYNC_DISK_RESULT,
    EP_NOTIFY_IS_BACK_RDY,
    EP_NOTIFY_SYNC_DEVNAME,
#ifdef EPBACK_PRIVATE
    EP_NOTIFY_SYNC_SYSDISK,//获取系统盘信息
    EP_NOTIFY_OPROM_VERSION,
#endif
    EP_NOTIFY_MAX_LIMIT
}ep_module_notify_op;

//must less than 56 bytes -- send_cmd() limit
typedef struct ep_module_notify{
    u16 opcode;
    u16 subcode;
    u32 data_len;
    u32 direction;
    u32 resv1;
    u64 paddr_data;
}module_notify_t;

typedef struct ep_module_notify_data{
    u32 crc32;
    unsigned char data[0];
}module_notify_data_t;

enum ep_queue_type{
#ifdef __ADMIN_CMD__
    EP_IO_Q,
    EP_GAB_Q,
    EP_ADM_CMD_SET_Q
#else
    EP_IO_Q,
    EP_GAB_Q
#endif
};

enum ep_queue_entry_type{
#ifdef __ADMIN_CMD__
    EP_IO_ENTRY,
    EP_GAB_AER_ENTRY,
    EP_ADM_CMD_SET_ENTRY
#else
    EP_IO_ENTRY,
    EP_GAB_AER_ENTRY
#endif
};

enum ep_cqe_status
{
    CQE_STATUS_SUCCESS = 0x0,
    CQE_STATUS_INVALID_SQTYPE = 0x1,
    CQE_STATUS_INVALID_SQPARA = 0x2,

    CQE_STATUS_ABORT_SQE = 0x3,
    CQE_STATUS_CRC32_FAILED = 0x4
};

struct ep_sqe{
    u8          entry_type;
    u8          data[63];
};
struct ep_cqe{
    u8          entry_type;
    u8          data[13];
    __le16      status;
};

#define EP_SQ_MAX_CDB_LEN 16
struct ep_io_sqe{
    u8                 entry_type;           //just for verify,  dispensable
    u8                 opcode;

    __le16             io_index;
    __le16             back_uniq_id;
    __le16             timeout;

    __le32             crc32;
    __le32             crc32_sgl;

    union{
        struct{
            __le32     scsi_cmnd_len;
            __le64     scsi_cmnd_paddr;
        }cdb_info;
        u8             cdb[EP_SQ_MAX_CDB_LEN];
    }scsi_cmnd_info;
    __le32             scsi_sg_len;
    __le32             scsi_sg_ex_count;
    __le64             scsi_sg_paddr;
    __le64             scsi_sg_ex_paddr;
    __le64             sense_buffer_phy;
};

struct ep_aer_sqe{
    u8                 entry_type;
    u8                 rsvd;

    __le16             aer_index;
    __le32             data_len;            //just for verify,  dispensable
    __le64             data_phys;
    __le32             last_ret;
    __le32             crc32;
    u8                 rsvd1[40];
};

#ifdef __ADMIN_CMD__
/*********************/
struct ep_adm_cmd_set_sqe{
    u8                 entry_type;
    u8                 rsvd;

    __le16             rsvd2;//aer_index;
    __le32             data_len;            //just for verify,  dispensable
    __le64             data_phys;
    __le32             last_ret;
    __le32             crc32;
    u8                 rsvd1[40];
};
/*********************/
#endif
struct ep_io_cqe{
    u8                 entry_type;          //just for verify,  dispensable
    u8                 sense_len;

    __le16             back_uniq_id;
    __le32             result;
    __le32             crc32;
    __le16             io_index;
    __le16             status;
};

#define EP_AER_CQE_EXT (6)
struct ep_aer_cqe{
    u8                 entry_type;
    u8                 rsvd;
    u8                 ext_data[EP_AER_CQE_EXT];
    __le32             crc32;
    __le16             aer_index;
    __le16             status;             //ep use it's last bit to reverse
};

#ifdef __ADMIN_CMD__
struct ep_admin_cmd_set_cqe{
    u8                 entry_type;
    u8                 rsvd;
    u8                 ext_data[EP_AER_CQE_EXT];
    __le32             crc32;
    __le16             aer_index;
    __le16             status;             //ep use it's last bit to reverse
};
#endif

//for analyze io sqe's opcode
//7~0, 1~0 direction; 6 sgl_clt; 7 cdb_ctl
#define OPCODE_SET_DIRECTION(opcode, vol) do{ (opcode) = ((opcode) & (~0x03)) | ((vol) & 0x03);}while(0)
#define OPCODE_GET_DIRECTION(opcode) ((opcode) & 0x03)

#define OPCODE_SET_CDB_CTL(opcode, vol) do{ (opcode) = ((opcode) & (~0x80)) | (((vol) & 0x01) << 7);}while(0)
#define OPCODE_GET_CDB_CTL(opcode) (((opcode) >> 7) & 0x01)

#define OPCODE_SET_SGL_CTL(opcode, vol) do{ (opcode) = ((opcode) & (~0x60)) | (((vol) & 0x03) << 5);}while(0)
#define OPCODE_GET_SGL_CTL(opcode) (((opcode) >> 5) & 0x03)

enum io_opcode_sgl_ctl{
    IO_SGL_NONE = 0,
    IO_SGL_IN_SQ = 1,
    IO_SGL_ON_SQ = 2,
    IO_SGL_EX = 3
};
enum io_opcode_cdb_ctl{
    IO_CDB_BY_SQ = 0,
    IO_CDB_IN_SQ = 1
};

enum ep_gab_aer_type{
    AER_ADD_DISK,
    AER_RMV_DISK,
//    AER_GET_DEV_NAME,
    AER_NOTIFY_RESCAN,
    AER_IO_SWITCH,
    AER_LINKDOWN,
    AER_MAX_LIMIT
};

#define EP_SYNC_DISK_ONCE_N (512)
//同步系统盘数量最大值1
#define EP_SYNC_SYSDISK_ONCE_N (1)
#define EP_SYNC_DEVNAME_ONCE_N (512)
#define EP_MAX_UNIQUE_ID (512)
#define EP_VOL_NAME_LEN (96)
#define EP_DEV_NAME_LEN (128)
#define EP_SYS_DISK (1)

typedef struct ep_aer_disk{
    u32 back_uniq_id;
    char vol_name[EP_VOL_NAME_LEN];
}ep_aer_disk_t;

/* add for add_disk x86_dev_name callback */
typedef struct ep_aer_disk_name{
    char dev_name[EP_DEV_NAME_LEN];
    u32 back_uniq_id;
    char vol_name[EP_VOL_NAME_LEN];
}ep_aer_disk_name_t;
/* end */

typedef struct ep_aer_dev_name{
    u32 back_uniq_id;
    char dev_name[EP_DEV_NAME_LEN];
}ep_aer_dev_name_t;

#define EP_MAX_HOST_NUMBER (24)
#define EP_DEFAULT_HOST_NUMBER (4)

#define EP_MAX_MAX_CHANNEL (65535)
#define EP_DEFAULT_MAX_CHANNEL (65535)

#define EP_MAX_MAX_ID (65535)
#define EP_DEFAULT_MAX_ID (65535)

#define EP_MAX_CDB_LEN (256)
#define EP_DEFAULT_CDB_LEN (16)

#define EP_MAX_SG_COUNT (8192)
#define EP_DEFAULT_SG_COUNT (128)

#define EP_MAX_IO_DEPTH_PER_LUN (2048)
#define EP_DEFAULT_IO_DEPTH_PER_LUN (128)

#define EP_MAX_MAX_LUN_PER_HOST (2048)
#define EP_DEFAULT_MAX_LUN_PER_HOST (512)

#define EP_MAX_MAX_CMD_NUMBER (EP_MAX_MAX_LUN_PER_HOST * EP_MAX_IO_DEPTH_PER_LUN)
#define EP_DEFAULT_MAX_CMD_NUMBER (EP_DEFAULT_MAX_LUN_PER_HOST * EP_DEFAULT_IO_DEPTH_PER_LUN)

#define EP_MAX_RQ_TIMEOUT (600)
#define EP_DEFAULT_RQ_TIMEOUT (180)
struct ep_global_config{
    u32         crc32;
    u32         host_n;
    u32         max_channel;
    u32         max_id;
    u32         max_lun;
    u32         max_cmd_len;
    u32         max_nr_cmds;
    u32         cmd_per_lun;
    u32         sg_count;      /* data size = sector *sg_count*/
    u32         rq_timeout;
};

#endif

