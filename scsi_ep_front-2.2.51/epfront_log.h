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

#ifndef __EPFRONT_LOG_H_
#define __EPFRONT_LOG_H_

extern int epfront_loglevel;

#ifndef UNREFERENCE_PARAM
    #define UNREFERENCE_PARAM(x) ((void)(x))
#endif

#define PREFIX "SD100EP: "

typedef enum
{
    EPFRONT_LOG_ERR = 0,
    EPFRONT_LOG_WARNING = 1,
    EPFRONT_LOG_INFO    = 2,
    EPFRONT_LOG_DEBUG     = 3,
    EPFRONT_LOG_TYPE_COUNT
}enum_epfront_log_type;

#define epfront_err(fmt, ...) do{\
        if(epfront_loglevel >= EPFRONT_LOG_ERR) (void)printk(KERN_ERR PREFIX "[ERR][%s][%d]: " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__);\
    } while (0)

#define epfront_err_limit(fmt, ...) do{\
        if(epfront_loglevel >= EPFRONT_LOG_ERR && printk_ratelimit()) (void)printk(KERN_ERR PREFIX "[ERR][%s][%d]: " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__);\
    } while (0)

#define epfront_warn(fmt, ...) do{\
        if(epfront_loglevel >= EPFRONT_LOG_WARNING) (void)printk(KERN_WARNING PREFIX "[WARN][%s][%d]: " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__);\
    }while(0)

#define epfront_info(fmt, ...) do{\
        if(epfront_loglevel >= EPFRONT_LOG_INFO) (void)printk(KERN_NOTICE PREFIX "[INFO][%s][%d]: " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__);\
    } while (0)

#define epfront_info_limit(fmt, ...) do{\
        if(epfront_loglevel >= EPFRONT_LOG_INFO && printk_ratelimit()) (void)printk(KERN_NOTICE PREFIX "[INFO][%s][%d]: " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__);\
    } while (0)

#define epfront_dbg(fmt, ...) do{\
        if(epfront_loglevel >= EPFRONT_LOG_DEBUG) (void)printk(KERN_DEBUG PREFIX "[DEBUG][%s][%d]: " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__);\
    } while (0)

#endif
