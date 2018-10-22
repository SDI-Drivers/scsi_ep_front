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

#ifndef __EPFRONT_BSFIFO_H_
#define __EPFRONT_BSFIFO_H_

#include <linux/spinlock.h>

struct epfront_bs_fifo{
	unsigned int	in;
	unsigned int	out;
	unsigned int	mask;
	unsigned int    recsize;
	void		*data;
};

static inline void bsfifo_reset_out(struct epfront_bs_fifo* fifo)
{
	fifo->out = fifo->in;
}

static inline bool bsfifo_is_empty(struct epfront_bs_fifo* fifo)
{
    return fifo->in == fifo->out;
}

static inline int bsfifo_alloc(struct epfront_bs_fifo* fifo, int size, gfp_t gfp_mask)
{
	/*
	 * round down to the next power of 2, since our 'let the indices
	 * wrap' technique works only in this case.
	 */
    /*lint -e587*/
	size = (int)roundup_pow_of_two((unsigned)( long long )size);

	fifo->in = 0;
	fifo->out = 0;
	fifo->recsize = 1;

	if (size < 2) {
		fifo->data = NULL;
		fifo->mask = 0;
		return -EINVAL;
	}
    /*lint +e587*/

	fifo->data = kmalloc(size * 1, gfp_mask);

	if (!fifo->data) {
		fifo->mask = 0;
		return -ENOMEM;
	}
	fifo->mask = size - 1;

	return 0;
}

static inline void bsfifo_free(struct epfront_bs_fifo* fifo)
{
	kfree(fifo->data);
	fifo->in = 0;
	fifo->out = 0;
	fifo->recsize = 0;
	fifo->data = NULL;
	fifo->mask = 0;
}

static inline void bsfifo_poke_n(struct epfront_bs_fifo *fifo, unsigned int n, unsigned int recsize)
{
	unsigned int mask = fifo->mask;
	unsigned char *data = fifo->data;

	(data)[(fifo->in) & (mask)] = (unsigned char)(n);

	if (recsize > 1)
		(data)[(fifo->in + 1) & (mask)] = (unsigned char)(n >> 8);
}

static inline void bsfifo_copy_in(struct epfront_bs_fifo *fifo, const void *src,
		unsigned int len, unsigned int off)
{
	unsigned int size = fifo->mask + 1;
	unsigned int l;

	off &= fifo->mask;

	l = min(len, size - off);
	memcpy((unsigned char*)fifo->data + off, (unsigned char*)src, l);
	memcpy((unsigned char*)fifo->data, (unsigned char*)src + l, len - l);
	/*
	 * make sure that the data in the fifo is up to date before
	 * incrementing the fifo->in index counter
	 */
	smp_wmb();
}

static inline unsigned int bsfifo_in(struct epfront_bs_fifo* fifo, void* buf, unsigned int len)
{
	if ( (len + fifo->recsize) >
		((fifo->mask + 1) - (fifo->in - fifo->out)) )
		return 0;

	bsfifo_poke_n(fifo, len, fifo->recsize);

	bsfifo_copy_in(fifo, buf, len, fifo->in + fifo->recsize);
	fifo->in += len + fifo->recsize;
	return len;
}

static inline unsigned int bsfifo_in_spinlocked(struct epfront_bs_fifo* fifo, void* buf, unsigned int len, spinlock_t* lock)
{
	unsigned long flags;
	unsigned int ret;
	spin_lock_irqsave(lock, flags);
	ret = bsfifo_in(fifo, buf, len);
	spin_unlock_irqrestore(lock, flags);
	return ret;
}

static inline unsigned int bsfifo_peek_n(struct epfront_bs_fifo *fifo, unsigned int recsize)
{
	unsigned int l;
	unsigned int mask = fifo->mask;
	unsigned char *data = fifo->data;

    l = ((data)[(fifo->out) & (mask)]);

	if (--recsize)
		l |= ( (data)[(fifo->out + 1) & (mask)] << 8 );

	return l;
}

static inline void bsfifo_copy_out(struct epfront_bs_fifo *fifo, void *dst,
		unsigned int len, unsigned int off)
{
	unsigned int size = fifo->mask + 1;
	unsigned int l;

	off &= fifo->mask;

	l = min(len, size - off);
	memcpy((unsigned char*)dst, (unsigned char*)fifo->data + off, l);
	memcpy((unsigned char*)dst + l, (unsigned char*)fifo->data, len - l);
	
	/*
	 * make sure that the data is copied before
	 * incrementing the fifo->out index counter
	 */
	smp_wmb();
}

static inline unsigned int bsfifo_out_copy_r(struct epfront_bs_fifo *fifo,
	void *buf, unsigned int len, unsigned int recsize, unsigned int *n)
{
	*n = bsfifo_peek_n(fifo, recsize);

	if (len > *n)
		len = *n;

	bsfifo_copy_out(fifo, buf, len, fifo->out + recsize);
	return len;
}

static inline unsigned int bsfifo_out(struct epfront_bs_fifo* fifo, void* buf, unsigned int len)
{
	unsigned int n;

	if (fifo->in == fifo->out)
		return 0;

	len = bsfifo_out_copy_r(fifo, buf, len, fifo->recsize, &n);
	fifo->out += n + fifo->recsize;
	return len;
}

static inline unsigned int bsfifo_out_spinlocked(struct epfront_bs_fifo* fifo, void* buf,unsigned int len, spinlock_t* lock)
{
	unsigned long flags;
	unsigned int ret;
	spin_lock_irqsave(lock, flags);
	ret = bsfifo_out(fifo, buf, len);
	spin_unlock_irqrestore(lock, flags);
	return ret;
}

#endif
