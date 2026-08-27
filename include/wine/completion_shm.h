/*
 * In-process I/O completion port queue, shared between wineserver and clients
 *
 * Copyright 2026 Haz / Claude
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_WINE_COMPLETION_SHM_H
#define __WINE_WINE_COMPLETION_SHM_H

/*
 * Layout of the memfd shared by every process that holds a handle to a
 * completion port (and by wineserver itself).
 *
 * This mirrors the NT KQUEUE algorithm:
 *
 *  - Threads blocked in NtRemoveIoCompletion(Ex) park in a waiter slot and
 *    push themselves on a LIFO stack.  A client NtSetIoCompletion hands the
 *    item straight into the top waiter's slot and wakes only that thread;
 *    the port object is *not* signaled in that case.
 *  - With no waiter, the item goes to a bounded lock-free ring and the port's
 *    event is set; consumers pop the ring and reset the event once both the
 *    ring and the server queue are empty.
 *  - Completions that originate inside wineserver (async I/O on files bound
 *    to the port, job notifications, non-inproc clients) still use the
 *    server's own queue.  The server mirrors that queue's depth, the ring
 *    position at which its oldest item arrived (for FIFO merging) and the
 *    number of threads blocked server-side into this page, and pokes the top
 *    client waiter (state RECHECK) when it queues something.
 *  - A client producer that sees server-side waiters after its push issues a
 *    "kick_completion" request so the server can hand the item over; the
 *    symmetric "increment, fence, re-check" on both sides makes this
 *    race-free.
 *
 * Client-side state (waiter stack, slot allocation, event hint) is guarded by
 * a small cross-process futex lock, held for nanoseconds with signals blocked.
 * wineserver never takes that lock: it only reads the stack head and writes
 * its own counters, so a stopped client can never wedge the server.
 */

#define COMPLETION_SHM_MAGIC   0x50434f49  /* 'IOCP' */
#define COMPLETION_SHM_VERSION 3
#define COMPLETION_RING_SIZE   16384       /* entries; power of two */
#define COMPLETION_MAX_WAITERS 256

enum completion_waiter_state
{
    COMPLETION_WAITER_FREE      = 0,   /* slot is on the freelist */
    COMPLETION_WAITER_IDLE      = 1,   /* parked, nothing happened yet */
    COMPLETION_WAITER_CLAIMED   = 2,   /* a producer won the slot and is writing the payload */
    COMPLETION_WAITER_DELIVERED = 3,   /* payload is in the slot */
    COMPLETION_WAITER_RECHECK   = 4,   /* woken to look at the server queue / closed flag */
    COMPLETION_WAITER_CANCELLED = 5,   /* waiter left (timeout/recheck); next producer pop discards */
};

/* Both the waiter stack and the freelist are Treiber stacks headed by a
 * 64-bit word packing {slot index + 1 (16 bits), ABA tag (48 bits)}.
 * index 0 means empty. */
#define COMPLETION_STACK_IDX(v)      ((unsigned int)((v) & 0xffff))
#define COMPLETION_STACK_MAKE(i, v)  (((v) & ~(unsigned long long)0xffff) + 0x10000ULL + (i))

struct completion_cell
{
    unsigned int  seq;          /* Vyukov sequence number */
    unsigned int  status;       /* completion result */
    apc_param_t   ckey;         /* completion key */
    apc_param_t   cvalue;       /* completion value */
    apc_param_t   information;  /* IO_STATUS_BLOCK.Information */
};

struct completion_waiter
{
    unsigned int  state;        /* enum completion_waiter_state; futex word */
    unsigned int  next;         /* next slot index + 1, 0 = end of stack */
    unsigned int  status;       /* delivered completion result */
    unsigned int  pad0;
    apc_param_t   ckey;
    apc_param_t   cvalue;
    apc_param_t   information;
};

struct completion_shm
{
    unsigned int  magic;
    unsigned int  version;
    unsigned int  size_mask;          /* ring size - 1 */
    unsigned int  closed;             /* set by server when last handle closed */

    /* written only by the server */
    unsigned int  server_depth;       /* items queued inside wineserver */
    unsigned int  server_waiters;     /* threads blocked in wineserver remove_completion */
    unsigned int  server_head_seq;    /* ring enqueue_pos when the oldest server item arrived */
    unsigned int  pad0[9];

    /* ring producer side */
    unsigned int  enqueue_pos;
    unsigned int  pad1[15];
    /* ring consumer side */
    unsigned int  dequeue_pos;
    unsigned int  pad2[15];

    /* lock-free client state */
    unsigned long long wait_head;     /* Treiber stack of parked waiters {idx+1, tag} */
    unsigned long long free_head;     /* Treiber freelist of waiter slots {idx+1, tag} */
    unsigned int  event_set;          /* hint: port event believed to be set */
    unsigned int  pad3[11];

    struct completion_waiter waiters[COMPLETION_MAX_WAITERS];
    struct completion_cell   cells[COMPLETION_RING_SIZE];
};

#define COMPLETION_SHM_SIZE  sizeof(struct completion_shm)

static inline void completion_shm_init( struct completion_shm *shm )
{
    unsigned int i;
    memset( shm, 0, sizeof(*shm) );
    shm->magic     = COMPLETION_SHM_MAGIC;
    shm->version   = COMPLETION_SHM_VERSION;
    shm->size_mask = COMPLETION_RING_SIZE - 1;
    for (i = 0; i < COMPLETION_RING_SIZE; i++) shm->cells[i].seq = i;
    /* chain every waiter slot onto the freelist */
    for (i = 0; i < COMPLETION_MAX_WAITERS - 1; i++) shm->waiters[i].next = i + 2;
    shm->free_head = 1;  /* index 1 (slot 0), tag 0 */
}

/* pop a slot index (0-based, or -1) off a Treiber stack head */
static inline int completion_stack_pop( unsigned long long *head, struct completion_waiter *slots )
{
    unsigned long long h = __atomic_load_n( head, __ATOMIC_ACQUIRE );
    for (;;)
    {
        unsigned int idx = COMPLETION_STACK_IDX( h );
        unsigned long long next;
        if (!idx) return -1;
        next = COMPLETION_STACK_MAKE( slots[idx - 1].next, h );
        if (__atomic_compare_exchange_n( head, &h, next, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
            return idx - 1;
    }
}

/* push slot index (0-based) onto a Treiber stack head */
static inline void completion_stack_push( unsigned long long *head, struct completion_waiter *slots,
                                          unsigned int idx )
{
    unsigned long long h = __atomic_load_n( head, __ATOMIC_RELAXED );
    for (;;)
    {
        slots[idx].next = COMPLETION_STACK_IDX( h );
        if (__atomic_compare_exchange_n( head, &h, COMPLETION_STACK_MAKE( idx + 1, h ), 1,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED ))
            return;
    }
}

/* pop a specific slot only if it is the current top ("leave without a trace") */
static inline int completion_stack_pop_if_top( unsigned long long *head, struct completion_waiter *slots,
                                               unsigned int idx )
{
    unsigned long long h = __atomic_load_n( head, __ATOMIC_ACQUIRE );
    if (COMPLETION_STACK_IDX( h ) != idx + 1) return 0;
    return __atomic_compare_exchange_n( head, &h, COMPLETION_STACK_MAKE( slots[idx].next, h ), 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED );
}

static inline int completion_shm_valid( const struct completion_shm *shm, size_t size )
{
    return size >= sizeof(*shm) && shm->magic == COMPLETION_SHM_MAGIC &&
           shm->version == COMPLETION_SHM_VERSION && shm->size_mask == COMPLETION_RING_SIZE - 1;
}

/* returns 0 if the ring is full */
static inline int completion_ring_push( struct completion_shm *shm, apc_param_t ckey, apc_param_t cvalue,
                                        unsigned int status, apc_param_t information )
{
    struct completion_cell *cell;
    unsigned int pos = __atomic_load_n( &shm->enqueue_pos, __ATOMIC_RELAXED );

    for (;;)
    {
        unsigned int seq;
        int dif;

        cell = &shm->cells[pos & shm->size_mask];
        seq  = __atomic_load_n( &cell->seq, __ATOMIC_ACQUIRE );
        dif  = (int)(seq - pos);
        if (!dif)
        {
            if (__atomic_compare_exchange_n( &shm->enqueue_pos, &pos, pos + 1, 1,
                                             __ATOMIC_RELAXED, __ATOMIC_RELAXED )) break;
        }
        else if (dif < 0) return 0;
        else pos = __atomic_load_n( &shm->enqueue_pos, __ATOMIC_RELAXED );
    }
    cell->ckey        = ckey;
    cell->cvalue      = cvalue;
    cell->status      = status;
    cell->information = information;
    __atomic_store_n( &cell->seq, pos + 1, __ATOMIC_RELEASE );
    return 1;
}

/* returns 0 if the ring is empty */
static inline int completion_ring_pop( struct completion_shm *shm, apc_param_t *ckey, apc_param_t *cvalue,
                                       unsigned int *status, apc_param_t *information )
{
    struct completion_cell *cell;
    unsigned int pos = __atomic_load_n( &shm->dequeue_pos, __ATOMIC_RELAXED );

    for (;;)
    {
        unsigned int seq;
        int dif;

        cell = &shm->cells[pos & shm->size_mask];
        seq  = __atomic_load_n( &cell->seq, __ATOMIC_ACQUIRE );
        dif  = (int)(seq - (pos + 1));
        if (!dif)
        {
            if (__atomic_compare_exchange_n( &shm->dequeue_pos, &pos, pos + 1, 1,
                                             __ATOMIC_RELAXED, __ATOMIC_RELAXED )) break;
        }
        else if (dif < 0) return 0;
        else pos = __atomic_load_n( &shm->dequeue_pos, __ATOMIC_RELAXED );
    }
    *ckey        = cell->ckey;
    *cvalue      = cell->cvalue;
    *status      = cell->status;
    *information = cell->information;
    __atomic_store_n( &cell->seq, pos + shm->size_mask + 1, __ATOMIC_RELEASE );
    return 1;
}

/* true if the oldest server-queued item is older than the oldest ring item */
static inline int completion_server_first( const struct completion_shm *shm )
{
    unsigned int head = __atomic_load_n( &shm->server_head_seq, __ATOMIC_ACQUIRE );
    unsigned int deq  = __atomic_load_n( &shm->dequeue_pos, __ATOMIC_RELAXED );
    return (int)(head - deq) <= 0;
}

/* approximate number of items in the ring */
static inline unsigned int completion_ring_count( const struct completion_shm *shm )
{
    unsigned int e = __atomic_load_n( &shm->enqueue_pos, __ATOMIC_RELAXED );
    unsigned int d = __atomic_load_n( &shm->dequeue_pos, __ATOMIC_RELAXED );
    return (int)(e - d) > 0 ? e - d : 0;
}

#endif /* __WINE_WINE_COMPLETION_SHM_H */
