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

#define EPFRONT_SYSFS_INFO_MAX_LEN 512

struct kobject* epfront_kobj = NULL;

/* default kobject attribute operations */
/*****************************************************************************
Function    : kobj_attr_show
Description : show kobj's attribute
Input       : struct kobject * kobj
              struct attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t kobj_attr_show(struct kobject *kobj, struct attribute *attr,
			      char *buf)
{
	struct kobj_attribute *kattr;
	ssize_t ret = -EIO;

	kattr = container_of(attr, struct kobj_attribute, attr);
	if (kattr->show)
		ret = kattr->show(kobj, kattr, buf);
	return ret;
}

/*****************************************************************************
Function    : kobj_attr_store
Description : store kobj's attribute
Input       : struct kobject * kobj
              struct attribute * attr
              const char * buf
              size_t count
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t kobj_attr_store(struct kobject *kobj, struct attribute *attr,
			       const char *buf, size_t count)
{
	struct kobj_attribute *kattr;
	ssize_t ret = -EIO;

	kattr = container_of(attr, struct kobj_attribute, attr);
	if (kattr->store)
		ret = kattr->store(kobj, kattr, buf, count);
	return ret;
}

static struct sysfs_ops epfront_kobj_sysfs_ops = {
	.show	= kobj_attr_show,
	.store	= kobj_attr_store,
};

static struct kobj_type epfront_sysfs_ktype = {
    .sysfs_ops = &epfront_kobj_sysfs_ops
};

/*****************************************************************************
Function    : epfront_lun_profile_show
Description : show epfront lun's profile
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t epfront_lun_profile_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
    struct epfront_lun_list* lun_lst = container_of(kobj, struct epfront_lun_list, kobj);
    
    return snprintf(buf, EPFRONT_SYSFS_INFO_MAX_LEN,
        "back_uniq_id  [%u],\t host_index  [%u]\n"
        "[%u:%u:%u:%u] %s\n",
        lun_lst->back_uniq_id, lun_lst->host_index,
        lun_lst->host, lun_lst->channel, lun_lst->id, lun_lst->lun,
        lun_lst->vol_name);
}

/*****************************************************************************
Function    : epfront_lun_stat_show
Description : show epfront lun's status
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t epfront_lun_stat_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
    struct epfront_lun_list* lun_lst = container_of(kobj, struct epfront_lun_list, kobj);

    return snprintf(buf, EPFRONT_SYSFS_INFO_MAX_LEN,
		"send_num [%u]\t recv_num [%u]\n"
		"abort_num [%u]\t back_abort[%u]\n"
		"crc_error [%u]\t crc_data_error [%u]\n",
        atomic_read(&lun_lst->send_num), atomic_read(&lun_lst->recv_num),
        atomic_read(&lun_lst->abort_num), atomic_read(&lun_lst->back_abort),
        atomic_read(&lun_lst->crc_error), atomic_read(&lun_lst->crc_data_error));
}

//can define lun_attribute  epfront_lun_stat_show(struct epfront_lun_list* lun_lst, struct lun_attribute* attr, char* buf) this formation
static struct kobj_attribute epfront_lun_profile_attr = __ATTR(profile, S_IRUGO, epfront_lun_profile_show, NULL);
static struct kobj_attribute epfront_lun_stat_attr = __ATTR(statistic, S_IRUGO, epfront_lun_stat_show, NULL);

/*****************************************************************************
Function    : epfront_destroy_lun_sysfs
Description : destroy epfront lun's sysfs
Input       : struct epfront_lun_list * lun_lst
Output      : void
Return      : void
*****************************************************************************/
void epfront_destroy_lun_sysfs(struct epfront_lun_list* lun_lst)
{
    struct kobject *kobj_ptr = &lun_lst->kobj;

    sysfs_remove_file(kobj_ptr, &epfront_lun_stat_attr.attr);
    sysfs_remove_file(kobj_ptr, &epfront_lun_profile_attr.attr);
    kobject_del(kobj_ptr);
    kobject_put(kobj_ptr);
}

/*****************************************************************************
Function    : epfront_create_lun_sysfs
Description : create epfront lun's sysfs
Input       : struct epfront_lun_list * lun_lst
              struct kobject * parent
Output      : int
Return      : int
*****************************************************************************/
int epfront_create_lun_sysfs(struct epfront_lun_list* lun_lst, struct kobject* parent)
{
    int ret = 0;
    struct kobject* kobj_ptr = NULL;
    
    if(unlikely(!lun_lst || !parent)){
        epfront_err("illegal para");
        return -EINVAL;
    }
	
    kobj_ptr = &lun_lst->kobj;
    ret = kobject_init_and_add(kobj_ptr, &epfront_sysfs_ktype, parent, "lun%u", lun_lst->back_uniq_id);
    if(ret){
        epfront_err("create lun[%u] sysfs failed", lun_lst->back_uniq_id);
        kobject_put(kobj_ptr);
        return ret;
    }


    ret = sysfs_create_file(kobj_ptr, &epfront_lun_profile_attr.attr);
    if(ret){
        epfront_err("create lun[%u] sysfs epfront_lun_profile_attr failed", lun_lst->back_uniq_id);
        goto rm_lun_sysfs;
    }

    ret = sysfs_create_file(kobj_ptr, &epfront_lun_stat_attr.attr);
    if(ret){
        epfront_err("create lun[%u] sysfs epfront_lun_stat_attr failed", lun_lst->back_uniq_id);
        goto rm_lun_profile;
    }

    return 0;
    
rm_lun_profile:
    sysfs_remove_file(kobj_ptr, &epfront_lun_stat_attr.attr);
rm_lun_sysfs:
    kobject_del(kobj_ptr);
    kobject_put(kobj_ptr);
    return ret;
}

/*****************************************************************************
Function    : epfront_host_stat_show
Description : show host's status
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t epfront_host_stat_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
    struct epfront_host_ctrl* ctrl_info = container_of(kobj, struct epfront_host_ctrl, kobj);
    
    return snprintf(buf, EPFRONT_SYSFS_INFO_MAX_LEN,
        "cmd_sn         [%u],\t cmds_num          [%u]\n"
        "abort_succ     [%u],\t abort_fail        [%u]\n"
        "reset_succ     [%u],\t reset_fail        [%u]\n"
        "conflict_num   [%u]\n",
        atomic_read(&ctrl_info->cmd_sn), atomic_read(&ctrl_info->cmds_num),
        atomic_read(&ctrl_info->abort_succ), atomic_read(&ctrl_info->abort_fail),
        atomic_read(&ctrl_info->reset_succ), atomic_read(&ctrl_info->reset_fail),
        atomic_read(&ctrl_info->conflict_num));
}

/*****************************************************************************
Function    : epfront_host_outline_show
Description : show host's outline
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t epfront_host_outline_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
    struct epfront_host_ctrl* ctrl_info = container_of(kobj, struct epfront_host_ctrl, kobj);

    return snprintf(buf, EPFRONT_SYSFS_INFO_MAX_LEN, "host_no : %u\t nr_cmds : %u\n",
        epfront_ctrl_get_host_no(ctrl_info), ctrl_info->nr_cmds);
}

static struct kobj_attribute epfront_host_stat_attr = __ATTR(statistic, S_IRUGO, epfront_host_stat_show, NULL);
static struct kobj_attribute epfront_host_outline_attr = __ATTR(outline, S_IRUGO, epfront_host_outline_show, NULL);

/*****************************************************************************
Function    : epfront_destroy_host_sysfs
Description : destroy host's sysfs
Input       : struct epfront_host_ctrl * h
Output      : void
Return      : void
*****************************************************************************/
void epfront_destroy_host_sysfs(struct epfront_host_ctrl* h)
{
    struct kobject *kobj_ptr = &h->kobj;

    sysfs_remove_file(kobj_ptr, &epfront_host_stat_attr.attr);
    sysfs_remove_file(kobj_ptr, &epfront_host_outline_attr.attr);
    kobject_del(kobj_ptr);
    kobject_put(kobj_ptr);
}


/*****************************************************************************
Function    : epfront_create_host_sysfs
Description : create host's sysfs
Input       : struct epfront_host_ctrl * h
              struct kobject * parent
Output      : int
Return      : int
*****************************************************************************/
/*h->host_id must be assigned diffrent value before call epfront_create_hostinfo_sysfs*/
int epfront_create_host_sysfs(struct epfront_host_ctrl* h, struct kobject* parent)
{
    int ret = 0;
    struct kobject* kobj_ptr = NULL;
    
    if(unlikely(!h||!parent))
    {
        epfront_err("illegal para");
        return -EINVAL;
    }
	
    kobj_ptr = &h->kobj;
    ret = kobject_init_and_add(kobj_ptr, &epfront_sysfs_ktype, parent, "host%u", epfront_ctrl_get_host_no(h));
    if(ret)
    {
        epfront_err("create host[%u] sysfs failed", epfront_ctrl_get_host_no(h));
        kobject_put(kobj_ptr);
        return ret;
    }

    ret = sysfs_create_file(kobj_ptr, &epfront_host_outline_attr.attr);
    if(ret)
    {
        epfront_err("create host[%u] sysfs epfront_host_outline_attr failed", epfront_ctrl_get_host_no(h));
        goto rm_hostinfo;
    }

    ret = sysfs_create_file(kobj_ptr, &epfront_host_stat_attr.attr);
    if(ret)
    {
        epfront_err("create host[%u] sysfs epfront_host_stat_attr failed", epfront_ctrl_get_host_no(h));
        goto rm_hostinfo_outline;
    }

    return 0;
    
rm_hostinfo_outline:
    sysfs_remove_file(kobj_ptr, &epfront_host_outline_attr.attr);
rm_hostinfo:
    kobject_del(kobj_ptr);
    kobject_put(kobj_ptr);
    return ret;
}

/*****************************************************************************
Function    : epfront_log_level_show
Description : show epfront log level
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t epfront_log_level_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
    return snprintf(buf, sizeof(epfront_loglevel), "%d\n", epfront_loglevel);
}

/*****************************************************************************
Function    : epfront_log_level_store
Description : store epfront log level
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              const char * buf
              size_t count
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t epfront_log_level_store(struct kobject* kobj, struct kobj_attribute* attr, const char* buf, size_t count)
{
    sscanf(buf, "%d", &epfront_loglevel);
    epfront_info("count is %lu", count);
    return (ssize_t)count;
}

static struct kobj_attribute epfront_log_level_attr = __ATTR(log_level, (S_IRUGO | S_IWUSR), epfront_log_level_show, epfront_log_level_store);

/*****************************************************************************
Function    : epfront_version_show
Description : show epfront version
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t epfront_version_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
	return snprintf(buf, EPFRONT_SYSFS_INFO_MAX_LEN, "%s\n", DRV_VERSION);
}

static struct kobj_attribute epfront_version_attr = __ATTR(version, S_IRUGO, epfront_version_show, NULL);

/*****************************************************************************
Function    : epfront_statistic_show
Description : show statistic
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t epfront_statistic_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
    int count = 0;
    int i = 0;

    count += snprintf(buf, EPFRONT_SYSFS_INFO_MAX_LEN,
		"ill_sqtype : %u, ill_sqpara: %u\n"
		"ill_aer_type : %u, ill_aer_cqe: %u\n"
		"ill_sqtype : %u\n"
		"crc_err_notify: %u, crc_err_aen: %u\n",
		atomic_read(&g_stats.ill_sqtype), atomic_read(&g_stats.ill_sqpara),
		atomic_read(&g_stats.ill_aer_type), atomic_read(&g_stats.ill_aer_cqe),
		atomic_read(&g_stats.ill_io_cqe),
		atomic_read(&g_stats.crc_err_notify), atomic_read(&g_stats.crc_err_aen));

#ifdef EPFRONT_DEBUG
    count += snprintf(buf+count, EPFRONT_SYSFS_INFO_MAX_LEN,
		"cur_run_type: %d, cur_run_subtype: %d\n",
		g_stats.cur_type, g_stats.cur_subtype);
	
    count += snprintf(buf+count, EPFRONT_SYSFS_INFO_MAX_LEN,
		"sv_type:  todo\t done\n");
    for(i = 0; i < SV_MAX_LIMIT; ++i){
		count += snprintf(buf+count, EPFRONT_SYSFS_INFO_MAX_LEN,
			"%d :\t   %u\t %u\n",
			i, atomic_read(&g_stats.sv_todo[i]), atomic_read(&g_stats.sv_done[i]));
	}
	
    count += snprintf(buf+count, EPFRONT_SYSFS_INFO_MAX_LEN,
		"aer_type: send\t todo\t done\n");
    for(i = 0; i < AER_MAX_LIMIT; ++i){
		count += snprintf(buf+count, EPFRONT_SYSFS_INFO_MAX_LEN,
			"%d :\t  %u\t %u\t %u\n",
			i, atomic_read(&g_stats.aer_send[i]), atomic_read(&g_stats.aer_todo[i]), atomic_read(&g_stats.aer_done[i]));
	}
#endif

    return count;
}

static struct kobj_attribute epfront_statistic_attr = __ATTR(statistic, S_IRUGO, epfront_statistic_show, NULL);

/*****************************************************************************
Function    : epfront_resource_show
Description : show epfront resource
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t epfront_resource_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
    return snprintf(buf, EPFRONT_SYSFS_INFO_MAX_LEN, "0x%lx\n", epfront_status);
}

static struct kobj_attribute epfront_resource_attr = __ATTR(resource, S_IRUGO, epfront_resource_show, NULL);

static struct attribute* epfront_common_attrs[] = {
    &epfront_log_level_attr.attr,
	&epfront_version_attr.attr,
	&epfront_statistic_attr.attr,
	&epfront_resource_attr.attr,
    NULL
};

static struct attribute_group epfront_common_attrs_group = {
    .attrs = epfront_common_attrs,
};

/*****************************************************************************
Function    : epfront_sysfs_init
Description : initialize sysfs
Input       : void
Output      : int
Return      : int
*****************************************************************************/
int epfront_sysfs_init(void)
{
    int ret = 0;
    
    epfront_kobj = kobject_create_and_add("epfront", kernel_kobj);
    if(NULL == epfront_kobj)
    {
        epfront_err("create and add epfront kobject failed");
        return -ENOMEM;
    }

    ret = sysfs_create_group(epfront_kobj, &epfront_common_attrs_group);
    if(ret)
    {
        kobject_del(epfront_kobj);
        kobject_put(epfront_kobj);
        epfront_err("create group attrs failed, ret[%d]", ret);
        return ret;
    }

    return 0;
}

/*****************************************************************************
Function    : epfront_sysfs_exit
Description : exit sysfs
Input       : void
Output      : void
Return      : void
*****************************************************************************/
void epfront_sysfs_exit(void)
{
    sysfs_remove_group(epfront_kobj, &epfront_common_attrs_group);
    kobject_del(epfront_kobj);
    kobject_put(epfront_kobj);
	epfront_kobj = NULL;
}


/*************************************pfx sys begin*************************************************/
/*****************************************************************************
Function    : sdi_cq_profile_show
Description : show cq's profile
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t sdi_cq_profile_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
    sdi_cq_info_t* cq_info = container_of(kobj, sdi_cq_info_t, kobj);
    
	return snprintf(buf, EPFRONT_SYSFS_INFO_MAX_LEN,
		"dma_addr[%Lx] cq_vector[%d] q_depth[%d] stride[%d] q_type[%d]\n",
		cq_info->dma_addr, cq_info->cq_vector, cq_info->q_depth, cq_info->stride, cq_info->q_type);
}

/*****************************************************************************
Function    : sdi_cq_stat_show
Description : show cq's status
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t sdi_cq_stat_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
    sdi_cq_info_t* cq_info = container_of(kobj, sdi_cq_info_t, kobj);

	return snprintf(buf, EPFRONT_SYSFS_INFO_MAX_LEN,
		"rx_ok_cnt[%llu]\t rx_ng_cnt[%llu]\n",
		cq_info->rx_ok_cnt, cq_info->rx_ng_cnt);
}

static struct kobj_attribute sdi_cq_profile_attr = __ATTR(profile, S_IRUGO, sdi_cq_profile_show, NULL);
static struct kobj_attribute sdi_cq_stat_attr = __ATTR(statistic, S_IRUGO, sdi_cq_stat_show, NULL);

/*****************************************************************************
Function    : sdi_destroy_cq_sysfs
Description : destroy cq's sysfs
Input       : sdi_cq_info_t * cq_info
Output      : void
Return      : void
*****************************************************************************/
void sdi_destroy_cq_sysfs(sdi_cq_info_t* cq_info)
{
    struct kobject *kobj_ptr = &cq_info->kobj;

    sysfs_remove_file(kobj_ptr, &sdi_cq_stat_attr.attr);
    sysfs_remove_file(kobj_ptr, &sdi_cq_profile_attr.attr);
    kobject_del(kobj_ptr);
    kobject_put(kobj_ptr);
}

/*****************************************************************************
Function    : sdi_create_cq_sysfs
Description : create cq's sysfs
Input       : sdi_cq_info_t * cq_info
              struct kobject * parent
Output      : int
Return      : int
*****************************************************************************/
int sdi_create_cq_sysfs(sdi_cq_info_t* cq_info, struct kobject* parent)
{
    int ret = 0;
    struct kobject* kobj_ptr = NULL;
    
    if(unlikely(!cq_info || !parent)){
        epfront_err("illegal para");
        return -EINVAL;
    }
	
    kobj_ptr = &cq_info->kobj;
    ret = kobject_init_and_add(kobj_ptr, &epfront_sysfs_ktype, parent, "cq%u", cq_info->cqid);
    if(ret){
        epfront_err("create cq[%u] sysfs failed", cq_info->cqid);
        kobject_put(kobj_ptr);
        return ret;
    }

    ret = sysfs_create_file(kobj_ptr, &sdi_cq_profile_attr.attr);
    if(ret){
        epfront_err("create cq[%u] sysfs sdi_cq_profile_attr failed", cq_info->cqid);
        goto rm_cq_sysfs;
    }

    ret = sysfs_create_file(kobj_ptr, &sdi_cq_stat_attr.attr);
    if(ret){
        epfront_err("create cq[%u] sysfs sdi_cq_stat_attr failed", cq_info->cqid);
        goto rm_cq_profile;
    }

    return 0;
    
rm_cq_profile:
    sysfs_remove_file(kobj_ptr, &sdi_cq_stat_attr.attr);
rm_cq_sysfs:
    kobject_del(kobj_ptr);
    kobject_put(kobj_ptr);
    return ret;
}

/*****************************************************************************
Function    : sdi_sq_profile_show
Description : show sq's profile
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t sdi_sq_profile_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
    int count = 0;
    u16 head, tail, depth;
    sdi_sq_info_t* sq_info = container_of(kobj, sdi_sq_info_t, kobj);
	
	head = sq_info->head;
	tail = sq_info->tail;
	depth = sq_info->q_depth;

	count += snprintf(buf, EPFRONT_SYSFS_INFO_MAX_LEN,
		"cq_id[%d] dma_addr[%Lx] q_depth[%d] stride[%d] q_type[%d]\n",
		sq_info->cq_id, sq_info->dma_addr, sq_info->q_depth, sq_info->stride, sq_info->q_type);
	
    count += snprintf(buf + count, EPFRONT_SYSFS_INFO_MAX_LEN,
		"head[%d]\t tail[%d]\t free[%d]\t used[%d]\n",
		head, tail, Q_USED(head, tail, depth), Q_FREE(head, tail, depth));

	return count;
}

/*****************************************************************************
Function    : sdi_sq_stat_show
Description : show sq's status
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t sdi_sq_stat_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
    sdi_sq_info_t* sq_info = container_of(kobj, sdi_sq_info_t, kobj);
	
	return snprintf(buf, EPFRONT_SYSFS_INFO_MAX_LEN,
		"tx_ok_cnt[%llu]\t tx_busy_cnt[%llu]\n",
		sq_info->tx_ok_cnt, sq_info->tx_busy_cnt);
}

static struct kobj_attribute sdi_sq_profile_attr = __ATTR(profile, S_IRUGO, sdi_sq_profile_show, NULL);
static struct kobj_attribute sdi_sq_stat_attr = __ATTR(statistic, S_IRUGO, sdi_sq_stat_show, NULL);

/*****************************************************************************
Function    : sdi_destroy_sq_sysfs
Description : destroy sq's sysfs
Input       : sdi_sq_info_t * sq_info
Output      : void
Return      : void
*****************************************************************************/
void sdi_destroy_sq_sysfs(sdi_sq_info_t* sq_info)
{
    struct kobject *kobj_ptr = &sq_info->kobj;

    sysfs_remove_file(kobj_ptr, &sdi_sq_stat_attr.attr);
    sysfs_remove_file(kobj_ptr, &sdi_sq_profile_attr.attr);
    kobject_del(kobj_ptr);
    kobject_put(kobj_ptr);
}

/*****************************************************************************
Function    : sdi_create_sq_sysfs
Description : create sq's sysfs
Input       : sdi_sq_info_t * sq_info
              struct kobject * parent
Output      : int
Return      : int
*****************************************************************************/
int sdi_create_sq_sysfs(sdi_sq_info_t* sq_info, struct kobject* parent)
{
    int ret = 0;
    struct kobject* kobj_ptr = NULL;
    
    if(unlikely(!sq_info || !parent)){
        epfront_err("illegal para");
        return -EINVAL;
    }
	
    kobj_ptr = &sq_info->kobj;
    ret = kobject_init_and_add(kobj_ptr, &epfront_sysfs_ktype, parent, "sq%u", sq_info->sqid);
    if(ret){
        epfront_err("create sq[%u] sysfs failed", sq_info->sqid);
        kobject_put(kobj_ptr);
        return ret;
    }

    ret = sysfs_create_file(kobj_ptr, &sdi_sq_profile_attr.attr);
    if(ret){
        epfront_err("create sq[%u] sysfs sdi_sq_profile_attr failed", sq_info->sqid);
        goto rm_sq_sysfs;
    }

    ret = sysfs_create_file(kobj_ptr, &sdi_sq_stat_attr.attr);
    if(ret){
        epfront_err("create sq[%u] sysfs sdi_sq_stat_attr failed", sq_info->sqid);
        goto rm_sq_profile;
    }

    return 0;
    
rm_sq_profile:
    sysfs_remove_file(kobj_ptr, &sdi_sq_stat_attr.attr);
rm_sq_sysfs:
    kobject_del(kobj_ptr);
    kobject_put(kobj_ptr);
    return ret;
}


/*****************************************************************************
Function    : sdi_queue_group_show
Description : show queue group information
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              char * buf
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t sdi_queue_group_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf)
{
#define GROUP_ID_MAX_LEN 32
    sdi_pdev_info_t* info = container_of(kobj, sdi_pdev_info_t, kobj);

	return snprintf(buf, GROUP_ID_MAX_LEN, "group id: %d\n", info->cur_queue_group);
#undef GROUP_ID_MAX_LEN
}

/*****************************************************************************
Function    : sdi_queue_group_store
Description : store queue group information
Input       : struct kobject * kobj
              struct kobj_attribute * attr
              const char * buf
              size_t count
Output      : ssize_t
Return      : ssize_t
*****************************************************************************/
static ssize_t sdi_queue_group_store(struct kobject* kobj, struct kobj_attribute* attr, const char* buf, size_t count)
{
    sdi_pdev_info_t* info = container_of(kobj, sdi_pdev_info_t, kobj);
    u32 num = (u32)simple_strtoul(buf, NULL, 0);
#define MAX_GROUP_ID 8
    if (num >= MAX_GROUP_ID)
    {
        epfront_err("group id is not invalid.");
        return (ssize_t)count;
    }
#undef MAX_GROUP_ID

    info->cur_queue_group = num;

    return (ssize_t)count;
}

static struct kobj_attribute sdi_queue_group_attr = __ATTR(queue_group, (S_IRUGO | S_IWUSR), sdi_queue_group_show, sdi_queue_group_store);

static struct attribute* sdi_pf1_attrs[] = {
    &sdi_queue_group_attr.attr,
    NULL
};

static struct attribute_group sdi_pf1_attrs_group = {
    .attrs = sdi_pf1_attrs,
};

/*****************************************************************************
Function    : create_sdi_pfx_sys
Description : create pfx group sysfs
Input       : sdi_pdev_info_t * adapter
              struct kobject * parent
Output      : int
Return      : int
*****************************************************************************/
int create_sdi_pfx_sys(sdi_pdev_info_t *adapter, struct kobject *parent)
{
    int ret;
    struct kobject *kobj_ptr = &adapter->kobj;

    ret = kobject_init_and_add(kobj_ptr, &epfront_sysfs_ktype, parent, "sdi_pf1");
    if (ret)
    {
        epfront_err("Create SDI PF[1] sys failed, ret[%d]", ret);
        kobject_put(kobj_ptr);
        return ret;
    }

    ret = sysfs_create_group(kobj_ptr, &sdi_pf1_attrs_group);
    if(ret)
    {
        kobject_del(kobj_ptr);
        kobject_put(kobj_ptr);
        epfront_err("create SDI PF[1] sys group attrs failed, ret[%d]", ret);
        return ret;
    }

    return 0;
}

/*****************************************************************************
Function    : clean_sdi_pfx_sys
Description : clean pfx group sysfs
Input       : sdi_pdev_info_t * adapter
Output      : void
Return      : void
*****************************************************************************/
void clean_sdi_pfx_sys(sdi_pdev_info_t *adapter)
{
    struct kobject *kobj_ptr = &adapter->kobj;
	
    sysfs_remove_group(kobj_ptr, &sdi_pf1_attrs_group);
    kobject_del(kobj_ptr);
    kobject_put(kobj_ptr);
}
/*************************************pfx sys end**************************************************/
