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

#include "epfront.h"
#include <linux/pci.h>
#include <linux/aer.h>

#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 32) && !defined(RHEL_RELEASE))
/*****************************************************************************
Function    : pcie_capability_read_word
Description : get pci capability and read config word
Input       : struct pci_dev * dev
              int pos
              u16 * val
Output      : int
Return      : int
*****************************************************************************/
static int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{
	int ret;
	int pcie_cap_reg;
	
	*val = 0;
	if (pos & 1)
		return -EINVAL;
	
	pcie_cap_reg = pci_find_capability(dev, PCI_CAP_ID_EXP);
	if (pcie_cap_reg <= 0) {
		epfront_err_limit("pci_find_capability failed");
		return -EFAULT;
	}

	ret = pci_read_config_word(dev, pcie_cap_reg + pos, val);
	/*
	 * Reset *val to 0 if pci_read_config_dword() fails, it may
	 * have been written as 0xFFFFFFFF if hardware error happens
	 * during pci_read_config_dword().
	 */
	if (ret)
		*val = 0;
	return ret;
}

/*****************************************************************************
Function    : pcie_capability_write_word
Description : get pci capability and write config word
Input       : struct pci_dev * dev
              int pos
              u16 val
Output      : int
Return      : int
*****************************************************************************/
static int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val)
{
	int pcie_cap_reg;

	if (pos & 1)
		return -EINVAL;
	
	pcie_cap_reg = pci_find_capability(dev, PCI_CAP_ID_EXP);
	if (pcie_cap_reg <= 0) {
		epfront_err_limit("pci_find_capability failed");
		return -EFAULT;
	}
	
	return pci_write_config_word(dev, pcie_cap_reg + pos, val);
}

/*****************************************************************************
Function    : pcie_capability_clear_and_set_word
Description : get pci capability and clear it by write word
Input       : struct pci_dev * dev
              int pos
              u16 clear
              u16 set
Output      : int
Return      : int
*****************************************************************************/
static int pcie_capability_clear_and_set_word(struct pci_dev *dev, int pos,
				       u16 clear, u16 set)
{
	int ret;
	u16 val;

	ret = pcie_capability_read_word(dev, pos, &val);
	if (!ret) {
		val &= ~clear;
		val |= set;
		ret = pcie_capability_write_word(dev, pos, val);
	}

	return ret;
}
#endif

static sdi_pdev_info_t gsdev;
static unsigned int probe_succ_flag = 0;

#define SDI_Q_MAX_DEPTH 4096

#define SDI_MAX_NRS 2
static int total_num = 0;

static DEFINE_SPINLOCK(gdev_lock);
static DEFINE_SPINLOCK(heartbeat_lock);
static DEFINE_SPINLOCK(update_lock);
static DEFINE_SPINLOCK(perceiv_reset_lock);

static struct task_struct *sdi_thread = NULL;
static struct task_struct *heartbeat_thread = NULL;
static struct task_struct *update_thread = NULL;
static struct task_struct *reset_thread = NULL;
static struct task_struct *sdiep_probe_thread = NULL;

#ifdef TEST_BY_NVME
#define PCI_CLASS_STORAGE_EXPRESS		0x010802
#endif

#define SDI_DEVICE_ID_1610			0x1610
#define SDI_VENDOR_ID				0x19e5

#define PCIE_AER_REG				0x3c
#define PCIE_AER_ENABLE				17

/**
 * PCI_DEVICE - macro used to describe a specific pci device
 * @subdev: the 16 bit PCI sub system ID
 *
 * This macro is used to create a struct pci_device_id that matches a
 * specific device.  The subvendor fields will be set to
 * PCI_ANY_ID.
 */
#define SDI_PCI_DEVICE(subdev) {\
    .vendor = (SDI_VENDOR_ID), .device = (SDI_DEVICE_ID_1610), \
    .subvendor = PCI_ANY_ID, .subdevice = (subdev) }

#define SDI_PF0_SUBDEV				0
#define SDI_PF1_SUBDEV				1
#define SDI_PF2_SUBDEV				2

#define CHECK_LINK_TIMEOUT_NUM			60
#define CHECK_LINK_WAITTIME			100
#define LINK_CHECK_STATE			0xFFFFFFFF
#define HEATBEAT_WAITTIME			1000
#define ARM_SDIEP_PROBE_WAITTIME		1000
#define AER_WAITTIME				1000
#define AER_WAITNUM				180
/* 
 * perceiving restart needs 60s wait time to process,
 * wait for arm reboot process and complete ep init.
 */
#define SDI_RESET_WAITTIME			90
#define HEATBEAT_TIMEOUT_NUM			3
#define UPDATE_WAIT_TIME			1
#define WAIT_COMPLETE_TIME			10000
#define WAIT_COMPLETE_NUM			10
#define WAIT_READY_TIMEOUT			(10 * HZ)
#define WAIT_FRONT_RMMOD_TIMEOUT		10000
#define WAIT_FRONT_RMMOD_TIMENUM		12
#define WAIT_HEARTBEAT_2_NORMAL			1000

volatile unsigned long demand_status = 0;

/* only matching SDI PF1 PF2 device.
 * when enable IOMMU, if match PF0 device, probe will failed
 * and cause IOMMU issue when PF1/2 using DMA.
 */
static struct pci_device_id sdi_pf12_pci_tbl[] = {
		SDI_PCI_DEVICE(SDI_PF0_SUBDEV),
		SDI_PCI_DEVICE(SDI_PF1_SUBDEV),
		SDI_PCI_DEVICE(SDI_PF2_SUBDEV),
		{0, }
};
MODULE_DEVICE_TABLE(pci, sdi_pf12_pci_tbl);


static void sdi_aq_remove (struct sdi_pdev_info *sdev);
static int gdev_list_add(void);
static void gdev_list_remove(void);
static int heartbeat_thread_add(void);
static void heartbeat_thread_remove(void);
static int update_thread_add(void);
static void update_thread_remove(void);
static int reset_thread_add(void);
static void reset_thread_remove(void);
static int sdi_start_probeep_thd(struct sdi_pdev_info *sdev);

static void sdi_admin_cancel_ios(sdi_admin_queue_t *admin_queue, bool timeout);
static int sdi_submit_admin_cmd(sdi_pdev_info_t *spdev, struct sdi_admin_command *cmd,u32 *result, unsigned timeout);
static void sdi_dev_shutdown(struct sdi_pdev_info *sdev);
static int heartbeat_kthread(void *data);
static int update_kthread(void *data);
static int reset_kthread(void *data);

static void transfer_do_aer_set(struct pci_dev *pdev, u32 enable);

enum
{
	SYNC_UCMD_USING_DMA,
	SYNC_UCMD_USING_PIO
};

enum {
	SDI_QUEUE_PHYS_CONTIG  = (1 << 0),
	SDI_CQ_IRQ_ENABLED     = (1 << 1)
};

/*****************************************************************************
Function    : ep_get_sdi_dev
Description : get sdi device
Input       : void
Output      : sdi_pdev_info_t*
Return      : sdi_pdev_info_t*
*****************************************************************************/
sdi_pdev_info_t* ep_get_sdi_dev(void)
{
    return &gsdev;
}

/*****************************************************************************
Function    : sdi_setup_sgl
Description : setup sgl
Input       : struct sdi_iod * iod
              u32 len
              gfp_t gfp
Output      : int
Return      : int
*****************************************************************************/
int sdi_setup_sgl(struct cmnd_dma *dma_info, struct scatterlist *sg, int nents, u32 len)
{
	u32 i;
	int status;
	unsigned int dma_len;
	int length;
    dma_addr_t dma_addr, sgs_dma;
	struct sdi_sgl_info *sg_list;
	const int last_sg = PAGE_SIZE / sizeof(struct sdi_sgl_info) - 1;

	if (!dma_info || !sg || !nents || !len) {
		epfront_err("Input parameter is invalid.");
		return -EINVAL;
	}

	sg_list = dma_info->sg_list;
    sgs_dma = dma_info->first_dma_addr;

	dma_addr = sg_dma_address(sg);
	dma_len = sg_dma_len(sg);
	length = (int)len;

	i = 0;
	for (;;) {
		if (i > last_sg) {
			struct sdi_sgl_info *old_sgl_list = sg_list;
			sg_list = (struct sdi_sgl_info *)((char *)sg_list + PAGE_SIZE);
            sgs_dma += PAGE_SIZE;

			sg_list[0].addr = old_sgl_list[i - 1].addr;
			sg_list[0].id = old_sgl_list[i - 1].id;
			sg_list[0].len = old_sgl_list[i - 1].len;

			old_sgl_list[i - 1].addr = cpu_to_le64(sgs_dma);
			old_sgl_list[i - 1].len = PAGE_SIZE;
			old_sgl_list[i - 1].id = SGL_SEGMENT_ID;

			i = 1;
		}

		sg_list[i].addr = cpu_to_le64(dma_addr);
		sg_list[i].len = cpu_to_le32(dma_len);

		if ((length -= (int)dma_len) > 0) {
			sg_list[i].id = SGL_DATA_BLOCK_ID;
		} else if (!length) {
			sg_list[i].id = SGL_LAST_SEGMENT_ID;
			break;
		} else {
			epfront_err("Parameter len is not equal iod's sgs total len.");
			status = -EINVAL;
			goto reset_dma;
		}

		sg = sg_next(sg);
		if (unlikely(!sg)) {
			epfront_err("Next SG is NULL.");
			status = -EINVAL;
			goto reset_dma;
		}

		dma_addr = sg_dma_address(sg);
		dma_len = sg_dma_len(sg);
		i++;
	}

	return 0;

reset_dma:
    memset(dma_info->sg_list, 0, dma_info->length);

	return status;
}


/*****************************************************************************
Function    : sq_alloc
Description : alloc sq
Input       : const ulp_sq_base_t * sq_base
              u16 qid
              void * addr
              dma_addr_t dma_addr
Output      : sdi_sq_info_t *
Return      : sdi_sq_info_t *
*****************************************************************************/
static sdi_sq_info_t *sq_alloc(const ulp_sq_base_t *sq_base, u16 qid,void *addr , dma_addr_t dma_addr){

	sdi_pdev_info_t *spdev = &gsdev;
	sdi_sq_info_t *sq_info;
	u32 size;

	BUG_ON(!spdev);
	BUG_ON(!addr);

	sq_info = (sdi_sq_info_t *)kzalloc(sizeof(sdi_sq_info_t), GFP_KERNEL);
	if (!sq_info) {
		epfront_err("Allocate memory failed.");
		return NULL;
	}

	size = sq_base->q_depth * sq_base->stride;

	sq_info->sq_addr   = addr;
	sq_info->dma_addr  = dma_addr;
	sq_info->q_db      = &spdev->dbs[(long)qid * 2 * spdev->db_stride];
	sq_info->sqid      = qid;
	sq_info->spdev     = spdev;
	sq_info->size      = size;
	sq_info->stride    = sq_base->stride;
	sq_info->q_depth   = sq_base->q_depth;
	sq_info->q_type    = sq_base->q_type;
	sq_info->cq_id     = qid;
	sq_info->spdev     = spdev;
	sq_info->q_prio	   = sq_base->qprio;
	spin_lock_init(&(sq_info->sq_lock));

	return sq_info;
}

/*****************************************************************************
Function    : send_create_sq_cmd
Description : create sq command
Input       : const sdi_sq_info_t * sq_info
Output      : int
Return      : int
*****************************************************************************/
static int send_create_sq_cmd(const sdi_sq_info_t *sq_info)
{
	sdi_pdev_info_t *spdev = &gsdev;
	struct sdi_admin_command c;
	int status;

	BUG_ON(!spdev);

	memset((void *)&c, 0, sizeof(c));
	c.create_sq.opcode     = sdi_admin_create_sq;
	c.create_sq.prp1       = cpu_to_le64(sq_info->dma_addr);
	c.create_sq.sqid       = cpu_to_le16(sq_info->sqid);
	c.create_sq.qsize      = cpu_to_le16(sq_info->q_depth - 1);
	c.create_sq.cqid       = cpu_to_le16(sq_info->sqid);
	c.create_sq.sq_flags   = cpu_to_le16(SDI_QUEUE_PHYS_CONTIG | sq_info->q_prio);
	c.create_sq.size       = cpu_to_le32(sq_info->size);
	c.create_sq.sq_type    = cpu_to_le16(sq_info->q_type);
	c.create_sq.stride     = cpu_to_le16(sq_info->stride);

	epfront_dbg("SDI_DBG_LOG: create SQ opcode: %d, sqid: %d, qsize: %d, cqid: %d, sq_flags: %d",
			c.create_sq.opcode, c.create_sq.sqid , c.create_sq.qsize,
			c.create_sq.cqid, c.create_sq.sq_flags);

	status = sdi_submit_admin_cmd(spdev, &c, NULL, ADMIN_TIMEOUT);
	if (status)
		epfront_err("Create SQ[%d] failed status: %d", sq_info->sqid, status);

	return status;

}

/*****************************************************************************
Function    : process_io_cq
Description : process io cq list
Input       : sdi_cq_info_t * cq
Output      : int
Return      : int
*****************************************************************************/
static int process_io_cq(sdi_cq_info_t *cq)
{
	u16 head;
	u8 phase;
	cqe_head_t *cqe;
	cqe_status_t head_info;
	sdi_sq_info_t *sq_info;
	sdi_pdev_info_t *spdev;

	spdev = cq->spdev;
	head = cq->head;
	phase = cq->cq_phase;

	for (; ;) {
		cqe = (cqe_head_t *)((unsigned char*)cq->cq_addr + (long)head * cq->stride);

		if(phase != cqe->phase)
			break;

		head_info.cqid = cq->cqid;

		if (++head == cq->q_depth) {
			head = 0;
			phase = !phase;
		}

		if (likely(cq->cqe_handler))
			cq->cqe_handler(cq->prv_data, cqe, cq->stride, &head_info);

		cq->rx_ok_cnt++;

		sq_info = spdev->sq_info[head_info.cqid];
		if (unlikely(!sq_info)) {
			epfront_warn("%s SQ[%d] not exist.", spdev->name, head_info.cqid);  ////head_info.sqid
			continue;
		}

		QUEUE_INC_ONE(sq_info->head, sq_info->q_depth);
	}

	if (head == cq->head && cq->cq_phase == phase)
		return 0;

	writel(head, cq->q_db);
	cq->head = head;
	cq->cq_phase = phase;
	cq->cqe_seen = 1;

	return 1;
}


/*****************************************************************************
Function    : sdi_irq_check
Description : check sdi irq
Input       : int irq
              void * data
Output      : irqreturn_t
Return      : irqreturn_t
*****************************************************************************/
static irqreturn_t sdi_irq_check(int irq, void *data)
{
	sdi_cq_info_t *cq = (sdi_cq_info_t *) data;
	cqe_head_t *cqe;

	UNREFERENCE_PARAM(irq);

	cqe = (cqe_head_t *)((unsigned char* )cq->cq_addr + (long)cq->head * cq->stride);
	if (cqe->phase != cq->cq_phase)
		return IRQ_NONE;

	return IRQ_WAKE_THREAD;
}


/*****************************************************************************
Function    : sdi_io_irq
Description : callback function for irq request
Input       : int irq
              void * data
Output      : irqreturn_t
Return      : irqreturn_t
*****************************************************************************/
static irqreturn_t sdi_io_irq(int irq, void *data)
{
	irqreturn_t result;
	sdi_cq_info_t *cq = (sdi_cq_info_t *) data;

	UNREFERENCE_PARAM(irq);

	spin_lock(&cq->cq_lock);
	(void)process_io_cq(cq);
	spin_unlock(&cq->cq_lock);

	result = cq->cqe_seen ? IRQ_HANDLED : IRQ_NONE;
	cq->cqe_seen = 0;

	return result;
}

/*****************************************************************************
Function    : sdi_pf12_cq_request_irq
Description : cq request thread irq
Input       : sdi_cq_info_t * cq_info
Output      : int
Return      : int
*****************************************************************************/
static int sdi_pf12_cq_request_irq(sdi_cq_info_t *cq_info)
{
	sdi_pdev_info_t *spdev;

	spdev = cq_info->spdev;

	if (spdev->use_threaded_interrupts)
		return request_threaded_irq(cq_info->cq_vector, sdi_irq_check, sdi_io_irq,
				(unsigned long)IRQF_SHARED, cq_info->irqname, (void *)cq_info);
	return request_irq(cq_info->cq_vector, sdi_io_irq, (unsigned long)IRQF_SHARED,
				cq_info->irqname, (void*)cq_info);
}


/*****************************************************************************
Function    : cq_alloc
Description : alloc cq
Input       : const ulp_cq_base_t * cq_base
              int qid
              void * addr
              dma_addr_t dma_addr
Output      : sdi_cq_info_t *
Return      : sdi_cq_info_t *
*****************************************************************************/
static sdi_cq_info_t *cq_alloc(const ulp_cq_base_t *cq_base,  int qid,void *addr, dma_addr_t dma_addr){

	int size;
	sdi_pdev_info_t *spdev = &gsdev;
	sdi_cq_info_t *cq_info = NULL;

	BUG_ON(!spdev);
	BUG_ON(!addr);

	cq_info = (sdi_cq_info_t *)kzalloc(sizeof(sdi_cq_info_t), GFP_KERNEL);
	if (!cq_info) {
		epfront_err("Allocate memory failed.");
		return NULL;
	}
	size = cq_base->stride * cq_base->q_depth;
	cq_info->cq_addr = addr;
	cq_info->dma_addr = dma_addr;

	memset((void *)cq_info->cq_addr, 0, size);

	cq_info->q_db	     = &spdev->dbs[(long)qid * 2 * spdev->db_stride] + (long)spdev->db_stride;
	cq_info->cq_vector   = spdev->entry[qid].vector;
	cq_info->cqid	     = qid;
	cq_info->spdev	     = spdev;
	cq_info->size	     = size;
	cq_info->vecid	     = qid;
	cq_info->stride      = cq_base->stride;
	cq_info->q_type      = cq_base->q_type;
	cq_info->q_depth     = cq_base->q_depth;
	cq_info->prv_data    = cq_base->prv_data;
	cq_info->cq_phase    = 1;
	cq_info->cqe_handler = cq_base->cqe_handler;
	cq_info->cpu_no      = cq_base->cpu_no;
	cq_info->prv_data    = cq_base->prv_data;
	spin_lock_init(&(cq_info->cq_lock));

	(void)snprintf(cq_info->irqname, sizeof(cq_info->irqname), "cqid:%02d",	qid);

	return cq_info;
}


/*****************************************************************************
Function    : send_create_cq_cmd
Description : create cq command
Input       : sdi_cq_info_t * cq_info
Output      : int
Return      : int
*****************************************************************************/
static int send_create_cq_cmd(sdi_cq_info_t *cq_info)
{
	struct sdi_admin_command c;
	sdi_pdev_info_t *spdev = &gsdev;
	int status;

	BUG_ON(!spdev);

	memset((void *)&c, 0, sizeof(c));
	c.create_cq.opcode	= sdi_admin_create_cq;
	c.create_cq.prp1	= cpu_to_le64(cq_info->dma_addr);
	c.create_cq.cqid	= cpu_to_le16(cq_info->cqid);
	c.create_cq.qsize	= cpu_to_le16(cq_info->q_depth- 1);
	c.create_cq.irq_vector  = cpu_to_le16(cq_info->vecid);
	c.create_cq.cq_flags    = cpu_to_le16(SDI_QUEUE_PHYS_CONTIG |SDI_CQ_IRQ_ENABLED);
	c.create_cq.size	= cpu_to_le32(cq_info->size);
	c.create_cq.cq_type     = cpu_to_le16(cq_info->q_type);
	c.create_cq.stride	= cpu_to_le16(cq_info->stride);

	epfront_dbg("SDI_DBG_LOG: create CQ opcode: %d, cqid: %d, qsize: %d, cq_flags: %d, irq_vector: %d",
			c.create_cq.opcode, c.create_cq.cqid , c.create_cq.qsize,
			c.create_cq.cq_flags, c.create_cq.irq_vector);

	status = sdi_submit_admin_cmd(spdev, &c, NULL, ADMIN_TIMEOUT);
	if (status)
		epfront_err("create CQ[%d] failed status: %x", cq_info->cqid ,status);

	return status;


}

/*****************************************************************************
Function    : send_delete_sq_cmd
Description : delete sq command
Input       : u16 qid
Output      : void
Return      : void
*****************************************************************************/
static void send_delete_sq_cmd(u16  qid){

	struct sdi_admin_command c;

	sdi_pdev_info_t *spdev = &gsdev;
	BUG_ON(!spdev);

	memset((void*)&c,0,sizeof(c));
	c.delete_queue.opcode = sdi_admin_delete_sq;
	c.delete_queue.qid = cpu_to_le16(qid);
	(void)sdi_submit_admin_cmd(spdev, &c, NULL, ADMIN_TIMEOUT);
}

/*****************************************************************************
Function    : send_delete_cq_cmd
Description : delete cq command
Input       : u16 qid
Output      : void
Return      : void
*****************************************************************************/
static void send_delete_cq_cmd(u16 qid){

	struct sdi_admin_command c;
	sdi_pdev_info_t *spdev = &gsdev;
	BUG_ON(!spdev);

	memset((void*)&c, 0, sizeof(c));
	c.delete_queue.opcode = sdi_admin_delete_cq;
	c.delete_queue.qid = cpu_to_le16(qid);
	(void)sdi_submit_admin_cmd(spdev, &c, NULL, ADMIN_TIMEOUT);
}


/*****************************************************************************
Function    : ep_create_queue
Description : create queue
Input       : ulp_cq_base_t * cq_base
              ulp_sq_base_t * sq_base
Output      : s16
Return      : s16
*****************************************************************************/
s16 ep_create_queue(ulp_cq_base_t *cq_base,ulp_sq_base_t *sq_base){

	sdi_pdev_info_t *spdev = &gsdev;
	u16 depth, qid;
	int status = -1;
	int cq_size = 0, sq_size = 0, offset = 0;
	void *addr = NULL;
	dma_addr_t dma_addr;

	depth = spdev->max_io_cq_nr + 1;
	spin_lock(&spdev->qid_lock);
	do {
		qid = (u16)find_first_zero_bit((unsigned long *)spdev->qids, (unsigned long)depth);
		if (qid >= depth) {
			spin_unlock(&spdev->qid_lock);
			epfront_err("%s no more CQ resource.", spdev->name);
			return -ENOMEM;
		}
	} while (test_and_set_bit(qid, spdev->qids));
	spin_unlock(&spdev->qid_lock);

	epfront_info("qids %lx ",spdev->qids[0]);

	cq_size = cq_base->stride * cq_base->q_depth;
	sq_size = sq_base->stride * sq_base->q_depth;
	offset  = (int)(((cq_size + PAGE_SIZE - 1 ) >> PAGE_SHIFT) << PAGE_SHIFT);

	addr = (u8 *)dma_alloc_coherent(&spdev->pdev->dev,
		(unsigned int)(cq_size + sq_size + (u32)PAGE_SIZE), &dma_addr, GFP_KERNEL);
	if (!addr) {
		epfront_err("alloc mem failed");
		status = -ENOMEM;
		goto free_qid;
	}

	spdev->cq_info[qid] = cq_alloc(cq_base, qid, addr, dma_addr);
	if (!spdev->cq_info[qid]) {
		epfront_err("alloc cq failed");
		status = -ENOMEM;
		goto free_queue;
	}

	sq_base->cq_id = qid;
	spdev->sq_info[qid] = sq_alloc(sq_base, qid,
		(unsigned char*)addr + offset, dma_addr + offset);
	if (!spdev->sq_info[qid]) {
		epfront_err("alloc cq failed");
		status = -ENOMEM;
		goto free_cq_info;
	}

	epfront_info("alloc queue %d success", qid);

	status= send_create_cq_cmd(spdev->cq_info[qid]);

	if (status) {
		epfront_err("send cq cmd failed");
		status = -ENODEV;
		goto free_sq_info;
	}

	status= send_create_sq_cmd(spdev->sq_info[qid]);

	if (status) {
		epfront_err("send sq cmd failed");
		status = -ENODEV;
		goto free_cq;
	}

	if (cq_base->cpu_no >= 0){
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 32) || defined(RHEL_RELEASE))
		(void)irq_set_affinity_hint(spdev->cq_info[qid]->cq_vector, get_cpu_mask((u32)cq_base->cpu_no));	
#endif
	}

	status = sdi_pf12_cq_request_irq(spdev->cq_info[qid]);
	if (status) {
		epfront_err("%s CQ[%d] request IRQ [%d] failed.", spdev->name, qid, spdev->cq_info[qid]->cq_vector);
		status = -ENOMEM;
		goto free_sq;
	}

	status = sdi_create_cq_sysfs(spdev->cq_info[qid], &spdev->kobj);
	if (status) {
		epfront_err("sdi_create_cq_sysfs failed, qid[%d] ret[%d]", qid, status);
		goto free_cq_irq;
	}

	status = sdi_create_sq_sysfs(spdev->sq_info[qid], &spdev->kobj);
	if (status) {
		epfront_err("sdi_create_sq_sysfs failed, qid[%d] ret[%d]", qid, status);
		goto destroy_cq_sys;
	}
	
	return (signed short)qid;
destroy_cq_sys:
	sdi_destroy_cq_sysfs(spdev->cq_info[qid]);		
free_cq_irq:
	free_irq(spdev->cq_info[qid]->cq_vector, spdev->cq_info[qid]);
free_sq:
	send_delete_sq_cmd(qid);
free_cq:
	send_delete_cq_cmd(qid);
free_sq_info:
	kfree(spdev->sq_info[qid]);
	spdev->sq_info[qid] = NULL;
free_cq_info:
	kfree(spdev->cq_info[qid]);
	spdev->cq_info[qid] = NULL;
free_queue:
	dma_free_coherent(&spdev->pdev->dev, (unsigned int)(cq_size + sq_size + (u32)PAGE_SIZE), addr, dma_addr);
free_qid:
	spin_lock(&spdev->qid_lock);
	clear_bit(qid, spdev->qids);
	spin_unlock(&spdev->qid_lock);
	return status;
}


/*****************************************************************************
Function    : __ep_delete_cq
Description : delete cq
Input       : u16 qid
Output      : void
Return      : void
*****************************************************************************/
static void __ep_delete_cq(u16 qid){

	sdi_pdev_info_t *spdev = &gsdev;
	sdi_cq_info_t *cq_info = NULL;

	if (!spdev || QID_CHECK(qid)) {
		epfront_err("Input parameter is invalid.");
		return;
	}

	cq_info = spdev->cq_info[qid];
	if (!cq_info) {
		epfront_err("cq is not exist,return");
		return;
	}

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 32) || defined(RHEL_RELEASE))
	(void)irq_set_affinity_hint(cq_info->cq_vector, NULL);	
#endif
	free_irq(cq_info->cq_vector, cq_info);
	kfree(cq_info);
	spdev->cq_info[qid]= NULL;
}

/*****************************************************************************
Function    : __ep_delete_sq
Description : delete sq
Input       : u16 qid
Output      : void
Return      : void
*****************************************************************************/
static void __ep_delete_sq(u16 qid)
{
	sdi_sq_info_t *sq_info = NULL;
	sdi_pdev_info_t *spdev = &gsdev;

	if (!spdev || QID_CHECK(qid)) {
		epfront_err("Input parameter is invalid.");
		return ;
	}

	sq_info = spdev->sq_info[qid];
	if (!sq_info) {
		epfront_err("sq is not exist,return");
		return;
	}
	kfree(sq_info);
	spdev->sq_info[qid]= NULL;
}

/*****************************************************************************
Function    : is_send_cmd
Description : check device sendcmd status
Input       : void
Output      : int
Return      : int
*****************************************************************************/
static int is_send_cmd(void)
{
	struct pci_dev *pf0 = NULL;
	u32 val = 0;
	sdi_pdev_info_t *sdev = &gsdev;

	pf0 = pci_get_subsys(SDI_VENDOR_ID, SDI_DEVICE_ID_1610, PCI_ANY_ID, 0, NULL);
	if (unlikely(!pf0)) {
		epfront_err("find pf failed");
		return 0;
	}

	(void)pci_read_config_dword(pf0, 0, &val);
	pci_dev_put(pf0);
	// check link state
	if ((val & LINK_CHECK_STATE) == LINK_CHECK_STATE) {
		epfront_info("link state is 0xffff");
		return 0;
	}
	// check bar state;check update state,if update,admin queue may be not exist.
	if (!(sdev->bar) || test_bit(SDI_FRONT_UPDATE, &demand_status)) {
		epfront_info("sdev->bar NULL, demand 0x%lx", demand_status);
		return 0;
	}
	val = readl(&sdev->bar->csts) & LINK_CHECK_STATE;
	// check csts reg is 0xffffffff?
	if (val == LINK_CHECK_STATE) {
		epfront_info("csts reg is 0xffffffff.");
		return 0;
	}

	/* 
	 * check csts 8bit : check sdi.ko exist
	 * check csts 0bit:check csts.ready,if not ready,admin queue may be not exist.
	 */
	if (((val & 0x1) == 0x1) && (((val >> 8) & 0x1) == 0x1))
		return 1;
	epfront_info("csts 0x%x", val);
	return 0;
}

/*****************************************************************************
Function    : ep_delete_queue
Description : delete queue
Input       : u16 qid
Output      : void
Return      : void
*****************************************************************************/
void ep_delete_queue(u16 qid)
{
	sdi_pdev_info_t *spdev = &gsdev;
	sdi_cq_info_t *cq_info = NULL;
	sdi_sq_info_t *sq_info = NULL;

	BUG_ON(!spdev);

	if (!spdev || QID_CHECK(qid)) {
		epfront_err("invalid qid %d",qid);
		return;
	}

	spin_lock(&spdev->qid_lock);
	if (!test_bit(qid, spdev->qids)) {
		epfront_err("qid %d has already delete",qid);
		spin_unlock(&spdev->qid_lock);
		return;
	}
	spin_unlock(&spdev->qid_lock);

	cq_info = spdev->cq_info[qid];
	sq_info = spdev->sq_info[qid];

	if (sq_info)
		sdi_destroy_sq_sysfs(sq_info);
	if (cq_info)
		sdi_destroy_cq_sysfs(cq_info);

	if (is_send_cmd()) {
		send_delete_sq_cmd(qid);
		send_delete_cq_cmd(qid);
	}

	if (cq_info && sq_info)
		dma_free_coherent(&sq_info->spdev->pdev->dev, 
			(unsigned int)(sq_info->size + cq_info->size + (u32)PAGE_SIZE),
			(void*)cq_info->cq_addr, cq_info->dma_addr);
	else
	    epfront_err("can't happen: qid %d sq_info[0x%p] cq_info[0x%p]", qid, sq_info, cq_info);

	__ep_delete_sq(qid);
	__ep_delete_cq(qid);

	spin_lock(&spdev->qid_lock);
	clear_bit(qid, spdev->qids);
	spin_unlock(&spdev->qid_lock);

	epfront_info("delete queue %d success", qid);
}


/*****************************************************************************
Function    : ep_send_cmd
Description : submit admin command
Input       : void * cmd_data
              u32 len
              int * result
              unsigned timeout
Output      : int
Return      : int
*****************************************************************************/
int ep_send_cmd(void *cmd_data, u32 len,int *result, unsigned timeout)
{
	sdi_pdev_info_t *spdev = &gsdev;
	int status;
	struct sdi_admin_command c;

	if (!spdev || !cmd_data || !len || !timeout) {
		epfront_err("Input parameter is invalid.");
		return -EINVAL;
	}

	memset((void *)&c, 0, sizeof(struct sdi_admin_command));

	c.ucmd.opcode = sdi_admin_udrv_send_cmd;
	c.ucmd.len = len;
	c.ucmd.flag = SYNC_UCMD_USING_PIO;
	memcpy((void *)c.ucmd.u.data, cmd_data, len);

	status = sdi_submit_admin_cmd(spdev, &c, result, timeout);

	return status;
}

/*****************************************************************************
Function    : ep_sqe_submit
Description : submit sqe
Input       : int sqid
              const void * data
Output      : int
Return      : int
*****************************************************************************/
int ep_sqe_submit(int sqid, const void *data)
{
	sdi_sq_info_t *sq_info;
	unsigned long flags;
	sdi_pdev_info_t *spdev = &gsdev;
	u16 stride, qdepth;

	if (unlikely(!spdev  || !data || SQID_CHECK(sqid))) {
		epfront_err("Input parameter is invalid.");
		return -EINVAL;
	}

	sq_info = spdev->sq_info[sqid];

	if (unlikely(!sq_info)) {
		epfront_warn("%s SQ[%d] not exist.", spdev->name, sqid);
		return -EEXIST;
	}

	if (unlikely(test_bit(SQ_SUSPEND, &sq_info->state))) {
		epfront_warn("%s SQ[%d] is pending.", spdev->name, sqid);
		return -EBUSY;
	}

	stride = sq_info->stride;
	qdepth = sq_info->q_depth;

	spin_lock_irqsave(&sq_info->sq_lock, flags);

	if (Q_FULL(sq_info->head, sq_info->tail, qdepth)) {
		sq_info->tx_busy_cnt++;
		spin_unlock_irqrestore(&sq_info->sq_lock, flags);
		epfront_err_limit("%s SQ[%d] is full.", spdev->name, sqid);
		return -EBUSY;
	}

	memcpy(sq_info->sq_addr + (long)sq_info->tail * stride, data, stride);

	QUEUE_INC_ONE(sq_info->tail, qdepth);

	writel(sq_info->tail, sq_info->q_db);

	sq_info->tx_ok_cnt++;

	spin_unlock_irqrestore(&sq_info->sq_lock, flags);

	return 0;
}

/*****************************************************************************
Function    : _sdi_check_size
Description : check struct size
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static inline void _sdi_check_size(void)
{
	BUILD_BUG_ON(sizeof(struct sdi_common_command) != 64);
	BUILD_BUG_ON(sizeof(struct sdi_rw_command)     != 64);
	BUILD_BUG_ON(sizeof(struct sdi_uspec_cmd)      != 64);
	BUILD_BUG_ON(sizeof(struct sdi_features)       != 64);
	BUILD_BUG_ON(sizeof(struct sdi_identify)       != 64);
	BUILD_BUG_ON(sizeof(struct sdi_create_cq)      != 64);
	BUILD_BUG_ON(sizeof(struct sdi_create_sq)      != 64);
	BUILD_BUG_ON(sizeof(struct sdi_delete_queue)   != 64);
}

/*****************************************************************************
Function    : get_sdi_cmd_info
Description : get command information
Input       : sdi_admin_queue_t * aq
Output      : struct sdi_cmd_info *
Return      : struct sdi_cmd_info *
*****************************************************************************/
static struct sdi_cmd_info *get_sdi_cmd_info(sdi_admin_queue_t *aq)
{
	return (void *)&aq->cmdid_data[BITS_TO_LONGS(aq->q_depth)];
}


/*****************************************************************************
Function    : alloc_cmdid
Description : alloc command queue id
Input       : sdi_admin_queue_t * aq
              void * ctx
              sdi_completion_fn handler
              unsigned timeout
              int flag
Output      : int
Return      : int
*****************************************************************************/
static int alloc_cmdid(sdi_admin_queue_t *aq, void *ctx,
			sdi_completion_fn handler, unsigned timeout, int flag)
{
	u16 depth = aq->q_depth - 1;
	int cmdid;
	unsigned long flags;
	struct sdi_cmd_info *info = get_sdi_cmd_info(aq);

	spin_lock_irqsave(&aq->q_lock, flags);
	do {
		cmdid = (int)find_first_zero_bit(aq->cmdid_data,(unsigned long)depth);
		if (cmdid >= depth) {
			spin_unlock_irqrestore(&aq->q_lock, flags);
			return -EBUSY;
		}
	} while (test_and_set_bit(cmdid, aq->cmdid_data));
	spin_unlock_irqrestore(&aq->q_lock, flags);

	info[cmdid].fn = handler;
	info[cmdid].ctx = ctx;
	info[cmdid].timeout = jiffies + timeout;
	info[cmdid].aborted = 0;
	info[cmdid].flags = flag;

	return cmdid;
}


/*****************************************************************************
Function    : special_completion
Description : special device completion progress
Input       : sdi_admin_queue_t * aq
              void * ctx
              struct sdi_completion * cqe
Output      : void
Return      : void
*****************************************************************************/
static void special_completion(sdi_admin_queue_t *aq, void *ctx,
							   struct sdi_completion *cqe)
{
	if (!ctx)
		return;
	if (ctx == CMD_CTX_CANCELLED)
		return;

	if (ctx == CMD_CTX_COMPLETED) {
		epfront_warn("Device %s completed id %d twice on queue %d.",
				 aq->sdev->name, cqe->command_id, le16_to_cpup(&cqe->sq_id));
		return;
	}

	if (ctx == CMD_CTX_INVALID) {
		epfront_warn("Device %s invalid id %d completed on queue %d.",
				 aq->sdev->name, cqe->command_id, le16_to_cpup(&cqe->sq_id));
		return;
	}

	epfront_warn("Device %s Unknown special completion %p.", aq->sdev->name, ctx);
}


/*****************************************************************************
Function    : sync_completion
Description : sync completion
Input       : sdi_admin_queue_t * aq
              void * ctx
              struct sdi_completion * cqe
Output      : void
Return      : void
*****************************************************************************/
static void sync_completion(sdi_admin_queue_t *aq, void *ctx,
							struct sdi_completion *cqe)
{
	struct sync_cmd_info *cmdinfo = ctx;

	UNREFERENCE_PARAM(aq);

	cmdinfo->result = le32_to_cpup(&cqe->result);
	cmdinfo->status = le16_to_cpup(&cqe->status) >> 1;

	wake_up_interruptible(&cmdinfo->wq);
}

/*****************************************************************************
Function    : cancel_cmdid
Description : cancel command id
Input       : sdi_admin_queue_t * aq
              int cmdid
              sdi_completion_fn * fn
Output      : void *
Return      : void *
*****************************************************************************/
static void *cancel_cmdid(sdi_admin_queue_t *aq, int cmdid,
			      sdi_completion_fn *fn)
{
	void *ctx;
	struct sdi_cmd_info *info = get_sdi_cmd_info(aq);
	if (fn)
		*fn = info[cmdid].fn;
	ctx = info[cmdid].ctx;
	info[cmdid].fn = special_completion;
	info[cmdid].ctx = CMD_CTX_CANCELLED;
	info[cmdid].flags = REQ_CMD_TIMEOUT_ABORT;

	return ctx;
}

/*****************************************************************************
Function    : sdi_abort_command
Description : abort command
Input       : sdi_admin_queue_t * aq
              int cmdid
Output      : void
Return      : void
*****************************************************************************/
static void sdi_abort_command(sdi_admin_queue_t *aq, int cmdid)
{
	unsigned long flags;

	spin_lock_irqsave(&aq->q_lock, flags);
	(void)cancel_cmdid(aq, cmdid, NULL);
	spin_unlock_irqrestore(&aq->q_lock, flags);
}

/*****************************************************************************
Function    : free_cmdid
Description : free command id
Input       : sdi_admin_queue_t * aq
              int cmdid
              sdi_completion_fn * fn
Output      : void *
Return      : void *
*****************************************************************************/
static void *free_cmdid(sdi_admin_queue_t *aq, int cmdid,
			   sdi_completion_fn *fn)
{
	void *ctx;
	struct sdi_cmd_info *info = get_sdi_cmd_info(aq);

	if (cmdid >= aq->q_depth || !info[cmdid].fn) {
		if (fn)
			*fn = special_completion;
		return CMD_CTX_INVALID;
	}
	if (fn)
		*fn = info[cmdid].fn;
	ctx = info[cmdid].ctx;
	info[cmdid].fn = special_completion;
	info[cmdid].ctx = CMD_CTX_COMPLETED;
	clear_bit(cmdid, aq->cmdid_data);
	wake_up(&aq->sq_full);
	return ctx;
}

/*****************************************************************************
Function    : sdi_submit_cmd
Description : submot command
Input       : sdi_admin_queue_t * aq
              struct sdi_admin_command * cmd
Output      : int
Return      : int
*****************************************************************************/
static int sdi_submit_cmd(sdi_admin_queue_t *aq, struct sdi_admin_command *cmd)
{
	unsigned long flags;
	u16 tail;
	spin_lock_irqsave(&aq->q_lock, flags);
	if (aq->q_suspended) {
		spin_unlock_irqrestore(&aq->q_lock, flags);
		return -EBUSY;
	}
	tail = aq->sq_tail;
	memcpy(&aq->sq_cmds[tail], cmd, sizeof(*cmd));
	if (++tail == aq->q_depth)
		tail = 0;
	writel(tail, aq->q_db);
	aq->sq_tail = tail;
	spin_unlock_irqrestore(&aq->q_lock, flags);

	return 0;
}

/*****************************************************************************
Function    : sdi_submit_sync_cmd
Description : submit sync command
Input       : sdi_pdev_info_t * spdev
              struct sdi_admin_command * cmd
              u32 * result
              unsigned timeout
Output      : int
Return      : int
*****************************************************************************/
static int sdi_submit_sync_cmd(sdi_pdev_info_t *spdev, struct sdi_admin_command *cmd,
				       u32 *result, unsigned timeout)
{
	int cmdid, ret = 0;
	struct sync_cmd_info cmdinfo;
	sdi_admin_queue_t *aq;

	aq = spdev->admin_queue;
	if (unlikely(!aq)) {
		epfront_warn("%s admin queue is NULL.", spdev->name);
		return -ENODEV;
	}

	cmdinfo.task = current;
	init_waitqueue_head(&cmdinfo.wq);
	cmdinfo.status = -EINTR;

	cmdid = alloc_cmdid(aq, &cmdinfo, sync_completion, timeout, REQ_CMD_TIMEOUT);
	if (cmdid < 0) {
		epfront_err("No CMDID resource.");
		return -ENOMEM;
	}
	cmd->common.command_id = (u16)cmdid;

	set_current_state(TASK_KILLABLE);
	ret = sdi_submit_cmd(aq, cmd);
	if (ret) {
		(void)free_cmdid(aq, cmdid, NULL);
		set_current_state(TASK_RUNNING);
		epfront_warn("%s admin queue is pending now.", spdev->name);
		return ret;
	}

	wait_event_interruptible_timeout(cmdinfo.wq, cmdinfo.status != -EINTR, timeout);

	if (cmdinfo.status == -EINTR) {
		aq = spdev->admin_queue;
		if (unlikely(!aq)) {
			epfront_warn("%s admin queue is NULL.", spdev->name);
			return -ENODEV;
		}

		sdi_abort_command(aq, cmdid);
		epfront_err("%s waiting for response timeout.", spdev->name);
		return -EINTR;
	}

	if (result)
		*result = cmdinfo.result;

	return cmdinfo.status;
}


/*****************************************************************************
Function    : sdi_submit_admin_cmd
Description : submit admin command
Input       : sdi_pdev_info_t * spdev
              struct sdi_admin_command * cmd
              u32 * result
              unsigned timeout
Output      : int
Return      : int
*****************************************************************************/
static int sdi_submit_admin_cmd(sdi_pdev_info_t *spdev, struct sdi_admin_command *cmd,
					u32 *result, unsigned timeout)
{
	return sdi_submit_sync_cmd(spdev, cmd, result, timeout);
}


/*****************************************************************************
Function    : sdi_process_cq
Description : process cq
Input       : sdi_admin_queue_t * aq
Output      : int
Return      : int
*****************************************************************************/
static int sdi_process_cq(sdi_admin_queue_t *aq)
{
	u16 head;
	u8 phase;

	head = aq->cq_head;
	phase = aq->cq_phase;

	for (;;) {
		void *ctx;
		sdi_completion_fn fn;
		struct sdi_completion cqe = aq->cqes[head];
		if ((le16_to_cpu(cqe.status) & 1) != phase)
			break;
		aq->sq_head = le16_to_cpu(cqe.sq_head);
		if (++head == aq->q_depth) {
			head = 0;
			phase = !phase;
		}

		ctx = free_cmdid(aq, cqe.command_id, &fn);
		fn(aq, ctx, &cqe);
	}

	/* 
	 * If the controller ignores the cq head doorbell and continuously
	 * writes to the queue, it is theoretically possible to wrap around
	 * the queue twice and mistakenly return IRQ_NONE.  Linux only
	 * requires that 0.1% of your interrupts are handled, so this isn't
	 * a big problem.
	 */
	if (head == aq->cq_head && phase == aq->cq_phase)
		return 0;

	writel(head, aq->q_db + aq->sdev->db_stride);
	aq->cq_head = head;
	aq->cq_phase = phase;

	aq->cqe_seen = 1;
	return 1;
}


/*****************************************************************************
Function    : admin_irq
Description : administrate irq
Input       : int irq
              void * data
Output      : irqreturn_t
Return      : irqreturn_t
*****************************************************************************/
static irqreturn_t admin_irq(int irq, void *data)
{
	irqreturn_t result;
	unsigned long flags;
	sdi_admin_queue_t *aq = (sdi_admin_queue_t *)data;

	UNREFERENCE_PARAM(irq);

	spin_lock_irqsave(&aq->q_lock, flags);
	(void)sdi_process_cq(aq);
	result = aq->cqe_seen ? IRQ_HANDLED : IRQ_NONE;
	aq->cqe_seen = 0;
	spin_unlock_irqrestore(&aq->q_lock, flags);

	return result;
}

/*****************************************************************************
Function    : admin_irq_check
Description : check admin irq
Input       : int irq
              void * data
Output      : irqreturn_t
Return      : irqreturn_t
*****************************************************************************/
static irqreturn_t admin_irq_check(int irq, void *data)
{
	sdi_admin_queue_t *aq = (sdi_admin_queue_t *)data;
	struct sdi_completion cqe = aq->cqes[aq->cq_head];

	UNREFERENCE_PARAM(irq);

	if ((le16_to_cpu(cqe.status) & 1) != aq->cq_phase)
		return IRQ_NONE;
	return IRQ_WAKE_THREAD;
}


#ifdef HEART_BEAT_CHECK
/*****************************************************************************
Function    : sdi_service_task
Description : service task
Input       : struct work_struct * work
Output      : void
Return      : void
*****************************************************************************/
static void sdi_service_task(struct work_struct *work)
{
	struct sdi_pdev_info *spdev = container_of(work,
			struct sdi_pdev_info, service_task);
	struct sdi_admin_command cmd;
	int result, status;
	unsigned long next_event_offset;

	if (!test_bit(SDI_EP_READY, &spdev->state))
		return;

	memset((void *)&cmd, 0, sizeof(struct sdi_admin_command));

	cmd.service_cmd.opcode = sdi_admin_service_task_cmd;

	status = sdi_submit_admin_cmd(spdev, &cmd, &result, 2 * HZ);

	if (!status) {
		next_event_offset = HZ;
		spdev->service_phase = SDI_SERVICE_TASK_PHASE0;
	} else if (status == -EINTR) {
		switch (spdev->service_phase) {
		case SDI_SERVICE_TASK_PHASE0:
			next_event_offset = HZ * 2;
			spdev->service_phase = SDI_SERVICE_TASK_PHASE1;
			break;
		case SDI_SERVICE_TASK_PHASE1:
			next_event_offset = HZ * 4;
			spdev->service_phase = SDI_SERVICE_TASK_PHASE2;
			break;
		case SDI_SERVICE_TASK_PHASE2:
			spdev->service_phase = SDI_SERVICE_TASK_PHASE3;
			break;
		default:
			return;
		}
	} else {
		epfront_warn("%s send heart beat command failed.", spdev->name);
		next_event_offset = 4 * HZ;
		spdev->service_phase = SDI_SERVICE_TASK_PHASE0;
	}

	if (SDI_SERVICE_TASK_PHASE3 == spdev->service_phase)
		epfront_err("Device %s heart beat timeout.", spdev->name);

	if (test_bit(SDI_EP_READY, &spdev->state) && 
	    spdev->uep_base && spdev->uep_base->event_handler_call) {
		spdev->uep_base->event_handler_call(SDI_EP_LINK_EVT, 
			spdev->uep_base->priv_data, SDI_EP_LINK_DOWN);
		return;
	}

	mod_timer(&spdev->service_timer, next_event_offset + jiffies);
}


/*****************************************************************************
Function    : sdi_service_timer
Description : service timer
Input       : unsigned long data
Output      : void
Return      : void
*****************************************************************************/
static void sdi_service_timer(unsigned long data)
{
	struct sdi_pdev_info *spdev = (struct sdi_pdev_info *)data;

	if(test_bit(SDI_EP_READY, &spdev->state))
		schedule_work(&spdev->service_task);
}
#endif


/*****************************************************************************
Function    : sdi_pf12_wait_ready
Description : wait pf12 ready
Input       : struct sdi_pdev_info * sdev
              u64 cap
              bool enabled
Output      : int
Return      : int
*****************************************************************************/
static int sdi_pf12_wait_ready(struct sdi_pdev_info *sdev, u64 cap,
							   bool enabled)
{
	unsigned long timeout;
	u32 bit = enabled ? SDI_PF12_CSTS_RDY : 0;
	timeout = ((SDI_PF12_CAP_TIMEOUT(cap) + 1) * HZ / 2);
	if (timeout > WAIT_READY_TIMEOUT)
		timeout = WAIT_READY_TIMEOUT;
	timeout += jiffies;

	while ((sdev->bar) && (readl(&sdev->bar->csts) & SDI_PF12_CSTS_RDY) != bit) {
		msleep(100);
		if (fatal_signal_pending(current)) {
			epfront_err("Device %s get fatal signal pending.", sdev->name);
			return -EINTR;
		}

		if (time_after(jiffies, timeout)) {
			epfront_err("Device %s not ready, aborting %s",
					sdev->name, enabled ? "initialization" : "reset");
			return -ENODEV;
		}
	}
	if (!sdev->bar)
		return -ENODEV;

	return 0;
}

/*****************************************************************************
Function    : sdi_pf12_disable_ctrl
Description : disable pf12 control
Input       : struct sdi_pdev_info * sdev
              u64 cap
Output      : int
Return      : int
*****************************************************************************/
/* If the device has been passed off to us in an enabled state, just clear
 * the enabled bit.  The spec says we should set the 'shutdown notification
 * bits', but doing so may cause the device to complete commands to the
 * admin queue ... and we don't know what memory that might be pointing at!
 */
static int sdi_pf12_disable_ctrl(struct sdi_pdev_info *sdev, u64 cap)
{
	sdev->ctrl_config &= ~SDI_PF12_CC_SHN_MASK;
	sdev->ctrl_config &= ~SDI_PF12_CC_ENABLE;
	writel(sdev->ctrl_config, &sdev->bar->cc);

	return sdi_pf12_wait_ready(sdev, cap, (bool)false);
}


/*****************************************************************************
Function    : sdi_pf12_enable_ctrl
Description : enbale pf12 control
Input       : struct sdi_pdev_info * sdev
              u64 cap
Output      : int
Return      : int
*****************************************************************************/
static int sdi_pf12_enable_ctrl(struct sdi_pdev_info *sdev, u64 cap)
{
	sdev->ctrl_config &= ~SDI_PF12_CC_SHN_MASK;
	sdev->ctrl_config |= SDI_PF12_CC_ENABLE;
	writel(sdev->ctrl_config, &sdev->bar->cc);

	return sdi_pf12_wait_ready(sdev, cap, (bool)true);
}


/*****************************************************************************
Function    : admin_queue_request_irq
Description : request irq for admin queue
Input       : struct sdi_pdev_info * sdev
              sdi_admin_queue_t * admin_queue
              const char * name
Output      : int
Return      : int
*****************************************************************************/
static int admin_queue_request_irq(struct sdi_pdev_info *sdev,
								   sdi_admin_queue_t *admin_queue, const char *name)
{
	if (sdev->use_threaded_interrupts)
		return request_threaded_irq(sdev->entry[admin_queue->cq_vector].vector,
				admin_irq_check, admin_irq, (unsigned long)IRQF_SHARED,
				name, admin_queue);
	return request_irq(sdev->entry[admin_queue->cq_vector].vector, admin_irq,
				(unsigned long)IRQF_SHARED, name, admin_queue);
}

/*****************************************************************************
Function    : sdi_admin_queue_extra
Description : get admin queue extra information's pointer
Input       : u32 depth
Output      : unsigned
Return      : unsigned
*****************************************************************************/
/* cmdid_data[]:
 *--------------------------------------------------
 * |--  unsigned long[]  --|--   sdi_cmd_info[]  --|
 * |bit0|bit1|bit2|...|bitN|--   sdi_cmd_info[]  --|
 * |idx0|idx1|idx2|...|idxN|cmd0|cmd1|cmd2|...|cmdN|
 *--------------------------------------------------
 */
static unsigned sdi_admin_queue_extra(u32 depth)
{
	return DIV_ROUND_UP(depth, sizeof(unsigned long)) + (depth * sizeof(struct sdi_cmd_info));
}

/*****************************************************************************
Function    : sdi_alloc_admin_queue
Description : alloc admin queue
Input       : struct sdi_pdev_info * sdev
              u32 depth
              int vector
Output      : sdi_admin_queue_t *
Return      : sdi_admin_queue_t *
*****************************************************************************/
static sdi_admin_queue_t *sdi_alloc_admin_queue(struct sdi_pdev_info *sdev,
												u32 depth, int vector)
{
	sdi_admin_queue_t *admin_queue;
	struct device *dmadev = &sdev->pdev->dev;
	unsigned extra = sdi_admin_queue_extra(depth);
	int offset = 0;

	admin_queue = (sdi_admin_queue_t *)kzalloc(sizeof(sdi_admin_queue_t) + extra, GFP_KERNEL);
	if (!admin_queue) {
		epfront_err("Allocate memory failed.");
		return NULL;
	}

	admin_queue->cq_size = sizeof(struct sdi_completion) * depth;
	admin_queue->sq_size = sizeof(struct sdi_admin_command) * depth;

	admin_queue->queue_size = admin_queue->cq_size + admin_queue->sq_size + (u32)PAGE_SIZE;
	admin_queue->addr = dma_alloc_coherent(dmadev, admin_queue->queue_size,
										   &admin_queue->dma_addr, GFP_KERNEL);
	if (!admin_queue->addr) {
		epfront_err("Allocate memory failed.");
		goto free_adminq;
	}

	offset = (int)(((admin_queue->cq_size + PAGE_SIZE - 1) >> PAGE_SHIFT )<< PAGE_SHIFT);

	admin_queue->cqes = admin_queue->addr;
	admin_queue->cq_dma_addr = admin_queue->dma_addr;

	admin_queue->sq_dma_addr = admin_queue->dma_addr + offset;
	admin_queue->sq_cmds = (struct sdi_admin_command*)((unsigned char*)admin_queue->addr + offset);
	
	memset((void*)admin_queue->cqes, 0, admin_queue->cq_size);
	memset((void*)admin_queue->sq_cmds, 0, admin_queue->sq_size);

	admin_queue->q_dmadev = dmadev;
	admin_queue->sdev = sdev;
	(void)snprintf(admin_queue->irqname, sizeof(admin_queue->irqname), "sdi_pf1_aq");

	spin_lock_init(&admin_queue->q_lock);
	spin_lock_init(&admin_queue->uaen_lock);
	init_waitqueue_head(&admin_queue->sq_full);

	admin_queue->cq_head = 0;
	admin_queue->cq_phase = 1;
	admin_queue->q_db = &sdev->dbs[0 * 2 * sdev->db_stride];
	admin_queue->q_depth = (u16)depth;
	admin_queue->cq_vector = (u16)vector;
	admin_queue->qid = 0;
	admin_queue->q_suspended = 0;

	return admin_queue;

free_adminq:
	kfree(admin_queue);

	return NULL;
}


/*****************************************************************************
Function    : sdi_configure_admin_queue
Description : configure admin queue
Input       : struct sdi_pdev_info * sdev
Output      : int
Return      : int
*****************************************************************************/
static int sdi_configure_admin_queue(struct sdi_pdev_info *sdev)
{
	int result;
	u32 aqa;
	u64 cap = readq(&sdev->bar->cap);

	BUG_ON(!sdev);
	BUG_ON(sdev->admin_queue);

	result = sdi_pf12_disable_ctrl(sdev, cap);
	if (result < 0) {
		epfront_err("%s disable pf12 ctrl failed.", sdev->name);
		return result;
	}

	msleep(500);

	sdev->admin_queue = sdi_alloc_admin_queue(sdev, SDI_AQ_DEPTH, 0);
	if (!sdev->admin_queue)
		return -ENOMEM;

	aqa = sdev->admin_queue->q_depth - 1;
	aqa |= aqa << 16;

	sdev->ctrl_config = SDI_PF12_CC_CSS_NVM;
	sdev->ctrl_config |= (PAGE_SHIFT - 12) << SDI_PF12_CC_MPS_SHIFT;
	sdev->ctrl_config |= SDI_PF12_CC_ARB_RR | SDI_PF12_CC_SHN_NONE;
	sdev->ctrl_config |= SDI_PF12_CC_IOSQES | SDI_PF12_CC_IOCQES;

	writel(aqa, &sdev->bar->aqa);
	writeq(sdev->admin_queue->cq_dma_addr, &sdev->bar->acq);
	writeq(sdev->admin_queue->sq_dma_addr, &sdev->bar->asq);	

	result = sdi_pf12_enable_ctrl(sdev, cap);
	if (result) {
		epfront_err("%s enable ctrl failed.", sdev->name);
		goto destroy_admin_queue;
	}

	result = admin_queue_request_irq(sdev, sdev->admin_queue, sdev->admin_queue->irqname);
	if (result) {
		epfront_err("request admin irq: %d failed.",result);
		goto destroy_admin_queue;
	}

	memset((void *)sdev->qids, 0, sizeof(unsigned long) * BITS_TO_LONGS(SDI_PF12_MAX_SQ_NR));

	set_bit(0, sdev->qids);        // set for Admin queue
	return 0;

destroy_admin_queue:
	sdi_aq_remove(sdev);
	return result;
}


/*****************************************************************************
Function    : sdi_dev_unmap
Description : unmap device
Input       : struct sdi_pdev_info * sdev
Output      : void
Return      : void
*****************************************************************************/
static void sdi_dev_unmap(struct sdi_pdev_info *sdev)
{
	if (sdev->pdev->msix_enabled) {
		pci_disable_msix(sdev->pdev);
		epfront_info("disable msix");
	}

	if (sdev->bar) {
		iounmap(sdev->bar);
		sdev->bar = NULL;
		pci_release_regions(sdev->pdev);
		epfront_info("unmap bar");
	}

	if (pci_is_enabled(sdev->pdev)) {
		pci_disable_device(sdev->pdev);
		epfront_info("disable device");
	}
}

/*****************************************************************************
Function    : check_dev_map
Description : check device mapping status
Input       : struct sdi_pdev_info * sdev
Output      : int
Return      : int
*****************************************************************************/
static int check_dev_map(struct sdi_pdev_info *sdev)
{
	unsigned int wait_times = 0;
	unsigned int csts = 0;

	do {
		if (!sdev->bar)
			break;
		csts = readl(&sdev->bar->csts);
		csts = (csts >> 8) & 0xff;
		if((wait_times++) >= WAIT_COMPLETE_NUM) {
			epfront_err("%s csts 0x%x arm sdi driver may not be ready.", sdev->name, csts);
			break;
		}
		msleep(100);
	} while((csts == 0xff) || (csts & 0x01) != 0x01);
		
	return ((csts == 0xff) || (csts & 0x01) != 0x01) ? -ENODEV : 0;
}

/*****************************************************************************
Function    : sdi_dev_map
Description : map device
Input       : struct sdi_pdev_info * sdev
Output      : int
Return      : int
*****************************************************************************/
static int sdi_dev_map(struct sdi_pdev_info *sdev)
{
	u64 cap;
	int bars, i, vector_threshold, vectors, result = -ENOMEM;
	
	struct pci_dev *pdev = sdev->pdev;

	if (pci_enable_device_mem(pdev)) {
		epfront_err("%s pci device enable memory failed.", sdev->name);
		return result;
	}

	pci_set_master(pdev);
	bars = pci_select_bars(pdev, (unsigned long)IORESOURCE_MEM);
	if (pci_request_selected_regions(pdev, bars, "sdi_pf12")) {
		epfront_err("%s pci device request regions failed.", sdev->name);
		goto disable_pci;
	}

	if (!dma_set_mask(&pdev->dev, DMA_BIT_MASK(64))) {
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 32) || defined(RHEL_RELEASE))
		(void)dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(64));
#else
		pdev->dev.coherent_dma_mask = DMA_BIT_MASK(64);
#endif
	} else if (!dma_set_mask(&pdev->dev, DMA_BIT_MASK(32))) {
		epfront_warn("%s pci device set mask 64 failed.", sdev->name);
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 32) || defined(RHEL_RELEASE))
		(void)dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
#else
		pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
#endif
	} else {
		epfront_err("%s pci device set mask failed.", sdev->name);
		goto disable;
	}

	sdev->bar = (struct pf12_bar __iomem *)ioremap(pci_resource_start(pdev, 0), (unsigned long)PF12_BAR0_SIZE);
	if (!sdev->bar) {
		epfront_err("%s pci device io remap failed.", sdev->name);
		goto disable;
	}

	if (check_dev_map(sdev)) {
		epfront_err("%s read drbl failed, hardware may not be ready.", sdev->name);
		result = -ENODEV;
		goto unmap;
	}

	cap = readq(&sdev->bar->cap);
	sdev->max_depth = min_t(int, SDI_PF12_CAP_MQES(cap) + 1, SDI_Q_MAX_DEPTH);
	sdev->db_stride = 1 << SDI_PF12_CAP_STRIDE(cap);
	sdev->dbs = (u32 __iomem *)((unsigned char*)sdev->bar + DOORBELL_OFFSET);

	for (i = 0; i < SDI_PF12_MAX_CQ_NR; i++)
		sdev->entry[i].entry = (u16)i;

	vector_threshold = SDI_MIN_MSIX_COUNT;
	vectors = SDI_PF12_MAX_CQ_NR;

	while (vectors >= vector_threshold) {
		result = pci_enable_msix(sdev->pdev, sdev->entry, vectors);
		if (!result)     /* Success in acquiring all requested vectors. */
			break;
		else if (result < 0)
			vectors = 0; /* Nasty failure, quit now */
		else             /* err == number of vectors we should try again with */
			vectors = result;
	}

	if (vectors < vector_threshold) {
		epfront_err("%s pci device enable msix failed.", sdev->name);
		result = -ENOMEM;
		goto unmap;
	}

	sdev->max_io_cq_nr = (u16)(vectors - 1); /*one used by admin queue*/

	return 0;
unmap:
	iounmap(sdev->bar);
	sdev->bar = NULL;
disable:
	pci_release_regions(pdev);
disable_pci:
	pci_disable_device(pdev);

	return result;
}


/*****************************************************************************
Function    : sdi_aq_remove
Description : remove aq
Input       : struct sdi_pdev_info * sdev
Output      : void
Return      : void
*****************************************************************************/
static void sdi_aq_remove (struct sdi_pdev_info *sdev)
{
	sdi_admin_queue_t *admin_queue;
	struct device *dmadev = &sdev->pdev->dev;

	admin_queue = sdev->admin_queue;
	if (!admin_queue) {
		epfront_warn("%s delete AQ which is not exist.", sdev->name);
		return;
	}

	if (!admin_queue->addr)
		epfront_warn("admin queue buff not exist");
	else
		dma_free_coherent(dmadev,admin_queue->queue_size,
			admin_queue->addr,admin_queue->dma_addr);

	kfree(admin_queue);
	sdev->admin_queue = NULL;
}


/*****************************************************************************
Function    : sdi_shutdown_ctrl
Description : shutdown control
Input       : struct sdi_pdev_info * sdev
Output      : int
Return      : int
*****************************************************************************/
static int sdi_shutdown_ctrl(struct sdi_pdev_info *sdev)
{
	unsigned long timeout;

	sdev->ctrl_config &= ~SDI_PF12_CC_SHN_MASK;
	sdev->ctrl_config |= SDI_PF12_CC_SHN_NORMAL;

	writel(sdev->ctrl_config, &sdev->bar->cc);

	timeout = SHUTDOWN_TIMEOUT + jiffies;
	while ((readl(&sdev->bar->csts) & SDI_PF12_CSTS_SHST_MASK)
		   != SDI_PF12_CSTS_SHST_CMPLT) {
		msleep(100);
		if (fatal_signal_pending(current))
			return -EINTR;
		if (time_after(jiffies, timeout)){
			epfront_warn("Device %s shutdown incomplete; abort shutdown",
					 sdev->name);
			return -ENODEV;
		}
	}

	return 0;
}


/*****************************************************************************
Function    : sdi_suspend_admin_queue
Description : suspend aq
Input       : sdi_admin_queue_t * admin_queue
Output      : int
Return      : int
*****************************************************************************/
static int sdi_suspend_admin_queue(sdi_admin_queue_t *admin_queue)
{
	unsigned long flags;
	int vector = admin_queue->cq_vector;

	spin_lock_irqsave(&admin_queue->q_lock, flags);
	if (admin_queue->q_suspended) {
		spin_unlock_irqrestore(&admin_queue->q_lock, flags);
		return 1;
	}

	admin_queue->q_suspended = 1;
	spin_unlock_irqrestore(&admin_queue->q_lock, flags);

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 32) || defined(RHEL_RELEASE))
	(void)irq_set_affinity_hint(admin_queue->sdev->entry[vector].vector, NULL);
#endif

	free_irq(admin_queue->sdev->entry[vector].vector, admin_queue);

	return 0;
}

/*****************************************************************************
Function    : sdi_clear_admin_queue
Description : clear aq
Input       : sdi_admin_queue_t * admin_queue
Output      : void
Return      : void
*****************************************************************************/
static void sdi_clear_admin_queue(sdi_admin_queue_t *admin_queue)
{
	unsigned long flags;

	spin_lock_irqsave(&admin_queue->q_lock, flags);
	(void)sdi_process_cq(admin_queue);
	sdi_admin_cancel_ios(admin_queue, (bool)false);
	spin_unlock_irqrestore(&admin_queue->q_lock, flags);
}


/*****************************************************************************
Function    : sdi_disable_admin_queue
Description : disable aq
Input       : sdi_admin_queue_t * admin_queue
Output      : void
Return      : void
*****************************************************************************/
static void sdi_disable_admin_queue(sdi_admin_queue_t *admin_queue)
{
	if (sdi_suspend_admin_queue(admin_queue))
		return;
	sdi_clear_admin_queue(admin_queue);
}

/*****************************************************************************
Function    : shutdown_notify
Description : shundown notify method
Input       : struct sdi_pdev_info * sdev
Output      : int
Return      : int
*****************************************************************************/
static int shutdown_notify(struct sdi_pdev_info *sdev){
	struct sdi_admin_command c;
	int result = 0;

	memset((void *)(&c), 0, sizeof(c));
	c.shutdown.opcode = sdi_admin_shutdown;
	c.shutdown.sid = system_state;
	(void)sdi_submit_admin_cmd(sdev, &c, &result, SDI_CARD_SHUTDOWN_TIMEOUT);
	return result;
}

/*****************************************************************************
Function    : sdi_dev_shutdown
Description : shutdown device method
Input       : struct sdi_pdev_info * sdev
Output      : void
Return      : void
*****************************************************************************/
static void sdi_dev_shutdown(struct sdi_pdev_info *sdev)
{
	int csts = -1;
	int ret;
	sdi_admin_queue_t *admin_queue;

	if (test_bit(SDI_EP_STARTING, &sdev->state) ||
	    test_and_set_bit(SDI_EP_STOPPING, &sdev->state)) {
		epfront_dbg("%s shutdown is abort as already or being.", sdev->name);
		return;
	}

	epfront_info("sys state %d",system_state);
	
	heartbeat_thread_remove();		//stop heartbeat thread
	ret = shutdown_notify(sdev);
	if(unlikely(ret)){
		epfront_err("wait sdi card shutdown timeout,err %d ",ret);
	} else {
		epfront_info("wait sdi shutdown");
		msleep(WAIT_SDI_CARD_SHUTDOWN_TIME);
	}

	gdev_list_remove();

	admin_queue = sdev->admin_queue;
	if (!admin_queue) {
		clear_bit(SDI_EP_STOPPING, &sdev->state);
		return;
	}

	if (sdev->bar)
		csts = (int)readl(&sdev->bar->csts);

	if (csts & SDI_PF12_CSTS_CFS || !(csts & SDI_PF12_CSTS_RDY)) {
		(void)sdi_suspend_admin_queue(sdev->admin_queue); // no need that rcu lock is ok
	} else {
		(void)sdi_shutdown_ctrl(sdev);
		sdi_disable_admin_queue(sdev->admin_queue);
	}

	sdi_aq_remove(sdev);
	sdi_dev_unmap(sdev);

	clear_bit(SDI_EP_STOPPING, &sdev->state);
}


/*****************************************************************************
Function    : sdi_dev_start
Description : start device
Input       : struct sdi_pdev_info * sdev
Output      : int
Return      : int
*****************************************************************************/
int sdi_dev_start(struct sdi_pdev_info *sdev)
{
	int result;

	if (test_bit(SDI_EP_READY, &sdev->state) ||
	    test_bit(SDI_EP_STOPPING, &sdev->state) ||
	    test_and_set_bit(SDI_EP_STARTING, &sdev->state)) {
		epfront_dbg("%s start is abort.", sdev->name);
		return -EBUSY;
	}

	result = sdi_dev_map(sdev);
	if (result) {
		epfront_err("sdi_dev_map fail.");
		goto clr_bit;
	}

	result = sdi_configure_admin_queue(sdev);
	if (result) {
		epfront_err("sdi_configure_admin_queue fail.");
		goto ummap_dev;
	}

	result = gdev_list_add();
	if (result) {
		epfront_err("gdev_list_add fail.");
		goto disable_aq;
	}
	result = heartbeat_thread_add();
	if (result) {
		epfront_err("heartbeat_thread_add fail.");
		goto disable_aq;
	}

	clear_bit(SDI_EP_STARTING, &sdev->state);
	set_bit(SDI_EP_READY, &sdev->state);

	return 0;

disable_aq:
	sdi_disable_admin_queue(sdev->admin_queue);
	sdi_aq_remove(sdev);
ummap_dev:
	sdi_dev_unmap(sdev);
clr_bit:
	clear_bit(SDI_EP_STARTING, &sdev->state);

	return result;
}


/*****************************************************************************
Function    : sdi_admin_cancel_ios
Description : cancel admin io
Input       : sdi_admin_queue_t * admin_queue
              bool timeout
Output      : void
Return      : void
*****************************************************************************/
static void sdi_admin_cancel_ios(sdi_admin_queue_t *admin_queue, bool timeout)
{
	u16 depth = admin_queue->q_depth - 1;
	struct sdi_cmd_info *info = get_sdi_cmd_info(admin_queue);
	unsigned long now = jiffies;
	unsigned long cmdid;

	for_each_set_bit(cmdid, admin_queue->cmdid_data, (unsigned long)depth) {
		void *ctx;
		sdi_completion_fn fn;
		static struct sdi_completion cqe =
				{
					  .status = cpu_to_le16((u16)SDI_PF12_SC_ABORT_REQ) << 1,
				};

		if (REQ_CMD_NO_TIMEOUT == info[cmdid].flags)
			continue;

		if (REQ_CMD_TIMEOUT_ABORT == info[cmdid].flags)
			continue;

		if (timeout && !time_after(now, info[cmdid].timeout))
			continue;

		epfront_warn("%s Cancelling I/O %ld", admin_queue->sdev->name, cmdid);
		ctx = cancel_cmdid(admin_queue, (int)cmdid, &fn);
		fn(admin_queue, ctx, &cqe);
	}
}


/*****************************************************************************
Function    : sdi_kthread
Description : sdi cq process kthread
Input       : void * data
Output      : int
Return      : int
*****************************************************************************/
static int sdi_kthread(void *data)
{
	unsigned long flags;
	sdi_pdev_info_t *dev;
	sdi_admin_queue_t *admin_queue;

	UNREFERENCE_PARAM(data);

	while (!kthread_should_stop()) {
		set_current_state(TASK_RUNNING);

		spin_lock(&gdev_lock);
		dev = &gsdev;
		admin_queue = dev->admin_queue;
		if (admin_queue){
			spin_lock_irqsave(&dev->admin_queue->q_lock, flags);
			(void)sdi_process_cq(dev->admin_queue);
			sdi_admin_cancel_ios(dev->admin_queue, (bool)true);
			spin_unlock_irqrestore(&dev->admin_queue->q_lock, flags);
		}
		spin_unlock(&gdev_lock);

		set_current_state(TASK_INTERRUPTIBLE);
		(void)schedule_timeout((signed long)round_jiffies_relative((unsigned long)HZ));
	}

	return 0;
}


/*****************************************************************************
Function    : gdev_list_add
Description : add list to global variable "gdev" 
Input       : void
Output      : int
Return      : int
*****************************************************************************/
static int gdev_list_add(void)
{
	int result = 0;
	bool start_thread_tmp = false;

	spin_lock(&gdev_lock);
	if (!total_num && IS_ERR_OR_NULL(sdi_thread)) {
		start_thread_tmp = true;
		sdi_thread = NULL;
	}
	total_num++;
	spin_unlock(&gdev_lock);

	if (start_thread_tmp)
		sdi_thread = kthread_run(sdi_kthread, NULL, "sdi_pfs");

	if (IS_ERR_OR_NULL(sdi_thread)) {
		result = sdi_thread ? (int)PTR_ERR(sdi_thread) : -EINTR;
		epfront_err("Create SDI PFs thread failed.");
		goto disable;
	}

	return result;

disable:
	gdev_list_remove();

	return result;
}


/*****************************************************************************
Function    : gdev_list_remove
Description : remove list to global variable "gdev" 
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void gdev_list_remove(void)
{
	struct task_struct *tmp = NULL;

	spin_lock(&gdev_lock);
	if (total_num)
		total_num--;

	if (!total_num && !IS_ERR_OR_NULL(sdi_thread)) {
		tmp = sdi_thread;
		sdi_thread = NULL;
	}
	spin_unlock(&gdev_lock);

	if (tmp)
		(void)kthread_stop(tmp); // we should stop Admin management thread if no PF driver is running.
}

/*****************************************************************************
Function    : is_remove_front
Description : check frontend whether is removed
Input       : void
Output      : bool
Return      : bool
*****************************************************************************/
static bool is_remove_front(void)
{
	if (test_bit(SDI_FRONT_REMOVE, &demand_status))
		return false;
	return true;
}

/*****************************************************************************
Function    : heartbeat_thread_add
Description : add heartbead thread
Input       : void
Output      : int
Return      : int
*****************************************************************************/
static int heartbeat_thread_add(void)
{
	int result = 0;
	bool start_thread_tmp = false;
	sdi_pdev_info_t *spdev = &gsdev;
	unsigned int try_num = 0;

	if (IS_ERR_OR_NULL(spdev)) {
		epfront_err("spdev is null.");
		return  -ENODEV;
	}

	do {
		spin_lock(&heartbeat_lock);
		if (IS_ERR_OR_NULL(heartbeat_thread)) {
			start_thread_tmp = is_remove_front();
			heartbeat_thread = NULL;	
		}
		spin_unlock(&heartbeat_lock);

		if (start_thread_tmp)
			heartbeat_thread = kthread_run(heartbeat_kthread, spdev, "heartbeat");

		if (IS_ERR_OR_NULL(heartbeat_thread)) {
			result = heartbeat_thread ? (int)PTR_ERR(heartbeat_thread) : -EINTR;
			epfront_err("Create heartbeat thread failed.");
			if ((try_num++) >= HEATBEAT_TIMEOUT_NUM)
				goto disable;
		} else {
			break;
		}
	}while (try_num);

	return result;

disable:
	spin_lock(&heartbeat_lock);
	heartbeat_thread = NULL;
	spin_unlock(&heartbeat_lock);
	return result;
}

/*****************************************************************************
Function    : heartbeat_thread_remove
Description : remove heartbead thread
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void heartbeat_thread_remove(void)
{
	struct task_struct *tmp = NULL;

	spin_lock(&heartbeat_lock);

	if (!IS_ERR_OR_NULL(heartbeat_thread)) {
		tmp = heartbeat_thread;
		heartbeat_thread = NULL;
	}
	spin_unlock(&heartbeat_lock);

	if (tmp)
		(void)kthread_stop(tmp);
}

/*****************************************************************************
Function    : update_thread_add
Description : add update thread
Input       : void
Output      : int
Return      : int
*****************************************************************************/
static int update_thread_add(void)
{
	int result = 0;
	bool start_thread_tmp = false;
	sdi_pdev_info_t *spdev = &gsdev;
	unsigned int try_num = 0;

	if (IS_ERR_OR_NULL(spdev)) {
		epfront_err("spdev is null.");
		return  -ENODEV;
	}

	do {
		spin_lock(&update_lock);
		if (IS_ERR_OR_NULL(update_thread)) {
			start_thread_tmp = is_remove_front();
			update_thread = NULL;
		}
		spin_unlock(&update_lock);

		if (start_thread_tmp)
			update_thread = kthread_run(update_kthread, spdev, "update");

		if (IS_ERR_OR_NULL(update_thread)) {
			result = update_thread ? (int)PTR_ERR(update_thread) : -EINTR;
			epfront_err("Create update thread failed.");
			if ((try_num++) >= HEATBEAT_TIMEOUT_NUM)
				goto disable;
		} else {
			break;
		}
	} while(try_num);

	return result;

disable:
	spin_lock(&update_lock);
	update_thread = NULL;
	spin_unlock(&update_lock);
	return result;
}

/*****************************************************************************
Function    : update_thread_remove
Description : remove update thread
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void update_thread_remove(void)
{
	struct task_struct *tmp = NULL;

	spin_lock(&update_lock);
	if (!IS_ERR_OR_NULL(update_thread)) {
		tmp = update_thread;
		update_thread = NULL;
	}
	spin_unlock(&update_lock);

	if (tmp)
		(void)kthread_stop(tmp);
}

/*****************************************************************************
Function    : reset_thread_add
Description : reset add thread
Input       : void
Output      : int
Return      : int
*****************************************************************************/
static int reset_thread_add(void)
{
	int result = 0;
	bool start_thread_tmp = false;
	sdi_pdev_info_t *spdev = &gsdev;
	unsigned int try_num = 0;

	if (IS_ERR_OR_NULL(spdev)) {
		epfront_err("spdev is null.");
		return  -ENODEV;
	}

	do {
		spin_lock(&perceiv_reset_lock);
		if (IS_ERR_OR_NULL(reset_thread)) {
			start_thread_tmp = is_remove_front();
			reset_thread = NULL;
		}
		spin_unlock(&perceiv_reset_lock);

		if (start_thread_tmp)
			reset_thread = kthread_run(reset_kthread, spdev, "reset");

		if (IS_ERR_OR_NULL(reset_thread)) {
			result = reset_thread ? (int)PTR_ERR(reset_thread) : -EINTR;
			epfront_err("Create reset thread failed.");
			if ((try_num++) >= HEATBEAT_TIMEOUT_NUM)
				goto disable;
		} else {
			break;
		}
	} while(try_num);

	return result;

disable:
	spin_lock(&perceiv_reset_lock);
	reset_thread = NULL;
	spin_unlock(&perceiv_reset_lock);
	return result;
}

/*****************************************************************************
Function    : reset_thread_remove
Description : reset remove thread
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void reset_thread_remove(void)
{
	struct task_struct *tmp = NULL;

	spin_lock(&perceiv_reset_lock);
	if (!IS_ERR_OR_NULL(reset_thread)) {
		tmp = reset_thread;
		reset_thread = NULL;
	}
	spin_unlock(&perceiv_reset_lock);

	if (tmp)
		(void)kthread_stop(tmp);
}

/*****************************************************************************
Function    : _pcie_get_mps
Description : get pcie mps
Input       : struct pci_dev * dev
Output      : int
Return      : int
*****************************************************************************/
static int _pcie_get_mps(struct pci_dev *dev)
{
	u16 ctl;

	(void)pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &ctl);

	return 128 << ((ctl & PCI_EXP_DEVCTL_PAYLOAD) >> 5);
}

/*****************************************************************************
Function    : _pcie_get_max_mps
Description : get pcie max mps
Input       : struct pci_dev * dev
Output      : int
Return      : int
*****************************************************************************/
static int _pcie_get_max_mps(struct pci_dev *dev)
{
	u16 ctl;

	(void)pcie_capability_read_word(dev, PCI_EXP_DEVCAP, &ctl);

	return 128 << ((ctl & PCI_EXP_DEVCAP_PAYLOAD) >> 0);
}

/*****************************************************************************
Function    : epfront_is_power_of_2
Description : is power of 2
Input       : int mps
Output      : bool
Return      : bool
*****************************************************************************/
static bool epfront_is_power_of_2(int mps){
	return ((mps) != 0 && (((mps) & ((mps) - 1)) == 0));
}

/*****************************************************************************
Function    : _pcie_set_mps
Description : set pcie mps
Input       : struct pci_dev * dev
              int mps
Output      : int
Return      : int
*****************************************************************************/
static int _pcie_set_mps(struct pci_dev *dev, int mps)
{
	u16 v;

	if (mps < 128 || mps > 4096 || !epfront_is_power_of_2(mps))
		return -EINVAL;

	v = ffs(mps) - 8;
	//no need to check the validation of 'v', because it's already ensured by enum

	v <<= 5;

	return pcie_capability_clear_and_set_word(dev, PCI_EXP_DEVCTL,
											  PCI_EXP_DEVCTL_PAYLOAD, v);
}


/*****************************************************************************
Function    : bridge_linkset
Description : pci bridge linkset
Input       : struct pci_dev * pdev
              u32 disable
Output      : int
Return      : int
*****************************************************************************/
static int bridge_linkset(struct pci_dev *pdev, u32 disable){
	int cap_ptr;
	u32 reg = 0;

	cap_ptr = pci_find_capability(pdev, PCI_CAP_ID_EXP);
	if (!cap_ptr) {
		epfront_err("can't find link ctrl capablities");
		return -ENODEV;
	}

	(void)pci_read_config_dword(pdev, cap_ptr + PCI_EXP_LNKCTL, &reg);
	epfront_info("link ctrl reg %x",reg);

	if (disable)
		reg |= PCI_EXP_LNKCTL_LD;
	else
		reg = reg & ~PCI_EXP_LNKCTL_LD;

	epfront_info("link ctrl reg%x", reg);
	(void)pci_write_config_dword(pdev, cap_ptr + PCI_EXP_LNKCTL, reg);
	return 0;
}

/*****************************************************************************
Function    : bridge_linkdown
Description : pci bridge linkdown
Input       : struct pci_dev * pdev
Output      : int
Return      : int
*****************************************************************************/
static int bridge_linkdown(struct pci_dev *pdev)
{
	epfront_set_linkdown();
	return bridge_linkset(pdev,1);
}

/*****************************************************************************
Function    : linkdown_restore
Description : pci linkdown status restore
Input       : int pf
Output      : int
Return      : int
*****************************************************************************/
static int linkdown_restore(int pf){
	struct pci_dev *pdev= NULL, *parent = NULL;
	u16 parent_mps, mps, parent_mrrs, mrrs;
	u32 bar01,bar23,bar45;
	u32 read01 = 0,read23 = 0,read45 = 0;
	epfront_info("start test thread, restore pf %d", pf);

	pdev = pci_get_subsys(SDI_VENDOR_ID, SDI_DEVICE_ID_1610, PCI_ANY_ID, pf, NULL);
	if (!pdev) {
		epfront_err("get pdev error");
		return -ENODEV;
	}

	parent = pdev->bus->self;
	if (!parent) {
		epfront_err("get bus pdev error");
		pci_dev_put(pdev);
		return -ENODEV;
	}

	bar01 = (u32)pci_resource_start(pdev, 0);
	bar23 = (u32)pci_resource_start(pdev, 2);
	bar45 = (u32)pci_resource_start(pdev, 4);
	(void)pci_write_config_dword(pdev, 0x10, bar01);
	(void)pci_write_config_dword(pdev, 0x18, bar23);
	(void)pci_write_config_dword(pdev, 0x20, bar45);
	(void)pci_read_config_dword(pdev, 0x10, &read01);
	(void)pci_read_config_dword(pdev, 0x18, &read23);
	(void)pci_read_config_dword(pdev, 0x20, &read45);
	epfront_info("bar0 0x%x, bar2 0x%x, bar4 0x%x", read01,read23,read45);

	parent_mrrs = pcie_get_readrq(parent);
	parent_mps = _pcie_get_mps(parent);
	mrrs = pcie_get_readrq(pdev);
	mps = _pcie_get_max_mps(pdev);
	epfront_info("parent mps %d, mrrs %d, pdev mps %d, mrrs %d", parent_mps, parent_mrrs, mps, mrrs);
	mps = parent_mps;
	epfront_info("set pdev mps as %d, mrrs as %d", mps, mrrs);
	_pcie_set_mps(pdev, mps);

	pci_dev_put(pdev);
	return 0;
}

/*****************************************************************************
Function    : pcie_link_check
Description : check pcie link status
Input       : void
Output      : int
Return      : int
*****************************************************************************/
static int pcie_link_check(void)
{
	struct pci_dev *pf0 = NULL;
	u32 val = 0;
	int time_stamp = 0;

	pf0 = pci_get_subsys(SDI_VENDOR_ID, SDI_DEVICE_ID_1610, PCI_ANY_ID, 0, NULL);

	if (unlikely(!pf0)) {
		epfront_err("find pf failed");
		return -ENODEV;
	}

	do {
		(void)pci_read_config_dword(pf0, 0, &val);
		msleep(CHECK_LINK_WAITTIME);
		val &= LINK_CHECK_STATE;
		if((time_stamp++) >= CHECK_LINK_TIMEOUT_NUM){
			epfront_err("reg val %x",val);
			break;
		}
	} while (val == LINK_CHECK_STATE);

	pci_dev_put(pf0);

	return val == LINK_CHECK_STATE ;
}

/*****************************************************************************
Function    : linkdown_reinit
Description : reinit linkdown
Input       : void
Output      : int
Return      : int
*****************************************************************************/
int linkdown_reinit(void){

	const int pf_nr = 3;
	struct sdi_pdev_info *sdev =  &gsdev;
	int i;

	epfront_info("reinit now..");
	if (!sdev || !sdev->pdev) {
		epfront_err("dev not exist");
		return  -ENODEV;
	}

	if (bridge_linkset(sdev->pdev->bus->self, 0)) {
		epfront_err("bridge linkup set failed");
		return -ENODEV;
	}
	epfront_info("bridge linkup success...");

	if (pcie_link_check()) {
		epfront_err("link check fail.");
		goto err_out;
	}
	epfront_info("link check success");

	for (i = 0; i< pf_nr; i++) {
		if (unlikely(linkdown_restore(i))) {
			epfront_err("link restore failed");
			goto err_out;
		}
	}
	(void)pci_enable_pcie_error_reporting(sdev->pdev);
	epfront_info("reinit success");

	return 0;
err_out:
	(void)bridge_linkset(sdev->pdev->bus->self, 1);
	return  -ENODEV;
}

/*****************************************************************************
Function    : __sdi_pf_disable_ctrl
Description : disable pf control
Input       : struct sdi_pdev_info * sdev
Output      : int
Return      : int
*****************************************************************************/
static int __sdi_pf_disable_ctrl(struct sdi_pdev_info *sdev){
	u64 cap;
	cap = readq(&sdev->bar->cap);
	epfront_info("disable dev now...");
	return sdi_pf12_disable_ctrl(sdev, cap);
}


/*****************************************************************************
Function    : sdi_pf_linkdown
Description : linkdown pf
Input       : void
Output      : int
Return      : int
*****************************************************************************/
int sdi_pf_linkdown(void)
{
	struct sdi_pdev_info *sdev = &gsdev;

	if (unlikely(!sdev->pdev)) {
		epfront_err("pcie dev not exist");
		return -ENODEV;
	}

	BUG_ON(!sdev->pdev->bus);
	BUG_ON(!sdev->pdev->bus->self);
	epfront_info("set linkdown now");
	return bridge_linkdown(sdev->pdev->bus->self);
}

/*****************************************************************************
Function    : transfer_sys_do_release
Description : sys release
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void transfer_sys_do_release(void)
{
	struct sdi_pdev_info *sdev = &gsdev;
	sdi_admin_queue_t *admin_queue = NULL;

	clear_bit(SDI_EP_READY,&sdev->state);
	if (unlikely(!sdev->bar || !readq(&sdev->bar->cap))) {
		epfront_err("iomem not exist");
		return;
	}
	msleep(10);//wait scsi_ep_back rmmod	
	
	if (__sdi_pf_disable_ctrl(sdev))
		epfront_err("dev disable failed");

	gdev_list_remove();
	udelay(3uL);
	admin_queue = sdev->admin_queue;
	if (!admin_queue) {
		epfront_info("admin queue is not exit");
		clear_bit(SDI_EP_STOPPING, &sdev->state);
		clear_bit(SDI_EP_READY,&sdev->state);
		return;
	}

	sdi_disable_admin_queue(sdev->admin_queue);
	sdi_aq_remove(sdev);
	set_bit(SDI_EP_UPDATING, &sdev->state);
}

/*****************************************************************************
Function    : transfer_sys_do_update_begin
Description : sys update process begin
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void transfer_sys_do_update_begin(void)
{    
	heartbeat_thread_remove();
	if (epfront_stop_trans(demand_status))
		epfront_err("epfront_stop_trans exist");
	transfer_sys_do_release();
	epfront_info("transfer_sys_do_update_begin end");
}

/*****************************************************************************
Function    : transfer_sys_do_apply
Description : sys apply
Input       : void
Output      : int
Return      : int
*****************************************************************************/
static int transfer_sys_do_apply(void)
{
	int ret;
	struct sdi_pdev_info *sdev = &gsdev;

	if ((!sdev->bar) && sdi_dev_map(sdev)) {
		epfront_err("sdi_dev_map fail.");
		goto disable_cfg;
	}

	if (test_bit(SDI_EP_READY, &sdev->state) ||
	    test_bit(SDI_EP_STOPPING, &sdev->state) ||
	    test_bit(SDI_EP_STARTING, &sdev->state)) {
		epfront_err("%s state is %lx start is abort.", sdev->name, sdev->state);
		return -ENODEV;
	}

	ret = sdi_configure_admin_queue(sdev);
	if (ret) {
		epfront_err("sdi_configure_admin_queue fail, ret = %d", ret);
		goto disable_cfg;
	}

	ret = gdev_list_add();
	if (ret) {
		epfront_err("gdev_list_add fail, ret = %d", ret);
		goto disable_aq;
	}

	ret = heartbeat_thread_add();
	if (ret) {
		epfront_err("heartbeat_thread_add fail, ret = %d", ret);
		goto disable_aq;
	}
	
	/* creat heartbeat_thread after admin queue ready*/
	clear_bit(SDI_EP_UPDATING, &sdev->state);
	set_bit(SDI_EP_READY, &sdev->state);

	return 0;

disable_aq:
	sdi_disable_admin_queue(sdev->admin_queue);
	sdi_aq_remove(sdev);
disable_cfg:
	epfront_err("transfer system has reset fail.");
	return -ENODEV;
}

/*****************************************************************************
Function    : transfer_sys_do_update_end
Description : sys update process end
Input       : void
Output      : int
Return      : int
*****************************************************************************/
static int transfer_sys_do_update_end(void)
{
	int ret;

	ret = transfer_sys_do_apply();
	if (ret) {
		epfront_err("transfer_sys_do_apply fail.");
		return ret;
	}

	epfront_start_trans();
	epfront_info("transfer system has reset success");
	return 0;
}

/*****************************************************************************
Function    : transfer_sys_do_heartbeat
Description : sys heartbeat
Input       : struct sdi_admin_command c
Output      : int
Return      : int
*****************************************************************************/
static int transfer_sys_do_heartbeat(struct sdi_admin_command *c)
{
	sdi_pdev_info_t *sdev = &gsdev;
	int status;

	status = sdi_submit_admin_cmd(sdev, c, NULL, HEARTBEAT_TIMEOUT);
	if (status) {
		epfront_err("heatbeat packet submit failed status: 0x%x",status);
		return status;
	}

	return 0;
}

/*****************************************************************************
Function    : heartbeat_kthread
Description : heartbeat kthread
Input       : void * data
Output      : int
Return      : int
*****************************************************************************/
static int heartbeat_kthread(void *data)
{	
	int result;
	unsigned int time_stamp = 0;
	struct sdi_admin_command c;
	clear_bit(SDI_FRONT_HEARTBEAT_ABNORMAL, &demand_status);
	memset((void *)&c, 0, sizeof(c));
	memset((void *)&c, 0, sizeof(struct sdi_admin_command));

	epfront_info("start heartbeat thread.");

	c.ucmd.opcode = sdi_admin_heart_beat;
	c.ucmd.len = 0;
	c.ucmd.flag = SYNC_UCMD_USING_PIO;
	
	while (!kthread_should_stop()) {
		set_current_state(TASK_RUNNING);
		set_current_state(TASK_INTERRUPTIBLE);
		
		result = transfer_sys_do_heartbeat(&c);
		if(result == -EINTR) {
			set_bit(SDI_FRONT_HEARTBEAT_ABNORMAL, &demand_status);
			++time_stamp;
			if (time_stamp >= HEATBEAT_TIMEOUT_NUM) {
				epfront_err("heartbeat fail.");
				(void)transfer_sys_do_reset();
				break;
			} else {
				epfront_err("heartbeat fail. update_result = %u,try_num = %u",result,time_stamp);
				continue;
			}
		} else if (result) {
			epfront_err("admin heartbeat sq alloc is NULL. status = %d", result);
		}		

		time_stamp = 0;
		clear_bit(SDI_FRONT_HEARTBEAT_ABNORMAL, &demand_status);
		/* period packet,heartbeat is 1s */
		msleep(HEATBEAT_WAITTIME);
	}
	spin_lock(&heartbeat_lock);
	heartbeat_thread = NULL;
	spin_unlock(&heartbeat_lock);
	clear_bit(SDI_FRONT_HEARTBEAT_ABNORMAL, &demand_status);
	epfront_info("stop heartbeat thread.");
	return 0;
}

/*****************************************************************************
Function    : update_kthread
Description : update kthread
Input       : void * data
Output      : int
Return      : int
*****************************************************************************/
static int update_kthread(void *data)
{
	struct sdi_pdev_info *sdev =(struct sdi_pdev_info *)data;
	unsigned int csts_update = 0;
	unsigned int oldcsts_update = 0;
	epfront_info("start update kthread.");
	clear_bit(SDI_FRONT_UPDATE, &demand_status);
	
	while (!kthread_should_stop()) {
		set_current_state(TASK_RUNNING);
		set_current_state(TASK_INTERRUPTIBLE);
		if (!sdev->bar) {
			epfront_err("sdev->bar is NULL.");
			break;
		}
		if (test_bit(SDI_FRONT_HEARTBEAT_ABNORMAL, &demand_status)) {
			epfront_err("heartbeat abnormal, demans = 0x%lx", demand_status);
			msleep(WAIT_HEARTBEAT_2_NORMAL);
			continue;
		}
		csts_update = (readl(&sdev->bar->csts) >> 6) & 0xffff;
		if (csts_update == 0xffff) {
			epfront_err("csts_update = 0xffff.");
			break;
		}
		csts_update &= 0x1;
		if ((csts_update != oldcsts_update) && csts_update) {
			set_bit(SDI_FRONT_UPDATE, &demand_status);
			epfront_info("update begin.");
			transfer_sys_do_update_begin();
		} else if ((csts_update != oldcsts_update) && !csts_update) {
			if (transfer_sys_do_update_end()) {
				msleep(WAIT_COMPLETE_TIME);
				continue;
			}
			epfront_info("update end success.");
			clear_bit(SDI_FRONT_UPDATE, &demand_status);
		}
		oldcsts_update = csts_update;
		msleep(UPDATE_WAIT_TIME);
	}	
	spin_lock(&update_lock);
	update_thread = NULL;
	spin_unlock(&update_lock);
	clear_bit(SDI_FRONT_UPDATE, &demand_status);
	epfront_info("stop update kthread.");
	return 0;
}


/*****************************************************************************
Function    : reset_kthread
Description : reset kthread
Input       : void * data
Output      : int
Return      : int
*****************************************************************************/
static int reset_kthread(void *data)
{	
	int ret;
	int ret_state = -1;
	unsigned long time_interval;
	unsigned long time_stamp;
	struct sdi_pdev_info *sdev = (struct sdi_pdev_info *)data;

	set_bit(SDI_FRONT_PERCEIV_RESET, &demand_status);
		
	heartbeat_thread_remove();	
	ret = sdi_pf_linkdown();
	if (ret) {
		epfront_err("sdi_pf_linkdown failed, ret[%d]", ret);
		goto fail;
	}
	epfront_info("linkdown,start reset thread.");
	time_interval = jiffies_64;
	
	update_thread_remove();
	transfer_sys_do_update_begin();
	sdi_dev_unmap(sdev);

	for (; ;) {
		time_stamp = jiffies_64 - time_interval;
		if(time_stamp > (HZ * SDI_RESET_WAITTIME))
			break;
		msleep(WAIT_COMPLETE_TIME);
	}
	
	epfront_info("start recovery.");

	while(!kthread_should_stop()) {
		set_current_state(TASK_RUNNING);
		set_current_state(TASK_INTERRUPTIBLE);	
		/* ret < 0,try again. */
		ret = linkdown_reinit();
		if(!ret)
			break;
		msleep(WAIT_COMPLETE_TIME);
	}

	/* if ret < 0, dont do recovery.*/
	while(!kthread_should_stop() && !ret) {
		set_current_state(TASK_RUNNING);
		set_current_state(TASK_INTERRUPTIBLE);
		ret_state = transfer_sys_do_update_end();
		if(!ret_state)
			break;
		msleep(WAIT_COMPLETE_TIME);
	}
	
	if (!ret_state) {
		if (update_thread_add()) {
			epfront_info("update_thread_add fail.");
			goto fail;
		}
		spin_lock(&perceiv_reset_lock);
		reset_thread = NULL;
		spin_unlock(&perceiv_reset_lock);
		clear_bit(SDI_FRONT_PERCEIV_RESET, &demand_status);
		epfront_info("stop reset thread , success.");
		return 0;
	}

fail:
	epfront_err("stop reset thread , fail.");
	spin_lock(&perceiv_reset_lock);
	reset_thread = NULL;
	spin_unlock(&perceiv_reset_lock);
	clear_bit(SDI_FRONT_PERCEIV_RESET, &demand_status);
	return -EFAULT;
}

/*****************************************************************************
Function    : sdiep_probe_kthread
Description : sdiep probe kthread
Input       : void * data
Output      : int
Return      : int
*****************************************************************************/
static int sdiep_probe_kthread(void *data)
{
	struct sdi_pdev_info *sdev =(struct sdi_pdev_info *)data;
	int result = 0;
	unsigned int timeut_num =0;
	unsigned long time_out = jiffies + PROBE_TIME_OUT * HZ;
	epfront_info("start sdiep_probe_kthread.");
	while (!kthread_should_stop()) {
		set_current_state(TASK_RUNNING);
		set_current_state(TASK_INTERRUPTIBLE);
		result = sdi_dev_start(sdev);
		if(result){
			if(time_after(jiffies,time_out)){
				time_out = jiffies + PROBE_TIME_OUT * HZ;
				timeut_num++;
				epfront_info("dev probe time out timeut_num 0x%x",timeut_num);
			}
			msleep(ARM_SDIEP_PROBE_WAITTIME);
			probe_succ_flag = 0;
		} else {
			epfront_info("dev probe successful");
			clear_bit(SDI_FRONT_REMOVE, &demand_status);
			probe_succ_flag = 1;
			break;
		}
	}
	if(probe_succ_flag == 0)
		goto free_resource;
#ifdef HEART_BEAT_CHECK
	INIT_WORK(&sdev->service_task, sdi_service_task);
	setup_timer(&sdev->service_timer, &sdi_service_timer, (unsigned long) sdev);
	mod_timer(&sdev->service_timer, jiffies);
#endif
	epfront_info("%s initialized successful.", sdev->name);

	epfront_scsi_probe();
	clear_bit(SDI_EP_UPDATING, &sdev->state);
	result = update_thread_add();
	if (result)
		epfront_err("update_thread_add fail.");
	(void)pci_enable_pcie_error_reporting(sdev->pdev);
	transfer_do_aer_set(sdev->pdev->bus->self, 1);
	probe_succ_flag = 1;
	sdiep_probe_thread = NULL;
	epfront_info("stop sdiep_probe_kthread.");
	return 0;

free_resource:
	destroy_workqueue(sdev->aenwork);
	kfree(sdev->entry);
	sdev->entry = NULL;
	clean_sdi_pfx_sys(sdev);
	epfront_err("%s Load Driver failed.", sdev->name);
	return result;
}

/*****************************************************************************
Function    : transfer_sys_do_reset
Description : sys reset
Input       : void
Output      : int
Return      : int
*****************************************************************************/
int transfer_sys_do_reset(void)
{
	int ret;
	
	epfront_info("reset io aer come.");
	ret = reset_thread_add();
	if (ret) {
		epfront_err("reset_thread_add fail, ret = %d", ret);
		goto disable_reset;
	}
	epfront_info("reset thread handle.");
	return 0;

disable_reset:
	reset_thread_remove();
	return -EFAULT;
}

/*****************************************************************************
Function    : transfer_do_aer_set
Description : set aer
Input       : struct pci_dev * pdev
              u32 enable
Output      : void
Return      : void
*****************************************************************************/
static void transfer_do_aer_set(struct pci_dev *pdev, u32 enable)
{
	u32 reg = 0;

	(void)pci_read_config_dword(pdev, PCIE_AER_REG, &reg);
	epfront_info("aer reg%x",reg);

	if (enable)
		reg |= (1 << PCIE_AER_ENABLE);
	else
		reg &= ~(1 << PCIE_AER_ENABLE);

	epfront_info("aer reg%x",reg);
	(void)pci_write_config_dword(pdev, PCIE_AER_REG, reg);
}


/*****************************************************************************
Function    : sdi_pf12_io_error_detected
Description : callback of pf12 io error detected
Input       : struct pci_dev * pdev
              pci_channel_state_t state
Output      : pci_ers_result_t
Return      : pci_ers_result_t
*****************************************************************************/
static pci_ers_result_t sdi_pf12_io_error_detected(struct pci_dev *pdev, pci_channel_state_t state)
{
	int ret;
	int pos;
	unsigned int uncor_status = 0;
	unsigned int uncor_severity = 0;
	unsigned int aer_time_stamp = 0;
	int pfno = PCI_FUNC(pdev->devfn);
	epfront_info("aer test reset.pfno 0x%x.", pfno);

	if ((pfno == 0) || (pfno == 2) ||
	    test_bit(SDI_FRONT_REMOVE, &demand_status)) {
		epfront_info("PCI_ERS_RESULT_RECOVERED.pfno 0x%x", pfno);
		return PCI_ERS_RESULT_NEED_RESET;
	}
	
	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_ERR);

	(void)pci_write_config_dword(pdev, pos + PCI_ERR_UNCOR_STATUS, uncor_status);
	(void)pci_read_config_dword(pdev, pos + PCI_ERR_UNCOR_STATUS, &uncor_status);
	epfront_info("aer clear uncor_status 0x%x, uncor_severity 0x%x, cpuid 0x%x",
			uncor_status, uncor_severity, smp_processor_id());

	while (test_bit(SDI_FRONT_HEARTBEAT_ABNORMAL, &demand_status) ||
		test_bit(SDI_FRONT_PERCEIV_RESET, &demand_status)) {
		epfront_err("demand 0x%lx", demand_status);
		if ((aer_time_stamp++) > AER_WAITNUM)
			return PCI_ERS_RESULT_CAN_RECOVER;
		msleep(AER_WAITTIME);
	}
	
	set_bit(SDI_FRONT_AER, &demand_status);
	update_thread_remove();
	heartbeat_thread_remove();
	ret = epfront_stop_trans(demand_status);
	if (ret) {
		clear_bit(SDI_FRONT_AER, &demand_status);
		epfront_err("epfront_stop_trans exist");
		return PCI_ERS_RESULT_NONE;
	}
	return PCI_ERS_RESULT_NEED_RESET;
}

/*****************************************************************************
Function    : sdi_pf12_io_slot_reset
Description : reset pf12 io slot
Input       : struct pci_dev * pdev
Output      : pci_ers_result_t
Return      : pci_ers_result_t
*****************************************************************************/
static pci_ers_result_t sdi_pf12_io_slot_reset(struct pci_dev *pdev)
{
	int ret;
	struct sdi_pdev_info *sdev = &gsdev;
	unsigned int try_num = 0;

	int pfno;
	pfno = PCI_FUNC(pdev->devfn);
	epfront_info("aer test reset.pfno 0x%x.", pfno);

	if ((pfno == 0) || (pfno == 2) ||
	    !test_bit(SDI_FRONT_AER, &demand_status)) {
		epfront_err("PCI_ERS_RESULT_RECOVERED.pfno 0x%x", pfno);
		return PCI_ERS_RESULT_RECOVERED;
	}

	transfer_sys_do_release();
	sdi_dev_unmap(sdev);
	ret = linkdown_reinit();
	if (ret) {
		epfront_err("linkdown_reinit failed, ret[%d]", ret);
		goto fail;
	}

	do {
		ret = transfer_sys_do_apply();
        if ((try_num++) > AER_WAITNUM)
            goto fail;
		epfront_err("transfer_sys_do_apply fail, try_num %u", try_num);
		msleep(AER_WAITTIME);
	} while (ret);

	epfront_info("io_slot_reset success.");
	return PCI_ERS_RESULT_RECOVERED;

fail:
	clear_bit(SDI_FRONT_AER, &demand_status);
	epfront_err("io_slot_reset fail.");
	return PCI_ERS_RESULT_DISCONNECT;
}


/*****************************************************************************
Function    : sdi_pf12_io_resume
Description : resume pf12 io
Input       : struct pci_dev * pdev
Output      : void
Return      : void
*****************************************************************************/
static void sdi_pf12_io_resume(struct pci_dev *pdev)
{
	int ret;
	int pfno;
	pfno = PCI_FUNC(pdev->devfn);
	epfront_info("aer test resume pfno 0x%x", pfno);
	if ((pfno == 0) || (pfno == 2) ||
	    !test_bit(SDI_FRONT_AER, &demand_status)) {
		epfront_err("PCI_ERS_RESULT_RECOVERED.pfno 0x%x", pfno);
		return;
	}
	epfront_start_trans();
	ret = update_thread_add();
	if (ret)
		epfront_err("update_thread_add fail, ret = %d", ret);
	clear_bit(SDI_FRONT_AER, &demand_status);
	epfront_info("io_resume success.");
}

static struct pci_error_handlers sdi_pf12_err_handler = {
		.error_detected = sdi_pf12_io_error_detected,
		.slot_reset = sdi_pf12_io_slot_reset,
		.resume = sdi_pf12_io_resume,
};

static int sdi_pf12_probe(struct pci_dev *pdev, const struct pci_device_id *id);
static void sdi_pf12_remove(struct pci_dev *pdev);
static struct pci_driver sdi_pf12_driver = {
	.name        = "sdi_ep_common",
	.id_table    = sdi_pf12_pci_tbl,
	.probe       = sdi_pf12_probe,
	.remove      = sdi_pf12_remove,
	.shutdown    = sdi_pf12_remove,
	.err_handler = &sdi_pf12_err_handler,
};

/*****************************************************************************
Function    : sdi_start_probeep_thd
Description : start probeep thread
Input       : struct sdi_pdev_info * sdev
Output      : int
Return      : int
*****************************************************************************/
static int sdi_start_probeep_thd(struct sdi_pdev_info *sdev)
{
	int result =0;
	sdiep_probe_thread = kthread_run(sdiep_probe_kthread, sdev, "heartbeat_thread");
	if (IS_ERR_OR_NULL(sdiep_probe_thread)) {
		epfront_err("Create SDI  sdiep_probe_thread failed.");
		result = sdiep_probe_thread ? (int)PTR_ERR(sdiep_probe_thread) : -EINTR;
	}
	return result;
}

/*****************************************************************************
Function    : sdi_pf12_probe
Description : pf12 probe function
Input       : struct pci_dev * pdev
              const struct pci_device_id * id
Output      : int
Return      : int
*****************************************************************************/
static int sdi_pf12_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int probe_period_times = 0;
	int result, pfno;
	struct sdi_pdev_info *sdev = &gsdev;
	pfno = PCI_FUNC(pdev->devfn);
	if ((pfno == 0) || (pfno == 2))
		return 0;

	UNREFERENCE_PARAM(id);

	if (total_num >= SDI_MAX_NRS) {
		epfront_err("Current Driver only support %d PF devices.", SDI_MAX_NRS);
		return -EINVAL;
	}
	(void)snprintf(sdev->name, SDI_PF12_EP_NAME_LEN, "sdi-pf%01d bdf: %02x:%02x.%02x",
			pfno, pdev->bus->number, PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));

	result = create_sdi_pfx_sys(sdev, epfront_kobj);
	if (result) {
		epfront_err("create_sdi_pfx_sys failed, result[%d]", result);
		return result;
	}

	sdev->parent = epfront_kobj;

	sdev->entry = (struct msix_entry *)kcalloc(SDI_PF12_MAX_CQ_NR, sizeof(*sdev->entry), GFP_KERNEL);
	if (!sdev->entry) {
		epfront_err("Allocate memory failed.");
		goto free_proc;
	}

	sdev->pdev = pdev;
	sdev->max_io_sq_nr = SDI_PF12_MAX_QUEUE_NUM - 1; /*one used by admin queue*/
	sdev->max_io_cq_nr = SDI_PF12_MAX_QUEUE_NUM - 1; /*one used by admin queue*/
	sdev->max_hw_sectors = SDI_PF12_MAX_SECTORS;

	spin_lock_init(&sdev->qid_lock);

	sdev->aenwork = create_workqueue("sdi_aen_wq");
	if (!sdev->aenwork) {
		epfront_err("%s create workqueue sdi_aen_wq failed.", sdev->name);
		result = -ENOMEM;
		goto free_entry;
	}

	pci_set_drvdata(pdev, sdev);

try_again:
	result = sdi_dev_start(sdev);
	if (result) {
		epfront_err("dev start time out");
        goto start_probe_sdiep_thd;
	}

#ifdef HEART_BEAT_CHECK
	INIT_WORK(&sdev->service_task, sdi_service_task);
	setup_timer(&sdev->service_timer, &sdi_service_timer, (unsigned long) sdev);
	mod_timer(&sdev->service_timer, jiffies);
#endif
	epfront_info("%s initialized successful.", sdev->name);
	epfront_scsi_probe();
	clear_bit(SDI_EP_UPDATING, &sdev->state);
	result = update_thread_add();
	if (result) {
		epfront_err("update_thread_add fail, result = %d", result);
		update_thread_remove();
	}
	(void)pci_enable_pcie_error_reporting(sdev->pdev);
	transfer_do_aer_set(sdev->pdev->bus->self, 1);

	clear_bit(SDI_FRONT_REMOVE, &demand_status);
	probe_succ_flag = 1;
	return 0;
start_probe_sdiep_thd:
	result = sdi_start_probeep_thd(sdev);
	if(result == 0)
		return 0;
    else{
        probe_period_times++;
        if(probe_period_times < MAX_PROBE_TRY_TIME_WHEN_THREAD_FAIL)
        {
            epfront_err("create probe_thread fail times:%d",probe_period_times);
            goto try_again;
        }
        else{
            epfront_err("probe has up to max times,quiting");
        }
    }

	destroy_workqueue(sdev->aenwork);
free_entry:
	kfree(sdev->entry);
	sdev->entry = NULL;
free_proc:
	clean_sdi_pfx_sys(sdev);
	epfront_err("%s Load Driver failed.", sdev->name);
	return result;
}

/*****************************************************************************
Function    : sdi_pf12_remove
Description : remove pf12
Input       : struct pci_dev * pdev
Output      : void
Return      : void
*****************************************************************************/
static void sdi_pf12_remove(struct pci_dev *pdev)
{
	char name[SDI_PF12_EP_NAME_LEN];
	int pfno;
	struct sdi_pdev_info *sdev = pci_get_drvdata(pdev);

	pfno = PCI_FUNC(pdev->devfn);
	dev_info(&pdev->dev, "pfno %d\n", pfno);
	if((pfno == 0) || (pfno == 2))
		return;
	set_bit(SDI_FRONT_REMOVE, &demand_status);
	transfer_do_aer_set(sdev->pdev->bus->self, 0);
	(void)pci_disable_pcie_error_reporting(pdev);
	if(!IS_ERR_OR_NULL(sdiep_probe_thread))
		(void)kthread_stop(sdiep_probe_thread);
	sdiep_probe_thread = NULL;
	
	if(!probe_succ_flag)
		return;
	reset_thread_remove();
	update_thread_remove();
	heartbeat_thread_remove();
	epfront_scsi_remove();

#ifdef HEART_BEAT_CHECK
	cancel_work_sync(&sdev->service_task);
	del_timer_sync(&sdev->service_timer);
#endif

	if (test_bit(SDI_EP_READY, &sdev->state)) {
		clear_bit(SDI_EP_READY, &sdev->state);
		sdi_dev_shutdown(sdev);
	} else {
		if (test_and_clear_bit(SDI_EP_UPDATING, &sdev->state))
			sdi_dev_unmap(sdev);
		epfront_info("device not ready");
	}

	pci_set_drvdata(pdev, NULL);

	if (sdev->aenwork) {
		flush_workqueue(sdev->aenwork);
		destroy_workqueue(sdev->aenwork);
	}

	kfree(sdev->entry);
	clean_sdi_pfx_sys(sdev);
	memcpy(name, sdev->name, SDI_PF12_EP_NAME_LEN);
	probe_succ_flag = 0;
	epfront_info("%s unload successful.", name);
}


/*****************************************************************************
Function    : sdi_pf12_common_init
Description : init pf12 common module
Input       : void
Output      : int
Return      : int
*****************************************************************************/
int sdi_pf12_common_init(void)
{
	int result;
	struct pci_dev *pf0 = NULL;
	u32 val = 0;

	epfront_info("transfer module loading...");
	memset((void*)&gsdev, 0, sizeof(sdi_pdev_info_t));
	
	pf0 = pci_get_subsys(SDI_VENDOR_ID, SDI_DEVICE_ID_1610, PCI_ANY_ID, 0, NULL);
	if (unlikely(!pf0)) {
		epfront_err("find pf failed");
		return -EINVAL;
	}
	(void)pci_read_config_dword(pf0, 0, &val);
	pci_dev_put(pf0);
	if ((val & LINK_CHECK_STATE) == LINK_CHECK_STATE) {
		epfront_err("link state is 0xffff");
		return -EINVAL;
	}
	
	sdi_thread = kthread_run(sdi_kthread, NULL, "sdi_pfs");
	if (IS_ERR_OR_NULL(sdi_thread)) {
		epfront_err("Create SDI PFs thread failed.");
		result = sdi_thread ? (int)PTR_ERR(sdi_thread) : -EINTR;
		return result;
	}

	result = pci_register_driver(&sdi_pf12_driver);
	if (result) {
		epfront_err("pci register failed");
		goto THREAD_STOP;
	}
	
	return result;

THREAD_STOP:
	if (!IS_ERR_OR_NULL(sdi_thread))
		(void)kthread_stop(sdi_thread);
	return  result;
}

/*****************************************************************************
Function    : sdi_pf12_common_exit
Description : exit pf12 common module
Input       : void
Output      : void
Return      : void
*****************************************************************************/
void sdi_pf12_common_exit(void)
{
	unsigned int time_stamp = 0;
	_sdi_check_size();
	
	while (demand_status) {	
		epfront_err("front remove fail, demand_status = 0x%lx, try_num = 0x%u.",
					demand_status, time_stamp);		
		if ((time_stamp++) >= WAIT_FRONT_RMMOD_TIMENUM)
			break;
		msleep(WAIT_FRONT_RMMOD_TIMEOUT);
	}

	pci_unregister_driver(&sdi_pf12_driver);

	if (!IS_ERR_OR_NULL(sdi_thread))
		(void)kthread_stop(sdi_thread);
	
	epfront_info("transfer module unloaded");
}

#define EPFRONT_DUMP_ENTRY_BEFORE 10
#define EPFRONT_DUMP_ENTRY_AFTER 54

void epfront_print_cq(int qid)
{
	sdi_pdev_info_t *spdev = &gsdev;
	sdi_cq_info_t *cq_info = spdev->cq_info[qid&(SDI_PF12_MAX_CQ_NR-1)];
	
	unsigned char* data;
	int ite_cqe, end_cqe;
	int i;

	if(likely(cq_info)){
		if(likely(cq_info->cq_addr)){
			ite_cqe = cq_info->head;
			end_cqe = cq_info->head;
			ite_cqe = (ite_cqe - EPFRONT_DUMP_ENTRY_BEFORE) >= 0 ?
				(ite_cqe - EPFRONT_DUMP_ENTRY_BEFORE) : (ite_cqe + cq_info->q_depth - EPFRONT_DUMP_ENTRY_BEFORE);
			end_cqe = (end_cqe + EPFRONT_DUMP_ENTRY_AFTER >= cq_info->q_depth) ?
				(end_cqe + EPFRONT_DUMP_ENTRY_AFTER - cq_info->q_depth) : (end_cqe + EPFRONT_DUMP_ENTRY_AFTER);

			while(ite_cqe != end_cqe){
				data = (unsigned char*)cq_info->cq_addr + (long)ite_cqe * cq_info->stride;
				for(i = 0; i < (cq_info->stride >> 4); ++i){
					epfront_info("cq[%d] cqe[%d]:"
						"%02x %02x %02x %02x    %02x %02x %02x %02x    %02x %02x %02x %02x    %02x %02x %02x %02x",
						qid, ite_cqe,
						data[(long)i*16],data[(long)i*16+1],data[(long)i*16+2],data[(long)i*16+3],
						data[(long)i*16+4],data[(long)i*16+5],data[(long)i*16+6],data[(long)i*16+7],
						data[(long)i*16+8],data[(long)i*16+9],data[(long)i*16+10],data[(long)i*16+11],
						data[(long)i*16+12],data[(long)i*16+13],data[(long)i*16+14],data[(long)i*16+15]);
				}
				
				if(++ite_cqe == cq_info->q_depth){
					ite_cqe = 0;
				}
			}
		}else{
			epfront_info("qid[%d]: cq_info->cq_addr[0x%p]", qid, cq_info->cq_addr);
		}
	} else{
		epfront_info("qid[%d]: cq_info[0x%p]", qid, cq_info);
	}
}

void epfront_print_queue(int qid)
{
	sdi_pdev_info_t *spdev = &gsdev;
	sdi_cq_info_t *cq_info = spdev->cq_info[qid&(SDI_PF12_MAX_CQ_NR-1)];
	sdi_sq_info_t *sq_info = spdev->sq_info[qid&(SDI_PF12_MAX_CQ_NR-1)];

	if(!cq_info || !sq_info){
		epfront_info("qid[%d]: sq_info[0x%p], cq_info[0x%p]", qid, sq_info, cq_info);
		return ;
	}
	
	epfront_info("cq[%d]: head[%d], tail[%d], q_depth[%d], stride[%d], q_type[%d]\n"
		"cq_addr[0x%p], dma_addr[0x%llx], cpu_no[%d], q_db[0x%p]\n"
		"rx_ng_cnt[0x%llx], rx_ok_cnt[0x%llx], size[%u], cq_vector[%u], vecid[%u]\n"
		"cqe_seen[%d], cq_phase[%d], suspend[%d]",
		cq_info->cqid, cq_info->head, cq_info->tail, cq_info->q_depth, cq_info->stride, cq_info->q_type,
		cq_info->cq_addr, cq_info->dma_addr, cq_info->cpu_no, cq_info->q_db,
		cq_info->rx_ng_cnt, cq_info->rx_ok_cnt, cq_info->size, cq_info->cq_vector, cq_info->vecid,
		cq_info->cqe_seen, cq_info->cq_phase, cq_info->suspend);

	epfront_info("sq[%d]: head[%d], tail[%d], q_depth[%d], stride[%d], q_type[%d]\n"
		"sq_addr[0x%p], dma_addr[0x%llx], q_db[0x%p], state[0x%lx]\n"
		"rx_ng_cnt[0x%llx], rx_ok_cnt[0x%llx], size[%u]\n"
		"cq_id[%u], udrv_type[%d], q_prio[%d]",
		sq_info->sqid, sq_info->head, sq_info->tail, sq_info->q_depth, sq_info->stride, sq_info->q_type,
		sq_info->sq_addr, sq_info->dma_addr, sq_info->q_db, sq_info->state,
		sq_info->tx_ok_cnt, sq_info->tx_busy_cnt, sq_info->size,
		sq_info->cq_id, sq_info->udrv_type, sq_info->q_prio);
}

void sdi_dump_queues(void)
{
    sdi_pdev_info_t *spdev = &gsdev;
	sdi_cq_info_t *cq_info;
	int i;

	for(i = 0; i < SDI_PF12_MAX_CQ_NR; ++i){
		cq_info = spdev->cq_info[i];
		if(cq_info){
			epfront_print_queue(i);
			epfront_print_cq(i);
		}
	}
}
