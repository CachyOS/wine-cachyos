/*
 * In-process synchronization primitives
 *
 * Copyright (C) 2021-2022 Elizabeth Figura for CodeWeavers
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

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"

#include "file.h"
#include "handle.h"
#include "request.h"
#include "thread.h"

#ifdef HAVE_LINUX_NTSYNC_H

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/ntsync.h>

struct linux_device
{
    struct object obj;      /* object header */
    struct fd *fd;          /* fd for unix fd */
};

static struct linux_device *linux_device_object;

static void linux_device_dump( struct object *obj, int verbose );
static struct fd *linux_device_get_fd( struct object *obj );
static void linux_device_destroy( struct object *obj );
static enum server_fd_type inproc_sync_get_fd_type( struct fd *fd );

static const struct object_ops linux_device_ops =
{
    sizeof(struct linux_device),        /* size */
    &no_type,                           /* type */
    linux_device_dump,                  /* dump */
    no_add_queue,                       /* add_queue */
    NULL,                               /* remove_queue */
    NULL,                               /* signaled */
    NULL,                               /* satisfied */
    no_signal,                          /* signal */
    linux_device_get_fd,                /* get_fd */
    default_map_access,                 /* map_access */
    default_get_sd,                     /* get_sd */
    default_set_sd,                     /* set_sd */
    no_get_full_name,                   /* get_full_name */
    no_lookup_name,                     /* lookup_name */
    no_link_name,                       /* link_name */
    NULL,                               /* unlink_name */
    no_open_file,                       /* open_file */
    no_kernel_obj_list,                 /* get_kernel_obj_list */
    no_get_inproc_sync,                 /* get_inproc_sync */
    no_close_handle,                    /* close_handle */
    linux_device_destroy                /* destroy */
};

static const struct fd_ops inproc_sync_fd_ops =
{
    default_fd_get_poll_events,     /* get_poll_events */
    default_poll_event,             /* poll_event */
    inproc_sync_get_fd_type,        /* get_fd_type */
    no_fd_read,                     /* read */
    no_fd_write,                    /* write */
    no_fd_flush,                    /* flush */
    no_fd_get_file_info,            /* get_file_info */
    no_fd_get_volume_info,          /* get_volume_info */
    no_fd_ioctl,                    /* ioctl */
    default_fd_cancel_async,        /* cancel_async */
    no_fd_queue_async,              /* queue_async */
    default_fd_reselect_async       /* reselect_async */
};

static void linux_device_dump( struct object *obj, int verbose )
{
    struct linux_device *device = (struct linux_device *)obj;
    assert( obj->ops == &linux_device_ops );
    fprintf( stderr, "In-process synchronization device fd=%p\n", device->fd );
}

static struct fd *linux_device_get_fd( struct object *obj )
{
    struct linux_device *device = (struct linux_device *)obj;
    return (struct fd *)grab_object( device->fd );
}

static void linux_device_destroy( struct object *obj )
{
    struct linux_device *device = (struct linux_device *)obj;
    assert( obj->ops == &linux_device_ops );
    if (device->fd) release_object( device->fd );
    linux_device_object = NULL;
}

static enum server_fd_type inproc_sync_get_fd_type( struct fd *fd )
{
    return FD_TYPE_FILE;
}

static struct linux_device *get_linux_device(void)
{
    struct linux_device *device;
    static int initialized;
    int unix_fd;

    if (initialized)
    {
        if (linux_device_object)
            grab_object( linux_device_object );
	else
	  set_error( STATUS_NOT_IMPLEMENTED );
        return linux_device_object;
    }

    if (getenv( "WINE_DISABLE_FAST_SYNC" ) && atoi( getenv( "WINE_DISABLE_FAST_SYNC" ) ))
    {
      static int once;
        set_error( STATUS_NOT_IMPLEMENTED );
	if (!once++) fprintf(stderr, "ntsync is explicitly disabled.\n");
	initialized = 1;
        return NULL;
    }

    unix_fd = open( "/dev/ntsync", O_CLOEXEC | O_RDONLY );
    if (unix_fd == -1)
    {
      static int once;
        file_set_error();
	if (!once++) fprintf(stderr, "Cannot open /dev/ntsync: %s\n", strerror(errno));
	initialized = 1;
        return NULL;
    }

    if (!(device = alloc_object( &linux_device_ops )))
    {
        close( unix_fd );
        set_error( STATUS_NO_MEMORY );
	initialized = 1;
        return NULL;
    }

    if (!(device->fd = create_anonymous_fd( &inproc_sync_fd_ops, unix_fd, &device->obj, 0 )))
    {
        release_object( device );
	initialized = 1;
        return NULL;
    }

    fprintf( stderr, "wine: using fast synchronization.\n" );
    linux_device_object = device;
    initialized = 1;
    return device;
}

struct inproc_sync
{
    struct object obj;
    enum inproc_sync_type type;
    struct fd *fd;
};

static void linux_obj_dump( struct object *obj, int verbose );
static void linux_obj_destroy( struct object *obj );
static struct fd *linux_obj_get_fd( struct object *obj );

static const struct object_ops linux_obj_ops =
{
    sizeof(struct inproc_sync), /* size */
    &no_type,                   /* type */
    linux_obj_dump,             /* dump */
    no_add_queue,               /* add_queue */
    NULL,                       /* remove_queue */
    NULL,                       /* signaled */
    NULL,                       /* satisfied */
    no_signal,                  /* signal */
    linux_obj_get_fd,           /* get_fd */
    default_map_access,         /* map_access */
    default_get_sd,             /* get_sd */
    default_set_sd,             /* set_sd */
    no_get_full_name,           /* get_full_name */
    no_lookup_name,             /* lookup_name */
    no_link_name,               /* link_name */
    NULL,                       /* unlink_name */
    no_open_file,               /* open_file */
    no_kernel_obj_list,         /* get_kernel_obj_list */
    no_get_inproc_sync,         /* get_inproc_sync */
    no_close_handle,            /* close_handle */
    linux_obj_destroy           /* destroy */
};

static void linux_obj_dump( struct object *obj, int verbose )
{
    struct inproc_sync *inproc_sync = (struct inproc_sync *)obj;
    assert( obj->ops == &linux_obj_ops );
    fprintf( stderr, "In-process synchronization object type=%u fd=%p\n", inproc_sync->type, inproc_sync->fd );
}

static void linux_obj_destroy( struct object *obj )
{
    struct inproc_sync *inproc_sync = (struct inproc_sync *)obj;
    assert( obj->ops == &linux_obj_ops );
    if (inproc_sync->fd) release_object( inproc_sync->fd );
}

static struct fd *linux_obj_get_fd( struct object *obj )
{
    struct inproc_sync *inproc_sync = (struct inproc_sync *)obj;
    assert( obj->ops == &linux_obj_ops );
    return (struct fd *)grab_object( inproc_sync->fd );
}

static struct inproc_sync *create_inproc_sync( enum inproc_sync_type type, int unix_fd )
{
    struct inproc_sync *inproc_sync;

    if (!(inproc_sync = alloc_object( &linux_obj_ops )))
    {
        close( unix_fd );
        return NULL;
    }

    inproc_sync->type = type;

    if (!(inproc_sync->fd = create_anonymous_fd( &inproc_sync_fd_ops, unix_fd, &inproc_sync->obj, 0 )))
    {
        release_object( inproc_sync );
        return NULL;
    }

    return inproc_sync;
}

struct inproc_sync *create_inproc_event( enum inproc_sync_type type, int signaled )
{
    struct ntsync_event_args args;
    struct linux_device *device;
    int event;

    if (!(device = get_linux_device())) return NULL;

    args.signaled = signaled;
    switch (type)
    {
        case INPROC_SYNC_AUTO_EVENT:
        case INPROC_SYNC_AUTO_SERVER:
            args.manual = 0;
            break;

        case INPROC_SYNC_MANUAL_EVENT:
        case INPROC_SYNC_MANUAL_SERVER:
        case INPROC_SYNC_QUEUE:
            args.manual = 1;
            break;

        case INPROC_SYNC_MUTEX:
        case INPROC_SYNC_SEMAPHORE:
            assert(0);
            break;
    }
    if ((event = ioctl( get_unix_fd( device->fd ), NTSYNC_IOC_CREATE_EVENT, &args )) < 0)
    {
        file_set_error();
        release_object( device );
        return NULL;
    }
    release_object( device );

    return create_inproc_sync( type, event );
}

struct inproc_sync *create_inproc_semaphore( unsigned int count, unsigned int max )
{
    struct ntsync_sem_args args;
    struct linux_device *device;
    int semaphore;

    if (!(device = get_linux_device())) return NULL;

    args.count = count;
    args.max = max;
    if ((semaphore = ioctl( get_unix_fd( device->fd ), NTSYNC_IOC_CREATE_SEM, &args )) < 0)
    {
        file_set_error();
        release_object( device );
        return NULL;
    }

    release_object( device );

    return create_inproc_sync( INPROC_SYNC_SEMAPHORE, semaphore );
}

struct inproc_sync *create_inproc_mutex( thread_id_t owner, unsigned int count )
{
    struct ntsync_mutex_args args;
    struct linux_device *device;
    int mutex;

    if (!(device = get_linux_device())) return NULL;

    args.owner = owner;
    args.count = count;
    if ((mutex = ioctl( get_unix_fd( device->fd ), NTSYNC_IOC_CREATE_MUTEX, &args )) < 0)
    {
        file_set_error();
        release_object( device );
        return NULL;
    }

    release_object( device );

    return create_inproc_sync( INPROC_SYNC_MUTEX, mutex );
}

void set_inproc_event( struct inproc_sync *inproc_sync )
{
    __u32 count;

    if (!inproc_sync) return;

    if (debug_level) fprintf( stderr, "set_inproc_event %p\n", inproc_sync->fd );

    ioctl( get_unix_fd( inproc_sync->fd ), NTSYNC_IOC_EVENT_SET, &count );
}

void reset_inproc_event( struct inproc_sync *inproc_sync )
{
    __u32 count;

    if (!inproc_sync) return;

    if (debug_level) fprintf( stderr, "set_inproc_event %p\n", inproc_sync->fd );

    ioctl( get_unix_fd( inproc_sync->fd ), NTSYNC_IOC_EVENT_RESET, &count );
}

void abandon_inproc_mutex( thread_id_t tid, struct inproc_sync *inproc_sync )
{
    ioctl( get_unix_fd( inproc_sync->fd ), NTSYNC_IOC_MUTEX_KILL, &tid );
}

#else

struct inproc_sync *create_inproc_event( enum inproc_sync_type type, int signaled )
{
    set_error( STATUS_NOT_IMPLEMENTED );
    return NULL;
}

struct inproc_sync *create_inproc_semaphore( unsigned int count, unsigned int max )
{
    set_error( STATUS_NOT_IMPLEMENTED );
    return NULL;
}

struct inproc_sync *create_inproc_mutex( thread_id_t owner, unsigned int count )
{
    set_error( STATUS_NOT_IMPLEMENTED );
    return NULL;
}

void set_inproc_event( struct inproc_sync *inproc_sync )
{
}

void reset_inproc_event( struct inproc_sync *obj )
{
}

void abandon_inproc_mutex( thread_id_t tid, struct inproc_sync *inproc_sync )
{
}

#endif

DECL_HANDLER(get_linux_sync_device)
{
#ifdef HAVE_LINUX_NTSYNC_H
    struct linux_device *device;

    if ((device = get_linux_device()))
    {
        reply->handle = alloc_handle( current->process, device, 0, 0 );
        release_object( device );
    }
#else
    set_error( STATUS_NOT_IMPLEMENTED );
#endif
}

DECL_HANDLER(get_linux_sync_obj)
{
#ifdef HAVE_LINUX_NTSYNC_H
    struct object *obj;

    if ((obj = get_handle_obj( current->process, req->handle, 0, NULL )))
    {
        struct inproc_sync *inproc_sync;

        if ((inproc_sync = obj->ops->get_inproc_sync( obj )))
        {
            reply->handle = alloc_handle( current->process, inproc_sync, 0, 0 );
            reply->type = inproc_sync->type;
            reply->access = get_handle_access( current->process, req->handle );
            release_object( inproc_sync );
        }
        release_object( obj );
    }
#else
    set_error( STATUS_NOT_IMPLEMENTED );
#endif
}
