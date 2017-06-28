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

#ifndef __EP_TRANSFER_H_
#define __EP_TRANSFER_H_

#include <linux/types.h>
#include <linux/scatterlist.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include <linux/spinlock.h>

struct sdi_pdev_info;


#define Q_FULL(head,tail,max) ((head >= tail)?(head - tail == 1):(head + max - tail == 1))
#define Q_USED(head,tail,max) ((head >= tail)?(head - tail):(head + max - tail))
#define Q_FREE(head,tail,max) ((head >= tail)?(tail + max -head):(tail - head))


#define QUEUE_INC(index,val,max)  {index+=val; if (index >= max) index -= max;}
#define QUEUE_INC_ONE(index,max)  {index++; if (index >= max) index -= max;}

#define SDI_PF12_MAX_QUEUE_NUM (8)
#define SDI_PF12_MAX_SQ_NR (8)
#define SDI_PF12_MAX_CQ_NR (8)
#define MAX_AEN_NR (8)
#define MAX_UAEN_NR (MAX_AEN_NR/2)
#define MAX_CAEN_NR (MAX_AEN_NR/2)
#define SDI_AQ_DEPTH (128)
#define SDI_PF12_MAX_SECTORS (1024)
#define SDI_MIN_MSIX_COUNT (2)


#define PF12_BAR0_SIZE (8192)
#define DOORBELL_OFFSET (4096)

#define SDI_CARD_SHUTDOWN_TIMEOUT    (50*HZ)
#define WAIT_SDI_CARD_SHUTDOWN_TIME    (3*HZ)
#define SHUTDOWN_TIMEOUT    (5 * HZ)
#define ADMIN_TIMEOUT (60 * HZ)

#define HEARTBEAT_TIMEOUT (6 * HZ)

#define SDI_PF12_IRQ_NAME_LEN (24)
#define SDI_PF12_EP_NAME_LEN (30)

#define SMALL_POOL_SIZE 256
#define POOL_SIZE PAGE_SIZE

#define QID_CHECK(qid)	(((qid) >= SDI_PF12_MAX_SQ_NR || (qid) == 0)? 1: 0)
#define SQID_CHECK(sqid) (QID_CHECK (sqid))
#define SQID_UNSIGNED_CHECK(sqid) (((sqid) >= SDI_PF12_MAX_SQ_NR)? 1: 0)
#define CQID_CHECK(cqid) (QID_CHECK (cqid))

#define PROBE_TIME_OUT 	(60)

enum
{
	REQ_CMD_TIMEOUT,
	REQ_CMD_TIMEOUT_ABORT,
	REQ_CMD_NO_TIMEOUT
};

#define SQ_SUSPEND 1

enum
{
	SGL_DATA_BLOCK_ID,
	SGL_DATA_BIT_BUCKET_ID,
	SGL_SEGMENT_ID,
	SGL_LAST_SEGMENT_ID
};


enum
{
	SDI_SERVICE_TASK_PHASE0,
	SDI_SERVICE_TASK_PHASE1,
	SDI_SERVICE_TASK_PHASE2,
	SDI_SERVICE_TASK_PHASE3
};


enum sdi_ep_state_t {
	SDI_EP_RESETTING,
	SDI_EP_DOWN,
	SDI_EP_UP,
	SDI_EP_READY,
	SDI_ULP_REGED,
	SDI_EP_STOPPING,
	SDI_EP_STARTING,
	SDI_EP_UPDATING
};

/*
 * SDI_FRONT_HEARTBEAT_ABNORMAL:heartbeat abnormal mark
 * SDI_FRONT_UPDATE:update mark
 * SDI_FRONT_PERCEIV_RESET:perceptible reboot mark
 * SDI_FRONT_AER:AER occur mark
 *
 * When multiple situation couple together(multiple business
 * scenarios are possible to be concurrent), there is a process
 * priority. By using these marks, it allows us to process the
 * higher priority situation and ignore the lower one for a while.
 */
enum sdi_front_state_t {
	SDI_FRONT_HEARTBEAT_ABNORMAL = 0,
	SDI_FRONT_UPDATE,
	SDI_FRONT_PERCEIV_RESET,
	SDI_FRONT_AER,
	SDI_FRONT_REMOVE					
	// when rmmod process goes to remove, other process shouldn't be open
};

typedef struct sdi_pdev_info sdi_pdev_info_t;

typedef struct sdi_admin_queue sdi_admin_queue_t;

struct sdi_completion;

typedef void (*sdi_completion_fn)(sdi_admin_queue_t *aq, void *,
								  struct sdi_completion *);


#define SDI_ULP_MODULE_SCSI 1  

struct sync_cmd_info {
	struct task_struct *task;
	wait_queue_head_t wq;
	u32 result;
	int status;
};

struct sdi_sgl_info
{
	__le64 addr;
	__le32 len;
	__u8 rsv[3];
	__u8 id;
};

struct udrv_type_check
{
	u16 udrv_type;
	u16 flag;
};


typedef struct sdi_completion_head
{
	__le32 result;
	__u32 rsvd;
	__le16 sq_head;
	__le16 sq_id;
	__u16 command_id;
	__le32 phase :1;
	__le32 status :15;
} cqe_head_t;


typedef struct sdi_sq_info
{
	struct kobject kobj;
	spinlock_t sq_lock;
	u8 *sq_addr;
	dma_addr_t dma_addr;
	sdi_pdev_info_t *spdev;
	u32 __iomem *q_db;
	unsigned long state;
	u64 tx_ok_cnt;
	u64 tx_busy_cnt;
	u32 size;
	u16 sqid;
	u16 q_depth;
	u16 stride;
	u16 q_type;
	u16 cq_id;
	u16 head;
	u16 tail;
	u16 udrv_type;
	u16 q_prio;
}sdi_sq_info_t;

struct async_cmd_info {
	struct work_struct work;
	sdi_admin_queue_t *aq;
	u32 result;
	int idx;
	u16 status;
	void *tag;
	void *ctx;
};

struct sdi_cmd_info {
	sdi_completion_fn fn;
	void *ctx;
	unsigned long timeout;
	int aborted;
	int flags;
};


struct sdi_completion {
	__le32  result;     /* Used by admin commands to return data */
	__u32   rsvd;
	__le16  sq_head;    /* how much of this queue may be reclaimed */
	__le16  sq_id;      /* submission queue that generated this entry */
	__u16   command_id; /* of the command which completed */
	__le16  status;     /* did the command fail, and if so, why? */
};


struct sdi_admin_queue
{
	struct device *q_dmadev;
	struct sdi_pdev_info *sdev;
	void *addr;
	dma_addr_t dma_addr;
	u32 queue_size;
	char irqname[SDI_PF12_IRQ_NAME_LEN];
	spinlock_t q_lock;
	spinlock_t uaen_lock;
	dma_addr_t sq_dma_addr;
	dma_addr_t cq_dma_addr;
	struct sdi_admin_command *sq_cmds;
	volatile struct sdi_completion *cqes;
	wait_queue_head_t sq_full;
	u32 __iomem *q_db;
	u32 sq_size;
	u32 cq_size;
	u16 q_depth;
	u16 cq_vector;
	u16 sq_head;
	u16 sq_tail;
	u16 cq_head;
	u16 qid;
	u8 cq_phase;
	u8 cqe_seen;
	u8 q_suspended;
	int cpu_no;
	struct async_cmd_info uaen[MAX_UAEN_NR];
	struct async_cmd_info caen[MAX_CAEN_NR];
	unsigned long uaen_id[BITS_TO_LONGS(MAX_UAEN_NR + 1)];
	unsigned long caen_id[BITS_TO_LONGS(MAX_CAEN_NR + 1)];
	unsigned long cmdid_data[];
};


struct queue_info
{
	u16 qid;
	u16 cqid;
	u16 flags;
	u16 stride;
	u16 depth;
	u16 type;
//    u16 udrv_type;
	u32 size;
	u32 vector;
	u16 vecid;
	u16 qprio;
};

#define GET_SDI_PDEV_BY_PF_NO(pf_no) &gsdev[(pf_no)];
#define GET_SDI_PDEV_BY_UINFO(udev_info) &gsdev[(udev_info)->pf_no];


#define ADMIN_CMD_DMA_ADDR_FLAG 1
#define ADMIN_CMD_PIO_ADDR_FLAG 0
#define LINKDOWN_WAIT_TIME  (90000UL)

struct pf12_bar {
	__u64           cap;    /* Controller Capabilities */
	__u32           vs;     /* Version */
	__u32           intms;  /* Interrupt Mask Set */
	__u32           intmc;  /* Interrupt Mask Clear */
	__u32           cc;     /* Controller Configuration */
	__u32           rsvd1;  /* Reserved */
	__u32           csts;   /* Controller Status */
	__u32           rsvd2;  /* Reserved */
	__u32           aqa;    /* Admin Queue Attributes */
	__u64           asq;    /* Admin SQ Base Address */
	__u64           acq;    /* Admin CQ Base Address */
};

#define SDI_PF12_CAP_MQES(cap)      ((cap) & 0xffff)
#define SDI_PF12_CAP_TIMEOUT(cap)   (((cap) >> 24) & 0xff)
#define SDI_PF12_CAP_STRIDE(cap)    (((cap) >> 32) & 0xf)
#define SDI_PF12_CAP_MPSMIN(cap)    (((cap) >> 48) & 0xf)
#define SDI_PF12_CAP_MPSMAX(cap)    (((cap) >> 52) & 0xf)


/* Special values must be less than 0x1000 */
#define CMD_CTX_BASE        ((void *)POISON_POINTER_DELTA)
#define CMD_CTX_CANCELLED   (0x30C + CMD_CTX_BASE)
#define CMD_CTX_COMPLETED   (0x310 + CMD_CTX_BASE)
#define CMD_CTX_INVALID     (0x314 + CMD_CTX_BASE)
#define CMD_CTX_ABORT       (0x318 + CMD_CTX_BASE)
#define CMD_CTX_ASYNC       (0x31C + CMD_CTX_BASE)


enum {
	SDI_PF12_CC_ENABLE          = 1 << 0,
	SDI_PF12_CC_CSS_NVM         = 0 << 4,
	SDI_PF12_CC_MPS_SHIFT       = 7,
	SDI_PF12_CC_ARB_RR          = 0 << 11,
	SDI_PF12_CC_ARB_WRRU        = 1 << 11,
	SDI_PF12_CC_ARB_VS          = 7 << 11,
	SDI_PF12_CC_SHN_NONE        = 0 << 14,
	SDI_PF12_CC_SHN_NORMAL      = 1 << 14,
	SDI_PF12_CC_SHN_ABRUPT      = 2 << 14,
	SDI_PF12_CC_SHN_MASK        = 3 << 14,
	SDI_PF12_CC_IOSQES          = 6 << 16,
	SDI_PF12_CC_IOCQES          = 4 << 20,
	SDI_PF12_CSTS_RDY           = 1 << 0,
	SDI_PF12_CSTS_CFS           = 1 << 1,
	SDI_PF12_CSTS_SHST_NORMAL   = 0 << 2,
	SDI_PF12_CSTS_SHST_OCCUR    = 1 << 2,
	SDI_PF12_CSTS_SHST_CMPLT    = 2 << 2,
	SDI_PF12_CSTS_SHST_MASK     = 3 << 2
};



/* Admin commands */

enum sdi_admin_opcode {
	sdi_admin_delete_sq         = 0x00,
	sdi_admin_create_sq         = 0x01,
	sdi_admin_delete_cq         = 0x04,
	sdi_admin_create_cq         = 0x05,
	sdi_admin_identify          = 0x06,
	sdi_admin_abort_cmd         = 0x08,
	sdi_admin_set_features      = 0x09,
	sdi_admin_get_features      = 0x0a,
	sdi_admin_heart_beat        = 0x0b,
	sdi_admin_async_event       = 0x0c,
	sdi_admin_shutdown		    = 0xeb,
	sdi_admin_service_task_cmd  = 0xec,
	sdi_admin_udrv_type_check   = 0xed,
	sdi_admin_udrv_send_aen_cmd = 0xee,
	sdi_admin_udrv_send_cmd     = 0xef
};


enum {
	SDI_PF12_SC_SUCCESS          = 0x0,
	SDI_PF12_SC_INVALID_OPCODE   = 0x1,
	SDI_PF12_SC_INVALID_FIELD    = 0x2,
	SDI_PF12_SC_CMDID_CONFLICT   = 0x3,
	SDI_PF12_SC_DATA_XFER_ERROR  = 0x4,
	SDI_PF12_SC_POWER_LOSS       = 0x5,
	SDI_PF12_SC_INTERNAL         = 0x6,
	SDI_PF12_SC_ABORT_REQ        = 0x7,
	SDI_PF12_SC_ABORT_QUEUE      = 0x8,
	SDI_PF12_SC_FUSED_FAIL       = 0x9,
	SDI_PF12_SC_FUSED_MISSING    = 0xa,
	SDI_PF12_SC_INVALID_NS       = 0xb,
	SDI_PF12_SC_LBA_RANGE        = 0x80,
	SDI_PF12_SC_CAP_EXCEEDED     = 0x81,
	SDI_PF12_SC_NS_NOT_READY     = 0x82,
	SDI_PF12_SC_CQ_INVALID       = 0x100,
	SDI_PF12_SC_QID_INVALID      = 0x101,
	SDI_PF12_SC_QUEUE_SIZE       = 0x102,
	SDI_PF12_SC_ABORT_LIMIT      = 0x103,
	SDI_PF12_SC_ABORT_MISSING    = 0x104,
	SDI_PF12_SC_ASYNC_LIMIT      = 0x105,
	SDI_PF12_SC_FIRMWARE_SLOT    = 0x106,
	SDI_PF12_SC_FIRMWARE_IMAGE   = 0x107,
	SDI_PF12_SC_INVALID_VECTOR   = 0x108,
	SDI_PF12_SC_INVALID_LOG_PAGE = 0x109,
	SDI_PF12_SC_INVALID_FORMAT   = 0x10a,
	SDI_PF12_SC_BAD_ATTRIBUTES   = 0x180,
	SDI_PF12_SC_WRITE_FAULT      = 0x280,
	SDI_PF12_SC_READ_ERROR       = 0x281,
	SDI_PF12_SC_GUARD_CHECK      = 0x282,
	SDI_PF12_SC_APPTAG_CHECK     = 0x283,
	SDI_PF12_SC_REFTAG_CHECK     = 0x284,
	SDI_PF12_SC_COMPARE_FAILED   = 0x285,
	SDI_PF12_SC_ACCESS_DENIED    = 0x286
};

struct sdi_features {
	__u8            opcode;
	__u8            flags;
	__u16           command_id;
	__le32          nsid;
	__u64           rsvd2[2];
	__le64          prp1;
	__le64          prp2;
	__le32          fid;
	__le32          dword11;
	__u32           rsvd12[4];
};

struct sdi_create_cq {
	__u8            opcode;
	__u8            flags;
	__u16           command_id;
	__le32          size;                   /*IO queue size*/
	__u32           rsvd1[4];
	__le64          prp1;
	__u64           rsvd8;
	__le16          cqid;
	__le16          qsize;
	__le16          cq_flags;
	__le16          irq_vector;
	__le16          udrv_type;
	__le16          cq_type;
	__le16          stride;
	__le16          rsvd12;
	__u32           rsvd13[2];
};

struct sdi_create_sq {
	__u8            opcode;
	__u8            flags;
	__u16           command_id;
	__le32          size;                   /*IO queue size*/
	__u32           rsvd1[4];
	__le64          prp1;
	__u64           rsvd8;
	__le16          sqid;
	__le16          qsize;                  /*queue depth be compatible with current NVMe driver*/
	__le16          sq_flags;
	__le16          cqid;
	__le16          udrv_type;
	__le16          sq_type;
	__le16          stride;
	__u16           rsvd12;
	__u32           rsvd13[2];
};

struct sdi_delete_queue {
	__u8            opcode;
	__u8            flags;
	__u16           command_id;
	__u32           rsvd1[9];
	__le16          qid;
	__u16           udrv_type;
	__u32           rsvd11[5];
};

struct sdi_identify {
	__u8            opcode;
	__u8            flags;
	__u16           command_id;
	__le32          nsid;
	__u64           rsvd2[2];
	__le64          prp1;
	__le64          prp2;
	__le32          cns;
	__u32           rsvd11[5];
};

typedef struct data_uaen
{
	__u16 cmd;
	__u16 flag;
	__u32 rsvd;
	__le32 xmit_len;
	__le32 recv_len;
	__le64 xfer_paddr;
	__le64 recv_paddr;
}data_uaen;

struct sdi_uspec_cmd {
	__u8            opcode;
	__u8            flag;
	__u16           command_id;
	__u32           len;
	union
	{
		data_uaen uaen_data;
		__le64    xfer_paddr;
		char      data[56];
	} u;
};

struct sdi_amdin_completion {
	__le32  result;     /* Used by admin commands to return data */
	__u32   rsvd;
	__le16  sq_head;    /* how much of this queue may be reclaimed */
	__le16  sq_id;      /* submission queue that generated this entry */
	__u16   command_id; /* of the command which completed */
	__le16  status;     /* did the command fail, and if so, why? */
};

struct sdi_rw_command {
	__u8            opcode;
	__u8            flags;
	__u16           command_id;
	__le32          nsid;
	__u64           rsvd2;
	__le64          metadata;
	__le64          prp1;
	__le64          prp2;
	__le64          slba;
	__le16          length;
	__le16          control;
	__le32          dsmgmt;
	__le32          reftag;
	__le16          apptag;
	__le16          appmask;
};

struct sdi_common_command {
	__u8            opcode;
	__u8            flags;
	__u16           command_id;
	__le32          nsid;
	__u32           cdw2[2];
	__le64          metadata;
	__le64          prp1;
	__le64          prp2;
	__u32           cdw10[6];
};

struct sdi_service_command {
	__u8            opcode;
	__u8            flags;
	__u16           command_id;
	__le32          rsvd[15];
};

struct sdi_shutdown {
	__u8            opcode;
	__u8            flags;
	__u16           command_id;
	__le32          sid;
	__le32          rsvd[14];
};

struct sdi_admin_command {
	union {
		struct sdi_common_command common;
		struct sdi_rw_command      rw;
		struct sdi_features        features;
		struct sdi_identify        identify;
		struct sdi_uspec_cmd       ucmd;
		struct sdi_create_cq       create_cq;
		struct sdi_create_sq       create_sq;
		struct sdi_delete_queue    delete_queue;
		struct sdi_service_command service_cmd;
		struct sdi_shutdown         shutdown;
	};
};


#define UCMD_LEN (sizeof(struct sdi_uspec_cmd) - (u32)offsetof(struct sdi_uspec_cmd, u))



#define UEP_NAME_LEN     (15)

enum
{
	SDI_EP_LINK_DOWN,
	SDI_ARM_UDRV_ADD,
	SDI_ARM_UDRV_REM
};

typedef enum
{
	SDI_EP_LINK_EVT,
	SDI_EP_RESET,
	SDI_EP_ERR_DETECTED,
	SDI_EP_SHUTDOWN,
	SDI_EP_SUSPEND,
	SDI_EP_RESUME,
	SDI_EP_ERR,
	SDI_EP_AEN,
	SDI_EP_UDRV_SPEC,
	SDI_EP_REMOVE
}sdi_ll_event_t;


typedef struct sdi_iod
{
	int npages;    /* In the PRP list. 0 means small pool in use */
	int offset;    /* Of sgl list */
	u32 nents;     /* Used in scatterlist */
	u32 length;    /* Of data, in bytes */
	dma_addr_t first_dma;
	struct scatterlist sg[0];
} sdi_iod_t;


typedef struct sdi_ep_base_info
{
	u16 drv_type;
	void *priv_data;
	char uep_name[UEP_NAME_LEN];
	void (*event_handler_call)(sdi_ll_event_t event, void *prv_data, unsigned long data);
} sdi_ep_base_info_t;


typedef struct cqe_status
{
	u16 cqid;
	u16 sqid;
	u32 status;
}cqe_status_t;


typedef struct ulp_cq_base
{
	u16 q_depth;
	u16 stride;
	u16 q_type;
	u16 rsvd;
	int cpu_no;
	void *prv_data;
	void (*cqe_handler)(void *prv, void *cqe_data, u16 len, cqe_status_t *head_info);
//    void (*pre_cqe_handler)(void *prv, u16 cqid);
//    void (*post_cqe_handler)(void *prv, u16 cqid);
} ulp_cq_base_t;



typedef struct sdi_cq_info
{
	struct kobject kobj;
	spinlock_t cq_lock;
	void *cq_addr;
	dma_addr_t dma_addr;
	int cpu_no;
	sdi_pdev_info_t *spdev;
	u32 __iomem *q_db;
	u64 rx_ng_cnt;
	u64 rx_ok_cnt;
	u32 size;
	u32 cq_vector;
	u16 vecid;
	u8 cqe_seen;
	u8 cq_phase;
	u8 suspend;
	u16 cqid;
	u16 head;
	u16 tail;
	u16 q_depth;
	u16 stride;
	u16 q_type;
	u16 rsvd;
	char irqname[SDI_PF12_IRQ_NAME_LEN];
	void (*cqe_handler)(void *prv, void *cqe_data, u16 len, cqe_status_t *head_info);
//    void (*pre_cqe_handler)(void *prv, u16 cqid);
//    void (*post_cqe_handler)(void *prv, u16 cqid);
	void *prv_data;
} sdi_cq_info_t;

enum
{
	SDI_SQ_PRIO_URGENT = (0 << 1),
	SDI_SQ_PRIO_HIGH   = (1 << 1),
	SDI_SQ_PRIO_MEDIUM = (2 << 1),
	SDI_SQ_PRIO_LOW    = (3 << 1)
};

typedef struct ulp_sq_base
{
	u16 q_depth;
	u16 stride;
	u16 q_type;
	u16 cq_id;
	u16 qprio;
} ulp_sq_base_t;


typedef struct uaen_info
{
	u16 status;
	u32 result;
	void *tag;
} uaen_info_t;


typedef struct uaen_data
{
	u16 cmd;
	u16 flag;
	u32 rsvd;
	u32 xfer_len;
	u32 recv_len;
	dma_addr_t xfer_paddr;
	dma_addr_t recv_paddr;
}uaen_data_t;

struct sdi_pdev_info
{
	struct kobject kobj;
	struct kobject *parent;
	struct pci_dev *pdev;
	char name[SDI_PF12_EP_NAME_LEN];
	unsigned long state;
	sdi_ep_base_info_t *uep_base;
	sdi_sq_info_t *sq_info[SDI_PF12_MAX_SQ_NR]; //waste index 0 to keep SQ id and array id same
	sdi_cq_info_t *cq_info[SDI_PF12_MAX_CQ_NR]; //waste index 0 to keep CQ id and array id same
	sdi_admin_queue_t *admin_queue;
	u8 __iomem *bar0;
	u8 __iomem *bar2;
	struct pf12_bar __iomem *bar;
	struct msix_entry *entry;
	struct dma_pool *sgl_page_pool;
	struct dma_pool *sgl_small_pool;
	int queue_count;
	int db_stride;
	int max_depth;
	u32 cur_queue_group;
	u32 ctrl_config;
	u32 __iomem *dbs;
	spinlock_t qid_lock;
	u16 max_io_sq_nr;
	u16 max_io_cq_nr;
	u16 vector_max_num;
	u16 max_hw_sectors;
	u16 irq_type;
	u32 queue_max_size;
	u64 dma_mask;
	struct work_struct service_task;
	struct timer_list service_timer;
	int service_phase;
	int event_limit;
	int aen_num;
	int page_size;
	struct workqueue_struct *aenwork;
	int use_threaded_interrupts;
	unsigned long qids[BITS_TO_LONGS(SDI_PF12_MAX_QUEUE_NUM)];
};

sdi_pdev_info_t* ep_get_sdi_dev(void);
int ep_send_cmd(void *cmd_data, u32 len,int *result, unsigned timeout);
int ep_sqe_submit(int sqid, const void *data);
int sdi_dev_start(struct sdi_pdev_info *sdev);

int sdi_pf_linkdown(void);
int linkdown_reinit(void);

struct sdi_iod *sdi_alloc_iod(u32 nseg, u32 nbytes, gfp_t gfp);
int sdi_setup_sgl(struct sdi_iod *iod, u32 len, gfp_t gfp);
void sdi_free_iod(struct sdi_iod *iod);

void ep_delete_queue(u16 qid);
s16 ep_create_queue(ulp_cq_base_t *cq_base,ulp_sq_base_t *sq_base);
int sdi_pf12_common_init(void);
void sdi_pf12_common_exit(void);

int transfer_sys_do_reset(void);


#endif

