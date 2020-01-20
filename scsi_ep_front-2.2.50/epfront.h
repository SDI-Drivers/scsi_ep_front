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

#ifndef __EPFRONT_COMMON_H_
#define __EPFRONT_COMMON_H_
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/spinlock_types.h>
#include <linux/version.h>

#include <linux/kobject.h>
#include <linux/sysfs.h>

#include <linux/mod_devicetable.h>
#include <asm/cpufeature.h>
#include <linux/crc32.h>

#ifdef PCLINT
#include <asm/dma-mapping-common.h>
#include <asm/bitsperlong.h>
#include <linux/fcntl.h>
#include <linux/limits.h>
#endif

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_transport.h>
#include <scsi/scsi_transport_sas.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_tcq.h>

#include "ep_com.h"
#include "epfront_bsfifo.h"

#include "epfront_log.h"
#include "epfront_transfer.h"
#include "epfront_main.h"
#include "epfront_sysfs.h"

#ifndef DRV_VERSION
#define DRV_VERSION "NA"
#endif

#ifndef DRV_DESCRIPTION
#define DRV_DESCRIPTION "NA"
#endif

static inline void print_data(char *data, unsigned int len)
{
    unsigned int i,offset;
    char print_buf[16];

    offset = len & 0xf;

    epfront_err_limit("data len %d, offset %d", len, offset);
    for(i =0 ; i< (len >> 4); i++){
        epfront_err_limit("%2x %2x %2x %2x\t%2x %2x %2x %2x\t%2x %2x %2x %2x\t%2x %2x %2x %2x"
                ,data[(long)i*16],data[(long)i*16+1],data[(long)i*16+2],data[(long)i*16+3]
                ,data[(long)i*16+4],data[(long)i*16+5],data[(long)i*16+6],data[(long)i*16+7]
                ,data[(long)i*16+8],data[(long)i*16+9],data[(long)i*16+10],data[(long)i*16+11]
                ,data[(long)i*16+12],data[(long)i*16+13],data[(long)i*16+14],data[(long)i*16+15]);
    }

    if(offset){
        memset((void*)print_buf, 0, 16);
        memcpy(print_buf,&data[(long)(len - offset)],offset);
        epfront_err_limit("%2x %2x %2x %2x\t%2x %2x %2x %2x\t%2x %2x %2x %2x\t%2x %2x %2x %2x"
                ,print_buf[0],print_buf[1],print_buf[2],print_buf[3]
                ,print_buf[4],print_buf[5],print_buf[6],print_buf[7]
                ,print_buf[8],print_buf[9],print_buf[10],print_buf[11]
                ,print_buf[12],print_buf[13],print_buf[14],print_buf[15]);
    }
}

static inline unsigned long get_ns_time(void)
{
    struct timespec now;
    getnstimeofday(&now);
    return timespec_to_ns(&now);
}


#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 32) && !defined(RHEL_RELEASE))
static inline bool __must_check IS_ERR_OR_NULL(const void *ptr)
{
    return !ptr || IS_ERR(ptr);
}

#define for_each_set_bit(bit, addr, size) \
        for ((bit) = find_first_bit((addr), (size));        \
             (bit) < (size);                    \
             (bit) = find_next_bit((addr), (size), (bit) + 1))
#endif

#endif

