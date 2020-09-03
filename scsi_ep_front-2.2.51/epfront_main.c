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

#include "epfront.h"
#ifdef __ADMIN_CMD__
#include "ep_admcmd.h"
#endif
#include <linux/kprobes.h>
#include <asm/traps.h>

#define EPFRONT_DUMP_QUEUES_INTERVAL_MSEC (1000)
#define EPFRONT_GET_DEVNAME_TIME_OUT (2 * HZ)
#define EPFRONT_GET_DEVNAME_INTERVAL_MSEC (10)
//#define EPFRONT_PRI_RESET_BACK_INTERVAL_MSEC (10000)
#define EPFRONT_PRI_RESET_BACK_INTERVAL_MSEC (60000*10)
//#define EPFRONT_PRI_BACK_RDY_INTERVAL_MSEC (30000)
#define EPFRONT_PRI_BACK_RDY_INTERVAL_MSEC (60000*10)
#define EPFRONT_SV_THREAD_ERR_INTERVAL_MSEC (60000*10)

#define EPFRONT_SV_SUSPEND_TIMOUT_MSEC (1800000)
#define EPFRONT_WAIT_QUEUECOMMAND_MSEC (10000)
#define EPFRONT_WAIT_IO_COMPLETE_MSEC (120000)
#define EPFRONT_EH_ABORT_TIMEOUT (140 * HZ)
#define EPFRONT_EH_RESET_TIMEOUT (10 * HZ)
#define EPFRONT_ROAD_LEN 512

#define EPFRONT_SV_LOW_PRI (1U << 8)
#define EPFRONT_SV_GET_PRI(x) ((x) & 0xff00)
#define EPFRONT_SV_GET_TYPE(x) ((x) & 0xff)

#define EPFRONT_UP_TO_MULTY4(x) ( ((x) + 4) & (~0x03) )

#define EPFRONT_PRINT_RETRY_INTERVAL_MSEC (1000*3600*24)




#define ILLEGAL_BACK_UNIQ_ID(ctrl, id) ( (~(unsigned)((ctrl)->mask)) & (id) )

static int epfront_aer_recv_add_disk(void* data, struct epfront_main_info *smain);
static int epfront_aer_recv_rmv_disk(void* data, struct epfront_main_info *smain);
static int epfront_aer_recv_notify_rescan(void* data, struct epfront_main_info *smain);
static int epfront_aer_recv_linkdown(void* data, struct epfront_main_info *smain);
static int epfront_aer_recv_io_switch(void* data, struct epfront_main_info *smain);

static int epfront_sv_back_notify_probe(void *data);
static int epfront_sv_reset_handle(void* data);
static int epfront_sv_rename_luns(void* data);
static int epfront_sv_sync_disk(void* data);
static int epfront_sv_aer_handle(void* data);
static int epfront_sv_start_trans(void* data);
static int epfront_sv_sync_devname(void* data);

void epfront_set_queue_off(struct epfront_main_info* smain);
static int epfront_wait_queue_off(int msec, struct epfront_main_info* smain);
static void epfront_handle_pending_io(struct epfront_main_info* smain);
static void epfront_trans_way_exit(struct epfront_main_info *smain);
static int epfront_trans_way_init(struct epfront_main_info *smain);
static int epfront_sync_result(enum ep_sync_type res, struct epfront_main_info *smain);
static int epfront_sync_reset_back_state(struct epfront_main_info *smain);
static inline int epfront_do_stop_trans(unsigned long status, struct epfront_main_info* smain);
#ifdef __ADMIN_CMD__
void epfront_clear_admcmd_operation(struct epfront_main_info *smain);
static void epfront_clear_cmdcmd_timer(struct epfront_main_info *smain);
static void epfront_admcmd_timer_init(struct epfront_main_info *smain,void (*timeout_fun)(unsigned long));
#endif

//debug to test use_cluster
static unsigned int use_cluster = 0;

//log level
int epfront_loglevel = EPFRONT_LOG_INFO;


struct epfront_io_list{
    struct list_head list;
    struct ep_io_cqe cqe;
    u32 task_index;
};
void epfront_io_handle(unsigned long data);
struct epfront_io_list* epfront_create_io_lst(struct ep_io_cqe* cqe_data, struct epfront_main_info *smain);

static epfront_main_info_t gsepmain[SDI_MAX_NRS];

/*******************************CRC32 MODULE start*********************************/
#define SCALE_F sizeof(unsigned long)
#define REX_PRE "0x48, "

#ifdef __EULER_AARCH64__
#define CRC_ERR   1
#define CRC_SUC   0
extern int crc32_iscsi(const void *buffer, unsigned int length, int seed);
int crc32_arm (const void *buffer, unsigned int length, int seed, int *crc)
{
    if (buffer == NULL || length == 0 || crc == NULL)
    {
        return CRC_ERR;
    }
	*crc = crc32_iscsi(buffer, length, seed);
	return CRC_SUC;
}

#endif

/*****************************************************************************
Function    : crc32c_intel_le_hw_byte
Description : crc32 calculation function for data less than 8 bytes
Input       : u32 crc
              unsigned char const * data
              size_t length
Output      : u32
Return      : u32 crc32_result
*****************************************************************************/
static inline u32 crc32c_intel_le_hw_byte(u32 crc, unsigned char const *data, size_t length)
{
#ifndef __EULER_AARCH64__
    if (!data)
    {
        return crc;
    }

    while (length--)
    {
        __asm__ __volatile__(
        ".byte 0xf2, 0xf, 0x38, 0xf0, 0xf1"
        :"=S"(crc)
        :"0"(crc), "c"(*data)
        );
        data++;
    }
#endif
    return crc;
}

/*****************************************************************************
Function    : epfront_crc32
Description : crc32 calculation function for data of any length
Input       : const void * p
              u32 len
              u32 * p_crc
Output      : u32 * p_crc
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_crc32(const void *p, u32 len, u32* p_crc)
{
#ifndef __EULER_AARCH64__
    u32 crc;
    unsigned int iquotient = len / SCALE_F;
    unsigned int iremainder = len % SCALE_F;
    unsigned long *ptmp = (unsigned long *)p;

    if (NULL == p || NULL == p_crc)
    {
        epfront_err_limit("illegal para");
        return -1;
    }
    crc = *p_crc;

    while (iquotient--) {
        __asm__ __volatile__(
        ".byte 0xf2, " REX_PRE "0xf, 0x38, 0xf1, 0xf1;"
        :"=S"(crc)
        :"0"(crc), "c"(*ptmp)
        );
        ptmp++;
    }

    if (iremainder)
        crc = crc32c_intel_le_hw_byte(crc, (unsigned char *)ptmp,
                                      iremainder);

    *p_crc = crc;
    return 0;
#else
   *p_crc = CRC32_SEED;
	return crc32_arm(p,len,(int)CRC32_SEED,p_crc); 
#endif
}


/*****************************************************************************
Function    : crc_calc_scsi_sgl
Description : crc32 calculation function for scsi sgl
Input       : struct scatterlist* scsi_sg
Input       : int count
Output      : u32
Return      : u32 crc32_result
*****************************************************************************/
static u32 crc_calc_scsi_sgl(struct scatterlist* scsi_sg, int count)
{
    u32 crc32 = CRC32_SEED;
    struct scatterlist* ite_sg = NULL;
    u64 scsi_sg_paddr;
    u32 scsi_sg_len;
    int i;

    if(unlikely(!scsi_sg))
        return crc32;

    for_each_sg(scsi_sg, ite_sg, count, i){
        scsi_sg_paddr = sg_dma_address(ite_sg);
        scsi_sg_len = sg_dma_len(ite_sg);
#ifndef __EULER_AARCH64__
        (void)epfront_crc32(&scsi_sg_paddr, sizeof(u64), &crc32);
        (void)epfront_crc32(&scsi_sg_len, sizeof(u32), &crc32);
#else
        (void)crc32_arm(&scsi_sg_paddr,sizeof(u64),(int)crc32,&crc32);
        (void)crc32_arm(&scsi_sg_len,sizeof(u32),(int)crc32,&crc32);
#endif
    }

    return crc32;
}

/*****************************************************************************
Function    : crc_calc_scsi_data
Description : crc32 calculation function for scsi data
Input       : struct scsi_cmnd * sc
Output      : u32
Return      : u32 crc32_result
*****************************************************************************/
static u32 crc_calc_scsi_data(struct scsi_cmnd* sc)
{
    u32 crc32 = CRC32_SEED;
    struct scatterlist* scsi_sg = NULL;
    struct scatterlist* ite_sg = NULL;
    void* data;
    u32 i;

    if(unlikely(!sc))
        return crc32;

    scsi_sg = scsi_sglist(sc);
    for_each_sg(scsi_sg, ite_sg, scsi_sg_count(sc), i){
        data = sg_virt(ite_sg);
#ifndef __EULER_AARCH64__
        (void)epfront_crc32(data, ite_sg->length, &crc32);
#else
        (void)crc32_arm(data,ite_sg->length,(int)crc32,&crc32);
#endif
    }

    return crc32;
}

/*******************************CRC32 MODULE end*********************************/


/*****************************************************************************
Function    : epfront_get_main_info
Description :
Input       : unsigned int idx
Output      : void
Return      : &gsepmain
*****************************************************************************/
epfront_main_info_t* epfront_get_main_info(unsigned int idx)
{
    if (idx >= SDI_MAX_NRS)
        return NULL;
    return &gsepmain[idx];
}


/*****************************************************************************
Function    : epfront_sv_set_empty
Description : set sv_ctrl to empty
Input       : epfront_sv_controler_t * sv_ctrl
Output      : void
Return      : void
*****************************************************************************/
static void epfront_sv_set_empty(epfront_sv_controler_t* sv_ctrl)
{
    if(sv_ctrl){
        bsfifo_reset_out(&sv_ctrl->bsfifo);
        bsfifo_reset_out(&sv_ctrl->lowpri_bsfifo);
    }
}

/*****************************************************************************
Function    : epfront_sv_assign_task
Description : assign sv_task
Input       : epfront_sv_controler_t * sv_ctrl
              int sv_type
              SV_CALLBACK_PTR func
              void * data
              unsigned int len
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sv_assign_task(epfront_sv_controler_t* sv_ctrl, int type_pri,
    SV_CALLBACK_PTR func, void* data, unsigned int len)
{
    int ret;
    unsigned char buff[EPFRONT_SV_MAX_DATA_SIZE + 16];
    struct epfront_main_info* spmain = NULL;
    struct epfront_sv_handle* sv_task = (struct epfront_sv_handle*)buff;
    int sv_type = EPFRONT_SV_GET_TYPE(type_pri);
    int sv_pri = EPFRONT_SV_GET_PRI(type_pri);

    if(!sv_ctrl || len >= EPFRONT_SV_MAX_DATA_SIZE || !func){
        epfront_err("illegal para: data_len[%d]", len);
        return -EINVAL;
    }

    spmain = container_of(sv_ctrl, struct epfront_main_info, sv_ctrl);

#ifdef EPFRONT_DEBUG
    atomic_inc(&(spmain->stats.sv_todo[sv_type]));
    if(SV_AER_HANDLE == sv_type && data){
        if(((struct ep_aer_cqe*)data)->aer_index < AER_MAX_LIMIT){
            atomic_inc(&(spmain->stats.aer_todo[((struct ep_aer_cqe*)data)->aer_index]));
        } else{
            epfront_err("aer_index[%u] is illegal", ((struct ep_aer_cqe*)data)->aer_index);
        }
    }
#endif

    sv_task->type = sv_type;
    sv_task->func = func;
    if(len && data)
        memcpy(sv_task->data, data, len);

    if(!sv_pri){
        ret = (int)bsfifo_in_spinlocked(&sv_ctrl->bsfifo, sv_task,
            len + sizeof(struct epfront_sv_handle), &sv_ctrl->lock);
    } else{
        ret = (int)bsfifo_in_spinlocked(&sv_ctrl->lowpri_bsfifo, sv_task,
            len + sizeof(struct epfront_sv_handle), &sv_ctrl->lock);
    }

    if(ret){
        ret = 0;
        if(waitqueue_active(&sv_ctrl->wait_queue))
            wake_up(&sv_ctrl->wait_queue);
    } else{
        ret = -ENOMEM;
        epfront_err_limit("bsfifo_in_spinlocked failed, len[%lu], sv_type[%d], sv_pri[%d]",
            len + sizeof(struct epfront_sv_handle), sv_type, sv_pri);
    }

    return ret;
}

/*****************************************************************************
Function    : epfront_sv_thread_resume
Description : resume sv_task thread
Input       : epfront_sv_controler_t * sv_ctrl
Output      : void
Return      : void
*****************************************************************************/
static inline void epfront_sv_thread_resume(epfront_sv_controler_t* sv_ctrl)
{
    if(sv_ctrl)
        sv_ctrl->suspend = 0;
}

/*****************************************************************************
Function    : epfront_sv_thread_suspend
Description : suspend sv_task thread
Input       : epfront_sv_controler_t * sv_ctrl
              unsigned long waittime
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static inline int epfront_sv_thread_suspend(epfront_sv_controler_t* sv_ctrl, unsigned int waittime)
{
    unsigned long timeout;

    if(!sv_ctrl){
        epfront_err("illegal para");
        return -EINVAL;
    }

    sv_ctrl->suspend_complete = 0;
    sv_ctrl->suspend = 1;
    smp_mb();  //mb(); can't ! why?
    if(waittime){
        if(waitqueue_active(&sv_ctrl->wait_queue))
            wake_up(&sv_ctrl->wait_queue);

        timeout = jiffies + msecs_to_jiffies(waittime);

        while(sv_ctrl->task && sv_ctrl->suspend && !sv_ctrl->suspend_complete){
            set_current_state(TASK_INTERRUPTIBLE);
            (void)schedule_timeout((long)HZ);

            if(time_after(jiffies, timeout)){
                set_current_state(TASK_RUNNING);
                epfront_err("wait suspend timeout [%u]msec", waittime);
                return -EBUSY;
            }
        }
        set_current_state(TASK_RUNNING);
    }

    if(IS_ERR_OR_NULL(sv_ctrl->task)){
        epfront_warn("sv thread has been stop");
        return -ENODEV;
    }

    return 0;
}

/*****************************************************************************
Function    : epfront_sv_thread_stop
Description : stop sv_task thread
Input       : epfront_sv_controler_t * sv_ctrl
Output      : void
Return      : void
*****************************************************************************/
void epfront_sv_thread_stop(epfront_sv_controler_t* sv_ctrl)
{
    if(sv_ctrl){
        if(!IS_ERR_OR_NULL(sv_ctrl->task)){
            (void)kthread_stop(sv_ctrl->task);
            sv_ctrl->task = NULL;
        }
    }
}

/*****************************************************************************
Function    : epfront_sv_wait_over
Description : whether sv thread need wait
Input       : epfront_sv_controler_t * sv_ctrl
Output      : bool
Return      : 0 on wait or 1 on wait over
*****************************************************************************/
static bool epfront_sv_wait_over(epfront_sv_controler_t* sv_ctrl)
{
    struct epfront_main_info* smain = NULL;

    if(unlikely(!sv_ctrl)){
        epfront_err("sv_ctrl is NULL");
        return 1;
    }

    smain = container_of(sv_ctrl, struct epfront_main_info, sv_ctrl);

    if(test_bit(EPFRONT_SCSI_STOP, &(smain->epfront_status))
        && test_bit(EPFRONT_SCSI_START, &(smain->epfront_status))){
        return 1;
    }

    if(sv_ctrl->suspend)
        return 1;

    if(!bsfifo_is_empty(&sv_ctrl->bsfifo))
        return 1;

    if(!bsfifo_is_empty(&sv_ctrl->lowpri_bsfifo)){
        if(time_after(jiffies, sv_ctrl->expire + EPFRONT_SV_LOWPRI_EXPIRE_TIME)){
            return 1;
        }
    }

    return 0;
}

/*****************************************************************************
Function    : epfront_sv_thread
Description : sv_task thread function
Input       : void * data
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sv_thread(void* data)
{
    static unsigned long sv_thread_err_interval = 0;
    int ret;
    unsigned int out_size;
    unsigned char buff[EPFRONT_SV_MAX_DATA_SIZE + 16];
    struct epfront_main_info* spmain = (struct epfront_main_info*)data;
    epfront_sv_controler_t* sv_ctrl = NULL;
    struct epfront_sv_handle* sv_task = (struct epfront_sv_handle*)buff;
    int task_pri;

    if(unlikely(!spmain)){
        epfront_err("spmain is NULL");
        return -EINVAL;
    }

    sv_ctrl = &(spmain->sv_ctrl);

    if(unlikely(!sv_ctrl)){
        epfront_err("sv_ctrl is NULL");
        return -EINVAL;
    }

#define EPFRONT_SV_HIGHPRI_BSFIFO 1
#define EPFRONT_SV_LOWPRI_BSFIFO 2

    while(!kthread_should_stop()){

        if(test_bit(EPFRONT_SCSI_STOP, &(spmain->epfront_status)) && test_bit(EPFRONT_SCSI_START, &(spmain->epfront_status))){
            clear_bit(EPFRONT_SCSI_STOP, &(spmain->epfront_status));
            clear_bit(EPFRONT_SCSI_START, &(spmain->epfront_status));
            epfront_sv_thread_resume(sv_ctrl);
            (void)epfront_sv_assign_task(sv_ctrl, SV_TRANS_REINIT, epfront_sv_start_trans,
                  &spmain, sizeof(struct epfront_main_info*));
        }

        if(sv_ctrl->suspend){
            sv_ctrl->suspend_complete = 1;
            set_current_state(TASK_INTERRUPTIBLE);
            (void)schedule_timeout((long)EPFRONT_SV_SUSPEND_TIME);
            continue;
        }
        set_current_state(TASK_RUNNING);

        sv_ctrl->suspend_complete = 0;

        task_pri = 0;
        smp_mb();

        if(time_after(jiffies, sv_ctrl->expire + EPFRONT_SV_LOWPRI_EXPIRE_TIME)){
            if(!bsfifo_is_empty(&sv_ctrl->lowpri_bsfifo)){
                out_size = bsfifo_out(&sv_ctrl->lowpri_bsfifo, sv_task, sizeof(buff));  //need kfifo_out_spinlocked?
                task_pri = out_size ? EPFRONT_SV_LOWPRI_BSFIFO : 0;
            } else{
                sv_ctrl->expire = jiffies;
            }
        }

        if(!task_pri){
            if(!bsfifo_is_empty(&sv_ctrl->bsfifo)){
                out_size = bsfifo_out(&sv_ctrl->bsfifo, sv_task, sizeof(buff));  //need kfifo_out_spinlocked?
                task_pri = out_size ? EPFRONT_SV_HIGHPRI_BSFIFO : 0;
            }
        }

        if(!task_pri){
            (void)wait_event_interruptible_timeout(sv_ctrl->wait_queue,
                epfront_sv_wait_over(sv_ctrl), EPFRONT_SV_SUSPEND_TIME);
        } else{
            //epfront_dbg_limit("run sv_type[%d]'s function", sv_task->type);

#ifdef EPFRONT_DEBUG
            spmain->stats.cur_type = sv_task->type;
            if(SV_AER_HANDLE == spmain->stats.cur_type){
                spmain->stats.cur_subtype = ((struct ep_aer_cqe*)(sv_task->data))->aer_index;
                epfront_info_limit("run aer_index[%d]'s function", spmain->stats.cur_subtype);
            }
#endif

            ret = sv_task->func((void*)sv_task->data);
            if(ret){
                if(printk_timed_ratelimit(&sv_thread_err_interval,EPFRONT_SV_THREAD_ERR_INTERVAL_MSEC)){
                    epfront_info("sv_type[%d] function's ret[%d]", sv_task->type, ret);
                }
                //epfront_err_limit("sv_type[%d] function's ret[%d]", sv_task->type, ret);
            }

#ifdef EPFRONT_DEBUG
            atomic_inc(&(spmain->stats.sv_done[spmain->stats.cur_type]));
            if(SV_AER_HANDLE == spmain->stats.cur_type){
                if(((struct ep_aer_cqe*)(sv_task->data))->aer_index < AER_MAX_LIMIT){
                    atomic_inc(&(spmain->stats.aer_done[((struct ep_aer_cqe*)(sv_task->data))->aer_index]));
                } else{
                    epfront_err("aer_index[%u] is illegal",
                        ((struct ep_aer_cqe*)(sv_task->data))->aer_index);
                }

                epfront_info_limit("aer_index[%d] function's ret[%d]", spmain->stats.cur_subtype, ret);
            }
            spmain->stats.cur_type = -1;
            spmain->stats.cur_subtype = -1;
#endif
        }

        if(EPFRONT_SV_LOWPRI_BSFIFO == task_pri){
            sv_ctrl->expire = jiffies;
        }
    }

#undef EPFRONT_SV_HIGHPRI_BSFIFO
#undef EPFRONT_SV_LOWPRI_BSFIFO

    sv_ctrl->task = NULL;
    epfront_info("supervise_epfront thread out");
    return 0;
}

/*****************************************************************************
Function    : epfront_set_global_config
Description : set epfront global config
Input       : struct ep_global_config * config
Output      : void
Return      : void
*****************************************************************************/
static void epfront_set_global_config(struct ep_global_config* config, struct epfront_main_info *smain)
{
    if(unlikely(NULL == config) || unlikely(NULL == smain)){
        epfront_err("invalid para");
        return ;
    }

    if(config->crc32 <= 1)
        smain->global_config.crc32 = config->crc32;

    if(config->host_n <= EP_MAX_HOST_NUMBER && config->host_n)
        smain->global_config.host_n = config->host_n;

    if(config->max_channel <= EP_MAX_MAX_CHANNEL && config->max_channel)
        smain->global_config.max_channel = config->max_channel;

    if(config->max_id <= EP_MAX_MAX_ID && config->max_id)
        smain->global_config.max_id = config->max_id;

    if(config->max_lun <= EP_MAX_MAX_LUN_PER_HOST && config->max_lun)
        smain->global_config.max_lun = config->max_lun;

    if(config->max_cmd_len <= EP_MAX_CDB_LEN && config->max_cmd_len)
        smain->global_config.max_cmd_len = config->max_cmd_len;

    if(config->max_nr_cmds <= EP_MAX_MAX_CMD_NUMBER && config->max_nr_cmds)
        smain->global_config.max_nr_cmds = config->max_nr_cmds;

    if(config->cmd_per_lun <= EP_MAX_IO_DEPTH_PER_LUN && config->cmd_per_lun)
        smain->global_config.cmd_per_lun = config->cmd_per_lun;

    if(config->sg_count <= EP_MAX_SG_COUNT && config->sg_count)
        smain->global_config.sg_count = config->sg_count;

    if(config->rq_timeout <= EP_MAX_RQ_TIMEOUT && config->rq_timeout)
        smain->global_config.rq_timeout = config->rq_timeout * HZ;

    epfront_info("global_config: crc32[%u], host_n[%u], max_channel[%u], max_id[%u], max_lun[%u],"
        "max_cmd_len[%u], max_nr_cmds[%u], cmd_per_lun[%u], sg_count[%u], rq_timeout[%u]",
        smain->global_config.crc32, smain->global_config.host_n,
        smain->global_config.max_channel, smain->global_config.max_id, smain->global_config.max_lun,
        smain->global_config.max_cmd_len, smain->global_config.max_nr_cmds, smain->global_config.cmd_per_lun,
        smain->global_config.sg_count, smain->global_config.rq_timeout);
}

/*****************************************************************************
Function    : epfront_update_global_config
Description : update epfront global config
Input       : struct ep_global_config * config
Output      : void
Return      : void
*****************************************************************************/
static void epfront_update_global_config(struct ep_global_config* config, struct epfront_main_info *smain)
{
    if(unlikely(NULL == config) || unlikely(NULL == smain)){
        epfront_err("invalid para");
        return ;
    }

    if(config->crc32 <= 1){
        if(smain->global_config.crc32 != config->crc32){
            smain->global_config.crc32 = config->crc32;
            epfront_info("config crc32 change to %u", config->crc32);
        }
    }

    if(config->host_n <= EP_MAX_HOST_NUMBER && config->host_n){
        if(smain->global_config.host_n != config->host_n){
            epfront_warn("config host_n can't change to %u", config->host_n);
        }
    }

    if(config->max_channel <= EP_MAX_MAX_CHANNEL && config->max_channel){
        if(smain->global_config.max_channel != config->max_channel){
            epfront_warn("config max_channel can't change to %u", config->max_channel);
        }
    }

    if(config->max_id <= EP_MAX_MAX_ID && config->max_id){
        if(smain->global_config.max_id != config->max_id){
            epfront_warn("config max_id can't change to %u", config->max_id);
        }
    }

    if(config->max_lun <= EP_MAX_MAX_LUN_PER_HOST && config->max_lun){
        if(smain->global_config.max_lun != config->max_lun){
            epfront_warn("config max_lun can't change to %u", config->max_lun);
        }
    }

    if(config->max_cmd_len <= EP_MAX_CDB_LEN && config->max_cmd_len){
        if(smain->global_config.max_cmd_len != config->max_cmd_len){
            epfront_warn("config max_cmd_len can't change to %u", config->max_cmd_len);
        }
    }

    if(config->max_nr_cmds <= EP_MAX_MAX_CMD_NUMBER && config->max_nr_cmds){
        if(smain->global_config.max_nr_cmds != config->max_nr_cmds){
            epfront_warn("config max_nr_cmds can't change to %u", config->max_nr_cmds);
        }
    }

    if(config->cmd_per_lun <= EP_MAX_IO_DEPTH_PER_LUN && config->cmd_per_lun){
        if(smain->global_config.cmd_per_lun != config->cmd_per_lun){
            epfront_warn("config cmd_per_lun can't change to %u", config->cmd_per_lun);
        }
    }

    if(config->sg_count <= EP_MAX_SG_COUNT && config->sg_count){
        if(smain->global_config.sg_count != config->sg_count){
            epfront_warn("config sg_count can't change to %u", config->sg_count);
        }
    }

    if(config->rq_timeout <= EP_MAX_RQ_TIMEOUT && config->rq_timeout){
        if(smain->global_config.rq_timeout != config->rq_timeout * HZ){
            epfront_warn("config rq_timeout can't change to %u", config->rq_timeout);
        }
    }
}

/*****************************************************************************
Function    : ep_check_state
Description : check epfront channel status
Input       : u16 status
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static inline int ep_check_state(u16 status, struct epfront_main_info *smain)
{
    switch(status >> 1){
        case CQE_STATUS_INVALID_SQTYPE:
            atomic_inc(&(smain->stats.ill_sqtype));
            epfront_err("channel error, INVALID_SQTYPE");
            return 1;
        case CQE_STATUS_INVALID_SQPARA:
            atomic_inc(&(smain->stats.ill_sqpara));
            epfront_err("channel error, INVALID_SQPARA");
            //epfront_sv_assign_task(SV_RESET_TRANS, epfront_sv_trans_reset, NULL, 0);
            return 1;
        default:
            return 0;
    }
}

/*****************************************************************************
Function    : ep_send_io
Description : send io_sqe
Input       : struct ep_io_sqe * sqe
              int seed
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int ep_send_io(struct ep_io_sqe* sqe, int seed, struct epfront_main_info* smain)
{
    int ret = 0;

    if(unlikely(seed >= EPFRONT_IO_SQ_N)){
        epfront_err("seed[%d] is big than EPFRONT_IO_Q_N[%u]", seed, EPFRONT_IO_SQ_N);
        return -EINVAL;
    }

    sqe->entry_type = EP_IO_ENTRY;

    ret = ep_sqe_submit(smain->trans_io_q[seed], sqe, smain->sdev);
    if(ret){
        epfront_err_limit("submit sqe to sq_id[%u] failed", smain->trans_io_q[seed]);
    }

    return ret;
}
#ifdef __ADMIN_CMD__
static int ep_send_adm_cmd_set(struct ep_adm_cmd_set_sqe* sqe, struct epfront_main_info* smain)
{
    int ret = 0;
	if(unlikely(!smain)){
		epfront_err("smain is NULL");
		return -EFAULT;
	}
    sqe->entry_type = EP_ADM_CMD_SET_ENTRY;
    ret = ep_sqe_submit(smain->trans_adm_cmd_set_q, sqe,smain->sdev);
    if(ret){
        epfront_err_limit("submit adm_cmd_set sqe to sq_id[%u] failed", smain->trans_adm_cmd_set_q);
    }
    return ret;
}
#endif
/*****************************************************************************
Function    : ep_send_aer
Description : send aer message
Input       : struct ep_aer_sqe * sqe
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int ep_send_aer(struct ep_aer_sqe* sqe, struct epfront_main_info* smain)
{
    int ret = 0;

    sqe->entry_type = EP_GAB_AER_ENTRY;

    ret = ep_sqe_submit(smain->trans_gab_q, sqe, smain->sdev);
    if(ret){
        epfront_err_limit("submit sqe to sq_id[%u] failed", smain->trans_gab_q);
    }

    return ret;
}

/*****************************************************************************
Function    : ep_send_notify
Description : send notify information to backend
Input       : module_notify_t * m_notify
              module_notify_data_t * data
              u32 timeout
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int ep_send_notify(module_notify_t* m_notify, module_notify_data_t* data, u32 timeout, struct epfront_main_info *smain)
{
    int ret = 0;
    int result = 0;
    u32 len = 0;
    u32 crc32 = 0;

    if(unlikely(!m_notify)){
        epfront_err("illegal para");
        return -EINVAL;
    }

    len = m_notify->data_len;

    if(len){
        if(!data || len <= sizeof(module_notify_data_t)){
            epfront_err("illegal para: opcode[0x%x], data_len is %u", m_notify->opcode, len);
            return -EINVAL;
        }
        if(/*global_config.crc32 && */(DMA_TO_DEVICE == m_notify->direction
            || DMA_BIDIRECTIONAL == m_notify->direction)){
            data->crc32 = CRC32_SEED;
            (void)epfront_crc32(data->data, len - sizeof(module_notify_data_t), &(data->crc32));
        }
    }

    ret = ep_send_cmd(m_notify, sizeof(*m_notify), &result, timeout, smain->sdev);
    if(ret){
        return ret;
    }

    if(len){
        if( (/*m_notify->opcode == EP_NOTIFY_SYNC_CONFIG)
            || ( global_config.crc32 && */(DMA_FROM_DEVICE == m_notify->direction
                    || DMA_BIDIRECTIONAL == m_notify->direction) ) ){
            crc32 = CRC32_SEED;
            (void)epfront_crc32(data->data,  len - sizeof(module_notify_data_t), &crc32);
            if(data->crc32 != crc32){
                atomic_inc(&(smain->stats.crc_err_notify));
                epfront_err("opcode[0x%x], back crc32[%u] != front crc32[%u]", m_notify->opcode, data->crc32, crc32);
                return -EIO;
            }
        }
    }

    return result;
}

/*****************************************************************************
Function    : epfront_check_lun_ctrl
Description : check lun_ctrl and back_uniq_id
Input       : epfront_lun_controler_t * lun_ctrl
              u32 back_uniq_id
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static inline int epfront_check_lun_ctrl(epfront_lun_controler_t* lun_ctrl, u32 back_uniq_id)
{
    if(unlikely(!lun_ctrl || ILLEGAL_BACK_UNIQ_ID(lun_ctrl, back_uniq_id)
        || !lun_ctrl->table)){
        return 1;
    }

    return 0;
}

/*****************************************************************************
Function    : epfront_clear_lun_tbl
Description : clear lun_tbl by using lun_ctrl and back_uniq_id
Input       : epfront_lun_controler_t * lun_ctrl
              u32 back_uniq_id
Output      : void
Return      : void
*****************************************************************************/
static inline void epfront_clear_lun_tbl(epfront_lun_controler_t* lun_ctrl, u32 back_uniq_id)
{
    if(epfront_check_lun_ctrl(lun_ctrl, back_uniq_id)){
        epfront_err("illegal para: back_uniq_id[%u]", back_uniq_id);
        return ;
    }

    lun_ctrl->table[back_uniq_id] = NULL;
}

/*****************************************************************************
Function    : epfront_set_lun_tbl
Description : set lun_tbl by using lun_ctrl and back_uniq_id
Input       : epfront_lun_controler_t * lun_ctrl
              u32 back_uniq_id
              struct epfront_lun_list * lun_lst
Output      : void
Return      : void
*****************************************************************************/
static inline void epfront_set_lun_tbl(epfront_lun_controler_t* lun_ctrl, u32 back_uniq_id, struct epfront_lun_list* lun_lst)
{
    if(epfront_check_lun_ctrl(lun_ctrl, back_uniq_id)){
        epfront_err("illegal para: back_uniq_id[%u]", back_uniq_id);
        return ;
    }

    lun_ctrl->table[back_uniq_id] = lun_lst;
}

/*****************************************************************************
Function    : epfront_get_lun_list
Description : get lun_list_entry by using lun_ctrl and back_uniq_id
Input       : epfront_lun_controler_t * lun_ctrl
              u32 back_uniq_id
Output      : struct epfront_lun_list*
Return      : struct epfront_lun_list*
*****************************************************************************/
static inline struct epfront_lun_list* epfront_get_lun_list(epfront_lun_controler_t* lun_ctrl, u32 back_uniq_id)
{
    if(epfront_check_lun_ctrl(lun_ctrl, back_uniq_id)){
        epfront_err("illegal para: back_uniq_id[%u]", back_uniq_id);
        return NULL;
    }

    return lun_ctrl->table[back_uniq_id];
}

/*****************************************************************************
Function    : epfront_get_ctrl_by_uniq
Description : get host_ctrl by using back_uniq_id
Input       : u32 back_uniq_id
Output      : struct epfront_host_ctrl *
Return      : struct epfront_host_ctrl *
*****************************************************************************/
static struct epfront_host_ctrl * epfront_get_ctrl_by_uniq(u32 back_uniq_id, struct epfront_main_info *smain)
{
    struct epfront_lun_list* lun_lst = NULL;
    u32 host_index;

    lun_lst = epfront_get_lun_list(&(smain->lun_ctrl), back_uniq_id);
    if(unlikely(NULL == lun_lst)){
        epfront_err("lun_lst is invalid, back_uniq_id[%u]", back_uniq_id);
        return NULL;
    }

    host_index = lun_lst->host_index;
    if(unlikely(host_index >= smain->epfront_host_n)){
        epfront_err("lun_lst->host_index[%u] is illegal", host_index);
        return NULL;
    }

    return smain->epfront_hosts[host_index];
}

/*****************************************************************************
Function    : epfront_restore_lun_tbl
Description : restore lun_tbl
Input       : struct list_head * list
Output      : void
Return      : void
*****************************************************************************/
static inline void epfront_restore_lun_tbl(struct list_head *list, struct epfront_main_info *smain)
{
    struct epfront_lun_list *pos = NULL, *tmp = NULL;

    list_for_each_entry_safe(pos,tmp,list,list){
        epfront_set_lun_tbl(&(smain->lun_ctrl), pos->back_uniq_id, pos);
        list_del_init(&pos->list);
        list_add_tail(&pos->list,&(smain->lun_ctrl.list));
    }
}

/*****************************************************************************
Function    : epfront_get_lun_bit
Description : get lun bit
Input       : u32 back_uniq_id
Output      : int
Return      : int
*****************************************************************************/
static inline int epfront_get_lun_bit(u32 back_uniq_id, struct epfront_main_info *smain)
{
    unsigned long loc;
    epfront_lun_controler_t* lun_ctrl = &(smain->lun_ctrl);

    do{
        loc = find_next_zero_bit(lun_ctrl->lun_bits, (unsigned long)lun_ctrl->size, (unsigned long)back_uniq_id);
        if(loc >= lun_ctrl->size){
            loc = find_next_zero_bit(lun_ctrl->lun_bits, (unsigned long)lun_ctrl->size, (unsigned long)0);
            if(loc >= lun_ctrl->size)
                return -ENOMEM;
        }
    }while(test_and_set_bit((int)loc, lun_ctrl->lun_bits) != 0);

    return (int)loc;
}

/*****************************************************************************
Function    : epfront_put_lun_bit
Description : put lun bit
Input       : unsigned int loc
Output      : void
Return      : void
*****************************************************************************/
static inline void epfront_put_lun_bit(unsigned int loc, struct epfront_main_info *smain)
{
    epfront_lun_controler_t* lun_ctrl = &(smain->lun_ctrl);

    if(loc < lun_ctrl->size){
        clear_bit((int)loc, lun_ctrl->lun_bits);
    } else{
        epfront_err("bit location is illegal: %d", loc);
    }
}

/*****************************************************************************
Function    : epfront_get_host_by_uniq
Description : get host by using back_uniq_id
Input       : u32 back_uniq_id
Output      : unsigned int
Return      : unsigned int
*****************************************************************************/
static inline unsigned int epfront_get_host_by_uniq(u32 back_uniq_id, struct epfront_main_info *smain)
{
    return back_uniq_id%(unsigned)(smain->epfront_host_n);
}

/*****************************************************************************
Function    : epfront_get_sq_by_uniq
Description : get sq id by using back_uniq_id
Input       : u32 back_uniq_id
Output      : int
Return      : int
*****************************************************************************/
static inline int epfront_get_sq_by_uniq(u32 back_uniq_id)
{
    return back_uniq_id & EPFRONT_IO_SQ_MASK;
}

/*****************************************************************************
Function    : epfront_scsi_get_sense
Description : get sense
Input       : struct epfront_cmnd_list * c
Output      : int __always_inline
Return      : VOS_OK
*****************************************************************************/
static int __always_inline epfront_scsi_get_sense(struct epfront_cmnd_list *c)
{
    struct scsi_cmnd *sc = NULL;

    if (!c || !c->scsi_cmd)
        return  0;

    sc = c->scsi_cmd;

    if(sc && sc->sense_buffer)
        memcpy(sc->sense_buffer, c->psense_buffer_virt, SCSI_SENSE_BUFFERSIZE);
    return 0;
}

/*****************************************************************************
Function    : sdev_to_ctrl_info
Description : obtain ctrl_info pointer
Input       : struct scsi_device * sdev
Output      : struct epfront_host_ctrl*
Return      : struct epfront_host_ctrl*
*****************************************************************************/
static inline struct epfront_host_ctrl* sdev_to_ctrl_info(struct scsi_device *sdev)
{
    return (struct epfront_host_ctrl*)(shost_priv(sdev->host));
}

/*****************************************************************************
Function    : sc_to_cmnd_list
Description : obtain epfront_cmnd_list pointer by specific scsi command
Input       : struct scsi_cmnd * sc
Output      : struct epfront_cmnd_list*
Return      : struct epfront_cmnd_list*
*****************************************************************************/
static inline struct epfront_cmnd_list* sc_to_cmnd_list(struct scsi_cmnd *sc)
{
    return (struct epfront_cmnd_list*)sc->host_scribble;
}

/*****************************************************************************
Function    : epfront_scmd_printk
Description : print scsi command info
Input       : struct scsi_cmnd * sc
Output      : void
Return      : void
*****************************************************************************/
static inline void epfront_scmd_printk(struct scsi_cmnd *sc)
{
    static unsigned long print_lst_time = 0;
    static unsigned long ioErrRetry_cnt = 0;
    if(unlikely(!sc)){
        epfront_err_limit("sc is NULL");
        return ;
    }
    ioErrRetry_cnt++;
    if(unlikely(sc->retries)){
        /*
        epfront_err_limit("scsi_cmnd retrying: serial_number[%lu] retries[%d], allowed[%d]",
            sc->serial_number, sc->retries, sc->allowed);
        */
        if(0 == print_lst_time || time_after(jiffies, print_lst_time + msecs_to_jiffies(EPFRONT_PRINT_RETRY_INTERVAL_MSEC))){
            epfront_info("scsi_cmnd retrying: serial_number[%lu] retries[%d], allowed[%d], errRetryCnt[%lu]",sc->serial_number, sc->retries, sc->allowed,ioErrRetry_cnt);
            print_lst_time = jiffies;
            ioErrRetry_cnt = 0;
            scsi_print_command(sc);
        }
    }
}

/*****************************************************************************
Function    : __epfront_scsi_device_lookup
Description : lookup scsi device
Input       : struct Scsi_Host * shost
              uint channel
              uint id
              uint lun
Output      : struct scsi_device *
Return      : struct scsi_device *
*****************************************************************************/
static struct scsi_device *__epfront_scsi_device_lookup(struct Scsi_Host *shost,
        uint channel, uint id, uint lun)
{
    struct scsi_device *sdev;

    list_for_each_entry(sdev, &shost->__devices, siblings) {
        if (sdev->channel == channel && sdev->id == id &&
                sdev->lun ==lun && sdev->sdev_state != SDEV_DEL)
            return sdev;
    }

    return NULL;
}

/*****************************************************************************
Function    : epfront_scsi_device_lookup
Description : lookup scsi device
Input       : struct Scsi_Host * shost
              uint channel
              uint id
              uint lun
Output      : struct scsi_device *
Return      : struct scsi_device *
*****************************************************************************/
static struct scsi_device *epfront_scsi_device_lookup(struct Scsi_Host *shost,
        uint channel, uint id, uint lun)
{
    struct scsi_device *sdev;
    unsigned long flags;

    spin_lock_irqsave(shost->host_lock, flags);
    sdev = __epfront_scsi_device_lookup(shost, channel, id, lun);
    if (sdev && scsi_device_get(sdev))
        sdev = NULL;
    spin_unlock_irqrestore(shost->host_lock, flags);

    return sdev;
}

/*****************************************************************************
Function    : epfront_ctrl_get_host_no
Description : get host_no of an epfront_host_ctrl
Input       : struct epfront_host_ctrl * h
Output      : unsigned int
Return      : unsigned int host_no
*****************************************************************************/
unsigned int epfront_ctrl_get_host_no(struct epfront_host_ctrl *h)
{
    if (unlikely(!h)){
        epfront_err("h is NULL");
        return (unsigned int)-1;
    }

    if (unlikely(!h->scsi_host)){
        epfront_err("h has no scsi_host");
        return (unsigned int)-1;
    }

    return h->scsi_host->host_no;
}

/*****************************************************************************
Function    : free_cmd_resource
Description : free resources of epfront_cmnd
Input       : struct epfront_cmnd_list * c
Output      : void
Return      : void
*****************************************************************************/
static void free_cmd_resource(struct epfront_cmnd_list* c, struct epfront_main_info* smain)
{
    struct scsi_cmnd *sc = NULL;

    BUG_ON(!smain->trans_device);

    if(c->scsi_cmnd_paddr)
        dma_unmap_single(smain->trans_device, c->scsi_cmnd_paddr, (size_t)c->scsi_cmnd_len, DMA_TO_DEVICE);

    sc = c->scsi_cmd;
    if(unlikely(!sc)){
        epfront_err_limit("cmnd's scsi_cmnd is NULL");
        return;
    }
    if(scsi_sg_count(sc))
        dma_unmap_sg(smain->trans_device, scsi_sglist(sc), (int)scsi_sg_count(sc), sc->sc_data_direction);
}

/*****************************************************************************
Function    : epfront_host_cmd_free
Description : free buffer of host cmd
Input       : struct epfront_cmnd_list * c
Output      : void
Return      : void
*****************************************************************************/
static void epfront_host_cmd_free(struct epfront_cmnd_list *c)
{
    int i;
    struct epfront_host_ctrl *h = NULL;

    if(unlikely(!c))
        return ;

    h = c->h;
    if(unlikely(!h))
        return;

    if(c->cmd_index >= h->nr_cmds){
        return;
    }

    /* calculate the command's position and clear the specific bit */
    i = (int)c->cmd_index;
    clear_bit(i & (BITS_PER_LONG - 1), h->cmd_pool_bits + (i / BITS_PER_LONG));

    atomic_dec(&(h->cmds_num));

    return;
}

/*****************************************************************************
Function    : epfront_host_cmd_alloc
Description : alloc available buffer for host cmd
Input       : struct epfront_host_ctrl * h
Output      : struct epfront_cmnd_list*
Return      : struct epfront_cmnd_list*
*****************************************************************************/
static struct epfront_cmnd_list* epfront_host_cmd_alloc(struct epfront_host_ctrl* h)
{
    int i;
    struct epfront_cmnd_list *c = NULL;

    /* lookup available buffer by bit */
    do{
        //i = (int)find_next_zero_bit(h->cmd_pool_bits, (unsigned long)h->nr_cmds,1);
        i = (int)find_first_zero_bit(h->cmd_pool_bits, (unsigned long)h->nr_cmds);

        //if not find arm return size+1 (implemented in arch/arm/lib/findbit.S) ,
        //but x86 return size, for universality we can use i >= h->nr_cmnds
        if (i == (int)h->nr_cmds){
            return NULL;
        }
    }while(test_and_set_bit(i & (BITS_PER_LONG - 1),
        h->cmd_pool_bits + (i / BITS_PER_LONG)) != 0);

    atomic_inc(&h->cmd_sn);
    c = h->cmd_pool_tbl[i];

    //init c, phase 1
    memset(c, 0, sizeof(*c));
    INIT_LIST_HEAD(&c->list);
    c->h = h;
    c->cmd_index = i;
    c->scsi_cmd = NULL;
    c->cmd_sn = atomic_read(&h->cmd_sn);

    if(likely(h->pscsi_sense_queue_phy)){
        c->psense_buffer_phy = &h->pscsi_sense_queue_phy[i];
    }else{
        goto err_out;
    }

    if(likely(h->pscsi_sense_queue_virt)){
        c->psense_buffer_virt = &h->pscsi_sense_queue_virt[i];
    }else{
        goto err_out;
    }

    memset(c->psense_buffer_virt, 0, sizeof(*c->psense_buffer_virt));

    atomic_inc(&(h->cmds_num));
    set_bit(CMD_STAT_INIT, &c->status);
    return c;

err_out:
    clear_bit(i & (BITS_PER_LONG - 1), h->cmd_pool_bits + (i / BITS_PER_LONG));
    return NULL;
}

/*****************************************************************************
Function    : epfront_device_block
Description : block device
Input       : struct scsi_device *sdev
Output      : int
Return      : int
*****************************************************************************/
static int epfront_device_block(struct scsi_device *sdev)
{
    struct request_queue *q = sdev->request_queue;
    unsigned long flags;
    int err = 0;

    err = scsi_device_set_state(sdev, SDEV_BLOCK);
    if (err) {
        return err;
    }

    /*
     * The device has transitioned to SDEV_BLOCK.  Stop the
     * block layer from calling the midlayer with this device's
     * request queue.
     */
#if (((LINUX_VERSION_CODE == 266752) && ((defined(RHEL_RELEASE_CODE)) && (RHEL_RELEASE_CODE == 2049))) || \
     ((LINUX_VERSION_CODE == KERNEL_VERSION(4, 18, 0)) && ((defined(RHEL_RELEASE_CODE)) && (RHEL_RELEASE_CODE >= 2048))))
    spin_lock_irqsave(&q->queue_lock, flags);
    blk_mq_stop_hw_queues(q);
    spin_unlock_irqrestore(&q->queue_lock, flags);
#else    
    spin_lock_irqsave(q->queue_lock, flags);
    blk_stop_queue(q);
    spin_unlock_irqrestore(q->queue_lock, flags);
#endif

    (void)dev_printk(KERN_INFO, &sdev->sdev_gendev, "has stoped");
    return 0;
}

/*****************************************************************************
Function    : epfront_device_unblock
Description : unblock device
Input       : struct scsi_device *sdev
Input       : enum scsi_device_state new_state
Output      : int
Return      : int
*****************************************************************************/
static int epfront_device_unblock(struct scsi_device *sdev,
                 enum scsi_device_state new_state)
{
    struct request_queue *q = sdev->request_queue;
    unsigned long flags;
    int err = 0;

    /*
     * Try to transition the scsi device to SDEV_RUNNING or one of the
     * offlined states and goose the device queue if successful.
     */
    err = scsi_device_set_state(sdev, new_state);
    if (err) {
        return err;
    }

#if (((LINUX_VERSION_CODE == 266752) && ((defined(RHEL_RELEASE_CODE)) && (RHEL_RELEASE_CODE == 2049))) || \
     ((LINUX_VERSION_CODE == KERNEL_VERSION(4, 18, 0)) && ((defined(RHEL_RELEASE_CODE)) && (RHEL_RELEASE_CODE >= 2048))))
    spin_lock_irqsave(&q->queue_lock, flags);
    blk_mq_start_hw_queues(q);
    spin_unlock_irqrestore(&q->queue_lock, flags);
#else
    spin_lock_irqsave(q->queue_lock, flags);
    blk_start_queue(q);
    spin_unlock_irqrestore(q->queue_lock, flags);
#endif

    (void)dev_printk(KERN_INFO, &sdev->sdev_gendev, "has start");
    return 0;
}

/*****************************************************************************
Function    : suspend_all_device
Description : suspend all scsi devices of host
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void suspend_all_device(struct epfront_main_info* smain)
{
    int ret;
    unsigned int i;
    struct Scsi_Host *h = NULL;
    struct scsi_device *sdev = NULL;

    BUG_ON(smain->epfront_host_n > EP_MAX_HOST_NUMBER);

    for (i = 0; i < smain->epfront_host_n; i++) {
        if(IS_ERR_OR_NULL(smain->epfront_hosts[i])){
            epfront_warn("efront_hosts[%d] is illegal\n", i);
            continue;
        }
        h = smain->epfront_hosts[i]->scsi_host;
        if(h){
            shost_for_each_device(sdev, h){
                if(sdev->sdev_state == SDEV_RUNNING){
                    ret = epfront_device_block(sdev);
                    //ret = scsi_device_set_state(sdev, SDEV_BLOCK);
                    if(ret){
                        epfront_warn("lun[%d:%d:%d:%llu] state %d can't change to SDEV_BLOCK",
                            smain->epfront_hosts[i]->sys_host_id,sdev->channel,sdev->id,(u64)sdev->lun,sdev->sdev_state);
                    }
                }
            }

            epfront_info("suspend host %d's device", smain->epfront_hosts[i]->sys_host_id);
        }
    }
}

/*****************************************************************************
Function    : suspend_only_host
Description : only suspend host
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void suspend_only_host(struct epfront_main_info* smain)
{
    unsigned int i;
    struct Scsi_Host *h = NULL;

    BUG_ON(smain->epfront_host_n > EP_MAX_HOST_NUMBER);

    for (i = 0; i < smain->epfront_host_n; i++) {
        if(IS_ERR_OR_NULL(smain->epfront_hosts[i])){
            epfront_warn("efront_hosts[%d] is illegal\n", i);
            continue;
        }
        h = smain->epfront_hosts[i]->scsi_host;
        if(h){
            scsi_block_requests(h);
            epfront_info("suspend only host %d", smain->epfront_hosts[i]->sys_host_id);
        }
    }
}

/*****************************************************************************
Function    : suspend_all_host
Description : suspend all host of scsi devices of
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void suspend_all_host(struct epfront_main_info *smain)
{
    int ret;
    unsigned int i;
    struct Scsi_Host *h = NULL;
    struct scsi_device *sdev = NULL;

    BUG_ON(smain->epfront_host_n > EP_MAX_HOST_NUMBER);

    for (i = 0; i < smain->epfront_host_n; i++) {
        if(IS_ERR_OR_NULL(smain->epfront_hosts[i])){
            epfront_warn("efront_hosts[%d] is illegal\n", i);
            continue;
        }
        h = smain->epfront_hosts[i]->scsi_host;
        if(h){
            shost_for_each_device(sdev, h) {
                if(sdev->sdev_state == SDEV_RUNNING){
                    ret = epfront_device_block(sdev);
                    //ret = scsi_device_set_state(sdev, SDEV_BLOCK);
                    if(ret){
                        epfront_warn("lun[%d:%d:%d:%llu] state %d can't change to SDEV_BLOCK",
                            smain->epfront_hosts[i]->sys_host_id,sdev->channel,sdev->id,(u64)sdev->lun,sdev->sdev_state);
                    }
                }
            }

            scsi_block_requests(h);
            epfront_info("suspend host %d", smain->epfront_hosts[i]->sys_host_id);
        }
    }
}

/*****************************************************************************
Function    : resume_all_host
Description : resume host of all scsi devices
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void resume_all_host(struct epfront_main_info* smain)
{
    int ret;
    unsigned int i;
    struct Scsi_Host *h = NULL;
    struct scsi_device *sdev = NULL;

    BUG_ON(smain->epfront_host_n > EP_MAX_HOST_NUMBER);

    for (i = 0; i < smain->epfront_host_n; i++) {
        if(IS_ERR_OR_NULL(smain->epfront_hosts[i])){
            epfront_warn("efront_hosts[%d] is illegal\n", i);
            continue;
        }
        h = smain->epfront_hosts[i]->scsi_host;
        if(h){
            scsi_unblock_requests(h);

            shost_for_each_device(sdev, h) {
                ret = epfront_device_unblock(sdev, SDEV_RUNNING);
                //ret = scsi_device_set_state(sdev, SDEV_RUNNING);
                if(ret){
                    epfront_err_limit("lun [%d:%d:%d:%llu] set to running failed, old state %d",
                        smain->epfront_hosts[i]->sys_host_id,sdev->channel,sdev->id,(u64)sdev->lun,sdev->sdev_state);
                }
            }

            epfront_info("resume host %d", smain->epfront_hosts[i]->sys_host_id);
        }
    }
}

/*****************************************************************************
Function    : epfront_rmv_async_disk
Description : remove disk from async_list by back_uniq_id
Input       : u32 back_uniq_id
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static inline int epfront_rmv_async_disk(u32 back_uniq_id, struct epfront_main_info *smain)
{
    struct epfront_lun_list* lun_lst = NULL;
    struct epfront_lun_list* tmp = NULL;

    list_for_each_entry_safe(lun_lst, tmp, &(smain->lun_ctrl.async_list), list){
        if(!IS_ERR(lun_lst)){
            if(back_uniq_id == lun_lst->back_uniq_id){
                list_del_init(&lun_lst->list);
                kfree(lun_lst);
                return 0;
            }
        }
    }

    return -ENODEV;
}

/*****************************************************************************
Function    : epfront_free_lun_list
Description : free lun_list
Input       : struct epfront_lun_list * lun_lst
Output      : void
Return      : void
*****************************************************************************/
static inline void epfront_free_lun_list(struct epfront_lun_list* lun_lst, struct epfront_main_info *smain)
{
    if(!IS_ERR_OR_NULL(lun_lst)){
        epfront_put_lun_bit((int)lun_lst->id, smain);
        kfree(lun_lst);
    }
}

/*****************************************************************************
Function    : epfront_alloc_lun_list
Description : alloc lun_list by back_uniq_id and vol_name
Input       : u32 back_uniq_id
              char * vol_name
Output      : struct epfront_lun_list*
Return      : struct epfront_lun_list*
*****************************************************************************/
static struct epfront_lun_list* epfront_alloc_lun_list(u32 back_uniq_id, char* vol_name, struct epfront_main_info *smain)
{
    int ret = 0;
    int loc = 0;
    u32 channel = 0;
    u32 id = 0;
    u32 lun = 0;
    unsigned int host_index = 0;
    struct epfront_host_ctrl* h = NULL;
    struct Scsi_Host* sh = NULL;
    struct scsi_device* sdev = NULL;
    struct epfront_lun_list* lun_lst = NULL;

    if(back_uniq_id >= EP_MAX_UNIQUE_ID){
        epfront_err("back_uniq_id[%u] is too big", back_uniq_id);
        ret = -EINVAL;
        goto err_out;
    }

    if(epfront_get_lun_list(&(smain->lun_ctrl), back_uniq_id)){
        epfront_err("back_uniq_id[%u] has added", back_uniq_id);
        ret = -EEXIST;
        goto err_out;
    }

    loc = epfront_get_lun_bit(back_uniq_id, smain);
    if(loc < 0){
        epfront_err("epfront_get_lun_bit failed, ret[%d], back_uniq_id[%u]",
            loc, back_uniq_id);
        ret = -ENOMEM;
        goto err_out;
    }

    channel = 65535;
    id = loc;
    lun = 0;

    host_index = epfront_get_host_by_uniq(back_uniq_id, smain);
    h = smain->epfront_hosts[host_index];
    if(unlikely(!h || !h->scsi_host)){
        epfront_err("can't happen, host in illegal state, host_index is %d", host_index);
        ret = -EFAULT;
        goto put_lun_bit;
    }

    sh = h->scsi_host;

    sdev = epfront_scsi_device_lookup(sh, channel, id, lun);
    if(sdev){
        scsi_device_put(sdev);
        epfront_err("divice has exist, channel[%u], id[%u], lun[%u]",
            channel, id, lun);
        ret = -EEXIST;
        goto put_lun_bit;
    }

    lun_lst = kzalloc(sizeof(*lun_lst), GFP_KERNEL);
    if(NULL == lun_lst){
        epfront_err("alloc for lun_lst failed, size[%lu]", sizeof(*lun_lst));
        ret = -ENOMEM;
        goto put_lun_bit;
    }

    //for lun_list ,must before scsi_add_device, because scsi_cmd_handle need host_index
    INIT_LIST_HEAD(&lun_lst->list);
    lun_lst->back_uniq_id = back_uniq_id;
    lun_lst->host_index = host_index;
    lun_lst->host = h->sys_host_id;
    lun_lst->channel = channel;
    lun_lst->id = id;
    lun_lst->lun = lun;
    atomic_set(&lun_lst->send_num, 0);
    atomic_set(&lun_lst->recv_num, 0);
    atomic_set(&lun_lst->abort_num, 0);
    atomic_set(&lun_lst->back_abort, 0);
    atomic_set(&lun_lst->crc_error, 0);
    atomic_set(&lun_lst->crc_data_error, 0);
    if(vol_name){
        strncpy(lun_lst->vol_name, vol_name, EP_VOL_NAME_LEN - 1);
        lun_lst->vol_name[EP_VOL_NAME_LEN - 1] = '\0';
    }

    return lun_lst;

put_lun_bit:
    epfront_put_lun_bit(loc, smain);
err_out:
    epfront_err("epfront_get_lun_list failed, back_uniq_id[%u], ret[%d]", back_uniq_id, ret);
    return ERR_PTR((long)ret);
}


struct epfront_getdents{
#if ((LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)) || ((defined(RHEL_RELEASE_CODE)) && (RHEL_RELEASE_CODE == 1797)) \
    || ((LINUX_VERSION_CODE == KERNEL_VERSION(3, 10, 0)) && defined(RHEL_RELEASE_CODE) && (1798 == RHEL_RELEASE_CODE)) \
    || ((LINUX_VERSION_CODE == KERNEL_VERSION(3, 10, 0)) && defined(RHEL_RELEASE_CODE) && (1799 == RHEL_RELEASE_CODE)))
    struct dir_context ctx;
#endif
    char disk_name[EP_DEV_NAME_LEN];
    int found;
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 11, 0) || LINUX_VERSION_CODE==200740
static int filldir_find(void * __buf, const char * name, int len,
            loff_t pos, u64 ino, unsigned int d_type)
{
    struct epfront_getdents* dents = (struct epfront_getdents*)__buf;
#else
static int filldir_find(struct dir_context *ctx, const char *name, int len,
            loff_t pos, u64 ino, unsigned int d_type)
{
    struct epfront_getdents *dents =
        container_of(ctx, struct epfront_getdents, ctx);
#endif

    if(unlikely(!name)) return -EINVAL;
    if(name[0] != 's') return 0;
    if(name[1] != 'd') return 0;

    strncpy(dents->disk_name, name, EP_DEV_NAME_LEN - 1);
    dents->found = 1;

    return 0;
}

/*****************************************************************************
Function    : epfront_get_dev_name
Description : get device name
Input       : struct epfront_lun_list * lun_lst
              struct ep_aer_disk_name * diskname
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_get_dev_name(struct epfront_lun_list* lun_lst, struct ep_aer_disk_name* diskname)
{
    int ret = 0;
    struct file* filp = NULL;
    u32 host;
    u32 channel;
    u32 id;
    u32 lun;
    char road[EPFRONT_ROAD_LEN] = "";
    unsigned long timeout = jiffies + EPFRONT_GET_DEVNAME_TIME_OUT;

    struct epfront_getdents dents = {
#if ((LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)) || ((defined(RHEL_RELEASE_CODE))&&(RHEL_RELEASE_CODE == 1797)) \
        || ((LINUX_VERSION_CODE == KERNEL_VERSION(3, 10, 0)) && defined(RHEL_RELEASE_CODE) && (1798 == RHEL_RELEASE_CODE)) \
        || ((LINUX_VERSION_CODE == KERNEL_VERSION(3, 10, 0)) && defined(RHEL_RELEASE_CODE) && (1799 == RHEL_RELEASE_CODE)))
        .ctx.actor = filldir_find,
#endif
        .disk_name = "",
        .found = 0,
    };

    host = lun_lst->host;
    channel = lun_lst->channel;
    id = lun_lst->id;
    lun = lun_lst->lun;

    (void)snprintf(road, EPFRONT_ROAD_LEN, "/sys/bus/scsi/devices/%d:%d:%d:%d/block", host, channel, id, lun);

    do
    {
        if(IS_ERR_OR_NULL(filp))
            filp = filp_open(road, O_RDONLY, 0);

        if (!IS_ERR_OR_NULL(filp)){

#if ((LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)) || ((defined(RHEL_RELEASE_CODE)) && (RHEL_RELEASE_CODE == 1797)) \
            || ((LINUX_VERSION_CODE == KERNEL_VERSION(3, 10, 0)) && defined(RHEL_RELEASE_CODE) && (1798 == RHEL_RELEASE_CODE)) \
            || ((LINUX_VERSION_CODE == KERNEL_VERSION(3, 10, 0)) && defined(RHEL_RELEASE_CODE) && (1799 == RHEL_RELEASE_CODE)))
            ret = iterate_dir(filp, &dents.ctx);
#else
            ret = vfs_readdir(filp, filldir_find, &dents);
#endif
/*
#if ((LINUX_VERSION_CODE < KERNEL_VERSION(3, 11, 0)) && (RHEL_RELEASE_CODE != 1797))
            ret = vfs_readdir(filp, filldir_find, &dents);
#else
            ret = iterate_dir(filp, &dents.ctx);
#endif
*/
            if(dents.found){
                ret = 0;

                (void)snprintf(lun_lst->dev_name, EP_DEV_NAME_LEN, "/dev/%s", dents.disk_name);
                if(NULL != diskname){
                    memset(diskname->dev_name, 0, EP_DEV_NAME_LEN);
                    (void)snprintf(diskname->dev_name, EP_DEV_NAME_LEN, "/dev/%s", dents.disk_name);
                }
                goto out;
            }
        } else{
            epfront_info_limit("openfile:%s failed", road);
            msleep(EPFRONT_GET_DEVNAME_INTERVAL_MSEC);
            continue;
        }
    } while (time_before(jiffies, timeout));

    epfront_err("get_devname timeout: disk [%u:%u:%u:%u] back_uniq_id[%u]",
                host, channel, id, lun, lun_lst->back_uniq_id);
    ret = -ENODEV;

out:
    if(!IS_ERR_OR_NULL(filp))
        (void)filp_close(filp, NULL);

    return ret;
}

/*****************************************************************************
Function    : epfront_add_disk_without_devname
Description : add disk by using sync disk
Input       : struct epfront_lun_list * lun_lst
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_add_disk_without_devname(struct epfront_lun_list* lun_lst, struct epfront_main_info *smain)
{
    int ret = 0;
    struct Scsi_Host* sh = NULL;
    struct scsi_device* sdev = NULL;

    if(unlikely(IS_ERR_OR_NULL(lun_lst))){
        epfront_err("lun_lst is illegal");
        return -EINVAL;
    }

    epfront_set_lun_tbl(&(smain->lun_ctrl),lun_lst->back_uniq_id, lun_lst);

    sh = smain->epfront_hosts[lun_lst->host_index]->scsi_host;
    sdev = __scsi_add_device(sh, lun_lst->channel, lun_lst->id, lun_lst->lun, (void*)lun_lst);
    if(IS_ERR(sdev)){
        ret = (int)PTR_ERR(sdev);
        epfront_err("scsi_add_device() failed, device to be added: [%u %u %u %u], ret[%d]",
                    lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun, ret);

        goto clear_lun_tbl;
    }
    scsi_device_put(sdev);

    sdev = epfront_scsi_device_lookup(sh, lun_lst->channel, lun_lst->id, lun_lst->lun);
    if(!sdev){
        epfront_err("divice not exist, can'n happen [%u %u %u %u] has added but not find",
                    lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun);
        ret = -EFAULT;
        goto clear_lun_tbl;
    }

    ret = epfront_create_lun_sysfs(lun_lst, &(smain->kobj));
    if(ret){
        epfront_err("epfront_create_lun_sysfs failed, ret[%d]", ret);
        goto rmv_lun;
    }

    list_add_tail(&lun_lst->list, &(smain->lun_ctrl.list));

    epfront_info("Disk [%u %u %u %u] added success, back_uniq_id[%u], dev_name[%s]!",
                 lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun, lun_lst->back_uniq_id, lun_lst->dev_name);

    scsi_device_put(sdev);
    return 0;

    rmv_lun:
    scsi_remove_device(sdev);
    scsi_device_put(sdev);
    clear_lun_tbl:
    epfront_clear_lun_tbl(&(smain->lun_ctrl),lun_lst->back_uniq_id);
    epfront_err("epfront_do_add_disk failed, [%u %u %u %u], back_uniq_id[%u], ret[%d]",
                lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun, lun_lst->back_uniq_id, ret);
    return ret;
}

/*****************************************************************************
Function    : epfront_do_add_disk
Description : add disk
Input       : struct epfront_lun_list * lun_lst
              struct ep_aer_disk_name * diskname
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_do_add_disk(struct epfront_lun_list* lun_lst, struct ep_aer_disk_name* diskname, struct epfront_main_info *smain)
{
    int ret = 0;
    struct Scsi_Host* sh = NULL;
    struct scsi_device* sdev = NULL;

    if(unlikely(IS_ERR_OR_NULL(lun_lst))){
        epfront_err("lun_lst is illegal");
        return -EINVAL;
    }

    epfront_set_lun_tbl(&(smain->lun_ctrl), lun_lst->back_uniq_id, lun_lst);

    sh = smain->epfront_hosts[lun_lst->host_index]->scsi_host;
    sdev = __scsi_add_device(sh, lun_lst->channel, lun_lst->id, lun_lst->lun, (void*)lun_lst);
    if(IS_ERR(sdev)){
        ret = (int)PTR_ERR(sdev);
        epfront_err("scsi_add_device() failed, device to be added: [%u %u %u %u], ret[%d]",
            lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun, ret);

        goto clear_lun_tbl;
    }
    scsi_device_put(sdev);

    sdev = epfront_scsi_device_lookup(sh, lun_lst->channel, lun_lst->id, lun_lst->lun);
    if(!sdev){
        epfront_err("divice not exist, can'n happen [%u %u %u %u] has added but not find",
            lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun);
        ret = -EFAULT;
        goto clear_lun_tbl;
    }

    ret = epfront_get_dev_name(lun_lst, diskname);
    if(ret){
        epfront_err("get_dev_name failed, ret %d", ret);
        goto rmv_lun;
    }

    ret = epfront_create_lun_sysfs(lun_lst, &(smain->kobj));
    if(ret){
        epfront_err("epfront_create_lun_sysfs failed, ret[%d]", ret);
        goto rmv_lun;
    }

    //add to lun_list
    list_add_tail(&lun_lst->list, &(smain->lun_ctrl.list));

    epfront_info("Disk [%u %u %u %u] added success, back_uniq_id[%u], dev_name[%s]!",
        lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun, lun_lst->back_uniq_id, lun_lst->dev_name);

    scsi_device_put(sdev);
    return 0;

rmv_lun:
    scsi_remove_device(sdev);
    scsi_device_put(sdev);
clear_lun_tbl:
    epfront_clear_lun_tbl(&(smain->lun_ctrl), lun_lst->back_uniq_id);
    epfront_err("epfront_do_add_disk failed, [%u %u %u %u], back_uniq_id[%u], ret[%d]",
        lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun, lun_lst->back_uniq_id, ret);
    return ret;

}

/*****************************************************************************
Function    : epfront_rmv_disk
Description : remove disk
Input       : u32 back_uniq_id
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_rmv_disk(u32 back_uniq_id, struct epfront_main_info *smain)
{
    struct epfront_host_ctrl* h = NULL;
    struct Scsi_Host* sh = NULL;
    struct scsi_device* sdev = NULL;
    struct epfront_lun_list* lun_lst = NULL;
    u32 channel = 0;
    u32 id = 0;
    u32 lun = 0;

    if(!epfront_rmv_async_disk(back_uniq_id, smain)){
        epfront_info("disk back_uniq_id[%u] has not synced", back_uniq_id);
        return 0;
    }

    lun_lst = epfront_get_lun_list(&(smain->lun_ctrl), back_uniq_id);
    if(NULL == lun_lst){
        epfront_err("lun_lst is NULL, back_uniq_id[%u]", back_uniq_id);
        return -ENODEV;
    }

    h = epfront_get_ctrl_by_uniq(back_uniq_id, smain);
    if(unlikely(!h)){
        epfront_err("back_uniq_id[%u] has not added", back_uniq_id);
        return -ENODEV;
    }

    sh = h->scsi_host;
    if(unlikely(!sh)){
        epfront_err("h has no scsi_host");
        return -EFAULT;
    }

    channel = lun_lst->channel;
    id = lun_lst->id;
    lun = lun_lst->lun;
    sdev = epfront_scsi_device_lookup(sh, channel, id, lun);
    if(!sdev){
        epfront_err("lookup scsi device sdev failed [%u %u %u %u] ",
            epfront_ctrl_get_host_no(h), channel, id, lun);
        return -ENODEV;
    }

    /* clearing lun tbl here will lead to failure of io return
       and wait of scsi_remove_device function.*/
    //epfront_clear_lun_tbl(back_uniq_id);
    //msleep(3);
//  #if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36))
//      (void)scsi_device_set_state(sdev,SDEV_OFFLINE);
//    #endif

    scsi_remove_device(sdev);
    epfront_info("disk [%u %u %u %u] removed!",
        epfront_ctrl_get_host_no(h), channel, id, lun);

    scsi_device_put(sdev);

    epfront_clear_lun_tbl(&(smain->lun_ctrl), back_uniq_id);
    list_del_init(&lun_lst->list);

    epfront_destroy_lun_sysfs(lun_lst);

    epfront_free_lun_list(lun_lst, smain);

    return 0;
}

/*****************************************************************************
Function    : epfront_add_disk
Description : add disk handler
Input       : struct ep_aer_disk_name * diskname
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_add_disk(struct ep_aer_disk_name* diskname, struct epfront_main_info *smain)
{
    int ret;
    struct epfront_lun_list* lun_lst = NULL;

    lun_lst = epfront_alloc_lun_list(diskname->back_uniq_id, diskname->vol_name, smain);
    if(IS_ERR(lun_lst)){
        ret = (int)PTR_ERR(lun_lst);
        epfront_err("epfront_alloc_lun_list failed, ret[%d]", ret);
        return ret;
    }

    ret = epfront_do_add_disk(lun_lst, diskname, smain);
    if(ret){
        epfront_err("epfront_do_add_disk failed, ret[%d]", ret);
        goto free_lun;
    }

    return 0;

free_lun:
    epfront_free_lun_list(lun_lst, smain);
    return ret;
}

/*****************************************************************************
Function    : epfront_sync_disk
Description : sync disk handler
Input       : u32 back_uniq_id
              char * vol_name
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sync_disk(u32 back_uniq_id, char* vol_name, struct epfront_main_info *smain)
{
    int ret;
    struct epfront_lun_list* lun_lst = NULL;

    lun_lst = epfront_alloc_lun_list(back_uniq_id, vol_name, smain);
    if(IS_ERR(lun_lst)){
        ret = (int)PTR_ERR(lun_lst);
        epfront_err("epfront_alloc_lun_list failed, ret[%d]", ret);
        return ret;
    }

    ret =epfront_add_disk_without_devname(lun_lst, smain);
    if(ret){
        epfront_err("sync add disk [%u:%u:%u:%u] back_uniq_id[%u] failed, add to async list, ret[%d]",
                    lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun, lun_lst->back_uniq_id, ret);
        list_add_tail(&lun_lst->list, &(smain->lun_ctrl.async_list));
    }

    return ret;
}

#ifdef __ADMIN_CMD__
static int epfront_adm_cmd_set_send(struct epfront_adm_cmd_set_ctrl* ctrl, int result, struct epfront_main_info *smain)
{
    struct ep_adm_cmd_set_sqe adm_cmd_set;
    adm_cmd_set.data_len = cpu_to_le32(ctrl->data_len);
    adm_cmd_set.data_phys = cpu_to_le64(ctrl->data_phys);
    adm_cmd_set.last_ret = cpu_to_le32((u32)result);
    adm_cmd_set.crc32 = cpu_to_le32(ctrl->crc32);
    return ep_send_adm_cmd_set(&adm_cmd_set,smain);
}
#endif
/*****************************************************************************
Function    : epfront_aer_send
Description : send aer message
Input       : struct epfront_aer_ctrl * ctrl
              int result
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_aer_send(struct epfront_aer_ctrl* ctrl, int result, struct epfront_main_info *smain)
{
    struct ep_aer_sqe aer;
    aer.aer_index = cpu_to_le16((__u16)(ctrl->aer_index));
    aer.data_len = cpu_to_le32(ctrl->data_len);
    aer.data_phys = cpu_to_le64(ctrl->data_phys);
    aer.last_ret = cpu_to_le32((u32)result);
    aer.crc32 = cpu_to_le32(ctrl->crc32);
    return ep_send_aer(&aer, smain);
}

/*****************************************************************************
Function    : epfront_aer_recv
Description : receive aer message
Input       : struct ep_aer_cqe * cqe
Output      : void
Return      : void
*****************************************************************************/
static void epfront_aer_recv(struct ep_aer_cqe* cqe)
{
    int ret = 0;
    int result = 0;
    struct epfront_aer_ctrl* ctrl = NULL;
    u16 aer_index = 0;
    u32 crc32;
    struct epfront_main_info *spmain = NULL;
	if(!cqe){
        epfront_err("cqe is NULL");
        return ;
    }
    spmain = epfront_get_main_info(cqe->rsvd);
    if(unlikely(!spmain)){
        epfront_err("get spmain fail");
        return ;
    }

    aer_index = le16_to_cpu(cqe->aer_index);
    if(aer_index >= AER_MAX_LIMIT){
        epfront_err("aer_index[%u] is illegal" , aer_index);
        ret = -EINVAL;
        goto errout;
    }

    epfront_dbg("receive aer_index[%u]", aer_index);

    ctrl = &(spmain->aer_ctrl.ctrl_table[aer_index]);

    if(ctrl->data_len && ctrl->data_len <= EP_AER_CQE_EXT){
        if(likely(ctrl->data_virt)){
            memcpy(ctrl->data_virt, cqe->ext_data, ctrl->data_len);
        } else{
            epfront_err("ctrl->data_virt is NULL");
            ret = -EFAULT;
            goto errout;
        }
    }
    else if(ctrl->data_len > EP_AER_CQE_EXT ){
        if (spmain->global_config.crc32) {
            crc32 = CRC32_SEED;
            (void)epfront_crc32(ctrl->data_virt, ctrl->data_len, &crc32);
            if(cqe->crc32 != crc32){
                atomic_inc(&(spmain->stats.crc_err_aen));
                epfront_err("crc check failed cqe_crc[%x] crc[%x], aer_index[%u]",cqe->crc32,crc32,ctrl->aer_index);
                result = -EDOM;
                goto RSP_AER;
            }
        }
    }

    if(likely(ctrl->recv)){
        result = ctrl->recv(ctrl->data_virt, spmain);
    } else{
        result = -EFAULT;
    }

RSP_AER:
    if(spmain->global_config.crc32 && (aer_index == AER_ADD_DISK)){
        ctrl->crc32 = CRC32_SEED;
        (void)epfront_crc32(ctrl->data_virt, ctrl->data_len, &ctrl->crc32);
    }

    if(AER_NEET_RESP == ctrl->ctrl_opt){
        ret = epfront_aer_send(ctrl, result, spmain);
        if(ret){
            epfront_err("epfront_aer_send failed, ret[%d]", ret);
            goto errout;
        }
    #ifdef EPFRONT_DEBUG
        atomic_inc(&(spmain->stats.aer_send[ctrl->aer_index]));
    #endif
    }

    return ;

errout:
    ///TODO:handle error
    atomic_inc(&(spmain->stats.ill_aer_cqe));
    epfront_err("aer handle error, ret[%d]", ret);
    return ;
}

/*****************************************************************************
Function    : epfront_io_send
Description : send io
Input       : struct epfront_cmnd_list * c
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_io_send(struct epfront_cmnd_list* c, struct epfront_main_info* smain)
{
    int ret = 0;
    struct ep_io_sqe io = {0};
    struct scsi_cmnd *sc = NULL;
    //struct sdi_iod *iod = NULL;
    struct epfront_cmnd_node *cnode = NULL;
    struct cmnd_dma *dma_info = NULL;
    struct scatterlist* scsi_sg = NULL;
    //struct scatterlist* iod_sg = NULL;
    struct scatterlist* ite_sg = NULL;
    dma_addr_t cmnd_mapping = 0;
    u32 cmnd_len = 0;
    u32 nents = 0;
    //int i = 0;
    u32 crc32_sgl = 0, crc32_data = 0;
    u32 c_cmd_index = 0, c_cmd_sn = 0;
    struct epfront_host_ctrl* h = c->h;
    int num_map_sg = 0;

    BUG_ON(!smain->trans_device);

    sc = c->scsi_cmd;
    cnode = container_of(c, struct epfront_cmnd_node, cmnd);
    dma_info = &(cnode->dma_info);
#if (LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 18))
    cmnd_len = COMMAND_SIZE(sc->cmnd[0]));
#else
    cmnd_len = scsi_command_size(sc->cmnd);
#endif

    OPCODE_SET_DIRECTION(io.opcode, sc->sc_data_direction);

    if(likely(cmnd_len <= EP_SQ_MAX_CDB_LEN)){
        memcpy(io.scsi_cmnd_info.cdb, sc->cmnd, cmnd_len);
        OPCODE_SET_CDB_CTL(io.opcode, IO_CDB_IN_SQ);
    } else{
        cmnd_mapping = dma_map_single(smain->trans_device, sc->cmnd, (int)cmnd_len, DMA_TO_DEVICE);
        if(dma_mapping_error(smain->trans_device, cmnd_mapping)){
            epfront_err("dma map failed");
            ret = -ENOSPC;
            goto errout;
        }
        c->scsi_cmnd_paddr = cmnd_mapping;
        c->scsi_cmnd_len = cmnd_len;

        io.scsi_cmnd_info.cdb_info.scsi_cmnd_len = cpu_to_le32(cmnd_len);
        io.scsi_cmnd_info.cdb_info.scsi_cmnd_paddr = cpu_to_le64(cmnd_mapping);
        OPCODE_SET_CDB_CTL(io.opcode, IO_CDB_BY_SQ);
    }

    scsi_sg = scsi_sglist(sc);
    ite_sg = scsi_sg;
    nents = scsi_sg_count(sc);
    if(0 == nents){
        OPCODE_SET_SGL_CTL(io.opcode, IO_SGL_NONE);
    } else {
        num_map_sg = dma_map_sg(smain->trans_device, scsi_sg, (int)nents, sc->sc_data_direction);
        if(0 == num_map_sg)
            goto unmap_cmnd_addr;

        if(IO_SGL_IN_SQ == num_map_sg){
            io.scsi_sg_paddr = cpu_to_le64(sg_dma_address(ite_sg));
            io.scsi_sg_len = cpu_to_le32(sg_dma_len(ite_sg));
            OPCODE_SET_SGL_CTL(io.opcode, IO_SGL_IN_SQ);
        } else if(IO_SGL_ON_SQ == num_map_sg){
            io.scsi_sg_paddr = cpu_to_le64(sg_dma_address(ite_sg));
            io.scsi_sg_len = cpu_to_le32(sg_dma_len(ite_sg));
            ite_sg = sg_next(ite_sg);
            io.scsi_sg_ex_paddr = cpu_to_le64(sg_dma_address(ite_sg));
            io.scsi_sg_ex_count = cpu_to_le64((dma_addr_t)sg_dma_len(ite_sg));
            OPCODE_SET_SGL_CTL(io.opcode, IO_SGL_ON_SQ);
        } else{
            ret = sdi_setup_sgl(dma_info, scsi_sg, num_map_sg, scsi_bufflen(sc));
			if(ret < 0){
                goto unmap_sg;
			}

            io.scsi_sg_ex_paddr = cpu_to_le64(dma_info->first_dma_addr);
            io.scsi_sg_ex_count = cpu_to_le32(num_map_sg);
            OPCODE_SET_SGL_CTL(io.opcode, IO_SGL_EX);
        }
    }
    c->data_direction = sc->sc_data_direction;

    //calc crc: sgl and data
    if(smain->global_config.crc32)
        crc32_sgl = crc_calc_scsi_sgl(scsi_sg, num_map_sg);
    if(smain->global_config.crc32 && sc->sc_data_direction == DMA_TO_DEVICE)
        crc32_data = crc_calc_scsi_data(sc);

    io.io_index            = cpu_to_le16((__u16)c->cmd_index);
    io.back_uniq_id        = cpu_to_le16((__u16)c->back_uniq_id);
    io.crc32               = cpu_to_le32(crc32_data);
    io.crc32_sgl           = cpu_to_le32(crc32_sgl);
    io.sense_buffer_phy    = cpu_to_le64((dma_addr_t)(c->psense_buffer_phy));
    io.timeout             = cpu_to_le16((__u16)(sc->request->timeout / HZ));

    c_cmd_index = c->cmd_index;
    c_cmd_sn = c->cmd_sn;

    set_bit(CMD_STAT_SEND_COMP, &c->status);
    spin_lock_irq(&h->lock);
    list_add_tail(&c->list, &h->cmdQ);
    spin_unlock_irq(&h->lock);

#ifdef UVP_X86_ENV
    ret = ep_send_io(&io, epfront_get_sq_by_uniq(c->back_uniq_id), smain);
#else
	ret = ep_send_io(&io, epfront_get_sq_by_uniq(c_cmd_index), smain);
#endif
    if(ret < 0){
        epfront_err_limit("send cmd %dth failed, io.io_index[%d]", c_cmd_sn, io.io_index);
        if(test_and_set_bit(CMD_STAT_RECV_RESP, &c->status)){
            epfront_err_limit("c has be handle, status[0x%lx]", c->status);
            atomic_inc(&h->conflict_num);
            return 0;
        } else{
            goto del_list;
        }
    }

    return 0;

del_list:
    spin_lock_irq(&h->lock);
    list_del_init(&c->list);
    spin_unlock_irq(&h->lock);

unmap_sg:
    if(nents){
        dma_unmap_sg(smain->trans_device, scsi_sg, (int)nents, sc->sc_data_direction);
    }
unmap_cmnd_addr:
    if(!OPCODE_GET_CDB_CTL(io.opcode))
        dma_unmap_single(smain->trans_device, cmnd_mapping, cmnd_len, DMA_TO_DEVICE);

errout:
    set_bit(CMD_STAT_DONE, &c->status);
    epfront_host_cmd_free(c);

    return ret;
}

/*****************************************************************************
Function    : epfront_io_recv
Description : receive io
Input       : struct ep_io_cqe * cqe
Output      : void
Return      : void
*****************************************************************************/
static void epfront_io_recv(struct ep_io_cqe* cqe, struct epfront_main_info *smain)
{
    struct epfront_host_ctrl* h = NULL;
    struct epfront_lun_list* lun_lst = NULL;
    struct epfront_cmnd_list *c = NULL;
    struct scsi_cmnd *sc = NULL;
    int sc_result = 0;
    unsigned long flag = 0;
    u32 host_index;
    u32 back_uniq_id = (u32)le16_to_cpu(cqe->back_uniq_id);
    u32 result = le32_to_cpu(cqe->result);
    u16 io_index = le16_to_cpu(cqe->io_index);
    u16 status = le16_to_cpu(cqe->status);

    lun_lst = epfront_get_lun_list(&(smain->lun_ctrl), back_uniq_id);
    if(unlikely(!lun_lst)){
        epfront_err_limit("lun_lst is invalid, back_uniq_id[%u]", back_uniq_id);
        goto reset_trans;
    }

    host_index = lun_lst->host_index;
    if(unlikely(host_index >= smain->epfront_host_n)){
        epfront_err("lun_lst->host_index[%u] is illegal", host_index);
        goto reset_trans;
    }

    h = smain->epfront_hosts[host_index];
    if(unlikely(!h)){
        epfront_err_limit("back_uniq_id[%u] has no host", back_uniq_id);
        goto reset_trans;
    }

    if(unlikely(io_index >= h->nr_cmds)){
        epfront_err_limit("recv->io_index[%u] is bigger than h->nr_cmds(%u)", io_index, h->nr_cmds);
        goto reset_trans;
    }

    c = h->cmd_pool_tbl[io_index];
    if(!test_bit(CMD_STAT_SEND_COMP, &c->status)
        || test_and_set_bit(CMD_STAT_RECV_RESP, &c->status)){
        epfront_err_limit("c has be handle, status[0x%lx]", c->status);
        atomic_inc(&h->conflict_num);
        return ;
    }

    atomic_inc(&lun_lst->recv_num);

    spin_lock_irqsave(&h->lock,flag);
    list_del_init(&c->list);
    spin_unlock_irqrestore(&h->lock,flag);

    sc = c->scsi_cmd;
    switch(status >> 1){
        case CQE_STATUS_ABORT_SQE:
            sc_result = DID_SOFT_ERROR << 16;    //DID_REQUEUE
            atomic_inc(&lun_lst->back_abort);
            epfront_err_limit("back abort: host_no[%u], cmd_sn[%u], cmd_index[%u],"
                "[%u:%u:%u:%u], back_uniq_id[%u], vol_name[%s]",
                epfront_ctrl_get_host_no(c->h), c->cmd_sn, c->cmd_index,
                lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun, lun_lst->back_uniq_id, lun_lst->vol_name);
            goto SET_RESULT;
        case CQE_STATUS_CRC32_FAILED:
            sc_result = DID_SOFT_ERROR << 16;    //DID_REQUEUE
            atomic_inc(&lun_lst->crc_error);
            epfront_err_limit("back crc32 verify error: host_no[%u], cmd_sn[%u], cmd_index[%u],"
                "[%u:%u:%u:%u], back_uniq_id[%u], vol_name[%s]",
                epfront_ctrl_get_host_no(c->h), c->cmd_sn, c->cmd_index,
                lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun, lun_lst->back_uniq_id, lun_lst->vol_name);
            goto SET_RESULT;
        default:
            break;
    }

    if(smain->global_config.crc32 && (sc->sc_data_direction == DMA_FROM_DEVICE) && !result){
        u32 back_crc32 = le32_to_cpu(cqe->crc32);
        u32 front_crc32 = crc_calc_scsi_data(sc);
        if(back_crc32 != front_crc32){
            sc_result = DID_SOFT_ERROR << 16;    //DID_REQUEUE
            atomic_inc(&lun_lst->crc_data_error);
            epfront_err_limit("crc32 verify error, back_crc32[0x%x], front_crc32[0x%x]:"
                "host_no[%u], cmd_sn[%u], cmd_index[%u],"
                "[%u:%u:%u:%u], back_uniq_id[%u], vol_name[%s]",
                back_crc32, front_crc32,
                epfront_ctrl_get_host_no(c->h), c->cmd_sn, c->cmd_index,
                lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun, lun_lst->back_uniq_id, lun_lst->vol_name);
        }
    }

SET_RESULT:
    if (result){
        c->callback = get_ns_time();
        if(epfront_loglevel >= EPFRONT_LOG_DEBUG && printk_ratelimit())
            epfront_err_limit("bio cmd_index[%u] err, back_uniq_id[%u] lun[%u:%u:%u:%u], "
                "result[0x%x], handle time: %lu",
                c->cmd_index, lun_lst->back_uniq_id, lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun,
                result, c->callback - c->submit);
        epfront_scmd_printk(sc);
    }

    if(cqe->sense_len){
        (void)epfront_scsi_get_sense(c);
        if(epfront_loglevel >= EPFRONT_LOG_DEBUG && printk_ratelimit()){
            epfront_err_limit("bio cmd_index[%u], back_uniq_id[%u] lun[%u:%u:%u:%u] sense:",
                c->cmd_index, lun_lst->back_uniq_id, lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun);
            print_data((char*)c->psense_buffer_virt, cqe->sense_len);
        }
    }else if(result){
        if(epfront_loglevel >= EPFRONT_LOG_DEBUG && printk_ratelimit()){
            epfront_err_limit("origin bio cmd_index[%u], back_uniq_id[%u] lun[%u:%u:%u:%u] sense:",
                c->cmd_index, lun_lst->back_uniq_id, lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun);
            print_data((char*)sc->sense_buffer, SCSI_SENSE_BUFFERSIZE);
        }
    }

    sc->result = (int)result;
    if (sc_result)
        sc->result |= sc_result;

    /* set remaining data length to 0 */
    if(!sc->result)
        scsi_set_resid(sc, 0);

    free_cmd_resource(c, smain);
    /* report result of scsi command */
    sc->scsi_done(sc);

    set_bit(CMD_STAT_DONE, &c->status);
    //if(waitqueue_active(&wait)) {
    //    wake_up_interruptible(&wait);
    //}
    wake_up(&(smain->wait));

    /* free command */
    epfront_host_cmd_free(c);

    return ;

reset_trans:
    atomic_inc(&(smain->stats.ill_io_cqe));
    //epfront_sv_assign_task(SV_RESET_TRANS, epfront_sv_trans_reset, NULL, 0);
}

/*****************************************************************************
Function    : epfront_user_scan
Description : device scan
Input       : struct Scsi_Host * shost
              unsigned int channel
              unsigned int id
              unsigned int lun
Output      : int
Return      : VOS_OK
*****************************************************************************/
#if LINUX_VERSION_CODE < (KERNEL_VERSION(3, 17, 0))
static int epfront_user_scan(struct Scsi_Host *shost, unsigned int channel, unsigned int id, unsigned int lun)
{
    epfront_info("hostno: %d, try to rescan device: channel: %d, id: %d, lun: %d",
                shost->host_no, channel, id, lun);

    return 0;
}
#else
static int epfront_user_scan(struct Scsi_Host *shost, unsigned int channel, unsigned int id, unsigned long long lun)
{
    epfront_info("hostno: %d, try to rescan device: channel: %d, id: %d, lun: %llu",
                shost->host_no, channel, id, lun);
    return 0;
}
#endif

/*****************************************************************************
Function    : epfront_scan_finished
Description : callback function for finish device scan
Input       : struct Scsi_Host * sh
Input       : unsigned long elapsed_time
Output      : int
Return      : 1-success 0-keep wait
*****************************************************************************/
static int epfront_scan_finished(struct Scsi_Host *sh, unsigned long elapsed_time)
{
    return 1;
}

/*****************************************************************************
Function    : epfront_scan_start
Description : callback function for start device scan
Input       : struct Scsi_Host * sh
Output      : void
Return      : void
*****************************************************************************/
static void epfront_scan_start(struct Scsi_Host *sh)
{
    return;
}

static struct scsi_transport_template epfront_transportt =
{
    .user_scan = epfront_user_scan,
};

/* Q:when will this function be called?
   A:device will be registered into device_attribute when scsi_add_lun is called
     so that it will be called by "echo 255 > /sys/block/sdx/device/queue_depth".
     In other words, it will be called when use the store function of struct
     sysfs_ops.
 */
/*****************************************************************************
Function    : epfront_change_queue_depth
Description : change queue depth
Input       : struct scsi_device * sdev
              int qdepth
              int reason
Output      : int
Return      : int qdepth
*****************************************************************************/
#if (LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 18) || LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0))
static int epfront_change_queue_depth(struct scsi_device *sdev, int qdepth)
#else
static int epfront_change_queue_depth(struct scsi_device *sdev, int qdepth, int reason)
#endif
{
    struct epfront_host_ctrl *h = sdev_to_ctrl_info(sdev);
#if (LINUX_VERSION_CODE != KERNEL_VERSION(2, 6, 18) || LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0) )
#else
    if (reason != SCSI_QDEPTH_DEFAULT) {
        return -ENOTSUPP;
    }
#endif

    if (qdepth < 1) {
        qdepth = 1;
    } else {
        if (qdepth > (int)h->nr_cmds) {
            qdepth = (int)h->nr_cmds;
        }
    }
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0))
    scsi_change_queue_depth(sdev, qdepth);
#else
    scsi_adjust_queue_depth(sdev, scsi_get_tag_type(sdev), qdepth);
#endif
    return (int)(sdev->queue_depth);
}

/* [zr] slave_alloc <- scsi_alloc_sdev <- scsi_probe_and_add_lun <-
   __scsi_add_device <- scsi_add_device */
#if (LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 18))
    #define QUEUE_FLAG_BIDI        7    /* queue supports bidi requests */
#endif
/*****************************************************************************
Function    : epfront_slave_alloc
Description : alloc slave
Input       : struct scsi_device * sdev
Output      : int
Return      : VOS_OK
*****************************************************************************/
static int epfront_slave_alloc(struct scsi_device *sdev)
{
    set_bit(QUEUE_FLAG_BIDI, &sdev->request_queue->queue_flags);
    return 0;
}

/* [zr] slave_configure <- scsi_add_lun <- scsi_probe_and_add_lun <-
   __scsi_add_device <- scsi_add_device */
/*****************************************************************************
Function    : epfront_slave_configure
Description : configure slave
Input       : struct scsi_device * sdev
Output      : int
Return      : VOS_OK
*****************************************************************************/
static int epfront_slave_configure(struct scsi_device *sdev)
{
    unsigned long timeout;
    struct request_queue *q = sdev->request_queue;
    struct epfront_host_ctrl *h = sdev_to_ctrl_info(sdev);

    if(unlikely(!h)){
        //only occurs when the position of c is reused
        epfront_err_limit("h is NULL");
        return 0;
    }

    timeout = h->smain->global_config.rq_timeout;
//    blk_queue_bounce_limit(sdev->request_queue, BLK_BOUNCE_ANY);//
//    blk_queue_dma_alignment(sdev->request_queue, 0);

    //set dma alignment by with DWord
    blk_queue_bounce_limit(sdev->request_queue, BLK_BOUNCE_HIGH);

    /*set the dma's descriptor is 4 bytes align*/
    blk_queue_dma_alignment(sdev->request_queue, 0x3);


    if(use_cluster)
        blk_queue_max_segment_size(sdev->request_queue, 256 * 1024);

    if(likely(q))
        blk_queue_rq_timeout(q, (unsigned int)timeout);

    return 0;
}

/**
 * epfronth_bios_param - fetch head, sector, cylinder info for a disk
 * @sdev: scsi device struct
 * @bdev: pointer to block device context
 * @capacity: device size (in 512 byte sectors)
 * @params: three element array to place output:
 *              params[0] number of heads (max 255)
 *              params[1] number of sectors (max 63)
 *              params[2] number of cylinders
 *
 * Return nothing.
 */

/*****************************************************************************
Function    : epfronth_bios_param
Description : fetch head, sector, cylinder info for a disk
Input       : struct scsi_device * sdev
              struct block_device * bdev
              sector_t capacity
              int params[]
Output      : int
Return      : VOS_OK
*****************************************************************************/
static int epfronth_bios_param(struct scsi_device *sdev, struct block_device *bdev,
                sector_t capacity, int params[])
{
    int        heads;
    int        sectors;
    sector_t    cylinders;
    ulong         dummy;

    heads = 64;
    sectors = 32;


    dummy = (long)(unsigned) heads * sectors;
    cylinders = capacity;
    sector_div(cylinders, dummy);

    /*
     * Handle extended translation size for logical drives
     * > 1Gb
     */
    if ((ulong)capacity >= 0x200000) {
        heads = 255;
        sectors = 63;
        dummy = (long)(unsigned) heads * sectors;
        cylinders = capacity;
        sector_div(cylinders, dummy);
    }

    /* return result */
    params[0] = heads;
    params[1] = sectors;
    params[2] = (int)cylinders;

    return 0;
}


/*****************************************************************************
Function    : epfront_is_unit_ready
Description : check ready status of scsi device
Input       : struct Scsi_Host * shost
              struct scsi_device * sdev
Output      : bool
Return      : 1-ready 0-not ready
*****************************************************************************/
static inline bool epfront_is_unit_ready(struct Scsi_Host* shost, struct scsi_device* sdev)
{
    if(unlikely(!shost || !sdev)){
        epfront_err_limit("illegal para");
        return 0;
    }

    if(unlikely( !scsi_device_online(sdev)
        || scsi_device_blocked(sdev) || shost->host_self_blocked )){
        return 0;
    }

    return 1;
}

/*****************************************************************************
Function    : epfront_scsi_queue_command, epfront_scsi_queue_command_lck
Description : process scsi command
Input       : struct scsi_cmnd * sc -> data pointer of scsi command
              * done                -> callback of scsi middle layer operation
Output      : int
Return      : 0-success or error code of scsi middle layer
*****************************************************************************/
#ifdef DEF_SCSI_QCMD
static int epfront_scsi_queue_command_lck(struct scsi_cmnd *sc, void (*done)(struct scsi_cmnd *))
#else
static int epfront_scsi_queue_command(struct scsi_cmnd *sc, void (*done)(struct scsi_cmnd *))
#endif
{
    int ret = 0;
    struct epfront_host_ctrl *h = NULL;
    struct epfront_cmnd_list *c = NULL;
    struct scsi_device *sdev = sc->device;
    struct epfront_lun_list* lun_lst = sdev->hostdata;
    struct epfront_main_info* spmain = NULL;

    epfront_scmd_printk(sc);

    h = sdev_to_ctrl_info(sdev);
    if (unlikely(!h) || unlikely(!h->scsi_host) || unlikely(!lun_lst)){
        epfront_err_limit("h is NULL");

        sc->result = (DID_ERROR << 16);
        done(sc);
        return 0;
    }

    spmain = h->smain;
    if (unlikely(!spmain)){
        epfront_err_limit("spmain is NULL");
        sc->result = (DID_ERROR << 16);
        done(sc);
        return 0;
    }

#ifndef __CHECKER__
    spin_unlock_irq(h->scsi_host->host_lock);
#endif

    set_bit(EPFRONT_SCSI_QUEUE_RUN, &(spmain->epfront_status));

    if(test_bit(EPFRONT_SCSI_QUEUE_OFF, &(spmain->epfront_status))){
        set_bit(EPFRONT_SCSI_QUEUE_OFF_DONE, &(spmain->epfront_status));
        epfront_err_limit("queue is off, epfront_status[0x%lx], lun back_uniq_id[%u]",
            spmain->epfront_status, lun_lst->back_uniq_id);
        sc->result = (DID_SOFT_ERROR << 16);
        done(sc);
        goto out;
    }

    if(!epfront_is_unit_ready(h->scsi_host, sdev)){
        epfront_err_limit("lun[%d:%d:%d:%llu] is not ready",
            epfront_ctrl_get_host_no(h), sdev->channel, sdev->id, (u64)sdev->lun);

        sc->result = (DID_SOFT_ERROR << 16);
        done(sc);
        goto out;
    }

    if(INVALID_BACK_ID == lun_lst->back_uniq_id){
        //epfront_err_limit("illegal back_uniq_id[%d]", lun_lst->back_uniq_id);
        sc->result = (DID_BAD_TARGET << 16);
        done(sc);
        goto out;
    }

    c = epfront_host_cmd_alloc(h);
    if (unlikely(c == NULL)){
        epfront_err_limit("epfront_cmd_alloc failed");
        ret = SCSI_MLQUEUE_HOST_BUSY;
        goto out;
    }

    sc->scsi_done = done;
    //save c for abort
    sc->host_scribble = (unsigned char *)c;

    //init c, phase 2
    c->scsi_cmd = sc;
    c->back_uniq_id = lun_lst->back_uniq_id;//epfront_get_uniq_by_tetrad(sdev->id);

    c->submit = get_ns_time();

    ret = epfront_io_send(c, spmain);
    if(unlikely(0 != ret)){
        epfront_err_limit("send cmd failed, ret[%d]", ret);
        ret = SCSI_MLQUEUE_HOST_BUSY;
        goto out;
    }

    atomic_inc(&lun_lst->send_num);

out:
#ifndef __CHECKER__
    spin_lock_irq(h->scsi_host->host_lock);
#endif

    clear_bit(EPFRONT_SCSI_QUEUE_RUN, &(spmain->epfront_status));

    return ret;    //For compatibility, any other non-zero return is treated the same as SCSI_MLQUEUE_HOST_BUSY
}

#ifdef DEF_SCSI_QCMD
static DEF_SCSI_QCMD(epfront_scsi_queue_command)
#endif


/*****************************************************************************
Function    : epfront_abort_cmnd
Description : abort command
Input       : struct epfront_cmnd_list * c
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_abort_cmnd(struct epfront_cmnd_list* c, struct epfront_main_info *smain)
{
    int ret;

    ret = wait_event_timeout(smain->wait, test_bit(CMD_STAT_DONE, &c->status), EPFRONT_EH_ABORT_TIMEOUT);
    if(!ret){
        return -EFAULT;
    } else{
        return 0;
    }
}

/*****************************************************************************
Function    : epfront_eh_abort_handler
Description : eh abort handler
Input       : struct scsi_cmnd * sc
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_eh_abort_handler(struct scsi_cmnd *sc)
{
    int ret;
    struct epfront_host_ctrl *h = NULL;
    struct epfront_cmnd_list *c = NULL;
    struct scsi_device *sdev = NULL;

    c = sc_to_cmnd_list(sc);
    if(!c){
        sdev = sc->device;
        if(sdev){
            epfront_err_limit("io abort: Get cmnd list failed. unit[x:%d:%d:%llu]", sdev->channel, sdev->id, (u64)sdev->lun);
        } else{
            epfront_err_limit("io abort: Get cmnd list failed. maybe it was freed by scsi_done");
        }
        return SUCCESS;
    }

    epfront_info("abort handle: cmd_index[%u] cmd_sn[%u] back_uniq_id[%u] status[0x%lx]",
        c->cmd_index, c->cmd_sn, c->back_uniq_id, c->status);

    h = c->h;
    if(unlikely(!h)){
        //only occurs when the position of c is reused
        epfront_err_limit("h is NULL");
        return SUCCESS;
    }

    ret = epfront_abort_cmnd(c, h->smain);
    if(ret){
        atomic_inc(&h->abort_fail);
        return FAILED;
    } else{
        atomic_inc(&h->abort_succ);
        return SUCCESS;
    }
}

/*****************************************************************************
Function    : epfront_eh_device_reset_handler
Description : eh device reset handler
Input       : struct scsi_cmnd * sc
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_eh_device_reset_handler(struct scsi_cmnd *sc)
{
    int ret;
    int left_io;
    struct epfront_host_ctrl* h = NULL;
    struct epfront_cmnd_list* c = NULL;
    struct scsi_device* sdev = NULL;
    static unsigned long dump_int = 0;

    c = sc_to_cmnd_list(sc);
    if(unlikely(!c)){
        sdev = sc->device;
        if(sdev){
            epfront_err_limit("device reset: Get cmnd list failed. unit[x:%d:%d:%llu]", sdev->channel, sdev->id, (u64)sdev->lun);
        } else{
            epfront_err_limit("device reset: Get cmnd list failed. maybe it was freed by scsi_done");
        }
        return SUCCESS;
    }

    epfront_info("reset handle: cmd_index[%u] cmd_sn[%u] back_uniq_id[%u] status[0x%lx]",
        c->cmd_index, c->cmd_sn, c->back_uniq_id, c->status);

    h = c->h;
    if(unlikely(!h)){
        epfront_err_limit("h is NULL");
        return SUCCESS;
    }

    if (unlikely(NULL == h->smain)){
        epfront_err_limit("h->smain is NULL");
        return SUCCESS;
    }

    epfront_info("flying io number: %d", atomic_read(&h->cmds_num));

    ret = wait_event_timeout(h->smain->wait, list_empty(&h->cmdQ), EPFRONT_EH_RESET_TIMEOUT);
    if(ret){
        atomic_inc(&h->reset_succ);
        epfront_info("wait for flying io success");
        ret = SUCCESS;
    } else{

        if(time_after(jiffies, dump_int
            + msecs_to_jiffies(EPFRONT_DUMP_QUEUES_INTERVAL_MSEC))){
            sdi_dump_queues(h->smain->sdev);
            dump_int = jiffies;
        }

        left_io = epfront_sync_reset_back_state(h->smain);
        epfront_info("epfront_sync_reset_back_state ret[%d]", left_io);

        /*can not reach sdi, what else can be done?*/
        epfront_set_queue_off(h->smain);

        ret = epfront_wait_queue_off(EPFRONT_WAIT_QUEUECOMMAND_MSEC, h->smain);
        if(ret){
            epfront_err("can't happen: epfront_wait_queue_off %d msec failed, ret[%d]",
                EPFRONT_WAIT_QUEUECOMMAND_MSEC, ret);
        }

        epfront_handle_pending_io(h->smain);
        epfront_info("handle pending io done");

        if(left_io < 0){
            atomic_inc(&h->reset_fail);
            ret = FAILED;
        } else{
            atomic_inc(&h->reset_succ);
            ret = SUCCESS;
        }
        (void)epfront_sv_assign_task(&h->smain->sv_ctrl, SV_RESET_HANDLE, epfront_sv_reset_handle,
              &h->smain, sizeof(struct epfront_main_info*));
    }
    epfront_info("reset handle ret[%d]", ret);
    return ret;
}

static struct scsi_host_template epfront_driver_template =
{
    .module                   = THIS_MODULE,
    .name                     = "epfront",
    .proc_name                = "epfront",
    .queuecommand             = epfront_scsi_queue_command,
    .this_id                  = -1,
    .max_sectors              = 0xFFFF,
    .use_clustering            = DISABLE_CLUSTERING,
    .bios_param               = epfronth_bios_param,
    .eh_abort_handler         = epfront_eh_abort_handler,
    .eh_device_reset_handler  = epfront_eh_device_reset_handler,
    .change_queue_depth       = epfront_change_queue_depth,
    .slave_alloc              = epfront_slave_alloc,
    .slave_configure          = epfront_slave_configure,
    .scan_finished            = epfront_scan_finished,
    .scan_start               = epfront_scan_start,
};

/*****************************************************************************
Function    : epfront_check_host_para
Description : check validation of host parameter
Input       : struct epfront_host_para * para
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static inline int epfront_check_host_para(struct epfront_host_para* para)
{
    if (unlikely(!para)){
        epfront_err("invalid para");
        return -EFAULT;
    }

    if (para->max_cmd_len > EP_MAX_CDB_LEN
        || para->max_cmd_len == 0
        || para->sg_count > EP_MAX_SG_COUNT
        || para->max_nr_cmds > EP_MAX_MAX_CMD_NUMBER
        || para->cmd_per_lun > EP_MAX_IO_DEPTH_PER_LUN
        || para->cmd_per_lun > para->max_nr_cmds){
        epfront_err("para is invalid");
        return -EINVAL;
    }

    return 0;
}

/*****************************************************************************
Function    : epfront_host_ctrl_destroy
Description : destory host ctrl
Input       : struct epfront_host_ctrl * h
Output      : void
Return      : void
*****************************************************************************/
static void inline epfront_host_ctrl_destroy(struct epfront_host_ctrl *h, struct epfront_main_info *smain)
{
    int i = 0;
    struct epfront_cmnd_list *c;
	struct epfront_cmnd_node *cnode;

    if(unlikely(!h)){
        epfront_err("h is illegal, NULL");
        return ;
    }

    if(!smain->trans_device){
        epfront_warn("trans_device is NULL");
        return ;
    }

    if (h->pscsi_sense_queue_virt){
        dma_free_coherent(smain->trans_device, h->sense_queue_len, h->pscsi_sense_queue_virt, (dma_addr_t)h->pscsi_sense_queue_phy);
        h->pscsi_sense_queue_virt = NULL;
        h->pscsi_sense_queue_phy = NULL;
        h->sense_queue_len = 0;
        epfront_info("host id[%d] free sense queue", h->sys_host_id);
    }

    if (h->cmd_pool_tbl){
        for (i = 0; i < (int)h->nr_cmds; i++){
            c = h->cmd_pool_tbl[i];
			cnode = container_of(c, struct epfront_cmnd_node, cmnd);
            if (c && cnode){
                if(cnode->dma_info.sg_list){
					dma_free_coherent(smain->trans_device, cnode->dma_info.length, cnode->dma_info.sg_list, cnode->dma_info.first_dma_addr);
                    cnode->dma_info.sg_list = NULL;
                }
				kfree(cnode);
                h->cmd_pool_tbl[i] = NULL;
            }
        }
        kfree(h->cmd_pool_tbl);
        h->cmd_pool_tbl = NULL;
    }

    if (h->cmd_pool_bits){
        kfree(h->cmd_pool_bits);
        h->cmd_pool_bits = NULL;
    }

    epfront_destroy_host_sysfs(h);

    return;
}

/*****************************************************************************
Function    : epfront_host_ctrl_init
Description : initialize host ctrl
Input       : struct epfront_host_ctrl * h
              struct Scsi_Host * sh
              struct epfront_host_para * para
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int inline epfront_host_ctrl_init(struct epfront_host_ctrl *h, struct Scsi_Host *sh, struct epfront_host_para* para, struct epfront_main_info *smain)
{
    int retval = 0;
    //struct epfront_cmnd_list *c;
    struct epfront_cmnd_node *cnode = NULL;
    size_t dma_len = 0;
    int i = 0;

    if(!smain->trans_device){
        epfront_err("trans_device is NULL");
        return -EFAULT;
    }

    if (!h || !sh || !para){
        epfront_err("para is ilegal");
        return -EINVAL;
    }

    memset(h, 0, sizeof(*h));
    spin_lock_init(&h->lock);
    h->scsi_host = sh;
    h->nr_cmds =  para->max_nr_cmds < (EPFRONT_IO_Q_BD_N) ?
        para->max_nr_cmds : (EPFRONT_IO_Q_BD_N);
    //h->nr_cmds =  para->max_nr_cmds < (((EPFRONT_IO_Q_BD_N - 1)/EPFRONT_IO_SQ_N)*EPFRONT_IO_SQ_N) ?
    //  para->max_nr_cmds : (((EPFRONT_IO_Q_BD_N - 1)/EPFRONT_IO_SQ_N)*EPFRONT_IO_SQ_N);

    INIT_LIST_HEAD(&h->cmdQ);
    atomic_set(&h->cmd_sn, 0);
    atomic_set(&(h->cmds_num), 0);

    atomic_set(&h->abort_succ, 0);
    atomic_set(&h->abort_fail, 0);
    atomic_set(&h->reset_succ, 0);
    atomic_set(&h->reset_fail, 0);
    atomic_set(&h->conflict_num, 0);

    h->parent = &(smain->kobj);
    retval = epfront_create_host_sysfs(h, &(smain->kobj));
    if(retval){
        epfront_err("create sysfs for host[%u] failed, ret[%d]",
            epfront_ctrl_get_host_no(h), retval);
        return retval;
    }

    h->cmd_pool_bits = kzalloc(((h->nr_cmds + BITS_PER_LONG - 1) / BITS_PER_LONG) * sizeof(unsigned long), GFP_KERNEL);
    if (!h->cmd_pool_bits){
        retval = -ENOMEM;
        epfront_err("malloc cmd memory for pool bits failed, host[%u].",
            epfront_ctrl_get_host_no(h));
        goto err_out;
    }

    h->cmd_pool_tbl = kzalloc( h->nr_cmds * sizeof(*h->cmd_pool_tbl), GFP_KERNEL);
    if (!h->cmd_pool_tbl){
        retval = -ENOMEM;
        epfront_err("malloc cmd memory for pool failed, host[%u].",
            epfront_ctrl_get_host_no(h));
        goto err_out;
    }

	dma_len = DIV_ROUND_UP(para->sg_count*sizeof(struct sdi_sgl_info),PAGE_SIZE - sizeof(struct sdi_sgl_info))*PAGE_SIZE;
    for (i = 0; i < (int)h->nr_cmds; i++){
	    	/*
        c = kmalloc(sizeof(*c), GFP_KERNEL);
        if (!c) {
            retval = -ENOMEM;
            goto err_out;
        }
        h->cmd_pool_tbl[i] = c;
       */
       cnode = kzalloc(sizeof(struct epfront_cmnd_node), GFP_KERNEL);
	   if(unlikely(!cnode)){
	       retval = -ENOMEM;
		   goto err_out;
       }
	   cnode->dma_info.length = dma_len;
	   cnode->dma_info.sg_list = dma_alloc_coherent(smain->trans_device, dma_len, &(cnode->dma_info.first_dma_addr), GFP_KERNEL);
	   if(!cnode->dma_info.sg_list){
	       epfront_err("alloc dma buffer for cmnd list failed");
		   kfree(cnode);
		   retval = -ENOMEM;
           goto err_out;
	   }
	   h->cmd_pool_tbl[i] = (struct epfront_cmnd_list *)((char *)cnode + offsetof(struct epfront_cmnd_node, cmnd));
    }

    h->sense_queue_len = sizeof(struct scsi_sense_info) * h->nr_cmds;
    h->pscsi_sense_queue_virt = (struct scsi_sense_info *)(dma_alloc_coherent(smain->trans_device, h->sense_queue_len,
        (dma_addr_t*)(void*)&h->pscsi_sense_queue_phy, GFP_KERNEL));
    if (!h->pscsi_sense_queue_virt){
        epfront_err("malloc sense buff failed");
        goto err_out;
    } else {
        epfront_info("sys_host[%d] scsi sense queue alloc success, queue depth[%d]",
            epfront_ctrl_get_host_no(h), h->nr_cmds);
    }

    return 0;

err_out:
    epfront_host_ctrl_destroy(h, smain);
    return retval;
}

/*****************************************************************************
Function    : epfront_unregister_host
Description : unregister host
Input       : struct epfront_host_ctrl * h
Output      : void
Return      : void
*****************************************************************************/
static void epfront_unregister_host(struct epfront_host_ctrl* h, struct epfront_main_info *smain)
{
    struct Scsi_Host *sh = NULL;

    if (unlikely(!h)){
        epfront_err("scsi ctlr info is null.");
        return ;
    }

    epfront_info("in epfront_unregister_host, host[%u]", h->sys_host_id);

    sh = h->scsi_host;
    if (unlikely(!sh)){
        epfront_err("can't happen: h has no scsi_host, host[%u]", h->sys_host_id);
        return ;
    }

    scsi_unblock_requests(sh);

    //(void)epfront_host_abort_all_io(h, DID_SOFT_ERROR << 16);    //DID_ABORT

    scsi_remove_host(sh);
    epfront_host_ctrl_destroy(h, smain);
    scsi_host_put(sh);

    epfront_info("remove host success.");
}

/*****************************************************************************
Function    : epfront_register_host
Description : register host
Input       : struct epfront_host_para * para
Output      : struct epfront_host_ctrl *
Return      : struct epfront_host_ctrl *
*****************************************************************************/
static struct epfront_host_ctrl *epfront_register_host(struct epfront_host_para* para, struct epfront_main_info *smain)
{
    int ret = 0;
    struct epfront_host_ctrl *h = NULL;
    struct Scsi_Host *sh  = NULL;

    if(!smain->trans_device){
        epfront_err("transfer_get_dmadev failed");
        return NULL;
    }

    if (epfront_check_host_para(para)){
        epfront_err("invalid param in host_para when add host!");
        return NULL;
    }

    //register host and alloc memory
    sh = scsi_host_alloc(&epfront_driver_template, sizeof(struct epfront_host_ctrl));
    if (!sh){
        epfront_err("register scsi driver template failed.");
        goto err_out;
    }

    h = shost_priv(sh);
    if (!h){
        epfront_err("get shost priv failed.");
        goto err_out;
    }

    ret = epfront_host_ctrl_init(h, sh, para, smain);
    if (ret){
        epfront_err("init ctrl info failed.");
        h = NULL;
        goto err_out;
    }

    sh->io_port = 0;
    sh->n_io_port = 0;
    sh->this_id = -1;
    sh->max_channel = para->max_channel;
    sh->max_cmd_len = para->max_cmd_len;
    sh->max_lun = para->max_lun;
    sh->max_id = para->max_id;
    sh->can_queue = (int)(para->max_nr_cmds < (((EPFRONT_IO_Q_BD_N - 1)/EPFRONT_IO_SQ_N)*EPFRONT_IO_SQ_N) ?
        para->max_nr_cmds : (((EPFRONT_IO_Q_BD_N - 1)/EPFRONT_IO_SQ_N)*EPFRONT_IO_SQ_N));
    sh->cmd_per_lun = para->cmd_per_lun;
    sh->shost_gendev.parent = NULL;
    sh->sg_tablesize = para->sg_count;
    sh->irq = 0;
    sh->unique_id = sh->irq;
    sh->transportt = &epfront_transportt;

    ret = scsi_add_host(sh, smain->trans_device);  //set dma device, can be set after
    if (ret){
        epfront_err("add scsi host failed, ret[%d]", ret);
        goto err_out;
    }

    h->sys_host_id = sh->host_no;

    epfront_info("register host success, host_no[%u]", h->sys_host_id);
    return h;

err_out:
    if (h){
        epfront_host_ctrl_destroy(h, smain);
    }
    if (sh){
        scsi_host_put(sh);
    }

    epfront_err("register host failed.");
    return NULL;
}

/*****************************************************************************
Function    : epfront_sync_reset_back_state
Description : sync backend device state of reset situation
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sync_reset_back_state(struct epfront_main_info *smain)
{
    module_notify_t m_notify;
    memset((void*)&m_notify, 0, sizeof(m_notify));
    m_notify.opcode = EP_NOTIFY_RESET_BACK_STATE;

    return ep_send_notify(&m_notify, NULL, EPFRONT_NOTIFY_TIMEOUT, smain);
}

/*****************************************************************************
Function    : epfront_sync_check_back_state
Description : sync backend device state
Input       : u32 state
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sync_check_back_state(u32 state, struct epfront_main_info *smain)
{
    int ret = 0;

    module_notify_t m_notify;
    memset((void*)&m_notify, 0, sizeof(m_notify));
    m_notify.opcode = EP_NOTIFY_CHECK_BACK_STATE;

    ret = ep_send_notify(&m_notify, NULL, EPFRONT_NOTIFY_TIMEOUT, smain);
    epfront_info_limit("ep_send_notify EP_NOTIFY_CHECK_BACK_STATE state[%u], ret[%d]", state, ret);

    if(state == (unsigned int)ret){
        return 0;
    } else{
        return -EFAULT;
    }
}

/*****************************************************************************
Function    : epfront_sync_reset_epback
Description : sync backend reset state
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static inline int epfront_sync_reset_epback(struct epfront_main_info *smain)
{
    int ret;
    static unsigned long reset_fail_int;

    ret = epfront_sync_reset_back_state(smain);
    if(ret){
        if(printk_timed_ratelimit(&reset_fail_int, EPFRONT_PRI_RESET_BACK_INTERVAL_MSEC)){
            //epfront_err("epfront_sync_reset_epback failed, ret[%d]", ret);
            epfront_info("epfront_sync_reset_epback failed, ret[%d]", ret);
        }
        return ret;
    } else{
        epfront_info_limit("ep_send_notify EP_NOTIFY_RESET_BACK_STATE ret[%d]", ret);
    }

    ret = epfront_sync_check_back_state(EP_STATE_FRONT_INIT, smain);
    if(ret){
        epfront_err_limit("epfront_sync_check_back_state EP_STATE_FRONT_INIT failed, ret[%d]", ret);
        return ret;
    }

    return 0;
}

/*****************************************************************************
Function    : epfront_notify_devname
Description : notify devices' names to backend
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_notify_devname(struct epfront_main_info *smain)
{
    int ret = 0;
    u32 index = 0;
    int data_len = EP_SYNC_DEVNAME_ONCE_N * sizeof(struct ep_aer_dev_name) + sizeof(module_notify_data_t);
    struct epfront_notify_ctrl* notify_ctrl = NULL;

    module_notify_t m_notify;
    module_notify_data_t* m_notify_data = NULL;

    struct ep_aer_dev_name* sync_devname = NULL;
    struct epfront_lun_list* lun_lst = NULL;
    struct epfront_lun_list* tmp_lst = NULL;

    notify_ctrl = &(smain->notify_ctrl.ctrl_table[EP_NOTIFY_SYNC_DEVNAME]);
    if( !notify_ctrl->data_virt || !notify_ctrl->data_phys
        || (notify_ctrl->data_len < (unsigned)data_len) ){
        epfront_err("notify[%d]'s dma para is illegal, data_virt[0x%p], data_phys[0x%lx], data_len[%u]",
            EP_NOTIFY_SYNC_DEVNAME, notify_ctrl->data_virt, (unsigned long)notify_ctrl->data_phys, notify_ctrl->data_len);
        return -EFAULT;
    }
    memset(notify_ctrl->data_virt, 0, notify_ctrl->data_len);

    m_notify_data = notify_ctrl->data_virt;

    sync_devname = (struct ep_aer_dev_name*)m_notify_data->data;
    list_for_each_entry_safe(lun_lst, tmp_lst, &(smain->lun_ctrl.list), list){
        sync_devname[index].back_uniq_id = lun_lst->back_uniq_id;
        strncpy(sync_devname[index].dev_name, lun_lst->dev_name, EP_DEV_NAME_LEN);
        sync_devname[index].dev_name[EP_DEV_NAME_LEN-1] = '\0';

        ++index;
        if(index == EP_SYNC_DEVNAME_ONCE_N)
            break;
    }

    memset((void*)&m_notify, 0, sizeof(m_notify));
    m_notify.opcode = EP_NOTIFY_SYNC_DEVNAME;
    m_notify.data_len = data_len;
    m_notify.direction= DMA_TO_DEVICE;
    m_notify.paddr_data = notify_ctrl->data_phys;

    ret = ep_send_notify(&m_notify, m_notify_data, EPFRONT_NOTIFY_TIMEOUT, smain);
    epfront_info_limit("ep_send_notify EP_NOTIFY_SYNC_DEVNAME ret[%d]", ret);

    return ret;
}

/*****************************************************************************
Function    : epfront_notify_spec_devname
Description : notify specific device's name to backend
Input       : u32 back_uniq_id
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_notify_spec_devname(u32 back_uniq_id, struct epfront_main_info *smain)
{
    int ret = 0;
    u32 index = 0;
    int data_len = sizeof(struct ep_aer_dev_name) + sizeof(module_notify_t);
    struct epfront_notify_ctrl* notify_ctrl = NULL;

    module_notify_t m_notify;
    module_notify_data_t* m_notify_data = NULL;

    struct ep_aer_dev_name* sync_devname = NULL;
    struct epfront_lun_list* lun_lst = NULL;

    notify_ctrl = &(smain->notify_ctrl.ctrl_table[EP_NOTIFY_SYNC_DEVNAME]);
    if( !notify_ctrl->data_virt || !notify_ctrl->data_phys
        || (notify_ctrl->data_len < (unsigned)data_len) ){
        epfront_err("notify[%d]'s dma para is illegal, data_virt[0x%p], data_phys[0x%lx], data_len[%u]",
            EP_NOTIFY_SYNC_DEVNAME, notify_ctrl->data_virt, (unsigned long)notify_ctrl->data_phys, notify_ctrl->data_len);
        return -EFAULT;
    }
    memset(notify_ctrl->data_virt, 0, notify_ctrl->data_len);

    m_notify_data = notify_ctrl->data_virt;

    sync_devname = (struct ep_aer_dev_name*)m_notify_data->data;
    lun_lst = epfront_get_lun_list(&(smain->lun_ctrl), back_uniq_id);
    if(NULL == lun_lst){
        kfree(m_notify_data);
        epfront_err("lun_lst is NULL, back_uniq_id[%u]", back_uniq_id);
        return -ENODEV;
    }

    sync_devname[index].back_uniq_id = back_uniq_id;
    strncpy(sync_devname[index].dev_name, lun_lst->dev_name, EP_DEV_NAME_LEN);
    sync_devname[index].dev_name[EP_DEV_NAME_LEN-1] = '\0';

    memset((void*)&m_notify, 0, sizeof(m_notify));
    m_notify.opcode = EP_NOTIFY_SYNC_DEVNAME;
    m_notify.data_len = data_len;
    m_notify.direction = DMA_TO_DEVICE;
    m_notify.paddr_data = notify_ctrl->data_phys;

    ret = ep_send_notify(&m_notify, m_notify_data, EPFRONT_NOTIFY_TIMEOUT, smain);
    epfront_info_limit("ep_send_notify EP_NOTIFY_SYNC_DEVNAME ret[%d]", ret);

    return ret;
}

/*****************************************************************************
Function    : epfront_quiry_epback_rdy
Description : query ready state of backend
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static inline int epfront_quiry_epback_rdy(struct epfront_main_info *smain)
{
    int ret;
    static unsigned long last_notify_jiffies = 0;

    module_notify_t m_notify;
    memset((void*)&m_notify, 0, sizeof(m_notify));
    m_notify.opcode = EP_NOTIFY_IS_BACK_RDY;

    ret = ep_send_notify(&m_notify, NULL, EPFRONT_NOTIFY_TIMEOUT, smain);

    if((0 == ret) || (1 == ret)){
        ret = !ret;
    }

    if(ret){
        if(printk_timed_ratelimit(&last_notify_jiffies, EPFRONT_PRI_BACK_RDY_INTERVAL_MSEC)){
            //epfront_err("epfront_quiry_epback_rdy failed, ret[%d]", ret);
            epfront_info("epfront_quiry_epback_rdy failed, ret[%d]", ret);
        }
    } else{
        epfront_info_limit("ep_send_notify EP_NOTIFY_IS_BACK_RDY ret[%d]", ret);
    }

    return ret;
}

/*****************************************************************************
Function    : epfront_sync_global_config
Description : sync global config from backend
Input       : struct ep_global_config * config
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sync_global_config(struct ep_global_config* config, struct epfront_main_info *smain)
{
    int ret = 0;
    module_notify_t m_notify;
    module_notify_data_t* m_notify_data = NULL;
    struct ep_global_config* t_config = NULL;
    int data_len = sizeof(struct ep_global_config) + sizeof(module_notify_data_t);
    struct epfront_notify_ctrl* notify_ctrl = NULL;

    notify_ctrl = &(smain->notify_ctrl.ctrl_table[EP_NOTIFY_SYNC_CONFIG]);
    if( !notify_ctrl->data_virt || !notify_ctrl->data_phys
        || (notify_ctrl->data_len < (unsigned)data_len) ){
        epfront_err("notify[%d]'s dma para is illegal, data_virt[0x%p], data_phys[0x%lx], data_len[%u]",
            EP_NOTIFY_SYNC_CONFIG, notify_ctrl->data_virt, (unsigned long)notify_ctrl->data_phys, notify_ctrl->data_len);
        return -EFAULT;
    }
    memset(notify_ctrl->data_virt, 0, notify_ctrl->data_len);

    m_notify_data = notify_ctrl->data_virt;

    memset((void*)&m_notify, 0, sizeof(m_notify));
    m_notify.opcode = EP_NOTIFY_SYNC_CONFIG;
    m_notify.data_len = data_len;
    m_notify.direction = DMA_FROM_DEVICE;
    m_notify.paddr_data = notify_ctrl->data_phys;

    ret = ep_send_notify(&m_notify, m_notify_data, EPFRONT_NOTIFY_TIMEOUT, smain);
    epfront_info_limit("ep_send_notify EP_NOTIFY_SYNC_CONFIG, ret[%d]", ret);

    if(unlikely(ret)){
        goto out;
    }

    t_config = (struct ep_global_config*)m_notify_data->data;
    memcpy(config, t_config, sizeof(struct ep_global_config));

out:
    return ret;
}

/*****************************************************************************
Function    : epfront_sync_back_config
Description : sync back config
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sync_back_config(struct epfront_main_info *smain)
{
    int ret = 0;
    struct ep_global_config config;

    memset((void*)&config, 0, sizeof(struct ep_global_config));
    ret = epfront_sync_global_config(&config, smain);
    if(ret){
        epfront_err("epfront_sync_global_config failed, ret[%d]", ret);
        return ret;
    }

    if(test_bit(EPFRONT_INIT_MGR, &(smain->epfront_status))){
        epfront_update_global_config(&config, smain);
    } else{
        epfront_set_global_config(&config, smain);
    }

    return 0;
}

/*****************************************************************************
Function    : epfront_sync_devname
Description : sync device name handler
Input       : enum ep_sync_devname_type type
Input       : u32 back_uniq_id
Output      : int
Return      : int
*****************************************************************************/
static int epfront_sync_devname(enum ep_sync_devname_type type, u32 back_uniq_id, struct epfront_main_info *smain)
{
    int ret = 0;

    if(EP_SYNC_ALL == type){
        ret = epfront_notify_devname(smain);
    }
    else{
        ret = epfront_notify_spec_devname(back_uniq_id, smain);
    }

    return ret;
}

/*****************************************************************************
Function    : epfront_sync_result
Description : sync process result handler
Input       : enum ep_sync_type res
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sync_result(enum ep_sync_type res, struct epfront_main_info *smain)
{
    int ret = 0;
    module_notify_t m_notify;

    if(smain->sync_disk_errreport_flag){
        epfront_dbg("already sync");
        return 0;
    }

    memset((void*)&m_notify, 0, sizeof(m_notify));
    m_notify.opcode = EP_NOTIFY_SYNC_DISK_RESULT ;
    m_notify.subcode = res;

    ret = ep_send_notify(&m_notify, NULL, EPFRONT_NOTIFY_TIMEOUT, smain);
    epfront_info_limit("ep_send_notify EP_NOTIFY_SYNC_DISK_RESULT ret[%d]", ret);

    if(!ret){
        smain->sync_disk_errreport_flag++;
    }
    return ret;
}

/*****************************************************************************
Function    : epfront_remove_lun_from_list
Description : remove lun from global lun list
Input       : struct list_head * rm_list
Output      : void
Return      : void
*****************************************************************************/
static void epfront_remove_lun_from_list(struct list_head *rm_list, struct epfront_main_info *smain) {

    struct Scsi_Host *sh = NULL;
    struct scsi_device *sdev = NULL;
    struct epfront_lun_list *pos = NULL, *tmp = NULL;

    list_for_each_entry_safe(pos,tmp,rm_list,list){
        BUG_ON(!smain->epfront_hosts[pos->host_index]);
        sh = smain->epfront_hosts[pos->host_index]->scsi_host;
        sdev = epfront_scsi_device_lookup(sh, pos->channel, pos->id, pos->lun);
        if(!sdev){
            epfront_err("divice not exist, can'n happen [%u %u %u %u] has added but not find",
                        pos->host, pos->channel, pos->id, pos->lun);
            continue;
        }

        (void)epfront_device_unblock(sdev, SDEV_OFFLINE);
        scsi_remove_device(sdev);
        scsi_device_put(sdev);
        list_del_init(&pos->list);
        epfront_destroy_lun_sysfs(pos);
        epfront_info("revmoe lun %s success ",pos->vol_name);
        epfront_free_lun_list(pos, smain);
    }
}

/*****************************************************************************
Function    : epfront_rescan_lun_from_list
Description : rescan lun from lun list
Input       : struct list_head * rescan_list
Output      : int
Return      : int success -> rescan sucess numbers
*****************************************************************************/
/*
static int epfront_rescan_lun_from_list(struct list_head *rescan_list) {

    int success = 0;
    struct Scsi_Host *sh = NULL;
    struct scsi_device *sdev = NULL;
    struct epfront_lun_list *pos = NULL, *tmp = NULL;

    list_for_each_entry_safe(pos,tmp,rescan_list,list){

        if(test_bit(EPFRONT_SCSI_QUEUE_OFF, &epfront_status) || test_bit(EPFRONT_SCSI_FAILFAST, &epfront_status)){
            epfront_warn("scsi has be stoped, epfront_status[0x%lx], rescan success lun number is %d",
                epfront_status, success);
            return -EFAULT;
        }

        BUG_ON(!epfront_hosts[pos->host_index]);
        sh = epfront_hosts[pos->host_index]->scsi_host;
        sdev = epfront_scsi_device_lookup(sh, pos->channel, pos->id, pos->lun);
        if(!sdev){
            epfront_err("divice not exist, can'n happen [%u %u %u %u] has added but not find",
                        pos->host, pos->channel, pos->id, pos->lun);
            continue;
        }
        //epfront_set_lun_tbl should be above scsi_rescan_device, or io will be dropped as invalid back_uniq_id
        scsi_rescan_device(&(sdev->sdev_gendev));

        if(sdev->sdev_state != SDEV_RUNNING){
            epfront_err_limit("rescan lun[%s] back_uniq_id[%u] failed", pos->vol_name, pos->back_uniq_id);
            (void)epfront_sync_result(EP_SYNC_FAILED);
        } else{
            success++;
        }
        scsi_device_put(sdev);

        if(unlikely(need_resched()))
            (void)cond_resched();
    }

    epfront_info("rescan success lun number is %d ", success);

    return success;
}
*/

/*****************************************************************************
Function    : epfront_add_lun_from_list
Description : add lun from lun list
Input       : struct list_head * list
Input       : int * succ_disk
Input       : int * fail_disk
Output      : void
Return      : void
*****************************************************************************/
static int epfront_add_lun_from_list(struct list_head *list, int *succ_disk, int *fail_disk, struct epfront_main_info *smain){

    struct ep_aer_disk_list *pos = NULL, *tmp = NULL;
    int ret = 0;

    list_for_each_entry_safe(pos,tmp,list,list){

        if(test_bit(EPFRONT_SCSI_QUEUE_OFF, &(smain->epfront_status)) || test_bit(EPFRONT_SCSI_FAILFAST, &(smain->epfront_status))){
            epfront_warn("scsi has be stoped, epfront_status[0x%lx], succ_disk[%d], fail_disk[%d]",
                smain->epfront_status, *succ_disk, *fail_disk);
            return -EFAULT;
        }

        ret = epfront_sync_disk(pos->disk.back_uniq_id, pos->disk.vol_name, smain);
        if(ret && (ret != -EEXIST)){
            epfront_err("sync disk failed, back_uniq_id[%u], vol_name[%s], ret[%d]",
                        pos->disk.back_uniq_id, pos->disk.vol_name, ret);
            ++(*fail_disk);
            list_del_init(&pos->list);
            kfree(pos);
            pos = NULL;
            (void)epfront_sync_result(EP_SYNC_FAILED, smain);
        } else{
            ++(*succ_disk);

            list_del_init(&pos->list);
            list_add_tail(&pos->list,&(smain->lun_ctrl.async_devname_list));
            epfront_info("sync disk success, back_uniq_id[%u], vol_name[%s], ret[%d]",
                         pos->disk.back_uniq_id, pos->disk.vol_name, ret);
        }
        if(unlikely(need_resched()))
            (void)cond_resched();
    }

    return 0;
}

/*****************************************************************************
Function    : clear_async_devname_list
Description : clear the async devname list
Input       : struct list_head * list
Output      : void
Return      : void
*****************************************************************************/
static void clear_async_devname_list(struct list_head *list){
    struct ep_aer_disk_list *pos = NULL ,*tmp = NULL;
    list_for_each_entry_safe(pos,tmp,list,list){
        list_del_init(&pos->list);
        kfree(pos);
        pos = NULL;
    }
    INIT_LIST_HEAD(list);
}

/*****************************************************************************
Function    : clear_async_list
Description : clear aysnc list
Input       : struct list_head * list
Output      : void
Return      : void
*****************************************************************************/
static void clear_async_list(struct list_head *list){
    struct epfront_lun_list *pos = NULL ,*tmp = NULL;
    list_for_each_entry_safe(pos,tmp,list,list){
        epfront_dbg("remove dev %s id %d",pos->vol_name,pos->back_uniq_id);
        list_del_init(&pos->list);
        kfree(pos);
        pos = NULL;
    }
    INIT_LIST_HEAD(list);
}

/*****************************************************************************
Function    : epfront_rename_lun_sys
Description : rename lun sys
Input       : struct epfront_lun_list * lun_lst
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_rename_lun_sys(struct epfront_lun_list* lun_lst)
{
    int ret = 0;
    char new_name[32];

    if(unlikely(!lun_lst)){
        epfront_err("illegal para");
        return -EINVAL;
    }

    (void)memset(new_name, 0, 32);
    (void)snprintf(new_name, 32, "lun%u", lun_lst->back_uniq_id);

    (void)kobject_get(&lun_lst->kobj);
    if(strcmp(kobject_name(&lun_lst->kobj), new_name)){
        ret = kobject_rename(&lun_lst->kobj, new_name);
        if(ret){
            epfront_err("lun[%s] sys rename [%s] -> [%s] failed",
                lun_lst->vol_name, kobject_name(&lun_lst->kobj), new_name);
        }
    }
    kobject_put(&lun_lst->kobj);

    return ret;
}

/*****************************************************************************
Function    : epfront_attached_lun_classify
Description : attach lun classify
Input       : struct list_head * sync_list
              struct list_head * rm_list
              struct list_head * rescan_list
Output      : int
Return      : int rescan disk_nr
*****************************************************************************/
static int epfront_attached_lun_classify(struct list_head *sync_list,struct list_head *rm_list,struct list_head *rescan_list){

    struct ep_aer_disk_list *disk_p = NULL, *disk_tmp = NULL;
    struct epfront_lun_list *pos = NULL, *tmp = NULL;
    int sync_disk_nr = 0;
    int rm_disk_nr = 0;
    int rescan_disk_nr = 0;

    //disable old back_uniq_id
    list_for_each_entry(pos, rm_list, list){
        ++rm_disk_nr;
        pos->back_uniq_id = INVALID_BACK_ID;
    }

    list_for_each_entry_safe(disk_p,disk_tmp,sync_list,list){
        ++sync_disk_nr;
        list_for_each_entry_safe(pos,tmp,rm_list,list){
            if(!strcmp(disk_p->disk.vol_name,pos->vol_name)){
                ++rescan_disk_nr;
                --rm_disk_nr;

                pos->back_uniq_id = disk_p->disk.back_uniq_id;

                list_del_init(&pos->list);
                list_add_tail(&pos->list,rescan_list);
                list_del_init(&disk_p->list);
                kfree(disk_p);
                disk_p = NULL;
                break;
            }
        }
    }

    epfront_info("sync_disk_nr[%d], rm_disk_nr[%d], rescan_disk_nr[%d]",
        sync_disk_nr, rm_disk_nr, rescan_disk_nr);

    return rescan_disk_nr;
}


/*****************************************************************************
Function    : epfront_notify_sync_luns
Description : notify sync luns to backend
Input       : struct list_head * add_list
              int * sync_disk_n
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_notify_sync_luns(struct list_head *add_list,int *sync_disk_n, struct epfront_main_info *smain)
{
    int i;
    int ret = 0;
    int data_len = (EP_SYNC_DISK_ONCE_N * sizeof(struct ep_aer_disk))
        + sizeof(module_notify_data_t);
    struct epfront_notify_ctrl* notify_ctrl = NULL;

    module_notify_t m_notify;
    module_notify_data_t* m_notify_data = NULL;

    struct ep_aer_disk* sync_disk = NULL;
    struct ep_aer_disk_list *disk = NULL, *disk_p = NULL, *disk_tmp = NULL;

    notify_ctrl = &(smain->notify_ctrl.ctrl_table[EP_NOTIFY_SYNC_DISK]);
    if( !notify_ctrl->data_virt || !notify_ctrl->data_phys
        || (notify_ctrl->data_len < (unsigned)data_len) ){
        epfront_err("notify[%d]'s dma para is illegal, data_virt[0x%p], data_phys[0x%lx], data_len[%u]",
            EP_NOTIFY_SYNC_DISK, notify_ctrl->data_virt, (unsigned long)notify_ctrl->data_phys, notify_ctrl->data_len);
        return -EFAULT;
    }
    memset(notify_ctrl->data_virt, 0, notify_ctrl->data_len);

    m_notify_data = notify_ctrl->data_virt;

    memset((void*)&m_notify, 0, sizeof(m_notify));
    m_notify.opcode = EP_NOTIFY_SYNC_DISK;
    m_notify.data_len = data_len;
    m_notify.direction = DMA_FROM_DEVICE;
    m_notify.paddr_data = notify_ctrl->data_phys;

    *sync_disk_n = ep_send_notify(&m_notify, m_notify_data, EPFRONT_NOTIFY_TIMEOUT, smain);
    epfront_info_limit("ep_send_notify EP_NOTIFY_SYNC_DISK sync_disk_n[%d]", *sync_disk_n);

    sync_disk = (struct ep_aer_disk*)m_notify_data->data;
    if(*sync_disk_n > 0){
        for (i = 0;i < *sync_disk_n; i++) {
            disk = kmalloc(sizeof(struct ep_aer_disk_list), GFP_KERNEL);
            if(!disk){
                list_for_each_entry_safe(disk_p,disk_tmp,add_list,list){
                    list_del_init(&disk_p->list);
                    kfree(disk_p);
                    disk_p = NULL;
                }
                epfront_err("malloc failed");
                ret = -ENOMEM;
                goto out;
            }

            memcpy((void*)&disk->disk, &sync_disk[i], sizeof(struct ep_aer_disk));
            disk->disk.vol_name[EP_VOL_NAME_LEN - 1] = '\0';
            list_add_tail(&disk->list,add_list);
        }
    } else{
        ret = *sync_disk_n;
    }

out:
    return ret;
}


/*****************************************************************************
Function    : epfront_sync_back_disk
Description : sync disk from backend
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sync_back_disk(struct epfront_main_info *smain)
{
    int ret= 0;
    int sync_disk_succ_n = 0;
    int sync_disk_fail_n = 0;
    int sync_disk_n = 0;

    LIST_HEAD(rm_list);
    LIST_HEAD(rescan_list);
    LIST_HEAD(add_list);

    smain->sync_disk_errreport_flag = 0;

    if(test_and_set_bit(EPFRONT_SCSI_SYNC_DISK, &(smain->epfront_status)))
        return 0;

    if(unlikely(!smain->trans_device)){
        epfront_err("transfer_get_dmadev failed");
        ret = -EFAULT;
        goto err_out;
    }

    ret = epfront_notify_sync_luns(&add_list, &sync_disk_n, smain);
    if(ret){
        epfront_err("sync luns from sdi failed %d", ret);
        goto err_out;
    }

    //usually async_lun_list is empty
    clear_async_list(&(smain->lun_ctrl.async_list));
    clear_async_devname_list(&(smain->lun_ctrl.async_devname_list));

    if(list_empty(&(smain->lun_ctrl.list))){
        epfront_info("lun list is empty");
    } else{
        epfront_info("disable old luns");
        list_replace_init(&(smain->lun_ctrl.list), &rm_list);
        memset(smain->lun_ctrl.table ,0,sizeof(struct epfront_lun_list*)*EP_MAX_UNIQUE_ID);

        sync_disk_succ_n += epfront_attached_lun_classify(&add_list,&rm_list,&rescan_list);

        //remove luns first,otherwise resume_all_host function will set these luns to running state again
        //epfront_remove_lun_from_list(&rm_list);

        epfront_restore_lun_tbl(&rescan_list, smain);

        ret = epfront_sv_rename_luns(&smain);
        if(ret){
            epfront_err("epfront_sv_rename_luns failed, ret[%d]", ret);
        }

        /*resume_all_host();
        ret = epfront_rescan_lun_from_list(&g_lun_ctrl.list);
        if(ret < 0){
            epfront_err("epfront_rescan_lun_from_list failed, ret[%d]", ret);
            goto err_out;
        } else{
            sync_disk_succ_n += ret;
        }*/

    }

    resume_all_host(smain);

    epfront_remove_lun_from_list(&rm_list, smain);

    ret = epfront_add_lun_from_list(&add_list,&sync_disk_succ_n,&sync_disk_fail_n,smain);
    if(ret){
        epfront_err("epfront_add_lun_from_list failed, ret[%d]", ret);
        goto err_out;
    }

    (void)epfront_sv_assign_task(&(smain->sv_ctrl), SV_SYNC_DEVNAME, epfront_sv_sync_devname,
          &smain, sizeof(struct epfront_main_info*));

    if (!list_empty(&(smain->lun_ctrl.async_list))){
        epfront_info("sync disk failed number[%d]", sync_disk_fail_n);
        (void)epfront_sv_assign_task(&(smain->sv_ctrl), SV_SYNC_DISK | EPFRONT_SV_LOW_PRI, epfront_sv_sync_disk,
              &smain, sizeof(struct epfront_main_info*));
    } else{
        (void)epfront_sync_result(EP_SYNC_SUCCESS, smain);
    }

    epfront_info("sync disk success number[%d]", sync_disk_succ_n);

    return 0;

err_out:
    clear_bit(EPFRONT_SCSI_SYNC_DISK, &(smain->epfront_status));
    return ret;
}



#ifdef __ADMIN_CMD__
static int epfront_call_usermode_cmd(char* cmd,const char* cmd_in,const char* filename)
{
    int ret = 0;
    char cmdPath[] = "/bin/bash";
    //char cmd[ADM_CMD_SET_DATA_LEN + 20]={0};
    char* cmdEnvp[]={"HOME=/","PATH=/sbin:/bin:/usr/bin",NULL};
    char* cmdArgv[]={cmdPath,"-c",NULL,NULL};
    char clr_cmd[100];
    if(unlikely(!cmd_in)){
		epfront_err("the cmd_in is NULL");
		return EPFRONT_INNER_ERR_OR_CMD_NULL;
	}
	if(unlikely(!filename)){
		epfront_err("the filename is NULL");
		return EPFRONT_INNER_ERR_OR_CMD_NULL;
	}
    if(unlikely(!cmd)){
        epfront_err(" smain:exec_admcmd_str is NULL");
		return EPFRONT_INNER_ERR_OR_CMD_NULL;
	}
    ret = snprintf(clr_cmd,sizeof(clr_cmd),"cat /dev/null > %s",filename);
    if(ret < 0 || ret > sizeof(clr_cmd)){
		epfront_err("clr_cmd format failure!");
    }
    else{
    	cmdArgv[2] = (char*)clr_cmd;
    	ret = call_usermodehelper(cmdPath,cmdArgv,cmdEnvp,UMH_WAIT_PROC);
    	if(ret != 0){
	        epfront_err("clear file failure");
    	}
	}
    ret = snprintf(cmd,ADM_CMD_SET_DATA_LEN+50,"0>/dev/null 2>/tmp/sdi_err %s > %s",cmd_in,filename);
	if(ret < 0 || ret > ADM_CMD_SET_DATA_LEN+50){
		epfront_err("the admin_command format failure!");
		return EPFRONT_INNER_ERR_OR_CMD_NULL;
	}
    cmdArgv[2] = (char*)cmd;
    
    ret = call_usermodehelper(cmdPath,cmdArgv,cmdEnvp,UMH_WAIT_PROC);
    return ret;
    
}

static int epfront_kernel_read(struct file *file, loff_t* offset,char *addr, unsigned long count)
{
    mm_segment_t old_fs;
    int result;

    old_fs = get_fs();
    set_fs((mm_segment_t)get_ds());
    /* The cast to a user pointer is valid due to the set_fs() */
    result = vfs_read(file, (void __user *)addr, (size_t)count, offset);
    set_fs(old_fs);
    return result;
}

static void epfront_admcmd_exec_timeout(unsigned long data)
{
    struct epfront_main_info* smain = (struct epfront_main_info*)data;
	if(unlikely(!smain)){
		epfront_err("smain is NULL");
		return ;
	}
    (void)schedule_work(&(smain->g_admcmd_timeout_work));
    //*ptimeout_flag = EPFRONT_ADMCMD_EXEC_TIMEOUT_ERRCODE;
    return;
}

static void epfront_adm_cmd_set_operation(struct work_struct* work)
{
    int ret = 0;
    int read_len = 0;
    loff_t pos = 0;
    struct file* fp = NULL;
    u32 crc32;
	u16 recv_len = 0;
    char end_char = 0;
    struct epfront_main_info *smain = container_of(work, struct epfront_main_info, g_adm_cmd_set_work);
  
	recv_len = (smain->g_cqe).aer_index;
    /*epfront_dbg("receive adm_cmd_set message,entry_type[%d]",(smain->g_cqe).entry_type);*/
    /*epfront_dbg("---the recv_len:%d",recv_len);*/
    if(recv_len <= EP_AER_CQE_EXT){
        if(likely((smain->g_adm_cmd_set_ctrl).data_virt)){
            memcpy((smain->g_adm_cmd_set_ctrl).data_virt,(smain->g_cqe).ext_data,recv_len);
            /*epfront_dbg("----the recv_len less then 6 bytes");*/
        }
        else{
            epfront_err("adm_cmd_set_ctrl.data_virt is NULL");
            ret = EPFRONT_CMD_VIRADDRESS_INVALID;
            goto ADM_RESP_NO_CLOSE;
        }
    }
    else if(recv_len > EP_AER_CQE_EXT){
        /*epfront_dbg("----the recv_len more then 6 bytes");*/
        if((smain->global_config).crc32){
            crc32 = CRC32_SEED;
            (void)epfront_crc32((smain->g_adm_cmd_set_ctrl).data_virt, recv_len, &crc32);
            if((smain->g_cqe).crc32 != crc32){
                //增加统计计数?
                epfront_err("amd_cmd_set crc check failure,cqe_crc[%x],crc[%x]",(smain->g_cqe).crc32,crc32);
                ret = EPFRONT_CMD_DATA_FROM_DEVICE_CRC_FAIL;
                goto ADM_RESP_NO_CLOSE;
            }
        }
    }
    /*epfront_dbg("----the message is %s",(char*)((smain->g_adm_cmd_set_ctrl).data_virt));*/
    //开启定时器,防止命令一直执行，进程无法退出，工作队列无法结束
    (smain->g_adm_cmd_timer).expires = jiffies + EPFRONT_ADM_CMD_EXEC_TIMEOUT;
    (smain->g_adm_cmd_timer).data = (unsigned long)(smain);
    add_timer(&(smain->g_adm_cmd_timer));
    mutex_lock(&(smain->adm_timer_inf.timer_exist_mutex));
    smain->adm_timer_inf.adm_cmd_timer_exist = 1;
    mutex_unlock(&(smain->adm_timer_inf.timer_exist_mutex));
    //epfront_dbg("the admin_cmd[%s]",(char*)((smain->g_adm_cmd_set_ctrl).data_virt));
    ret = epfront_call_usermode_cmd(smain->exec_admcmd_str,(smain->g_adm_cmd_set_ctrl).data_virt,smain->ep_cmd_res_filename);
    epfront_info("the admin_cmd[%s] ret[%d]",(char*)((smain->g_adm_cmd_set_ctrl).data_virt),ret);
    if(EPFRONT_INNER_ERR_OR_CMD_NULL == ret){
		epfront_err("the admin_command err or inter err");
		goto ADM_RESP_NO_CLOSE;
	}

    mutex_lock(&(smain->adm_timer_inf.timer_exist_mutex));
    if(1 == smain->adm_timer_inf.adm_cmd_timer_exist){
        
        (void)del_timer_sync(&(smain->g_adm_cmd_timer));
        smain->adm_timer_inf.adm_cmd_timer_exist = 0;
    }
    else{
	
        mutex_unlock(&(smain->adm_timer_inf.timer_exist_mutex));
        ret = EPFRONT_ADM_CMD_EXEC_TIMEOUT_ERR;
        epfront_info("the timeout admcmd has been killed successfully");
        goto ADM_RESP_NO_CLOSE;
    }

    mutex_unlock(&(smain->adm_timer_inf.timer_exist_mutex));
    //子进程调用执行命令的返回值(低)
    /*
      call_ret = ret&0x0ff;
      if(!call_ret){
          epfront_err("the admin_cmd[%s] exec failure,epfront_call_usermode_cmd ret[%d],call_ret[%d]",
                        (char*)((smain->g_adm_cmd_set_ctrl).data_virt),ret,call_ret);
        //错误处理，给后端回复，命令执行错误
      }
      cmd_ret = (ret>>8)&0x0ff;
      */
    //打开文件移到驱动初始化中?????????待优化
    fp = filp_open(smain->ep_cmd_res_filename,O_RDONLY|O_CREAT,0640);
    if(IS_ERR(fp)){
        epfront_err("open file[%s] failure!",smain->ep_cmd_res_filename);
        ret = EPFRONT_CMD_RES_FILE_OPEN_FAIL;
        goto ADM_RESP_NO_CLOSE;
    }

    read_len = epfront_kernel_read(fp,&pos,(char*)((smain->g_adm_cmd_set_ctrl).data_virt),ADM_CMD_SET_DATA_LEN-1);
    if(read_len < 0|| read_len > ADM_CMD_SET_DATA_LEN-1){
        epfront_err("read_len[%d] from file",read_len);
        //读取出错，不传输数据了，直接回复
        ret = EPFRONT_ADM_CMD_FILE_READ_ERR;
        goto ADM_RESP;
    }
    else if(read_len == 0){
        memset((void*)((smain->g_adm_cmd_set_ctrl).data_virt),0,ADM_CMD_SET_DATA_LEN); 
        ret &= ~ADM_CMD_DATA_RESULT_IF_COMPLETED_MASK;         
    }
    else{
        ((char*)((smain->g_adm_cmd_set_ctrl).data_virt))[read_len] = '\0';
        read_len = epfront_kernel_read(fp,&pos,&end_char,1);
        if(1 == read_len){
            //the res_data is not completed
            ret &= ~ADM_CMD_DATA_RESULT_IF_COMPLETED_MASK;
        }
    else{
        //the res_data is completed
        ret |= ADM_CMD_DATA_RESULT_IF_COMPLETED_MASK;
        }
    }
    /*epfront_dbg("--the buf read fron file is:[%s]",(char*)(smain->g_adm_cmd_set_ctrl).data_virt);*/
    if((smain->global_config).crc32){
        (smain->g_adm_cmd_set_ctrl).crc32 = CRC32_SEED;
        (void)epfront_crc32((smain->g_adm_cmd_set_ctrl).data_virt, (smain->g_adm_cmd_set_ctrl).data_len, &(smain->g_adm_cmd_set_ctrl).crc32);
    }

ADM_RESP:
	(void)filp_close(fp, NULL);
ADM_RESP_NO_CLOSE:
    ret = epfront_adm_cmd_set_send(&(smain->g_adm_cmd_set_ctrl),ret,smain);
    if(ret){
        epfront_err("amd_cmd_set handle err,ret[%d]",ret);
    }
         
    return ;
}

static void ep_adm_cmd_set_queue_callback(void * priv_data, void * cqe_data, u16  len, cqe_status_t * head_info)
{
    struct ep_cqe* cqe = cqe_data;
    struct epfront_main_info *spmain = (struct epfront_main_info *)priv_data;
	if(unlikely(!spmain)){
		epfront_err("the spmain is NULL");
		return ;
	}
    if(unlikely(!cqe_data)){
        epfront_err("cqe_data is NULL");
        return ;
    }

    //epfront_dbg("-- before ep_check_state[ep_adm_cmd_set_queue_callback]");
    if(ep_check_state(le16_to_cpu(cqe->status), spmain))
        return ;
    //epfront_dbg("-- after ep_check_state[ep_adm_cmd_set_queue_callback]");
    switch(cqe->entry_type){
        case EP_ADM_CMD_SET_ENTRY:
            
            cqe->data[0] = spmain->index;
            //epfront_info("--the adm_cmd_set message is coming");
            memcpy(&(spmain->g_cqe),cqe,sizeof(struct ep_cqe));
            (void)schedule_work(&(spmain->g_adm_cmd_set_work));
            break;
        default:
            //atomic_inc(&(spmain->g_stats.ill_aer_type));
            epfront_err("entry_type[%d] is illegal", cqe->entry_type);
            break;
        
    }
    
}
#endif
/*****************************************************************************
Function    : ep_gab_queue_callback
Description : callback of gab queue
Input       : void * priv_data
              void * cqe_data
              u16 len
              cqe_status_t * head_info
Output      : void
Return      : void
*****************************************************************************/
static void ep_gab_queue_callback(void * priv_data, void * cqe_data, u16  len, cqe_status_t * head_info)
{
    struct ep_cqe* cqe = cqe_data;
    struct epfront_main_info *spmain = (struct epfront_main_info *)priv_data;

    if(unlikely(!cqe_data)){
        epfront_err("cqe_data is NULL");
        return ;
    }

    if(ep_check_state(le16_to_cpu(cqe->status), spmain))
        return ;

    switch(cqe->entry_type){
        case EP_GAB_AER_ENTRY:
            cqe->data[0] = spmain->index;
            (void)epfront_sv_assign_task(&(spmain->sv_ctrl), SV_AER_HANDLE, epfront_sv_aer_handle,
                cqe_data, sizeof(struct ep_cqe));
            break;
        default:
            atomic_inc(&(spmain->stats.ill_aer_type));
            epfront_err("entry_type[%d] is illegal", cqe->entry_type);
            break;
    }
}

/*****************************************************************************
Function    : epfront_create_io_lst
Description : create node of io_list by cqe_data
Input       : struct ep_io_cqe * cqe_data
Output      : struct epfront_io_list*
Return      : struct epfront_io_list*
*****************************************************************************/
struct epfront_io_list* epfront_create_io_lst(struct ep_io_cqe * cqe_data, struct epfront_main_info *smain)
{
    struct epfront_io_list* node = NULL;

    node = kzalloc(sizeof(struct epfront_io_list), GFP_ATOMIC);
    if(NULL == node){
        epfront_err("alloc for epfront_io_list failed, size[%lu]", sizeof(struct epfront_io_list));
        return NULL;
    }

    INIT_LIST_HEAD(&node->list);
    node->task_index = (smain->task_idx & IO_LIST_MASK);
    memcpy(&node->cqe, cqe_data, sizeof(struct ep_io_cqe));
    smain->task_idx++;
    return node;
}

/*****************************************************************************
Function    : epfront_io_handle
Description : io handler within tasklet
Input       : unsigned long data
Output      : void
Return      : void
*****************************************************************************/
void epfront_io_handle(unsigned long data)
{
    int index = (int) data & 0xf;
    struct list_head new_head;
    struct epfront_io_list* io_lst = NULL;
    struct epfront_io_list* tmp_lst = NULL;
    struct epfront_main_info* spmain = NULL;
    unsigned long flags;

    spmain = epfront_get_main_info((data >> 16) & 0xf);
    if(unlikely(!spmain)){
        epfront_err("get spmain fail");
        return;
    }

    while(!list_empty(&(spmain->io_list[index]))){
        spin_lock_irqsave(&(spmain->io_list_lock[index]), flags);
        list_replace_init(&(spmain->io_list[index]), &new_head);
        spin_unlock_irqrestore(&(spmain->io_list_lock[index]), flags);

        list_for_each_entry_safe(io_lst, tmp_lst, &new_head,  list){
            epfront_io_recv(&io_lst->cqe, spmain);
            list_del_init(&io_lst->list);
            kfree(io_lst);
        }
    }

    return;
}

/*****************************************************************************
Function    : ep_io_queue_callback
Description : callback of io queue process
Input       : void * priv_data
              void * cqe_data
              u16 len
              cqe_status_t * head_info
Output      : void
Return      : void
*****************************************************************************/
static void ep_io_queue_callback(void * priv_data, void * cqe_data, u16  len, cqe_status_t * head_info)
{
    struct ep_io_cqe* cqe = cqe_data;
    struct epfront_io_list* io_lst = NULL;
    struct epfront_main_info *spmain = (struct epfront_main_info *)priv_data;
    unsigned long flags;

    if(unlikely(NULL == cqe)){
        epfront_err("cqe_data is NULL");
        return ;
    }
    if(ep_check_state(le16_to_cpu(cqe->status), spmain))
        return ;

    io_lst = epfront_create_io_lst((struct ep_io_cqe*)cqe, spmain);
    if(!io_lst){
        epfront_io_recv(cqe, spmain);
        epfront_err("epfront_create_io_lst failed");
        return ;
    }

    spin_lock_irqsave(&(spmain->io_list_lock[io_lst->task_index]), flags);
    list_add_tail(&io_lst->list, &(spmain->io_list[io_lst->task_index]));
    spin_unlock_irqrestore(&(spmain->io_list_lock[io_lst->task_index]), flags);

    tasklet_schedule(&(spmain->io_tasklet[io_lst->task_index]));

    return ;
}

/*****************************************************************************
Function    : epfront_trans_free_io_index
Description : free trans io index
Input       : int i
Output      : void
Return      : void
*****************************************************************************/
static void epfront_trans_free_io_index(int i, struct epfront_main_info *smain)
{
    s16 q_id = 0;
    if(i < 0 || i >= EPFRONT_IO_SQ_N)
        return ;

    q_id = smain->trans_io_q[i];
    smain->trans_io_q[i] = 0;
    if(q_id > 0){
        ep_delete_queue(q_id, smain->sdev);
    }
}

/*****************************************************************************
Function    : epfront_trans_alloc_io_index
Description : alloc trans io index
Input       : int i
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_trans_alloc_io_index(int i, struct epfront_main_info *smain)
{
    s16 qid = -1;
    ulp_cq_base_t cmd_cq_info;
    ulp_sq_base_t cmd_sq_info;

    memset((void*)&cmd_cq_info, 0, sizeof(ulp_cq_base_t));
    memset((void*)&cmd_sq_info, 0, sizeof(ulp_sq_base_t));

    cmd_cq_info.stride = sizeof(struct ep_cqe);//element's data struct of cq
    cmd_cq_info.q_depth = EPFRONT_IO_Q_BD_N;
    cmd_cq_info.q_type = EP_IO_Q;
    cmd_cq_info.prv_data = smain;
    cmd_cq_info.cqe_handler = ep_io_queue_callback;
    cmd_cq_info.cpu_no = i + 1;

    cmd_sq_info.stride = sizeof(struct ep_sqe);//element's data struct of sq
    cmd_sq_info.q_depth = EPFRONT_IO_Q_BD_N;
    cmd_sq_info.q_type = EP_IO_Q;
    cmd_sq_info.qprio = SDI_SQ_PRIO_HIGH;  //SDI_SQ_PRIO_MEDIUM;

    qid =  ep_create_queue(&cmd_cq_info, &cmd_sq_info, smain->sdev);
    if (qid <= 0){
        epfront_err("ep_create_queue io queue failed");
        return -ENOMEM;
    }

    smain->trans_io_q[i] = qid;

    return 0;
}

/*****************************************************************************
Function    : epfront_trans_free_io
Description : free trans io
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void epfront_trans_free_io(struct epfront_main_info *smain)
{
    int i;

    for(i = 0; i < EPFRONT_IO_SQ_N; ++i)
        epfront_trans_free_io_index(i, smain);
}

/*****************************************************************************
Function    : epfront_trans_alloc_io
Description : alloc trans io
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_trans_alloc_io(struct epfront_main_info *smain)
{
    int ret = 0;
    int i, j;

    for(i = 0; i < EPFRONT_IO_SQ_N; ++i){
        ret = epfront_trans_alloc_io_index(i, smain);
        if(ret < 0){
            epfront_err("epfront_trans_alloc_io_index %dth faild", i);
            goto err_remove_resource;
        }
    }

    return 0;

err_remove_resource:
    for(j = 0; j < i; ++j)
        epfront_trans_free_io_index(j, smain);
    return ret;
}

#ifdef __ADMIN_CMD__
static void epfront_trans_free_adm_cmd_set(struct epfront_main_info *smain)
{
    s16 q_id = smain->trans_adm_cmd_set_q;
    smain->trans_adm_cmd_set_q = 0;
    if(q_id > 0){
        ep_delete_queue(q_id, smain->sdev);
    }
}
#endif
/*****************************************************************************
Function    : epfront_trans_free_gab
Description : free trans gab
Output      : void
Return      : void
*****************************************************************************/
static void epfront_trans_free_gab(struct epfront_main_info *smain)
{
    s16 q_id = smain->trans_gab_q;
    smain->trans_gab_q = 0;
    if(q_id > 0){
        ep_delete_queue(q_id, smain->sdev);
    }
}
#ifdef __ADMIN_CMD__
static int epfront_trans_alloc_adm_cmd_set(struct epfront_main_info *smain)
{
    s16 qid = -1;
    ulp_cq_base_t cmd_cq_info;
    ulp_sq_base_t cmd_sq_info;
    memset((void*)&cmd_cq_info, 0, sizeof(ulp_cq_base_t));
    memset((void*)&cmd_sq_info, 0, sizeof(ulp_sq_base_t));
    cmd_cq_info.stride = sizeof(struct ep_cqe);//element's data struct of cq
    cmd_cq_info.q_depth = EPFRONT_GAB_Q_BD_N;
    cmd_cq_info.q_type = EP_ADM_CMD_SET_Q;
    cmd_cq_info.prv_data = smain;

    cmd_cq_info.cqe_handler = ep_adm_cmd_set_queue_callback;
    cmd_cq_info.cpu_no = 0;

    cmd_sq_info.stride = sizeof(struct ep_sqe);//element's data struct of sq
    cmd_sq_info.q_depth = EPFRONT_GAB_Q_BD_N;
    cmd_sq_info.q_type = EP_ADM_CMD_SET_Q;//data type will be determined afterwards
    cmd_sq_info.qprio = SDI_SQ_PRIO_LOW;

    qid =  ep_create_queue(&cmd_cq_info,&cmd_sq_info,smain->sdev);
    if (qid <= 0){
        epfront_err("ep_create_queue io queue failed");
        return -ENOMEM;
    }

    smain->trans_adm_cmd_set_q = qid;
    return 0;
}
#endif
/*****************************************************************************
Function    : epfront_trans_alloc_gab
Description : alloc trans gab
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_trans_alloc_gab(struct epfront_main_info *smain)
{
    s16 qid = -1;
    ulp_cq_base_t event_manage_cq_info;
    ulp_sq_base_t event_manage_sq_info;

    memset((void*)&event_manage_cq_info, 0, sizeof(ulp_cq_base_t));
    memset((void*)&event_manage_sq_info, 0, sizeof(ulp_sq_base_t));

    event_manage_cq_info.stride = sizeof(struct ep_cqe);//element's data struct of cq
    event_manage_cq_info.q_depth = EPFRONT_GAB_Q_BD_N;
    event_manage_cq_info.q_type = EP_GAB_Q;//data type will be determined afterwards
    event_manage_cq_info.prv_data = smain;
    event_manage_cq_info.cqe_handler = ep_gab_queue_callback;
    event_manage_cq_info.cpu_no = 0;

    event_manage_sq_info.stride = sizeof(struct ep_sqe);//element's data struct of sq
    event_manage_sq_info.q_depth = EPFRONT_GAB_Q_BD_N;
    event_manage_sq_info.q_type = EP_GAB_Q;//data type will be determined afterwards
    event_manage_sq_info.qprio = SDI_SQ_PRIO_MEDIUM;

    qid = ep_create_queue(&event_manage_cq_info, &event_manage_sq_info, smain->sdev);
    if (qid < 0){
        epfront_err("ep_create_queue gab queue failed");
        return  -ENOMEM;
    }

    smain->trans_gab_q = qid;

    return 0;
}

/*****************************************************************************
Function    : epfront_trans_way_exit
Description : trans exit
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void epfront_trans_way_exit(struct epfront_main_info *smain)
{
    if(!test_and_clear_bit(EPFRONT_INIT_TRANS, &(smain->epfront_status)))
        return ;

    epfront_trans_free_io(smain);
    epfront_trans_free_gab(smain);
#ifdef __ADMIN_CMD__
    epfront_trans_free_adm_cmd_set(smain);
#endif
}

/*****************************************************************************
Function    : epfront_trans_way_init
Description : trans init
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_trans_way_init(struct epfront_main_info *smain)
{
    int ret = 0;

    if(test_and_set_bit(EPFRONT_INIT_TRANS, &(smain->epfront_status)))
        return 0;

    //create sq,cq,and cqe buffer
    ret = epfront_trans_alloc_gab(smain);
    if (ret < 0){
        epfront_err("epfront_trans_alloc_gab faild");
        goto err_out;
    }

    ret = epfront_trans_alloc_io(smain);
    if (ret < 0){
        epfront_err("epfront_trans_alloc_io faild");
        goto free_gab;
    }
#ifdef __ADMIN_CMD__
    ret = epfront_trans_alloc_adm_cmd_set(smain);
    if(ret < 0){
        epfront_err("epfront_trans_alloc_adm_cmd_set faild");
        goto free_io;
    }
#endif
    return 0;
#ifdef __ADMIN_CMD__
free_io:
    epfront_trans_free_io(smain);
#endif
free_gab:
    epfront_trans_free_gab(smain);
err_out:
    clear_bit(EPFRONT_INIT_TRANS, &(smain->epfront_status));
    return ret;
}
#ifdef __ADMIN_CMD__
static void epfront_adm_cdm_set_ctrl_free(struct epfront_main_info *smain)
{
    struct epfront_adm_cmd_set_ctrl* adm_ctrl = &(smain->g_adm_cmd_set_ctrl);
    if(unlikely(!smain->trans_device || !adm_ctrl)){
        epfront_err("illegal para");
        return ;
    }

    if(adm_ctrl->data_virt){
        if(adm_ctrl->data_phys && adm_ctrl->data_len){
            //释放的长度是否正确???
            dma_free_coherent(smain->trans_device, adm_ctrl->data_len, adm_ctrl->data_virt, adm_ctrl->data_phys);
            adm_ctrl->data_virt = NULL;
            adm_ctrl->data_phys = 0;
            adm_ctrl->data_len = 0;
        }
        else{
            epfront_err("can't happen: adm_ctrl_data_virt/phys/len not coherence");
        }
    }

}
#endif
/*****************************************************************************
Function    : epfront_aer_ctrl_free
Description : free aer ctrl
Input       : epfront_aer_controler_t * aer_ctrl
Output      : void
Return      : void
*****************************************************************************/
static void epfront_aer_ctrl_free(struct epfront_main_info *smain)
{
    epfront_aer_controler_t* aer_ctrl = &(smain->aer_ctrl);

    if(unlikely(!smain->trans_device || !aer_ctrl)){
        epfront_err("illegal para");
        return ;
    }

    if(aer_ctrl->data_virt){
        if(aer_ctrl->data_phys && aer_ctrl->data_len){
            dma_free_coherent(smain->trans_device, aer_ctrl->data_len, aer_ctrl->data_virt, aer_ctrl->data_phys);
            aer_ctrl->data_virt = NULL;
            aer_ctrl->data_phys = 0;
            aer_ctrl->data_len = 0;
        } else{
            epfront_err("can't happen: aer_ctrl_data_virt/phys/len not coherence");
        }
    }
}

#ifdef __ADMIN_CMD__
static int epfront_adm_cmd_set_ctrl_alloc(struct epfront_main_info *smain)
{
    void* virt = NULL;
    dma_addr_t phys = 0;
    struct epfront_adm_cmd_set_ctrl* adm_cmd_set_ctrl = &(smain->g_adm_cmd_set_ctrl);

    if(unlikely(!smain->trans_device || !adm_cmd_set_ctrl)){
        epfront_err("illegal para");
        return -EINVAL;
    }
    //memset((void*)adm_cmd_set_ctrl, 0, sizeof(struct epfront_adm_cmd_set_ctrl));
    adm_cmd_set_ctrl->ctrl_opt = AER_NEET_RESP;//需要回应
    adm_cmd_set_ctrl->data_len = ADM_CMD_SET_DATA_LEN; //暂时定的命令的最大长度为128字节
    adm_cmd_set_ctrl->crc32 = 0;
    virt =(void*)dma_alloc_coherent(smain->trans_device, ADM_CMD_SET_DATA_LEN, &phys, GFP_KERNEL);
    if(unlikely(NULL == virt)){
        epfront_err("admin_cmd_set dma_alloc_coherent failed");
        return -ENOMEM;
    }
    adm_cmd_set_ctrl->data_virt = virt;
    adm_cmd_set_ctrl->data_phys = phys;

    return 0;
}
#endif
/*****************************************************************************
Function    : epfront_aer_ctrl_alloc
Description : alloc aer ctrl
Input       : epfront_aer_controler_t * aer_ctrl
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_aer_ctrl_alloc(struct epfront_main_info *smain)
{
    int i;
    struct epfront_aer_ctrl* ctrl = NULL;
    epfront_aer_controler_t* aer_ctrl = &(smain->aer_ctrl);
    u32 offset = 0;
    u32 sum_len = 0;
    void* virt = NULL;
    dma_addr_t phys = 0;
    //len must multy 4Bytes
    struct epfront_aer_ctrl ctrl_table[AER_MAX_LIMIT] = {
        { AER_ADD_DISK,         AER_NEET_RESP,      sizeof(ep_aer_disk_name_t),   0,    epfront_aer_recv_add_disk,         NULL, 0},
        { AER_RMV_DISK,         AER_NEET_RESP,      sizeof(ep_aer_disk_t),        0,    epfront_aer_recv_rmv_disk,         NULL, 0},
        { AER_NOTIFY_RESCAN,    AER_NEET_RESP,      sizeof(u32),                  0,    epfront_aer_recv_notify_rescan,    NULL, 0},
        { AER_IO_SWITCH,        AER_NEET_RESP,      sizeof(u32),                  0,    epfront_aer_recv_io_switch,        NULL, 0},
        { AER_LINKDOWN,         AER_NOT_NEEN_RESP,  0,                            0,    epfront_aer_recv_linkdown,         NULL, 0}
    };

    if(unlikely(!smain->trans_device || !aer_ctrl)){
        epfront_err("illegal para");
        return -EINVAL;
    }

    memset((void*)aer_ctrl, 0, sizeof(epfront_aer_controler_t));

    memcpy((void*)aer_ctrl->ctrl_table, ctrl_table, sizeof(struct epfront_aer_ctrl) * AER_MAX_LIMIT);

    for(i = 0; i < AER_MAX_LIMIT; ++i){
        ctrl = &aer_ctrl->ctrl_table[i];
        if(ctrl->data_len){
            if(ctrl->data_len & 0x03){
                epfront_warn("%dth aer data_len[%u] is not multy 4Bytes", i, ctrl->data_len);
                ctrl->data_len = EPFRONT_UP_TO_MULTY4(ctrl->data_len);
            }
            sum_len += ctrl->data_len;
        }
    }

    virt =(void*)dma_alloc_coherent(smain->trans_device, sum_len, &phys, GFP_KERNEL);
    if(unlikely(NULL == virt)){
        epfront_err("dma_alloc_coherent failed, size[%u]", sum_len);
        return -ENOMEM;
    }

    for(i = 0; i < AER_MAX_LIMIT; ++i){
        ctrl = &aer_ctrl->ctrl_table[i];
        if(ctrl->data_len){
            ctrl->data_virt = (char*)virt + offset;
            ctrl->data_phys = phys + offset;
            offset += ctrl->data_len;
        }
        epfront_info("aen[%d] data: data_len[%u]", i, ctrl->data_len);
    }

    aer_ctrl->data_virt = virt;
    aer_ctrl->data_phys = phys;
    aer_ctrl->data_len = (int)sum_len;

    return 0;

}

/*****************************************************************************
Function    : epfront_notify_ctrl_free
Description : free notify ctrl
Input       : epfront_notify_controler_t * notify_ctrl
Output      : void
Return      : void
*****************************************************************************/
static void epfront_notify_ctrl_free(struct epfront_main_info *smain)
{
    epfront_notify_controler_t* notify_ctrl = &(smain->notify_ctrl);

    if(unlikely(!smain->trans_device || !notify_ctrl)){
        epfront_err("illegal para");
        return ;
    }

    if(notify_ctrl->data_virt){
        if(notify_ctrl->data_phys && notify_ctrl->data_len){
            dma_free_coherent(smain->trans_device, notify_ctrl->data_len, notify_ctrl->data_virt, notify_ctrl->data_phys);
            notify_ctrl->data_virt = NULL;
            notify_ctrl->data_phys = 0;
            notify_ctrl->data_len = 0;
        } else{
            epfront_err("can't happen: notify_ctrl_data_virt/phys/len not coherence");
        }
    }
}

/*****************************************************************************
Function    : epfront_notify_ctrl_alloc
Description : alloc notify ctrl
Input       : epfront_notify_controler_t * notify_ctrl
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_notify_ctrl_alloc(struct epfront_main_info *smain)
{
    int i;
    struct epfront_notify_ctrl* ctrl = NULL;
    epfront_notify_controler_t* notify_ctrl = &(smain->notify_ctrl);
    u32 offset = 0;
    u32 sum_len = 0;
    void* virt = NULL;
    dma_addr_t phys = 0;
    //len must multy 4Bytes
    struct epfront_notify_ctrl ctrl_table[EP_NOTIFY_MAX_LIMIT] = {
        { EP_NOTIFY_RESET_BACK_STATE,    0,             NULL, 0},
        { EP_NOTIFY_CHECK_BACK_STATE,    0,             NULL, 0},
        { EP_NOTIFY_SYNC_DISK,           (EP_SYNC_DISK_ONCE_N * sizeof(struct ep_aer_disk)) + sizeof(module_notify_data_t),
                                                        NULL, 0},
        { EP_NOTIFY_SYNC_CONFIG,         sizeof(struct ep_global_config) + sizeof(module_notify_data_t),
                                                        NULL, 0},
        { EP_NOTIFY_SYNC_DISK_RESULT,    0,             NULL, 0},
        { EP_NOTIFY_IS_BACK_RDY,         0,             NULL, 0},
        { EP_NOTIFY_SYNC_DEVNAME,        EP_SYNC_DEVNAME_ONCE_N * sizeof(struct ep_aer_dev_name) + sizeof(module_notify_data_t),
                                                        NULL, 0},
    };

    if(unlikely(!smain->trans_device || !notify_ctrl)){
        epfront_err("illegal para");
        return -EINVAL;
    }

    memset((void*)notify_ctrl, 0, sizeof(epfront_notify_controler_t));

    memcpy((void*)notify_ctrl->ctrl_table, ctrl_table, sizeof(struct epfront_notify_ctrl) * EP_NOTIFY_MAX_LIMIT);

    for(i = 0; i < EP_NOTIFY_MAX_LIMIT; ++i){
        ctrl = &notify_ctrl->ctrl_table[i];
        if(ctrl->data_len){
            if(ctrl->data_len & 0x03){
                epfront_warn("%dth notify data_len[%u] is not multy 4Bytes", i, ctrl->data_len);
                ctrl->data_len = EPFRONT_UP_TO_MULTY4(ctrl->data_len);
            }
            sum_len += ctrl->data_len;
        }
    }

    virt =(void*)dma_alloc_coherent(smain->trans_device, sum_len, &phys, GFP_KERNEL);
    if(unlikely(NULL == virt)){
        epfront_err("dma_alloc_coherent failed, size[%u]", sum_len);
        return -ENOMEM;
    }

    for(i = 0; i < EP_NOTIFY_MAX_LIMIT; ++i){
        ctrl = &notify_ctrl->ctrl_table[i];
        if(ctrl->data_len){
            ctrl->data_virt = (char*)virt + offset;
            ctrl->data_phys = phys + offset;
            offset += ctrl->data_len;
        }
        epfront_info("notify[%d] data: len[%u]", i, ctrl->data_len);
    }

    notify_ctrl->data_virt = virt;
    notify_ctrl->data_phys = phys;
    notify_ctrl->data_len = (int)sum_len;

    return 0;
}

/*****************************************************************************
Function    : epfront_dev_exit
Description : dma device exit
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void epfront_dev_exit(struct epfront_main_info *smain)
{
    if(!test_and_clear_bit(EPFRONT_INIT_DEV, &(smain->epfront_status)))
        return ;

    epfront_aer_ctrl_free(smain);
    epfront_notify_ctrl_free(smain);
#ifdef __ADMIN_CMD__
    epfront_adm_cdm_set_ctrl_free(smain);
#endif
    smain->trans_device = NULL;
}

/*****************************************************************************
Function    : epfront_dev_init
Description : dma device init
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_dev_init(struct epfront_main_info *smain)
{
    int ret;

    if(test_and_set_bit(EPFRONT_INIT_DEV, &(smain->epfront_status)))
        return 0;

    if(smain->sdev->pdev){
        smain->trans_device = &(smain->sdev->pdev->dev);
    } else{
        epfront_err_limit("sdi device's struct is illegal");
        return -EINVAL;
    }

    ret = epfront_notify_ctrl_alloc(smain);
    if(ret){
        epfront_err_limit("epfront_notify_ctrl_alloc failed, ret[%d]", ret);
        goto clear_dev;
    }

    ret = epfront_aer_ctrl_alloc(smain);
    if(ret){
        epfront_err_limit("epfront_aer_ctrl_alloc failed, ret[%d]", ret);
        goto notify_free;
    }
#ifdef __ADMIN_CMD__
    ret = epfront_adm_cmd_set_ctrl_alloc(smain);
    if(ret){
        epfront_err_limit("epfront_adm_cmd_set_ctrl_alloc failed, ret[%d]", ret);
        goto aer_free;
    }
#endif
    return 0;
#ifdef __ADMIN_CMD__
aer_free:
    epfront_aer_ctrl_free(smain);
#endif
notify_free:
    epfront_notify_ctrl_free(smain);
clear_dev:
    smain->trans_device = NULL;
    clear_bit(EPFRONT_INIT_DEV, &(smain->epfront_status));
    return ret;
}

/*****************************************************************************
Function    : epfront_mgr_exit
Description : epfront management exit
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void epfront_mgr_exit(struct epfront_main_info *smain)
{
    unsigned int i;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36))
    struct scsi_device *sdev = NULL;
#endif

    if(!test_and_clear_bit(EPFRONT_INIT_MGR, &(smain->epfront_status)))
        return ;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36))
    for(i = 0; i < smain->epfront_host_n; ++i){
        if(smain->epfront_hosts[i]){
             list_for_each_entry(sdev, &(smain->epfront_hosts[i]->scsi_host->__devices), siblings) {
                (void)scsi_device_set_state(sdev,SDEV_OFFLINE);
            }
        }
    }
    msleep(6000);
#endif

    for(i = 0; i < smain->epfront_host_n; ++i){
        if(smain->epfront_hosts[i]){
            epfront_unregister_host(smain->epfront_hosts[i], smain);
            smain->epfront_hosts[i] = NULL;
        }
    }
}

/*****************************************************************************
Function    : epfront_mgr_init
Description : epfront management init
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_mgr_init(struct epfront_main_info *smain)
{
    int ret = 0;
    unsigned int i,j;
    struct epfront_host_para para;
    struct epfront_host_ctrl* h = NULL;

    if(test_and_set_bit(EPFRONT_INIT_MGR, &(smain->epfront_status)))
        return 0;

    para.max_channel = smain->global_config.max_channel;
    para.max_id = smain->global_config.max_id;
    para.max_lun = smain->global_config.max_lun;
    para.max_cmd_len = smain->global_config.max_cmd_len;
    para.max_nr_cmds = smain->global_config.max_nr_cmds;
    para.cmd_per_lun = smain->global_config.cmd_per_lun;
    para.sg_count = smain->global_config.sg_count;

    smain->epfront_host_n = smain->global_config.host_n;

    //epfront_hosts
    if(smain->epfront_host_n > EP_MAX_HOST_NUMBER){
        epfront_err("epfront_host_n[%d] is big than %d", smain->epfront_host_n, EP_MAX_HOST_NUMBER);
        ret = -EINVAL;
        goto err_out;
    }

    for(i = 0; i < smain->epfront_host_n; ++i){
        h = epfront_register_host(&para, smain);
        if(NULL == h){
            epfront_err("register host failed, [%d]th", i);
            ret = -EFAULT;
            goto unreg;
        }
        else{
            smain->epfront_hosts[i] = h;
            h->smain = smain;
        }
    }

    epfront_info("epfront_mgr_init success");
    return 0;

unreg:
    for(j = 0; j < i; ++j){
        epfront_unregister_host(smain->epfront_hosts[j], smain);
        smain->epfront_hosts[j] = NULL;
    }
err_out:
    clear_bit(EPFRONT_INIT_MGR, &(smain->epfront_status));
    return ret;
}

/*****************************************************************************
Function    : epfront_host_handle_pending_io
Description : handle pending io of host
Input       : struct epfront_host_ctrl * h
Output      : void
Return      : void
*****************************************************************************/
static void epfront_host_handle_pending_io(struct epfront_host_ctrl* h, struct epfront_main_info* smain)
{
    struct epfront_cmnd_list *c = NULL , *n = NULL;
    struct epfront_lun_list* lun_lst = NULL;
    struct scsi_cmnd *sc = NULL;
    int requeue_count = 0, softerr_count = 0;

    if(unlikely(NULL == h)){
        epfront_err("para illegal");
        return ;
    }

    spin_lock_irq(&h->lock);
    list_for_each_entry_safe(c, n, &h->cmdQ, list){
        list_del_init(&c->list);

        if(!test_bit(CMD_STAT_SEND_COMP, &c->status)
            || test_and_set_bit(CMD_STAT_RECV_RESP, &c->status)){
            epfront_err_limit("c has be handle, status[0x%lx]", c->status);
            atomic_inc(&h->conflict_num);
            continue ;
        }

        sc = c->scsi_cmd;
        if(!sc){
            epfront_err("fatal error, host:%u sn:%u, stat = 0x%lx", h->sys_host_id, c->cmd_sn, c->status);
            epfront_host_cmd_free(c);
            continue;
        }

        free_cmd_resource(c, smain);

        if(sc->device && sc->device->sdev_state == SDEV_BLOCK){
            sc->result = (DID_REQUEUE << 16);
            ++requeue_count;
        } else{
            sc->result = (DID_SOFT_ERROR << 16);
            ++softerr_count;
        }
        sc->scsi_done(sc);

        set_bit(CMD_STAT_DONE, &c->status);
        wake_up(&(smain->wait));

        lun_lst = epfront_get_lun_list(&(smain->lun_ctrl), c->back_uniq_id);
        if(unlikely(!lun_lst)){
            epfront_err_limit("lun_lst is invalid, back_uniq_id[%u]", c->back_uniq_id);
        } else{
            atomic_inc(&lun_lst->abort_num);
        }

        epfront_host_cmd_free(c);

    }
    spin_unlock_irq(&h->lock);

    epfront_info("handle host %d io complete, requeue[%d], softerr[%d]",
        h->sys_host_id, requeue_count, softerr_count);
}

/*****************************************************************************
Function    : epfront_handle_pending_io
Description : handle pending io
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void epfront_handle_pending_io(struct epfront_main_info* smain)
{
    unsigned int i;
    struct epfront_host_ctrl* h = NULL;

    int index = 0;
    struct list_head new_head;
    struct epfront_io_list* io_lst = NULL;
    struct epfront_io_list* tmp_lst = NULL;
    unsigned long flags;

    for(index = 0; index < IO_LIST_N; index++){
        tasklet_disable(&(smain->io_tasklet[index]));

        if( unlikely(!list_empty(&(smain->io_list[index]))) ){
            epfront_err_limit("can't happen: io_list[%d] has residual", index);
            spin_lock_irqsave(&(smain->io_list_lock[index]),flags);
            list_replace_init(&(smain->io_list[index]), &new_head);
            spin_unlock_irqrestore(&(smain->io_list_lock[index]),flags);

            list_for_each_entry_safe(io_lst, tmp_lst, &new_head, list){
                epfront_io_recv(&io_lst->cqe, smain);
                list_del_init(&io_lst->list);
                kfree(io_lst);
            }
        }
    }

    for (i = 0;  i< smain->epfront_host_n; i++) {
        h = smain->epfront_hosts[i];
        if(unlikely(!h)){
            epfront_err("epfront_hosts[%d] is NULL", i);
            continue;
        }
        if(h->scsi_host)
            epfront_host_handle_pending_io(h, smain);
    }

    for(index = 0; index < IO_LIST_N; index++){
        tasklet_enable(&(smain->io_tasklet[index]));
    }
}

/*****************************************************************************
Function    : epfront_wait_queue_off
Description : wait scsi queue off
Input       : int msec
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_wait_queue_off(int msec, struct epfront_main_info* smain)
{
    unsigned long timeout = jiffies + msecs_to_jiffies(msec);

    while( !test_bit(EPFRONT_SCSI_QUEUE_OFF_DONE, &(smain->epfront_status))
        && test_bit(EPFRONT_SCSI_QUEUE_RUN, &(smain->epfront_status)) ){

        msleep(50);

        if(time_after(jiffies, timeout)){
            epfront_err("wait queuecommand timeout [%d]msec", msec);
            return -EFAULT;
        }
    }

    return 0;
}

/*****************************************************************************
Function    : epfront_wait_io_complete
Description : wait scsi io complete
Input       : int msec
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_wait_io_complete(int msec, struct epfront_main_info* smain)
{
    unsigned int i;
    struct epfront_host_ctrl* h = NULL;
    unsigned long timeout = jiffies + msecs_to_jiffies(msec);

    BUG_ON(smain->epfront_host_n > EP_MAX_HOST_NUMBER);

    for (i = 0; i < smain->epfront_host_n; i++) {
        h = smain->epfront_hosts[i];
        if(IS_ERR_OR_NULL(h)){
            epfront_warn("efront_hosts[%d] is illegal\n", i);
            continue;
        }

        do{
            if( atomic_read(&h->cmds_num) == 0
                || atomic_read(&h->cmds_num) == atomic_read(&h->abort_fail) ){
                epfront_info("host[%d]'s cmdQ has no io", h->sys_host_id);
                break;
            } else{
                epfront_err_limit("host[%d]'s cmdQ has %u io",
                    h->sys_host_id, atomic_read(&h->cmds_num));

                msleep(50);
            }
        }while(time_before(jiffies, timeout));

        if(time_after(jiffies, timeout)){
            return -EFAULT;
        }
    }

    return 0;
}

/*****************************************************************************
Function    : epfront_set_queue_off
Description : set scsi queue off
Input       : void
Output      : void
Return      : void
*****************************************************************************/
void epfront_set_queue_off(struct epfront_main_info* smain)
{
    suspend_all_device(smain);

    clear_bit(EPFRONT_SCSI_QUEUE_OFF_DONE, &(smain->epfront_status));
    smp_mb();
    set_bit(EPFRONT_SCSI_QUEUE_OFF, &(smain->epfront_status));
    epfront_info("epfront_set_queue_off done");
}

void epfront_set_linkdown(struct epfront_main_info *smain)
{
    if (unlikely(!smain)){
        epfront_err("smain is NULL");
        return;
    }

    set_bit(EPFRONT_SCSI_LINKDOWN, &(smain->epfront_status));
    epfront_set_queue_off(smain);
    epfront_info("epfront_set_linkdown done");
}

/*****************************************************************************
Function    : epfront_do_stop_trans
Description : stop trans
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static inline int epfront_do_stop_trans(unsigned long status, struct epfront_main_info* smain)
{
    int ret;

    epfront_info("epfront_stop_trans start");

    clear_bit(EPFRONT_SCSI_STOP, &(smain->epfront_status));
    clear_bit(EPFRONT_SCSI_START, &(smain->epfront_status));

    set_bit(EPFRONT_SCSI_FAILFAST, &(smain->epfront_status));

    if(test_bit(EPFRONT_SCSI_LINKDOWN, &(smain->epfront_status)) || test_bit(SDI_FRONT_UPDATE, &status)){
        if(!test_bit(EPFRONT_SCSI_LINKDOWN, &(smain->epfront_status))){
            epfront_set_queue_off(smain);
        }

        ret = epfront_wait_queue_off(EPFRONT_WAIT_QUEUECOMMAND_MSEC, smain);
        if(ret){
            epfront_err("can't happen: epfront_wait_queue_off %d msec failed, ret[%d]",
                EPFRONT_WAIT_QUEUECOMMAND_MSEC, ret);
        }
        //link down, can handle io
        epfront_handle_pending_io(smain);
        epfront_info("handle pending io done");
    }

    if(current != smain->sv_ctrl.task){
        ret = epfront_sv_thread_suspend(&(smain->sv_ctrl), EPFRONT_SV_SUSPEND_TIMOUT_MSEC);
        if(ret){
            epfront_err("epfront_sv_thread_suspend failed, ret[%d]" ,ret);
            clear_bit(EPFRONT_SCSI_FAILFAST, &(smain->epfront_status));
            return ret;
        }
    }
    epfront_info("epfront_sv_thread_suspend success");
#ifdef __ADMIN_CMD__
    epfront_clear_admcmd_operation(smain);
    epfront_info("epfront_adm_cmd_workqueue clear success");
#endif
    clear_bit(EPFRONT_SCSI_FAILFAST, &(smain->epfront_status));
    epfront_sv_set_empty(&(smain->sv_ctrl));

    suspend_only_host(smain);

    if( !( test_bit(EPFRONT_SCSI_LINKDOWN, &(smain->epfront_status)) || test_bit(SDI_FRONT_UPDATE, &status) ) ){
        epfront_set_queue_off(smain);
        ret = epfront_wait_queue_off(EPFRONT_WAIT_QUEUECOMMAND_MSEC, smain);
        if(ret){
            epfront_err("can't happen: epfront_wait_queue_off %d msec failed, ret[%d]",
                EPFRONT_WAIT_QUEUECOMMAND_MSEC, ret);
        }
        epfront_info("scsi queue has off");

        if(test_bit(SDI_FRONT_AER, &status)){
            ret = epfront_wait_io_complete(EPFRONT_WAIT_IO_COMPLETE_MSEC, smain);
            if(ret){
                epfront_err("epfront_wait_io_complete %d msec failed, ret[%d]",
                    EPFRONT_WAIT_IO_COMPLETE_MSEC, ret);
            } else{
                epfront_info("scsi io has complete");
            }
        }
        //can not distinguish if io can back or not, do what?
        epfront_handle_pending_io(smain);
    }

    epfront_trans_way_exit(smain);

    clear_bit(EPFRONT_SCSI_READY, &(smain->epfront_status));
    clear_bit(EPFRONT_SCSI_SYNC_DISK, &(smain->epfront_status));

    set_bit(EPFRONT_SCSI_STOP, &(smain->epfront_status));
    epfront_info("epfront_stop_trans success");
    return 0;
}

/*****************************************************************************
Function    : epfront_stop_trans
Description : stop trans
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
int epfront_stop_trans(unsigned long status, struct epfront_main_info* smain)
{
    int ret;

    if (unlikely(!smain)){
        epfront_err("smain is NULL");
        return -EINVAL;
    }

    if(test_and_set_bit(EPFRONT_SCSI_RESETTING, &(smain->epfront_status))){
        while(test_bit(EPFRONT_SCSI_RESETTING, &(smain->epfront_status))){
            set_current_state(TASK_INTERRUPTIBLE);
            (void)schedule_timeout((long)HZ/10);
            set_current_state(TASK_RUNNING);
        }
    }

    ret = epfront_do_stop_trans(status, smain);

    clear_bit(EPFRONT_SCSI_RESETTING, &(smain->epfront_status));

    return ret;
}

/*****************************************************************************
Function    : epfront_start_trans
Description : start trans
Input       : void
Output      : void
Return      : void
*****************************************************************************/
void epfront_start_trans(struct epfront_main_info* smain)
{
    if (unlikely(!smain)){
        epfront_err("smain is NULL");
        return;
    }

    clear_bit(EPFRONT_SCSI_LINKDOWN, &(smain->epfront_status));
    clear_bit(EPFRONT_SCSI_QUEUE_OFF, &(smain->epfront_status));
    set_bit(EPFRONT_SCSI_START, &(smain->epfront_status));
    epfront_sv_set_empty(&(smain->sv_ctrl));
    if(waitqueue_active(&(smain->sv_ctrl.wait_queue)))
        wake_up(&(smain->sv_ctrl.wait_queue));

    epfront_info("epfront_start_trans has submit to supervise thread");
}

/*****************************************************************************
Function    : epfront_scsi_back_insmod_probe
Description : probe function when scsi back driver insmod
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_scsi_back_insmod_probe(struct epfront_main_info *smain){

    int ret = 0;

    ret = epfront_sync_reset_epback(smain);
    if(ret){
        goto err_out;
    }

    ret = epfront_sync_back_config(smain);
    if(ret){
        goto err_out;
    }

    ret = epfront_mgr_init(smain);
    if(ret){
        epfront_err("epfront_mgr_init failed, ret[%d]", ret);
        goto err_out;
    }

    ret = epfront_trans_way_init(smain);
    if(ret){
        epfront_err("epfront_trans_way_init failed,ret[%d],ret",ret);
        goto err_out;
    }

    return 0;

err_out:
    return ret;
}

/*****************************************************************************
Function    : epfront_scsi_back_notify_probe
Description : probe function when scsi back driver notify
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_scsi_back_notify_probe(struct epfront_main_info *smain){

    int ret = 0;
    int i;

    ret = epfront_quiry_epback_rdy(smain);
    if(ret){
        goto err_out;
    }

    ret = epfront_sync_back_disk(smain);
    if(ret){
        epfront_err("epfront_sync_back_disk failed, ret[%d]", ret);
        goto err_out;
    }

    for(i = 0; i < AER_MAX_LIMIT; ++i){
        ret = epfront_aer_send(&(smain->aer_ctrl.ctrl_table[i]), 0, smain);
        if(ret){
            epfront_err("epfront_aer_send type[%d] failed, ret[%d]", i, ret);
            goto err_out;
        }
#ifdef EPFRONT_DEBUG
        atomic_set(&(smain->stats.aer_send[i]), 1);
        atomic_set(&(smain->stats.aer_todo[i]), 0);
        atomic_set(&(smain->stats.aer_done[i]), 0);
#endif
    }
#ifdef __ADMIN_CMD__
    ret = epfront_adm_cmd_set_send(&(smain->g_adm_cmd_set_ctrl),0,smain);
    if(ret){
        epfront_err("epfront_adm_cmd_set_send failed,ret[%d]",ret);
        goto err_out;
    }
#endif
    ret = epfront_sync_check_back_state(EP_STATE_FRONT_CONNECTED, smain);
    if(ret){
        epfront_err("epfront_sync_check_back_state EP_STATE_FRONT_CONNECTED failed, ret[%d]", ret);
        goto err_out;
    }

    set_bit(EPFRONT_SCSI_READY, &(smain->epfront_status));

err_out:
    return  ret;
}

/*****************************************************************************
Function    : epfront_scsi_remove
Description : remove scsi
Input       : void
Output      : void
Return      : void
*****************************************************************************/
void epfront_scsi_remove(struct epfront_main_info *smain)
{
    if (unlikely(!smain)){
        epfront_err("smain is NULL");
        return;
    }

    epfront_sv_thread_stop(&(smain->sv_ctrl));

    //test: change order for x86 calltrace
    epfront_mgr_exit(smain);

    epfront_trans_way_exit(smain);

    epfront_dev_exit(smain);
}

/*****************************************************************************
Function    : epfront_scsi_probe
Description : probe scsi
Input       : void
Output      : void
Return      : void
*****************************************************************************/
void epfront_scsi_probe(struct epfront_main_info* smain)
{
    const unsigned long timeout = jiffies + EPFRONT_SCSI_PROBE_TIME_OUT;

    if (unlikely(!smain)){
        epfront_err("smain is NULL");
        return;
    }

    epfront_start_trans(smain);
#if 1
    while(!test_bit(EPFRONT_SCSI_READY, &(smain->epfront_status))
        && time_before(jiffies, timeout)){
        msleep(EPFRONT_SCSI_PROBE_INTERVAL_TIME);
    }
#endif
    if(!test_bit(EPFRONT_SCSI_READY, &(smain->epfront_status)))
        epfront_info("epfront change to async probe");

    epfront_info("epfront_scsi_probe success");
}

/*****************************************************************************
Function    : epfront_sv_ctrl_exit
Description : exit sv thread ctrl
Input       : epfront_sv_controler_t * sv_ctrl
Output      : void
Return      : void
*****************************************************************************/
static void epfront_sv_ctrl_exit(epfront_sv_controler_t* sv_ctrl)
{
    if(!sv_ctrl)
        return ;

    sv_ctrl->suspend = sv_ctrl->suspend_complete = 0;

    epfront_sv_thread_stop(sv_ctrl);

    bsfifo_free(&sv_ctrl->lowpri_bsfifo);
    bsfifo_free(&sv_ctrl->bsfifo);
}

/*****************************************************************************
Function    : epfront_sv_ctrl_init
Description : init sv thread ctrl
Input       : epfront_sv_controler_t * sv_ctrl
              char * name
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sv_ctrl_init(struct epfront_main_info *smain, char* name)
{
    int ret;
    epfront_sv_controler_t* sv_ctrl = &(smain->sv_ctrl);

    if(!sv_ctrl){
        epfront_err("illegal para");
        return -EINVAL;
    }

    spin_lock_init(&sv_ctrl->lock);
    init_waitqueue_head(&sv_ctrl->wait_queue);

    sv_ctrl->suspend = 0;
    sv_ctrl->suspend_complete = 0;
    sv_ctrl->expire = 0;

    ret = bsfifo_alloc(&sv_ctrl->bsfifo, EPFRONT_SV_KFIFO_SIZE, GFP_KERNEL);
    if(ret){
        epfront_err("alloc bsfifo failed, size[%u]", EPFRONT_SV_KFIFO_SIZE);
        return ret;
    }

    ret = bsfifo_alloc(&sv_ctrl->lowpri_bsfifo, EPFRONT_SV_KFIFO_SIZE, GFP_KERNEL);
    if(ret){
        epfront_err("alloc lowpri_bsfifo failed, size[%u]", EPFRONT_SV_KFIFO_SIZE);
        goto free_bsfifo;
    }

    sv_ctrl->task = kthread_run(epfront_sv_thread, (void*)smain, "%s%02x", name, smain->sdev->pdev->bus->number);
    if(IS_ERR_OR_NULL(sv_ctrl->task)){
        epfront_err("create task thread failed");
        ret = (int)PTR_ERR(sv_ctrl->task) ?: -EFAULT;
        goto free_lowpri_bsfifo;
    }

    return 0;

free_lowpri_bsfifo:
    bsfifo_free(&sv_ctrl->lowpri_bsfifo);
free_bsfifo:
    bsfifo_free(&sv_ctrl->bsfifo);
    return ret;
}

/*****************************************************************************
Function    : epfront_lun_ctrl_exit
Description : exit lun ctrl
Input       : epfront_lun_controler_t * lun_ctrl
Output      : void
Return      : void
*****************************************************************************/
static void epfront_lun_ctrl_exit(epfront_lun_controler_t* lun_ctrl)
{
    struct epfront_lun_list* lun_lst = NULL;
    struct epfront_lun_list* tmp = NULL;
    struct epfront_main_info* spmain = NULL;

    if(!lun_ctrl){
        epfront_err("illegal para");
        return ;
    }

    spmain = container_of(lun_ctrl, struct epfront_main_info, lun_ctrl);

    list_for_each_entry_safe(lun_lst, tmp, &lun_ctrl->async_list, list){
        if(!IS_ERR_OR_NULL(lun_lst)){
            list_del_init(&lun_lst->list);
            epfront_free_lun_list(lun_lst, spmain);
        }
    }
    INIT_LIST_HEAD(&lun_ctrl->async_list);

    clear_async_devname_list(&lun_ctrl->async_devname_list);

    list_for_each_entry_safe(lun_lst, tmp, &lun_ctrl->list, list){
        if(!IS_ERR_OR_NULL(lun_lst)){
            list_del_init(&lun_lst->list);
            epfront_destroy_lun_sysfs(lun_lst);
            epfront_free_lun_list(lun_lst, spmain);
        }
    }
    INIT_LIST_HEAD(&lun_ctrl->list);

    if(lun_ctrl->table){
        kfree(lun_ctrl->table);
        lun_ctrl->table = NULL;
    }

    if(lun_ctrl->lun_bits){
        kfree(lun_ctrl->lun_bits);
        lun_ctrl->lun_bits = NULL;
    }

}

/*****************************************************************************
Function    : epfront_lun_ctrl_init
Description : init lun ctrl
Input       : epfront_lun_controler_t * lun_ctrl
              unsigned int size
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_lun_ctrl_init(epfront_lun_controler_t* lun_ctrl, unsigned int size)
{
    int ret;

    if(!lun_ctrl || !size){
        epfront_err("illegal para: size[%u]", size);
        return -EINVAL;
    }

    INIT_LIST_HEAD(&lun_ctrl->list);
    INIT_LIST_HEAD(&lun_ctrl->async_list);
    INIT_LIST_HEAD(&lun_ctrl->async_devname_list);

    lun_ctrl->size = size;
    /*lint -e587*/
    size = (int)roundup_pow_of_two(size);
    if(size < 2){
        lun_ctrl->mask = 0;
        return -EINVAL;
    }
    /*lint +e587*/

    lun_ctrl->lun_bits = kzalloc( ((size + BITS_PER_LONG - 1) / BITS_PER_LONG)
        * sizeof(unsigned long), GFP_KERNEL );
    if(NULL == lun_ctrl->lun_bits){
        epfront_err("alloc memory fo lun_bits failed, size[%u]", size);
        return -ENOMEM;
    }

    lun_ctrl->table = kzalloc(size * sizeof(struct epfront_lun_list*), GFP_KERNEL);
    if(NULL == lun_ctrl->table){
        epfront_err("alloc lun_tbl failed, size[%lu]", size * sizeof(struct epfront_lun_list*));
        ret = -ENOMEM;
        goto err_free_bits;
    }
    lun_ctrl->mask = size - 1;

    return 0;

err_free_bits:
    kfree(lun_ctrl->lun_bits);
    lun_ctrl->lun_bits = NULL;
    return ret;
}

/*****************************************************************************
Function    : epfront_io_list_init
Description : init io list ctrl
Input       : void
Output      : void
Return      : void
*****************************************************************************/
static void epfront_io_list_init(struct epfront_main_info *smain)
{
    unsigned long index = 0;

    for(index = 0; index < IO_LIST_N; index++){
        spin_lock_init(&(smain->io_list_lock[index]));
        INIT_LIST_HEAD(&(smain->io_list[index]));

        tasklet_init(&(smain->io_tasklet[index]), epfront_io_handle, ((unsigned long)smain->index << 16) | index);
    }

    return;
}

#ifdef __ADMIN_CMD__
void epfront_clear_admcmd_operation(struct epfront_main_info *smain)
{
    (void)flush_work(&(smain->g_adm_cmd_set_work));
    (void)flush_work(&(smain->g_admcmd_timeout_work));
}
void epfront_exec_admcmdstr_exit(struct epfront_main_info *smain)
{
    if(unlikely(!smain)){
        epfront_err("smain is NULL");
        return;
    }
    if(smain->exec_admcmd_str){
        kfree(smain->exec_admcmd_str);
        smain->exec_admcmd_str = NULL;
    }
    if(smain->exec_admcmd_timeout_str){
        kfree(smain->exec_admcmd_timeout_str);
        smain->exec_admcmd_timeout_str = NULL;
    }
}
#endif
/*****************************************************************************
Function    : epfront_base_exit
Description : base exit function
Input       : void
Output      : void
Return      : void
*****************************************************************************/
void epfront_base_exit(struct epfront_main_info *smain)
{
    if (unlikely(!smain)){
        epfront_err("smain is NULL");
        return;
    }

    if(!test_and_clear_bit(EPFRONT_INIT_BASE, &(smain->epfront_status)))
        return ;

    epfront_lun_ctrl_exit(&(smain->lun_ctrl));

    epfront_sv_ctrl_exit(&(smain->sv_ctrl));
#ifdef __ADMIN_CMD__
    epfront_clear_admcmd_operation(smain);
    epfront_clear_cmdcmd_timer(smain);
    epfront_exec_admcmdstr_exit(smain);
#endif
}
#ifdef __ADMIN_CMD__
static int epfront_admcmd_res_filename_init(struct epfront_main_info *smain)
{
    int ret = 0;
    if(unlikely(!smain)){
        epfront_err("the smain is NULL");
        return -EFAULT;
    }
    memset(smain->ep_cmd_res_filename,0,EPFRONT_ADMCMD_RES_FILE_MAX_LENGTH);
    ret = snprintf(smain->ep_cmd_res_filename,EPFRONT_ADMCMD_RES_FILE_MAX_LENGTH,"%s_%d",ADM_CMD_SET_FILE_NAME,smain->sdev->pdev->bus->number);
    if(ret < 0){
        epfront_err("format admin_command_res_filename failure!");
        return -EFAULT;
    }
    return 0;
}
int epfront_exec_admcmdstr_init(struct epfront_main_info *smain)
{
    int ret = 0;
    if(unlikely(!smain)){
        epfront_err("the smain is NULL");
        return -EFAULT;
    }
    smain->exec_admcmd_str = kzalloc(ADM_CMD_SET_DATA_LEN + 20,GFP_KERNEL);
    if(NULL == smain->exec_admcmd_str){
        epfront_err("alloc memory for smain:exec_admcmd_str failure");
        return -ENOMEM;
    }
    smain->exec_admcmd_timeout_str = kzalloc(ADM_CMD_SET_DATA_LEN + 100,GFP_KERNEL);
    if(NULL == smain->exec_admcmd_timeout_str){
        epfront_err("alloc memory for smain:exec_admcmd_timeout_str failure");
        ret = -ENOMEM;
        goto err_free_admcmd_str;
    }
    return 0;
err_free_admcmd_str:
    kfree(smain->exec_admcmd_str);
    smain->exec_admcmd_str = NULL;
    return ret;
}

static void epfront_admcmd_timeout_operation(struct work_struct* work)
{
    int ret = 0;
    char* cmd = NULL;
    char cmdPath[] = "/bin/bash";
    char* cmdEnvp[]={"HOME=/","PATH=/sbin:/bin:/usr/bin",NULL};
    char* cmdArgv[]={cmdPath,"-c",NULL,NULL};
    struct epfront_main_info *smain = container_of(work, struct epfront_main_info, g_admcmd_timeout_work);
    if(unlikely(!smain)){
		epfront_err("smain is NULL");
		return ;
	}
    cmd = smain->exec_admcmd_timeout_str;
    if(unlikely(!cmd)){
        epfront_err(" smain:exec_admcmd_timeout_str is NULL ");
        return;
    }
    //ret = snprintf(cmd,ADM_CMD_SET_DATA_LEN+100,"sdiadm_pid=`ps -ef|grep -v 'grep'|grep -v '%s'|grep '%s'|awk '{print $2}'`;kill -9 $sdiadm_pid",smain->ep_cmd_res_filename,(char*)((smain->g_adm_cmd_set_ctrl).data_virt));
	
    ret = snprintf(cmd,ADM_CMD_SET_DATA_LEN+100,"sdiadm_pid=`ps -ef|grep -v 'grep'|grep '%s'|awk '{print $2}'`;kill -9 $sdiadm_pid",(char*)((smain->g_adm_cmd_set_ctrl).data_virt));
	epfront_info("the timeout_exec_cmd:%s",cmd);
	if(ret < 0 || ret > ADM_CMD_SET_DATA_LEN+100){
		epfront_err("the kill_thd_name format failure!");
		return ;
	}
	cmdArgv[2] = (char*)cmd;
    epfront_info("the admcmd[%s] exec timeout,then will kill it",(char*)((smain->g_adm_cmd_set_ctrl).data_virt));
	mutex_lock(&(smain->adm_timer_inf.timer_exist_mutex));
    (void)call_usermodehelper(cmdPath,cmdArgv,cmdEnvp,UMH_WAIT_PROC);
    smain->adm_timer_inf.adm_cmd_timer_exist = 0;
    mutex_unlock(&(smain->adm_timer_inf.timer_exist_mutex));
    return;
}
#endif
/*****************************************************************************
Function    : epfront_base_init
Description : base init function
Input       : void
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
int epfront_base_init(struct epfront_main_info *smain)
{
    int ret = 0;

    if(test_and_set_bit(EPFRONT_INIT_BASE, &(smain->epfront_status)))
        return 0;
#ifdef __ADMIN_CMD__
    INIT_WORK(&(smain->g_adm_cmd_set_work),epfront_adm_cmd_set_operation);
    memset(&(smain->g_cqe),0,sizeof(struct ep_cqe));
    INIT_WORK(&(smain->g_admcmd_timeout_work),epfront_admcmd_timeout_operation);

    ret = epfront_admcmd_res_filename_init(smain);
    if(ret){
        epfront_err("the admin_command cannot support,ret[%d]",ret);
        goto err_out;
    }
    ret = epfront_exec_admcmdstr_init(smain);
    if(ret){
        epfront_err("epfront_exec_admcmdstr_init failure,ret[%d]",ret);
        goto err_out;
    }
#endif
    memset((void*)smain->epfront_hosts, 0, sizeof(struct epfront_host_ctrl*) * EP_MAX_HOST_NUMBER);
    epfront_io_list_init(smain);

    ret = epfront_lun_ctrl_init(&(smain->lun_ctrl), EP_MAX_UNIQUE_ID);
    if(ret){
        epfront_err("epfront_lun_ctrl_init failed, ret[%d]", ret);
        goto free_exec_cmd_str;
    }

    ret = epfront_sv_ctrl_init(smain, "ep_supervise");
    if(ret){
        epfront_err("epfront_sv_ctrl_init failed, ret[%d]", ret);
        goto lun_ctrl_exit;
    }
#ifdef __ADMIN_CMD__
    epfront_admcmd_timer_init(smain,epfront_admcmd_exec_timeout);
#endif
    return 0;

lun_ctrl_exit:
    epfront_lun_ctrl_exit(&(smain->lun_ctrl));
free_exec_cmd_str:
#ifdef __ADMIN_CMD__
    kfree(smain->exec_admcmd_str);
    smain->exec_admcmd_str = NULL;
    kfree(smain->exec_admcmd_timeout_str);
    smain->exec_admcmd_timeout_str = NULL;
err_out:
#endif
    clear_bit(EPFRONT_INIT_BASE, &(smain->epfront_status));
    return ret;
}

/*****************************************************************************
Function    : epfront_statistic_init
Description : statistic init function
Input       : void
Output      : void
Return      : void
*****************************************************************************/
void epfront_statistic_init(struct epfront_main_info *smain)
{
    int i;

    atomic_set(&(smain->stats.ill_sqtype), 0);
    atomic_set(&(smain->stats.ill_sqpara), 0);
    atomic_set(&(smain->stats.ill_aer_type), 0);
    atomic_set(&(smain->stats.ill_aer_cqe), 0);
    atomic_set(&(smain->stats.ill_io_cqe), 0);
    atomic_set(&(smain->stats.crc_err_notify), 0);
    atomic_set(&(smain->stats.crc_err_aen), 0);

#ifdef EPFRONT_DEBUG
    for(i = 0; i < SV_MAX_LIMIT; ++i){
        atomic_set(&(smain->stats.sv_todo[i]), 0);
        atomic_set(&(smain->stats.sv_done[i]), 0);
    }
    for(i = 0; i < AER_MAX_LIMIT; ++i){
        atomic_set(&(smain->stats.aer_send[i]), 0);
        atomic_set(&(smain->stats.aer_todo[i]), 0);
        atomic_set(&(smain->stats.aer_done[i]), 0);
    }
    smain->stats.cur_type = 0;
    smain->stats.cur_subtype = 0;
#endif
}
#ifdef __ADMIN_CMD__
static void epfront_admcmd_timer_init(struct epfront_main_info *smain,void (*timeout_fun)(unsigned long))
{
    init_timer(&(smain->g_adm_cmd_timer));
    (smain->g_adm_cmd_timer).function = timeout_fun;
   // smain->g_adm_cmd_timer_exist = 0;
   smain->adm_timer_inf.adm_cmd_timer_exist = 0;
   mutex_init(&(smain->adm_timer_inf.timer_exist_mutex));
}
#endif
/*****************************************************************************
Function    : epfront_init
Description : epfront driver init function
Input       : void
Output      : int __init
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int __init epfront_init( void )
{
    int i, ret = 0;

    if(use_cluster)
            epfront_driver_template.use_clustering = ENABLE_CLUSTERING;
    epfront_info("epfront_driver_template.use_clustering is %u", epfront_driver_template.use_clustering);

    memset((void*)gsepmain, 0, SDI_MAX_NRS * sizeof(epfront_main_info_t));

    for (i = 0; i < SDI_MAX_NRS; i++)
    {
        gsepmain[i].global_config.crc32 = 0;
        gsepmain[i].global_config.host_n = EP_DEFAULT_HOST_NUMBER;
        gsepmain[i].global_config.max_channel = EP_DEFAULT_MAX_CHANNEL;
        gsepmain[i].global_config.max_id = EP_DEFAULT_MAX_ID;
        gsepmain[i].global_config.max_lun = EP_DEFAULT_MAX_LUN_PER_HOST;
        gsepmain[i].global_config.max_cmd_len = EP_DEFAULT_CDB_LEN;
        gsepmain[i].global_config.max_nr_cmds = EP_DEFAULT_MAX_CMD_NUMBER;
        gsepmain[i].global_config.cmd_per_lun = EP_DEFAULT_IO_DEPTH_PER_LUN;
        gsepmain[i].global_config.sg_count = EP_DEFAULT_SG_COUNT;
        gsepmain[i].global_config.rq_timeout = EP_DEFAULT_RQ_TIMEOUT * HZ;

        init_waitqueue_head(&gsepmain[i].wait);
    }

    //sysfs init
    ret = epfront_sysfs_init();
    if(ret){
        epfront_err("epfront_sysfs_init failed, ret:%d.", ret);
        goto errout;
    }

    ret = sdi_pf12_common_init();
    if(ret){
        epfront_err("sdi_pf12_common_init failed, ret:%d.", ret);
        goto sysfs_exit;
    }

    epfront_info("epfront_init success.");
    epfront_info("################SCSI_EP_FRONT:"DRV_VERSION" "DRV_DESCRIPTION"################");
    return 0;

sysfs_exit:
    epfront_sysfs_exit();
errout:
    epfront_err("epfront_init failed, ret = %d", ret);
    return ret;
}
#ifdef __ADMIN_CMD__
static void epfront_clear_cmdcmd_timer(struct epfront_main_info *smain)
{
    mutex_lock(&(smain->adm_timer_inf.timer_exist_mutex));
    //smain->adm_timer_inf->adm_cmd_timer_exist = 1;
    if(1 == smain->adm_timer_inf.adm_cmd_timer_exist){
        (void)del_timer_sync(&smain->g_adm_cmd_timer);
        smain->adm_timer_inf.adm_cmd_timer_exist = 0;
    }
   
    mutex_unlock(&(smain->adm_timer_inf.timer_exist_mutex));
}
#endif
/*****************************************************************************
Function    : epfront_exit
Description : epfront exit function
Input       : void
Output      : void __exit
Return      : void __exit
*****************************************************************************/
static void __exit epfront_exit(void)
{
    epfront_info("enter epfront exit");
    sdi_pf12_common_exit();

    epfront_sysfs_exit();
    epfront_info("Virtual storage controller driver unregistered.");
}


/*****************************************************************************
Function    : epfront_aer_recv_add_disk
Description : callback when receive aer message of add disk
Input       : void * data
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_aer_recv_add_disk(void* data, struct epfront_main_info *smain)
{
    int ret = 0;
    struct ep_aer_disk_name* disk_info = NULL;

    disk_info = (struct ep_aer_disk_name*)data;
    if(unlikely(NULL == disk_info)){
        epfront_err("data_virt is NULL");
        return -EINVAL;
    }
    epfront_dbg("back_uniq_id[%u]", disk_info->back_uniq_id);
    disk_info->vol_name[EP_VOL_NAME_LEN - 1] = '\0';
    epfront_dbg("vol_name[%s]", disk_info->vol_name);

    ret = epfront_add_disk(disk_info, smain);
    if(ret){
        epfront_err("add disk failed, back_uniq_id[%u], vol_name[%s]",
                    disk_info->back_uniq_id, disk_info->vol_name);
    } else{
        epfront_info("add disk success, back_uniq_id[%u], vol_name[%s], dev_name[%s]",
                     disk_info->back_uniq_id, disk_info->vol_name, disk_info->dev_name);
    }

    return ret;
}

/*****************************************************************************
Function    : epfront_aer_recv_rmv_disk
Description : callback when receive aer message of remove disk
Input       : void * data
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_aer_recv_rmv_disk(void* data, struct epfront_main_info *smain)
{
    int ret = 0;
    struct ep_aer_disk* disk_info = NULL;

    disk_info = (struct ep_aer_disk*)data;
    if(unlikely(NULL == disk_info)){
        epfront_err("data_virt is NULL");
        return -EINVAL;
    }

    ret = epfront_rmv_disk(disk_info->back_uniq_id, smain);
    if(ret){
        epfront_err("rmv disk failed, back_uniq_id[%u], vol_name[%s]",
            disk_info->back_uniq_id, disk_info->vol_name);
    }
    else{
        epfront_info("rmv disk success, back_uniq_id[%u], vol_name[%s]",
            disk_info->back_uniq_id, disk_info->vol_name);
    }

    return ret;
}

/*****************************************************************************
Function    : epfront_aer_recv_notify_rescan
Description : callback when receive aer message of notify rescan
Input       : void * data
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_aer_recv_notify_rescan(void* data, struct epfront_main_info *smain)
{
    struct epfront_host_ctrl* h = NULL;
    struct epfront_lun_list* lun_lst = NULL;
    struct Scsi_Host *sh = NULL;
    struct scsi_device* sdev = NULL;
    u32 back_uniq_id = 0;
    u32 channel = 0, id = 0, lun = 0;

    if(!data){
        epfront_err("notify data is NULL");
        return -EINVAL;
    }

    back_uniq_id = *(u32*)data;

    h = epfront_get_ctrl_by_uniq(back_uniq_id, smain);
    if(NULL == h){
        epfront_err("epfront_get_ctrl_by_uniq failed");
        return -EFAULT;
    }

    lun_lst = epfront_get_lun_list(&(smain->lun_ctrl), back_uniq_id);
    if(NULL == lun_lst){
        epfront_err("epfront_get_lun_list failed");
        return -EFAULT;
    }

    sh = h->scsi_host;
    if(NULL == sh){
        epfront_err("h's scsi_host is NULL, host_no[%u]", h->sys_host_id);
        return -EFAULT;
    }

    sdev = epfront_scsi_device_lookup(sh, lun_lst->channel, lun_lst->id, lun_lst->lun);
    if(!sdev){
        epfront_err("divice not exist, can'n happen [%u %u %u %u] has added but not find",
            lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun);
        return -ENODEV;
    }

    scsi_rescan_device(&(sdev->sdev_gendev));
    scsi_device_put(sdev);

    epfront_info("notify_rescan success, back_uniq_id[%u], device[%u:%u:%u:%u]",
        back_uniq_id, h->sys_host_id, channel, id, lun);

    return 0;
}

/*****************************************************************************
Function    : epfront_aer_recv_io_switch
Description : callback when receive aer message of io switch
Input       : void * data
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_aer_recv_io_switch(void* data, struct epfront_main_info *smain)
{
    u32 io_switch = 0;

    if(!data){
        epfront_err("io switch data is NULL");
        return -EINVAL;
    }

    if(!list_empty(&(smain->lun_ctrl.async_list))){
        epfront_err("some luns are still attaching,can't block io now");
        return -EPERM;
    }

    io_switch = *(u32*)data;
    switch(io_switch){
        case 0:
            suspend_all_host(smain);
            break;
        case 1:
            resume_all_host(smain);
            break;
        default:
            epfront_err("io_switch[%u] is illegal", io_switch);
            return -EINVAL;
    }

    return 0;
}

/*****************************************************************************
Function    : epfront_aer_recv_linkdown
Description : callback when receive aer message of linkdown
Input       : void * data
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_aer_recv_linkdown(void* data, struct epfront_main_info *smain)
{
    int ret = 0;

    epfront_info("start linkdown process");
    ret = transfer_sys_do_reset(smain->sdev);
    if(ret){
        epfront_err("linkdown_reinit failed, ret[%d]", ret);
        return -EFAULT;
    }
    epfront_info("linkdown reinit sucess");
    return 0;
}

/*****************************************************************************
Function    : epfront_sv_reset_handle
Description : sv thread reset handle
Input       : void * data
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sv_reset_handle(void* data)
{
    int ret = 0;
    unsigned long tmp_status = 0;
    struct epfront_main_info **p = (struct epfront_main_info **)data;
    struct epfront_main_info* spmain = (struct epfront_main_info*)(*p);

    if (unlikely(!spmain)){
        epfront_err("spmain is NULL");
        return -EINVAL;
    }

    if(!test_and_set_bit(EPFRONT_SCSI_RESETTING, &(spmain->epfront_status))){
        set_bit(SDI_FRONT_UPDATE, &tmp_status);
        smp_mb();
        ret = epfront_do_stop_trans(tmp_status, spmain);
        epfront_info("epfront_do_stop_trans ret[%d]", ret);
        epfront_start_trans(spmain);
        clear_bit(EPFRONT_SCSI_RESETTING, &(spmain->epfront_status));
    }

    return ret;
}

/*****************************************************************************
Function    : epfront_sv_rename_luns
Description : sv thread rename luns
Input       : void * data
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sv_rename_luns(void* data)
{
    struct epfront_lun_list *pos = NULL, *tmp = NULL;
    struct epfront_main_info **p = (struct epfront_main_info **)data;
    struct epfront_main_info *spmain = (struct epfront_main_info *)(*p);
    int result = 0;

    if (unlikely(!spmain)){
        epfront_err("spmain is NULL");
        return -EINVAL;
    }

    list_for_each_entry_safe(pos, tmp, &(spmain->lun_ctrl.list), list){
        result = epfront_rename_lun_sys(pos);
        if(result){
            epfront_err("epfront_rename_lun_sys failed");
        }
    }

    if(result){
        (void)epfront_sv_assign_task(&(spmain->sv_ctrl), SV_RENAME_LUNS | EPFRONT_SV_LOW_PRI, epfront_sv_rename_luns,
              &spmain, sizeof(struct epfront_main_info*));
    }

    return result;
}

/*****************************************************************************
Function    : epfront_sv_sync_devname
Description : sv thread sync devices' names
Input       : void * data
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sv_sync_devname(void* data){
    struct ep_aer_disk_list *pos = NULL, *tmp = NULL;
    int ret = 0;
    struct epfront_lun_list *lun_lst = NULL;
    struct epfront_main_info **p = (struct epfront_main_info **)data;
    struct epfront_main_info *smain = (struct epfront_main_info *)(*p);

    if (unlikely(!smain)){
        epfront_err("smain is NULL");
        return 0;
    }

    list_for_each_entry_safe(pos,tmp,&(smain->lun_ctrl.async_devname_list),list){
        lun_lst = epfront_get_lun_list(&(smain->lun_ctrl),pos->disk.back_uniq_id);
        if(!lun_lst){
            epfront_err("invalid unique id %d",pos->disk.back_uniq_id);
            list_del_init(&pos->list);
            kfree(pos);
            continue;
        }
        ret = epfront_get_dev_name(lun_lst, NULL);
        if(!ret){
            list_del_init(&pos->list);
            kfree(pos);
        } else{
            break;
        }
    }

    ret = epfront_sync_devname(EP_SYNC_ALL, EPFRONT_SYNC_DEVNAME_DEFAULT, smain);

    if(!list_empty(&(smain->lun_ctrl.async_devname_list)) || ret){
        (void)epfront_sv_assign_task(&(smain->sv_ctrl), SV_SYNC_DEVNAME | EPFRONT_SV_LOW_PRI, epfront_sv_sync_devname,
              &smain, sizeof(struct epfront_main_info*));
    }

    return 0;
}

/*****************************************************************************
Function    : epfront_sv_sync_disk
Description : sv thread sync disk
Input       : void * data
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sv_sync_disk(void* data)
{
    int ret;
    struct epfront_lun_list* lun_lst = NULL;
    struct epfront_lun_list* tmp = NULL;
    struct ep_aer_disk_name* diskname = NULL;
    struct ep_aer_disk_list *disk_list = NULL;
    struct epfront_main_info **p = (struct epfront_main_info **)data;
    struct epfront_main_info *smain = (struct epfront_main_info *)(*p);

    if (unlikely(!smain)){
        epfront_err("smain is NULL");
        return 0;
    }

    list_for_each_entry_safe(lun_lst, tmp, &(smain->lun_ctrl.async_list), list){

        list_del_init(&lun_lst->list);

        ret = epfront_do_add_disk(lun_lst, (struct ep_aer_disk_name*)diskname, smain);
        if(ret){
            list_add_tail(&lun_lst->list, &(smain->lun_ctrl.async_list));
            epfront_err("async add disk [%u:%u:%u:%u] back_uniq_id[%u] failed, ret[%d]",
                lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun, lun_lst->back_uniq_id, ret);
            break;
        } else{
            ret = epfront_sync_devname(EP_SYNC_ONE, lun_lst->back_uniq_id, smain);
            epfront_err("async devname [%u:%u:%u:%u] back_uniq_id[%u] failed, ret[%d]",
                        lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun, lun_lst->back_uniq_id, ret);
            if(ret){
                disk_list = kmalloc(sizeof(struct ep_aer_disk_list), GFP_KERNEL);
                if(!disk_list){
                    epfront_err("mem is not enough,async add disk [%u:%u:%u:%u] back_uniq_id[%u] devname[%s] failed",
                   lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun, lun_lst->back_uniq_id,lun_lst->dev_name);
                }
                else{
                    disk_list->disk.back_uniq_id = lun_lst->back_uniq_id;
                    memcpy(disk_list->disk.vol_name, lun_lst->vol_name, sizeof(char)*EP_VOL_NAME_LEN);
                    list_add_tail(&disk_list->list,&(smain->lun_ctrl.async_devname_list));
                    (void)epfront_sv_assign_task(&(smain->sv_ctrl), SV_SYNC_DEVNAME, epfront_sv_sync_devname,
                          &smain, sizeof(struct epfront_main_info*));
                }
            }
        }
    }

    if(!list_empty(&(smain->lun_ctrl.async_list))){
        (void)epfront_sv_assign_task(&(smain->sv_ctrl), SV_SYNC_DISK | EPFRONT_SV_LOW_PRI, epfront_sv_sync_disk,
              &smain, sizeof(struct epfront_main_info*));
    } else{
        smain->sync_disk_errreport_flag = 0;
        (void)epfront_sync_result(EP_SYNC_SUCCESS, smain);
        epfront_info("sync all disk success");
    }

    return 0;
}

/*****************************************************************************
Function    : epfront_sv_aer_handle
Description : sv thread aer message handler
Input       : void * data
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sv_aer_handle(void* data)
{
    epfront_aer_recv((struct ep_aer_cqe*)data);
    return 0;
}

/*****************************************************************************
Function    : epfront_sv_back_notify_probe
Description : sv thread probe function of backend notify
Input       : void * data
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sv_back_notify_probe(void *data){

    int ret = 0;
    struct epfront_main_info **p = (struct epfront_main_info **)data;
    struct epfront_main_info *spmain = (struct epfront_main_info *)(*p);

    if (unlikely(!spmain)){
        epfront_err("spmain is NULL");
        return -EINVAL;
    }

    ret = epfront_scsi_back_notify_probe(spmain);
    if(ret){
        //epfront_err("epfront_scsi_back_notify_probe failed, ret[%d]", ret);
        goto wait_notify;
    }

    epfront_info("epfront_start_trans success");
    return 0;

wait_notify:
    if(!test_bit(EPFRONT_SCSI_FAILFAST, &(spmain->epfront_status))){
        msleep(EPFRONT_TRANS_RESET_INTERVAL_TIME);
    }
    (void)epfront_sv_assign_task(&(spmain->sv_ctrl), SV_TRANS_REINIT, epfront_sv_back_notify_probe,
          &spmain, sizeof(struct epfront_main_info*));
    return ret;
}


/*****************************************************************************
Function    : epfront_sv_start_trans
Description : sv thread start trans
Input       : void * data
Output      : int
Return      : VOS_OK on success or error code on failure
*****************************************************************************/
static int epfront_sv_start_trans(void* data)
{
    int ret = 0;
    struct epfront_main_info **p = (struct epfront_main_info **)data;
    struct epfront_main_info *spmain = (struct epfront_main_info *)(*p);

    if (unlikely(!spmain)){
        epfront_err("spmain is NULL");
        return -EINVAL;
    }

    ret = epfront_dev_init(spmain);
    if(ret){
        goto wait_ready;
    }

    ret = epfront_scsi_back_insmod_probe(spmain);
    if(ret){
        goto wait_ready;
    }

    ret = epfront_scsi_back_notify_probe(spmain);
    if(ret){
        goto wait_notify;
    }

    epfront_info("epfront_start_trans success");

    return 0;

wait_ready:
    if(!test_bit(EPFRONT_SCSI_FAILFAST, &(spmain->epfront_status))){
        msleep(EPFRONT_TRANS_RESET_INTERVAL_TIME);
        (void)epfront_sv_assign_task(&(spmain->sv_ctrl), SV_TRANS_REINIT, epfront_sv_start_trans,
              &spmain, sizeof(struct epfront_main_info*));
    }
    return ret;

wait_notify:
    if(!test_bit(EPFRONT_SCSI_FAILFAST, &(spmain->epfront_status))){
        msleep(EPFRONT_TRANS_RESET_INTERVAL_TIME);
        (void)epfront_sv_assign_task(&(spmain->sv_ctrl), SV_TRANS_REINIT, epfront_sv_back_notify_probe,
              &spmain, sizeof(struct epfront_main_info*));
    }

    return ret;
}

module_init(epfront_init);
module_exit(epfront_exit);

module_param(use_cluster,uint,S_IRUGO);
MODULE_PARM_DESC(use_cluster, "use_cluster is 0 or 1, def 0");


MODULE_VERSION(DRV_VERSION);
MODULE_DESCRIPTION("Huawei Cloudstorage SD100 EP SCSI front driver. (EXT: "DRV_DESCRIPTION")");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Huawei Technologies Co., Ltd.");


