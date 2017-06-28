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

#ifndef __EPFRONT_SYSFS_H_
#define __EPFRONT_SYSFS_H_

#include <linux/kobject.h>
#include <linux/sysfs.h>
#include "epfront_main.h"

extern struct kobject* epfront_kobj;

void epfront_destroy_lun_sysfs(struct epfront_lun_list* lun_lst);
int epfront_create_lun_sysfs(struct epfront_lun_list* lun_lst, struct kobject* parent);

void epfront_destroy_host_sysfs(struct epfront_host_ctrl* h);
int epfront_create_host_sysfs(struct epfront_host_ctrl* h, struct kobject* parent);


void sdi_destroy_cq_sysfs(sdi_cq_info_t* cq_info);
int sdi_create_cq_sysfs(sdi_cq_info_t* cq_info, struct kobject* parent);
void sdi_destroy_sq_sysfs(sdi_sq_info_t* sq_info);
int sdi_create_sq_sysfs(sdi_sq_info_t* sq_info, struct kobject* parent);

void clean_sdi_pfx_sys(sdi_pdev_info_t *adapter);
int create_sdi_pfx_sys(sdi_pdev_info_t *adapter, struct kobject *parent);

int  epfront_sysfs_init(void);
void epfront_sysfs_exit(void);

#endif
