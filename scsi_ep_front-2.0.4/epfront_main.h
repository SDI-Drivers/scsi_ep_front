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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORSz
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


#ifndef __EPFRONT_MAIN_H_
#define __EPFRONT_MAIN_H_

#include "epfront.h"

#define EPFRONT_DEBUG

#define EPFRONT_IO_SQ_N (4)
#define EPFRONT_IO_SQ_MASK (0x3)
#define EPFRONT_GAB_Q_BD_N (128)
#define EPFRONT_IO_Q_BD_N  (4U << 10)

#define EPFRONT_SV_KFIFO_SIZE (1024)
#define EPFRONT_SV_MAX_DATA_SIZE (128)
#define EPFRONT_SV_SUSPEND_TIME (2 * HZ)
#define EPFRONT_SV_DELAY_TIME (3 * HZ)
#define EPFRONT_SV_LOWPRI_EXPIRE_TIME (5 * HZ)
#define EPFRONT_SCSI_PROBE_TIME_OUT (60 * HZ)
#define EPFRONT_SCSI_PROBE_INTERVAL_TIME (3000)  //ms
#define EPFRONT_NOTIFY_TIMEOUT (30 * HZ)
#define EPFRONT_TRANS_RESET_INTERVAL_TIME (2000)
#define EPFRONT_SYNC_DEVNAME_DEFAULT (0)

/* CRC32 default seed */
#define CRC32_SEED                (~0U)

enum epfront_init_status{
	EPFRONT_INIT_BASE,
	EPFRONT_INIT_DEV,
	EPFRONT_INIT_MGR,
	EPFRONT_INIT_TRANS,

	EPFRONT_SCSI_SYNC_DISK,
	EPFRONT_SCSI_READY,

    EPFRONT_SCSI_START,
	EPFRONT_SCSI_STOP,
	EPFRONT_SCSI_RESETTING,
	EPFRONT_SCSI_LINKDOWN,
	EPFRONT_SCSI_FAILFAST,
	EPFRONT_SCSI_QUEUE_OFF,
	EPFRONT_SCSI_QUEUE_OFF_DONE,
	EPFRONT_SCSI_QUEUE_RUN
};

enum epfront_cmd_stat{
    CMD_STAT_INIT,
    CMD_STAT_SEND_COMP,
	CMD_STAT_RECV_RESP,
    CMD_STAT_DONE
};

/* sense buff data struct */
struct scsi_sense_info{
    u8 sense_buffer[SCSI_SENSE_BUFFERSIZE];
};

struct epfront_cmnd_list
{
    struct list_head                  list;
    struct epfront_host_ctrl*         h;
    struct scsi_sense_info            *psense_buffer_virt;
    struct scsi_sense_info            *psense_buffer_phy;
    __u32                             cmd_sn;                /* command serial number */
    __u32                             cmd_index;             /* command index */

    void*                             scsi_cmd;
	__u32                             back_uniq_id;

    enum dma_data_direction           data_direction;
    sdi_iod_t                         *iod;

    __u32                             scsi_cmnd_len;
    __u32                             sense_buffer_len;
    dma_addr_t                        scsi_cmnd_paddr;
    dma_addr_t                        sense_buffer_paddr;
	
//    __u32                             crc32;
//    __u32                             crc32_sgl;
	
	unsigned long                     status;        /* command status */
//	wait_queue_head_t                 wait;

	unsigned long submit;
	unsigned long callback;	
};

struct epfront_host_ctrl{
    struct kobject kobj;
    struct kobject* parent;

    spinlock_t                    lock;
    struct Scsi_Host              *scsi_host;            /* scsi_host struct pointer */

    unsigned int                  sys_host_id;           // front system's host_id

    unsigned int                  nr_cmds;              
    struct list_head              cmdQ;                   /*command queue*/
    struct epfront_cmnd_list      **cmd_pool_tbl;        /* command buffer pool */
    unsigned long                 *cmd_pool_bits;      
    struct scsi_sense_info        *pscsi_sense_queue_virt; /* sense queue buffer queue virtual address*/
    struct scsi_sense_info        *pscsi_sense_queue_phy;  /* sense queue buffer queue physical address*/
    __u32                         sense_queue_len;       /* sense queue buffer queue length*/
    atomic_t                      cmd_sn;               
    
    atomic_t                      cmds_num; 

	//statistic
	atomic_t                      abort_succ;
	atomic_t                      abort_fail;
	atomic_t                      reset_succ;
	atomic_t                      reset_fail;
	atomic_t                      conflict_num;    //can't happen
};

struct epfront_lun_list{
	struct list_head list;
	
    struct kobject kobj;
    //struct kobject* parent;

	u32 back_uniq_id;
	u32 host_index;
	
	u32 host;
	u32 channel;
	u32 id;
	u32 lun;
	char vol_name[EP_VOL_NAME_LEN];
	char dev_name[EP_DEV_NAME_LEN];	//front's dev_name

    atomic_t send_num;
	atomic_t recv_num;
	atomic_t abort_num;
	atomic_t back_abort;
	atomic_t crc_error;
	atomic_t crc_data_error;
};

enum epfront_aer_ctrl_opt{
	AER_NOT_NEEN_RESP,
	AER_NEET_RESP
};

struct epfront_notify_ctrl{
	u32 notify_type;
	u32 data_len;
	void* data_virt;
	dma_addr_t data_phys;
};

struct epfront_aer_ctrl{
	u16 aer_index;
	u16 ctrl_opt;
	u32 data_len;
	u32 crc32;
	int (*recv)(void* data);
	
	void* data_virt;
	dma_addr_t data_phys;
};

struct epfront_host_para{
	u32 max_cmd_len;
	u32 sg_count;
	u32 max_nr_cmds;
	u32 cmd_per_lun;
	u32 max_channel;
	u32 max_id;
	u32 max_lun;
};

//supervise thread's task type
enum epfront_sv_type{
	SV_TRANS_REINIT,
	SV_RENAME_LUNS,
	SV_SYNC_DISK,
    SV_SYNC_DEVNAME,
    SV_RESET_HANDLE,
    SV_AER_HANDLE,
    SV_MAX_LIMIT
};

enum ep_sync_devname_type{
	EP_SYNC_ALL,
	EP_SYNC_ONE
};

typedef int (*SV_CALLBACK_PTR)(void* data);
struct epfront_sv_handle{
	int type;
	SV_CALLBACK_PTR func;
	unsigned char data[0];
};

struct epfront_statistic{
	atomic_t ill_sqtype;
	atomic_t ill_sqpara;
	atomic_t ill_aer_type;
	atomic_t ill_aer_cqe;
	atomic_t ill_io_cqe;

	atomic_t crc_err_notify;
	atomic_t crc_err_aen;
	
#ifdef EPFRONT_DEBUG
    atomic_t sv_todo[SV_MAX_LIMIT];
    atomic_t sv_done[SV_MAX_LIMIT];
    atomic_t aer_send[AER_MAX_LIMIT];
    atomic_t aer_todo[AER_MAX_LIMIT];
    atomic_t aer_done[AER_MAX_LIMIT];
	
	int cur_type;
	int cur_subtype;
#endif
};


typedef struct ep_aer_disk_list{
	struct list_head list;
	ep_aer_disk_t disk;
}ep_aer_disk_list_t;

//for sys begin
extern unsigned long epfront_status;
extern struct epfront_statistic g_stats;

unsigned int epfront_ctrl_get_host_no(struct epfront_host_ctrl *h);
//for sys end

void epfront_scsi_probe(void);
void epfront_scsi_remove(void);
void epfront_start_trans(void);
int epfront_stop_trans(unsigned long status);
void epfront_set_linkdown(void);

#endif


