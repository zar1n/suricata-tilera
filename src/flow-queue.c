/* Copyright (C) 2007-2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 * Flow queue handler functions
 */

#include "suricata-common.h"
#include "threads.h"
#include "debug.h"
#include "flow-queue.h"
#include "flow-util.h"
#include "util-error.h"
#include "util-debug.h"
#include "util-print.h"
#include <string.h>

FlowQueue *FlowQueueNew() {
    FlowQueue *q = (FlowQueue *)SCMalloc(sizeof(FlowQueue));
    if (q == NULL) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in FlowQueueNew. Exiting...");
        exit(EXIT_SUCCESS);
    }
    q = FlowQueueInit(q);
    return q;
}

FlowQueue *FlowQueueInit (FlowQueue *q) {
    if (q != NULL) {
        memset(q, 0, sizeof(FlowQueue));
#ifdef __tile__
        tmc_spin_queued_mutex_init(&q->mutex_q);
        q->cond_q = 0;
#else
        SCMutexInit(&q->mutex_q, NULL);
        SCCondInit(&q->cond_q, NULL);
#endif
    }
    return q;
}

/**
 *  \brief Destroy a flow queue
 *
 *  \param q the flow queue to destroy
 */
void FlowQueueDestroy (FlowQueue *q) {
#ifdef __tile__
#else
    SCMutexDestroy(&q->mutex_q);
    SCCondDestroy(&q->cond_q);
#endif
}

void FlowEnqueue (FlowQueue *q, Flow *f) {
    /* more packets in queue */
    if (q->top != NULL) {
        f->lnext = q->top;
        q->top->lprev = f;
        q->top = f;
    /* only packet */
    } else {
        q->top = f;
        q->bot = f;
    }
    q->len++;
#ifdef DBG_PERF
    if (q->len > q->dbg_maxlen)
        q->dbg_maxlen = q->len;
#endif /* DBG_PERF */
}

Flow *FlowDequeue (FlowQueue *q) {
#ifdef __tile__
    tmc_spin_queued_mutex_lock(&q->mutex_q);
#else
    SCMutexLock(&q->mutex_q);
#endif

    Flow *f = q->bot;
    if (f == NULL) {
#ifdef __tile__
        tmc_spin_queued_mutex_unlock(&q->mutex_q);
#else
        SCMutexUnlock(&q->mutex_q);
#endif
        return NULL;
    }

    /* more packets in queue */
    if (q->bot->lprev != NULL) {
        q->bot = q->bot->lprev;
        q->bot->lnext = NULL;
    /* just the one we remove, so now empty */
    } else {
        q->top = NULL;
        q->bot = NULL;
    }

    q->len--;

    f->lnext = NULL;
    f->lprev = NULL;

#ifdef __tile__
    tmc_spin_queued_mutex_unlock(&q->mutex_q);
#else
    SCMutexUnlock(&q->mutex_q);
#endif
    return f;
}

/**
 *  \brief Transfer a flow from one queue to another
 *
 *  \param f the flow to be transfered
 *  \param srcq the source queue, where the flow will be removed. The param may
 *              be NULL.
 *  \param dstq the dest queue where the flow will be placed
 *  \param need_srclock does the srcq need locking? 1 yes, 0 no
 *
 */
void FlowRequeue(Flow *f, FlowQueue *srcq, FlowQueue *dstq, uint8_t need_srclock)
{
#ifdef DEBUG
    BUG_ON(dstq == NULL);
#endif /* DEBUG */

    if (srcq != NULL) {
        if (need_srclock == 1) {
#ifdef __tile__
            tmc_spin_queued_mutex_lock(&srcq->mutex_q);
#else
            SCMutexLock(&srcq->mutex_q);
#endif
        }
        /* remove from old queue */
        if (srcq->top == f)
            srcq->top = f->lnext;       /* remove from queue top */
        if (srcq->bot == f)
            srcq->bot = f->lprev;       /* remove from queue bot */
        if (f->lprev)
            f->lprev->lnext = f->lnext; /* remove from flow prev */
        if (f->lnext)
            f->lnext->lprev = f->lprev; /* remove from flow next */

        srcq->len--; /* adjust len */

        f->lnext = NULL;
        f->lprev = NULL;

        /* don't unlock if src and dst are the same */
        if (srcq != dstq && need_srclock == 1) {
#ifdef __tile__
            tmc_spin_queued_mutex_unlock(&srcq->mutex_q);
#else
            SCMutexUnlock(&srcq->mutex_q);
#endif
        }
    }

    /* now put it in dst */
    if (srcq != dstq) {
#ifdef __tile__
        tmc_spin_queued_mutex_lock(&dstq->mutex_q);
#else
        SCMutexLock(&dstq->mutex_q);
#endif
    }

    /* add to new queue (append) */
    f->lprev = dstq->bot;
    if (f->lprev)
        f->lprev->lnext = f;
    f->lnext = NULL;
    dstq->bot = f;
    if (dstq->top == NULL)
        dstq->top = f;

    dstq->len++;
#ifdef DBG_PERF
    if (dstq->len > dstq->dbg_maxlen)
        dstq->dbg_maxlen = dstq->len;
#endif /* DBG_PERF */

#ifdef __tile__
    tmc_spin_queued_mutex_unlock(&dstq->mutex_q);
#else
    SCMutexUnlock(&dstq->mutex_q);
#endif
}

