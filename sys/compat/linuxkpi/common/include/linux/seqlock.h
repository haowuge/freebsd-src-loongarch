/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2021 Vladimir Kondratyev <wulf@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _LINUXKPI_LINUX_SEQLOCK_H__
#define	_LINUXKPI_LINUX_SEQLOCK_H__

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/seqc.h>

struct lock_class_key;

struct seqcount {
	seqc_t		seqc;
};
typedef struct seqcount seqcount_t;

struct seqlock {
	struct mtx	seql_lock;
	struct seqcount	seql_count;
};
typedef struct seqlock seqlock_t;

static inline void
__seqcount_init(struct seqcount *seqcount, const char *name __unused,
    struct lock_class_key *key __unused)
{
	seqcount->seqc = 0;
}
#define	seqcount_init(seqcount)	__seqcount_init(seqcount, NULL, NULL)

static inline void
write_seqcount_begin(struct seqcount *seqcount)
{
	seqc_sleepable_write_begin(&seqcount->seqc);
}

static inline void
write_seqcount_end(struct seqcount *seqcount)
{
	seqc_sleepable_write_end(&seqcount->seqc);
}

/*
 * XXX: Are predicts from inline functions still not honored by clang?
 */
#define	__read_seqcount_retry(seqcount, gen)	\
	(!seqc_consistent_no_fence(&(seqcount)->seqc, gen))
#define	read_seqcount_retry(seqcount, gen)	\
	(!seqc_consistent(&(seqcount)->seqc, gen))

static inline unsigned
read_seqcount_begin(const struct seqcount *seqcount)
{
	return (seqc_read(&seqcount->seqc));
}

static inline unsigned
raw_read_seqcount(const struct seqcount *seqcount)
{
	return (seqc_read_any(&seqcount->seqc));
}

static inline void
seqlock_init(struct seqlock *seqlock)
{
	/*
	 * Don't enroll to witness(4) to avoid orphaned references after struct
	 * seqlock has been freed. There is no seqlock destructor exists so we
	 * can't expect automatic mtx_destroy() execution before free().
	 */
	mtx_init(&seqlock->seql_lock, "seqlock", NULL, MTX_DEF|MTX_NOWITNESS);
	seqcount_init(&seqlock->seql_count);
}

static inline void
write_seqlock(struct seqlock *seqlock)
{
	mtx_lock(&seqlock->seql_lock);
	write_seqcount_begin(&seqlock->seql_count);
}

static inline void
write_sequnlock(struct seqlock *seqlock)
{
	write_seqcount_end(&seqlock->seql_count);
	mtx_unlock(&seqlock->seql_lock);
}

#define	write_seqlock_irqsave(seqlock, flags)	do {	\
	(flags) = 0;					\
	write_seqlock(seqlock);				\
} while (0)

static inline void
write_sequnlock_irqrestore(struct seqlock *seqlock,
    unsigned long flags __unused)
{
	write_sequnlock(seqlock);
}

static inline unsigned
read_seqbegin(const struct seqlock *seqlock)
{
	return (read_seqcount_begin(&seqlock->seql_count));
}

#define	read_seqretry(seqlock, gen)	\
	read_seqcount_retry(&(seqlock)->seql_count, gen)

#endif	/* _LINUXKPI_LINUX_SEQLOCK_H__ */
