/*
 * Server-side IO completion ports implementation
 *
 * Copyright (C) 2007 Andrey Turkin
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
 *
 */

/* FIXME: "max concurrent active threads" parameter is not used */

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#ifdef HAVE_LINUX_FUTEX_H
# include <linux/futex.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "object.h"
#include "file.h"
#include "handle.h"
#include "request.h"
#include "wine/completion_shm.h"


static const WCHAR completion_name[] = {'I','o','C','o','m','p','l','e','t','i','o','n'};

struct type_descr completion_type =
{
    { completion_name, sizeof(completion_name) },   /* name */
    IO_COMPLETION_ALL_ACCESS,                       /* valid_access */
    {                                               /* mapping */
        STANDARD_RIGHTS_READ | IO_COMPLETION_QUERY_STATE,
        STANDARD_RIGHTS_WRITE | IO_COMPLETION_MODIFY_STATE,
        STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
        IO_COMPLETION_ALL_ACCESS
    },
};

struct comp_msg
{
    struct   list queue_entry;
    apc_param_t   ckey;
    apc_param_t   cvalue;
    apc_param_t   information;
    unsigned int  status;
    unsigned int  seq;          /* ring enqueue_pos when this message was queued */
};

struct completion_wait
{
    struct object      obj;
    obj_handle_t       handle;
    struct completion *completion;
    struct thread     *thread;
    struct comp_msg   *msg;
    struct list        wait_queue_entry;
    int                blocked;      /* thread is blocked in the server waiting for a message */
};

struct completion
{
    struct object       obj;
    struct object      *sync;
    struct list         queue;
    struct list         wait_queue;
    unsigned int        depth;
    unsigned int        blocked;     /* number of threads blocked in the server */
    int                 shm_fd;      /* memfd backing the in-process queue, or -1 */
    struct completion_shm *shm;      /* in-process queue mapping */
};

/* --- in-process queue helpers --- */

static int inproc_iocp_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1)
    {
        const char *env = getenv( "WINE_DISABLE_INPROC_IOCP" );
        enabled = !(env && atoi( env ));
    }
    return enabled;
}

/* wake one client waiter slot so it re-examines the queues / closed flag */
static void completion_poke_waiter( struct completion_waiter *w )
{
#ifdef HAVE_LINUX_FUTEX_H
    unsigned int expected = COMPLETION_WAITER_IDLE;
    if (__atomic_compare_exchange_n( &w->state, &expected, COMPLETION_WAITER_RECHECK, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
        syscall( __NR_futex, &w->state, FUTEX_WAKE, 1, NULL, NULL, 0 );
#endif
}

/* poke the most recent client waiter, if any */
static void completion_poke_top_waiter( struct completion_shm *shm )
{
    unsigned int idx = COMPLETION_STACK_IDX( __atomic_load_n( &shm->wait_head, __ATOMIC_ACQUIRE ) );
    if (idx && idx <= COMPLETION_MAX_WAITERS) completion_poke_waiter( &shm->waiters[idx - 1] );
}

/* wake every client waiter (port closed) */
static void completion_poke_all_waiters( struct completion_shm *shm )
{
    unsigned int i;
    for (i = 0; i < COMPLETION_MAX_WAITERS; i++) completion_poke_waiter( &shm->waiters[i] );
}

static void completion_signal( struct completion *completion )
{
    signal_sync( completion->sync );
    if (completion->shm) __atomic_store_n( &completion->shm->event_set, 1, __ATOMIC_RELEASE );
}

static void completion_reset( struct completion *completion )
{
    if (completion->shm && completion_ring_count( completion->shm )) return;  /* ring still holds items */
    reset_sync( completion->sync );
    if (completion->shm) __atomic_store_n( &completion->shm->event_set, 0, __ATOMIC_RELEASE );
}

/* mirror server-side state into the shared page */
static void completion_sync_shm( struct completion *completion )
{
    struct list *head;

    if (!completion->shm) return;
    if ((head = list_head( &completion->queue )))
        __atomic_store_n( &completion->shm->server_head_seq,
                          LIST_ENTRY( head, struct comp_msg, queue_entry )->seq, __ATOMIC_RELEASE );
    __atomic_store_n( &completion->shm->server_depth, completion->depth, __ATOMIC_RELEASE );
    __atomic_store_n( &completion->shm->server_waiters, completion->blocked, __ATOMIC_RELEASE );
}

/* should the next item come from the ring rather than the server queue? */
static int completion_ring_first( struct completion *completion )
{
    if (!completion->shm || !completion_ring_count( completion->shm )) return 0;
    if (list_empty( &completion->queue )) return 1;
    return !completion_server_first( completion->shm );
}

static void completion_set_blocked( struct completion_wait *wait, int blocked )
{
    if (wait->blocked == blocked || !wait->completion) return;
    wait->blocked = blocked;
    if (blocked) wait->completion->blocked++;
    else wait->completion->blocked--;
    completion_sync_shm( wait->completion );
}

static void completion_create_shm( struct completion *completion )
{
#if defined(HAVE_MEMFD_CREATE) && defined(MFD_CLOEXEC)
    void *ptr;
    int fd;

    if (!inproc_iocp_enabled()) return;
    if ((fd = memfd_create( "wine-iocp", MFD_CLOEXEC )) == -1) return;
    if (ftruncate( fd, COMPLETION_SHM_SIZE ) == -1) { close( fd ); return; }
    ptr = mmap( NULL, COMPLETION_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if (ptr == MAP_FAILED) { close( fd ); return; }
    completion_shm_init( ptr );
    completion->shm_fd = fd;
    completion->shm = ptr;
#else
    (void)completion;  /* no memfd_create: ports fall back to pure server behavior */
#endif
}

/* pop a ring entry into a freshly allocated server message */
static struct comp_msg *completion_pop_ring( struct completion *completion )
{
    struct comp_msg *msg;

    if (!completion->shm) return NULL;
    if (!(msg = mem_alloc( sizeof(*msg) ))) return NULL;
    if (!completion_ring_pop( completion->shm, &msg->ckey, &msg->cvalue, &msg->status, &msg->information ))
    {
        free( msg );
        return NULL;
    }
    return msg;
}

static void completion_wait_dump( struct object*, int );
static int completion_wait_signaled( struct object *obj, struct wait_queue_entry *entry );
static void completion_wait_satisfied( struct object *obj, struct wait_queue_entry *entry );
static void completion_wait_destroy( struct object * );

static const struct object_ops completion_wait_ops =
{
    sizeof(struct completion_wait), /* size */
    &no_type,                       /* type */
    completion_wait_dump,           /* dump */
    add_queue,                      /* add_queue */
    remove_queue,                   /* remove_queue */
    completion_wait_signaled,       /* signaled */
    completion_wait_satisfied,      /* satisfied */
    no_signal,                      /* signal */
    no_get_fd,                      /* get_fd */
    default_get_sync,               /* get_sync */
    default_map_access,             /* map_access */
    default_get_sd,                 /* get_sd */
    default_set_sd,                 /* set_sd */
    no_get_full_name,               /* get_full_name */
    no_lookup_name,                 /* lookup_name */
    no_link_name,                   /* link_name */
    NULL,                           /* unlink_name */
    no_open_file,                   /* open_file */
    no_kernel_obj_list,             /* get_kernel_obj_list */
    no_close_handle,                /* close_handle */
    completion_wait_destroy         /* destroy */
};

static void completion_wait_destroy( struct object *obj )
{
    struct completion_wait *wait = (struct completion_wait *)obj;

    free( wait->msg );
}

static void completion_wait_dump( struct object *obj, int verbose )
{
    struct completion_wait *wait = (struct completion_wait *)obj;

    assert( obj->ops == &completion_wait_ops );
    fprintf( stderr, "Completion wait completion=%p\n", wait->completion );
}

static int completion_wait_signaled( struct object *obj, struct wait_queue_entry *entry )
{
    struct completion_wait *wait = (struct completion_wait *)obj;

    assert( obj->ops == &completion_wait_ops );
    if (!wait->completion) return 1;
    return wait->completion->depth;
}

static void completion_wait_satisfied( struct object *obj, struct wait_queue_entry *entry )
{
    struct completion_wait *wait = (struct completion_wait *)obj;
    struct list *msg_entry;
    struct comp_msg *msg;

    assert( obj->ops == &completion_wait_ops );
    if (!wait->completion)
    {
        make_wait_abandoned( entry );
        return;
    }
    msg_entry = list_head( &wait->completion->queue );
    assert( msg_entry );
    msg = LIST_ENTRY( msg_entry, struct comp_msg, queue_entry );
    --wait->completion->depth;
    list_remove( &msg->queue_entry );
    if (wait->msg) free( wait->msg );
    wait->msg = msg;
    completion_set_blocked( wait, 0 );
    completion_sync_shm( wait->completion );
}

static void completion_dump( struct object*, int );
static struct object *completion_get_sync( struct object * );
static int completion_close_handle( struct object *obj, struct process *process, obj_handle_t handle );
static void completion_destroy( struct object * );

static const struct object_ops completion_ops =
{
    sizeof(struct completion), /* size */
    &completion_type,          /* type */
    completion_dump,           /* dump */
    NULL,                      /* add_queue */
    NULL,                      /* remove_queue */
    NULL,                      /* signaled */
    NULL,                      /* satisfied */
    no_signal,                 /* signal */
    no_get_fd,                 /* get_fd */
    completion_get_sync,       /* get_sync */
    default_map_access,        /* map_access */
    default_get_sd,            /* get_sd */
    default_set_sd,            /* set_sd */
    default_get_full_name,     /* get_full_name */
    no_lookup_name,            /* lookup_name */
    directory_link_name,       /* link_name */
    default_unlink_name,       /* unlink_name */
    no_open_file,              /* open_file */
    no_kernel_obj_list,        /* get_kernel_obj_list */
    completion_close_handle,   /* close_handle */
    completion_destroy         /* destroy */
};

static void completion_destroy( struct object *obj)
{
    struct completion *completion = (struct completion *) obj;
    struct comp_msg *tmp, *next;

    LIST_FOR_EACH_ENTRY_SAFE( tmp, next, &completion->queue, struct comp_msg, queue_entry )
    {
        free( tmp );
    }

    if (completion->sync) release_object( completion->sync );
    if (completion->shm) munmap( completion->shm, COMPLETION_SHM_SIZE );
    if (completion->shm_fd != -1) close( completion->shm_fd );
}

static void completion_dump( struct object *obj, int verbose )
{
    struct completion *completion = (struct completion *) obj;

    assert( obj->ops == &completion_ops );
    fprintf( stderr, "Completion depth=%u\n", completion->depth );
}

static struct object *completion_get_sync( struct object *obj )
{
    struct completion *completion = (struct completion *)obj;
    assert( obj->ops == &completion_ops );
    return grab_object( completion->sync );
}

static int completion_close_handle( struct object *obj, struct process *process, obj_handle_t handle )
{
    struct completion *completion = (struct completion *)obj;
    struct completion_wait *wait, *wait_next;

    if (completion->obj.handle_count != 1) return 1;

    LIST_FOR_EACH_ENTRY_SAFE( wait, wait_next, &completion->wait_queue, struct completion_wait, wait_queue_entry )
    {
        assert( wait->completion );
        completion_set_blocked( wait, 0 );
        wait->completion = NULL;
        list_remove( &wait->wait_queue_entry );
        if (!wait->msg)
        {
            wake_up( &wait->obj, 0 );
            cleanup_thread_completion( wait->thread );
        }
    }
    completion_signal( completion );
    if (completion->shm)
    {
        __atomic_store_n( &completion->shm->closed, 1, __ATOMIC_SEQ_CST );
        completion_poke_all_waiters( completion->shm );
    }
    return 1;
}

void cleanup_thread_completion( struct thread *thread )
{
    if (!thread->completion_wait) return;

    if (thread->completion_wait->handle)
    {
        close_handle( thread->process, thread->completion_wait->handle );
        thread->completion_wait->handle = 0;
    }
    if (thread->completion_wait->completion)
    {
        completion_set_blocked( thread->completion_wait, 0 );
        list_remove( &thread->completion_wait->wait_queue_entry );
    }
    release_object( &thread->completion_wait->obj );
    thread->completion_wait = NULL;
}

static struct completion_wait *create_completion_wait( struct thread *thread )
{
    struct completion_wait *wait;

    if (!(wait = alloc_object( &completion_wait_ops ))) return NULL;
    wait->completion = NULL;
    wait->thread = thread;
    wait->msg = NULL;
    wait->blocked = 0;
    if (!(wait->handle = alloc_handle( current->process, wait, SYNCHRONIZE, 0 )))
    {
        release_object( &wait->obj );
        return NULL;
    }
    return wait;
}

static struct completion *create_completion( struct object *root, const struct unicode_str *name,
                                             unsigned int attr, unsigned int concurrent,
                                             const struct security_descriptor *sd )
{
    struct completion *completion;

    if ((completion = create_named_object( root, &completion_ops, name, attr, sd )))
    {
        if (get_error() != STATUS_OBJECT_NAME_EXISTS)
        {
            completion->sync = NULL;
            list_init( &completion->queue );
            list_init( &completion->wait_queue );
            completion->depth = 0;
            completion->blocked = 0;
            completion->shm_fd = -1;
            completion->shm = NULL;

            if (!(completion->sync = create_internal_sync( 1, 0 )))
            {
                release_object( completion );
                return NULL;
            }
            completion_create_shm( completion );
        }
    }

    return completion;
}

struct completion *get_completion_obj( struct process *process, obj_handle_t handle, unsigned int access )
{
    return (struct completion *) get_handle_obj( process, handle, access, &completion_ops );
}

void add_completion( struct completion *completion, apc_param_t ckey, apc_param_t cvalue,
                     unsigned int status, apc_param_t information )
{
    struct comp_msg *msg = mem_alloc( sizeof( *msg ) );
    struct completion_wait *wait;

    if (!msg)
        return;

    msg->ckey = ckey;
    msg->cvalue = cvalue;
    msg->status = status;
    msg->information = information;
    msg->seq = completion->shm ? __atomic_load_n( &completion->shm->enqueue_pos, __ATOMIC_ACQUIRE ) : 0;

    list_add_tail( &completion->queue, &msg->queue_entry );
    completion->depth++;
    completion_sync_shm( completion );
    LIST_FOR_EACH_ENTRY( wait, &completion->wait_queue, struct completion_wait, wait_queue_entry )
    {
        wake_up( &wait->obj, 1 );
        if (list_empty( &completion->queue )) return;
    }
    if (!list_empty( &completion->queue ))
    {
        if (completion->shm && COMPLETION_STACK_IDX( __atomic_load_n( &completion->shm->wait_head, __ATOMIC_ACQUIRE ) ))
        {
            /* a client thread is blocked in NtRemoveIoCompletion: hand over
             * without signaling the port object, like a direct handoff */
            __atomic_thread_fence( __ATOMIC_SEQ_CST );
            completion_poke_top_waiter( completion->shm );
        }
        else completion_signal( completion );
    }
}

/* create a completion */
DECL_HANDLER(create_completion)
{
    struct completion *completion;
    struct unicode_str name;
    struct object *root;
    const struct security_descriptor *sd;
    const struct object_attributes *objattr = get_req_object_attributes( &sd, &name, &root );

    if (!objattr) return;

    if ((completion = create_completion( root, &name, objattr->attributes, req->concurrent, sd )))
    {
        if (get_error() == STATUS_OBJECT_NAME_EXISTS)
            reply->handle = alloc_handle( current->process, completion, req->access, objattr->attributes );
        else
            reply->handle = alloc_handle_no_access_check( current->process, completion,
                                                          req->access, objattr->attributes );
        release_object( completion );
    }

    if (root) release_object( root );
}

/* open a completion */
DECL_HANDLER(open_completion)
{
    struct unicode_str name = get_req_unicode_str();

    reply->handle = open_object( current->process, req->rootdir, req->access,
                                 &completion_ops, &name, req->attributes );
}


/* add completion to completion port */
DECL_HANDLER(add_completion)
{
    struct completion* completion = get_completion_obj( current->process, req->handle, IO_COMPLETION_MODIFY_STATE );
    struct reserve *reserve = NULL;

    if (!completion) return;

    if (req->reserve_handle && !(reserve = get_completion_reserve_obj( current->process, req->reserve_handle, 0 )))
    {
        release_object( completion );
        return;
    }

    add_completion( completion, req->ckey, req->cvalue, req->status, req->information );

    if (reserve) release_object( reserve );
    release_object( completion );
}

/* get completion from completion port */
DECL_HANDLER(remove_completion)
{
    struct completion* completion = get_completion_obj( current->process, req->handle, IO_COMPLETION_MODIFY_STATE );
    struct list *entry;
    struct comp_msg *msg;

    if (!completion) return;

    entry = list_head( &completion->queue );
    if (req->alertable && !list_empty( &current->user_apc )
        && !((entry || (completion->shm && completion_ring_count( completion->shm )))
             && (req->associated ||
                 (current->completion_wait && current->completion_wait->completion == completion))))
    {
        set_error( STATUS_USER_APC );
        release_object( completion );
        return;
    }

    /* serve from the in-process ring when it holds the oldest item */
    if (completion_ring_first( completion ) && (msg = completion_pop_ring( completion )))
    {
        reply->ckey = msg->ckey;
        reply->cvalue = msg->cvalue;
        reply->status = msg->status;
        reply->information = msg->information;
        reply->wait_handle = 0;
        free( msg );
        release_object( completion );
        return;
    }

    if (!entry && req->no_wait)
    {
        reply->wait_handle = 0;
        set_error( STATUS_PENDING );
        release_object( completion );
        return;
    }

    if (current->completion_wait)
    {
        completion_set_blocked( current->completion_wait, 0 );
        list_remove( &current->completion_wait->wait_queue_entry );
    }
    else if (!(current->completion_wait = create_completion_wait( current )))
    {
        release_object( completion );
        return;
    }
    current->completion_wait->completion = completion;
    list_add_head( &completion->wait_queue, &current->completion_wait->wait_queue_entry );
    if (!entry)
    {
        /* announce that we're blocking, then re-check the ring (paired with
         * the producer's push -> fence -> server_waiters check) */
        completion_set_blocked( current->completion_wait, 1 );
        __atomic_thread_fence( __ATOMIC_SEQ_CST );
        if ((msg = completion_pop_ring( completion )))
        {
            completion_set_blocked( current->completion_wait, 0 );
            reply->ckey = msg->ckey;
            reply->cvalue = msg->cvalue;
            reply->status = msg->status;
            reply->information = msg->information;
            reply->wait_handle = 0;
            free( msg );
            release_object( completion );
            return;
        }
        reply->wait_handle = current->completion_wait->handle;
        set_error( STATUS_PENDING );
    }
    else
    {
        list_remove( entry );
        completion->depth--;
        completion_sync_shm( completion );
        msg = LIST_ENTRY( entry, struct comp_msg, queue_entry );
        reply->ckey = msg->ckey;
        reply->cvalue = msg->cvalue;
        reply->status = msg->status;
        reply->information = msg->information;
        free( msg );
        reply->wait_handle = 0;
        if (list_empty( &completion->queue )) completion_reset( completion );
    }

    release_object( completion );
}

/* get completion after successful waiting for it */
DECL_HANDLER(get_thread_completion)
{
    struct comp_msg *msg;

    /* the client gave up waiting (timeout/APC); it is no longer blocked */
    if (current->completion_wait) completion_set_blocked( current->completion_wait, 0 );

    if (!current->completion_wait || !(msg = current->completion_wait->msg))
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }

    reply->ckey = msg->ckey;
    reply->cvalue = msg->cvalue;
    reply->status = msg->status;
    reply->information = msg->information;
    free( msg );
    current->completion_wait->msg = NULL;
    if (!current->completion_wait->completion) cleanup_thread_completion( current );
}

/* get queue depth for completion port */
DECL_HANDLER(query_completion)
{
    struct completion* completion = get_completion_obj( current->process, req->handle, IO_COMPLETION_QUERY_STATE );

    if (!completion) return;

    reply->depth = completion->depth;
    if (completion->shm) reply->depth += completion_ring_count( completion->shm );

    release_object( completion );
}

/* get the in-process queue mapping of a completion port */
DECL_HANDLER(get_completion_shm)
{
    struct completion* completion = get_completion_obj( current->process, req->handle, 0 );

    if (!completion) return;

    if (!completion->shm) set_error( STATUS_NOT_IMPLEMENTED );
    else
    {
        reply->size = COMPLETION_SHM_SIZE;
        reply->access = get_handle_access( current->process, req->handle );
        send_client_fd( current->process, completion->shm_fd, req->handle );
    }
    release_object( completion );
}

/* move in-process queue entries to threads blocked in the server */
DECL_HANDLER(kick_completion)
{
    struct completion* completion = get_completion_obj( current->process, req->handle, IO_COMPLETION_MODIFY_STATE );
    struct comp_msg *msg;

    if (!completion) return;

    while (completion->blocked && (msg = completion_pop_ring( completion )))
    {
        add_completion( completion, msg->ckey, msg->cvalue, msg->status, msg->information );
        free( msg );
    }
    release_object( completion );
}
