/*
 * PipeWire audio driver for Wine
 *
 * Copyright 2026 M0n7y5
 *
 * The WASAPI ring-buffer, timer, clock, volume and packet mechanics in this
 * file are transplanted from Wine's winepulse.drv (pulse.c) and remain under
 * its notice:
 *   Copyright 2011-2012 Maarten Lankhorst
 *   Copyright 2010-2011 Maarten Lankhorst for CodeWeavers
 *   Copyright 2011 Andrew Eikum for CodeWeavers
 *   Copyright 2022 Huw Davies
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

#if 0
#pragma makedep unix
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  /* dladdr() */
#endif

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/json.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"

#include "mmdeviceapi.h"
#include "initguid.h"
#include "audioclient.h"

#include "wine/debug.h"
#include "wine/list.h"
#include "wine/unixlib.h"

#include "../mmdevapi/unixlib.h"

#include "mult.h"

WINE_DEFAULT_DEBUG_CHANNEL(pipewire);

#define PW_CHANNELS_MAX 64

/* Constrains driver allocations to the 32-bit address space for WoW64
 * callers; left 0 for native 64-bit processes. */
static ULONG_PTR zero_bits = 0;

/* ----------------------------------------------------------------------
 * Stream and device structures (struct pulse_stream transplant)
 * ---------------------------------------------------------------------- */

struct pipewire_stream
{
    EDataFlow dataflow;

    struct pw_stream *pw;
    struct spa_hook stream_listener;
    struct spa_audio_info_raw info;
    UINT32 frame_size;
    UINT32 rate_connected; /* negotiated stream rate; SPA_PROP_rate is absolute vs this */

    DWORD flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;
    float vol[PW_CHANNELS_MAX];

    REFERENCE_TIME def_period;
    REFERENCE_TIME duration;

    INT32 locked;
    BOOL started;
    SIZE_T bufsize_frames, real_bufsize_bytes, period_bytes;
    /* render ring bookkeeping: lcl_offs/held track the application side,
     * pa_offs/pa_held track the process-callback reader (field names kept
     * from the pulse.c transplant for diffability).  pa_held_bytes is
     * SPSC-atomic (producer release_render_buffer release-adds, the RT
     * process callback acquire-loads and subtracts); pa_offs_bytes stays
     * plain, a bounded residual race the producer only hits on overflow. */
    SIZE_T lcl_offs_bytes, pa_offs_bytes;
    SIZE_T tmp_buffer_bytes, held_bytes, pa_held_bytes;
    BYTE *local_buffer, *tmp_buffer;
    void *locked_ptr;
    UINT64 mmdev_period_usec;

    /* capture staging ring: the analogue of PulseAudio's internal record
     * buffer.  The pw_stream process callback (a foreign pthread) appends
     * raw bytes here; the Wine timer thread slices period-sized ACPackets
     * out of it with QPC timestamps.  cap_held_bytes is SPSC-atomic (RT
     * producer release-adds, pipewire_read acquire-loads and subtracts);
     * cap_read_offs stays plain, raced only in the overrun-drop path. */
    BYTE *capture_ring;
    SIZE_T capture_ring_size, cap_read_offs, cap_held_bytes;

    INT64 clock_lastpos, clock_written;
    UINT32 underrun_count, overrun_count; /* render starves / capture drops; one-line summary at release */

    struct list packet_free_head;
    struct list packet_filled_head;

    char *device;
    struct list period_entry;
    struct pipewire_period *period;
};

typedef struct _ACPacket
{
    struct list entry;
    UINT64 qpcpos;
    BYTE *data;
    UINT32 discont;
} ACPacket;

struct pw_phys_device
{
    struct list entry;
    WCHAR *display;
    EndpointFormFactor form;
    UINT channel_mask;
    REFERENCE_TIME min_period, def_period;
    WAVEFORMATEXTENSIBLE fmt;
    char pw_name[];
};

struct pipewire_period
{
    struct list entry;
    char *device;
    UINT64 period_usec;
    struct list streams;
    HANDLE timer_thread;
    BOOL please_quit;
    struct pipewire_stream *timer_stream;
    UINT64 last_time;
    BOOL grid_valid;
};

/* ----------------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------------- */

static struct pw_thread_loop *pw_loop_global;
static struct pw_context *pw_ctx;
static struct pw_core *pw_core_global;
static struct spa_hook core_listener;
static BOOL core_listener_added;
static BOOL core_dead;

static pthread_mutex_t pw_init_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct list g_render_devices = LIST_INIT(g_render_devices);
static struct list g_capture_devices = LIST_INIT(g_capture_devices);
static struct list active_periods = LIST_INIT(active_periods);
static char g_default_sink[256];
static char g_default_source[256];

/* ----------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

/* copied from kernelbase */
static int muldiv(int a, int b, int c)
{
    LONGLONG ret;

    if (!c) return -1;

    if (c < 0)
    {
        a = -a;
        c = -c;
    }

    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        ret = (((LONGLONG)a * b) + (c / 2)) / c;
    else
        ret = (((LONGLONG)a * b) - (c / 2)) / c;

    if (ret > 2147483647 || ret < -2147483647) return -1;
    return ret;
}

static char *wstr_to_str(const WCHAR *wstr)
{
    const int len = wcslen(wstr);
    char *str = malloc(len * 3 + 1);
    if (!str) return NULL;
    ntdll_wcstoumbs(wstr, len + 1, str, len * 3 + 1, FALSE);
    return str;
}

/* Validate before UTF-8 conversion; non-ASCII names must not depend on char signedness. */
static char *app_name_from_wstr(const WCHAR *appname)
{
    const WCHAR *w;

    if (!appname)
        return NULL;
    for (w = appname; *w; w++)
        if (*w > ' ')
            return wstr_to_str(appname);
    return NULL;
}

static WCHAR *utf8_to_wstr(const char *s)
{
    size_t len = strlen(s);
    WCHAR *w = malloc((len + 1) * sizeof(WCHAR));
    DWORD n;

    if (!w) return NULL;
    n = ntdll_umbstowcs(s, len, w, len);
    w[n] = '\0';
    return w;
}

static struct pipewire_stream *handle_get_stream(stream_handle h)
{
    return (struct pipewire_stream *)(UINT_PTR)h;
}

static UINT spa_format_bytes(enum spa_audio_format f)
{
    switch (f)
    {
    case SPA_AUDIO_FORMAT_U8:
    case SPA_AUDIO_FORMAT_S8:
    case SPA_AUDIO_FORMAT_ULAW:
    case SPA_AUDIO_FORMAT_ALAW:
        return 1;
    case SPA_AUDIO_FORMAT_S16_LE:
        return 2;
    case SPA_AUDIO_FORMAT_S24_LE:
        return 3;
    case SPA_AUDIO_FORMAT_S32_LE:
    case SPA_AUDIO_FORMAT_S24_32_LE:
    case SPA_AUDIO_FORMAT_F32_LE:
        return 4;
    default:
        return 0;
    }
}

static void silence_buffer(enum spa_audio_format format, BYTE *buffer, UINT32 bytes)
{
    memset(buffer, format == SPA_AUDIO_FORMAT_U8 ? 0x80 : 0, bytes);
}

/* copy n bytes out of a byte ring starting at offs, wrapping at ring_size */
static void copy_from_ring(BYTE *dst, const BYTE *ring, SIZE_T ring_size, SIZE_T offs, SIZE_T n)
{
    SIZE_T first = min(n, ring_size - offs);
    memcpy(dst, ring + offs, first);
    if (n > first)
        memcpy(dst + first, ring, n - first);
}

/* Parse {"name":"<value>"} metadata values.  spa_json_str_object_find needs
 * PipeWire 1.4; this uses only the stable spa_json core (verified in 1.0.0). */
static int parse_json_str_field(const char *json, const char *field, char *dst, size_t maxlen)
{
    struct spa_json it[2];
    char key[64];
    const char *val;
    int len;

    spa_json_init(&it[0], json, strlen(json));
    if (spa_json_enter_object(&it[0], &it[1]) <= 0)
        return -1;
    while (spa_json_get_string(&it[1], key, sizeof(key)) > 0)
    {
        if (!strcmp(key, field))
            return spa_json_get_string(&it[1], dst, maxlen) > 0 ? 0 : -1;
        if ((len = spa_json_next(&it[1], &val)) <= 0)
            return -1;
        if (spa_json_is_container(val, len) && spa_json_container_len(&it[1], val, len) <= 0)
            return -1;
    }
    return -1;
}

/* SPA channel position -> WASAPI speaker bit */
static UINT spa_position_to_mask_bit(uint32_t pos)
{
    switch (pos)
    {
    case SPA_AUDIO_CHANNEL_MONO:
    case SPA_AUDIO_CHANNEL_FC:   return SPEAKER_FRONT_CENTER;
    case SPA_AUDIO_CHANNEL_FL:   return SPEAKER_FRONT_LEFT;
    case SPA_AUDIO_CHANNEL_FR:   return SPEAKER_FRONT_RIGHT;
    case SPA_AUDIO_CHANNEL_LFE:  return SPEAKER_LOW_FREQUENCY;
    case SPA_AUDIO_CHANNEL_RL:   return SPEAKER_BACK_LEFT;
    case SPA_AUDIO_CHANNEL_RR:   return SPEAKER_BACK_RIGHT;
    case SPA_AUDIO_CHANNEL_FLC:  return SPEAKER_FRONT_LEFT_OF_CENTER;
    case SPA_AUDIO_CHANNEL_FRC:  return SPEAKER_FRONT_RIGHT_OF_CENTER;
    case SPA_AUDIO_CHANNEL_RC:   return SPEAKER_BACK_CENTER;
    case SPA_AUDIO_CHANNEL_SL:   return SPEAKER_SIDE_LEFT;
    case SPA_AUDIO_CHANNEL_SR:   return SPEAKER_SIDE_RIGHT;
    case SPA_AUDIO_CHANNEL_TC:   return SPEAKER_TOP_CENTER;
    case SPA_AUDIO_CHANNEL_TFL:  return SPEAKER_TOP_FRONT_LEFT;
    case SPA_AUDIO_CHANNEL_TFC:  return SPEAKER_TOP_FRONT_CENTER;
    case SPA_AUDIO_CHANNEL_TFR:  return SPEAKER_TOP_FRONT_RIGHT;
    case SPA_AUDIO_CHANNEL_TRL:  return SPEAKER_TOP_BACK_LEFT;
    case SPA_AUDIO_CHANNEL_TRC:  return SPEAKER_TOP_BACK_CENTER;
    case SPA_AUDIO_CHANNEL_TRR:  return SPEAKER_TOP_BACK_RIGHT;
    default:                     return 0;
    }
}

static UINT positions_to_mask(const uint32_t *pos, uint32_t channels)
{
    UINT mask = 0, i;
    for (i = 0; i < channels; i++)
        mask |= spa_position_to_mask_bit(pos[i]);
    return mask;
}

/* WASAPI speaker bit index -> SPA channel position (inverse table) */
static const uint32_t spa_pos_from_wfx[] = {
    SPA_AUDIO_CHANNEL_FL,   /* SPEAKER_FRONT_LEFT */
    SPA_AUDIO_CHANNEL_FR,   /* SPEAKER_FRONT_RIGHT */
    SPA_AUDIO_CHANNEL_FC,   /* SPEAKER_FRONT_CENTER */
    SPA_AUDIO_CHANNEL_LFE,  /* SPEAKER_LOW_FREQUENCY */
    SPA_AUDIO_CHANNEL_RL,   /* SPEAKER_BACK_LEFT */
    SPA_AUDIO_CHANNEL_RR,   /* SPEAKER_BACK_RIGHT */
    SPA_AUDIO_CHANNEL_FLC,  /* SPEAKER_FRONT_LEFT_OF_CENTER */
    SPA_AUDIO_CHANNEL_FRC,  /* SPEAKER_FRONT_RIGHT_OF_CENTER */
    SPA_AUDIO_CHANNEL_RC,   /* SPEAKER_BACK_CENTER */
    SPA_AUDIO_CHANNEL_SL,   /* SPEAKER_SIDE_LEFT */
    SPA_AUDIO_CHANNEL_SR,   /* SPEAKER_SIDE_RIGHT */
    SPA_AUDIO_CHANNEL_TC,   /* SPEAKER_TOP_CENTER */
    SPA_AUDIO_CHANNEL_TFL,  /* SPEAKER_TOP_FRONT_LEFT */
    SPA_AUDIO_CHANNEL_TFC,  /* SPEAKER_TOP_FRONT_CENTER */
    SPA_AUDIO_CHANNEL_TFR,  /* SPEAKER_TOP_FRONT_RIGHT */
    SPA_AUDIO_CHANNEL_TRL,  /* SPEAKER_TOP_BACK_LEFT */
    SPA_AUDIO_CHANNEL_TRC,  /* SPEAKER_TOP_BACK_CENTER */
    SPA_AUDIO_CHANNEL_TRR,  /* SPEAKER_TOP_BACK_RIGHT */
};

static UINT get_channel_mask(unsigned int channels)
{
    switch (channels)
    {
    case 0:  return 0;
    case 1:  return KSAUDIO_SPEAKER_MONO;
    case 2:  return KSAUDIO_SPEAKER_STEREO;
    case 3:  return KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY;
    case 4:  return KSAUDIO_SPEAKER_QUAD;
    case 5:  return KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY;
    case 6:  return KSAUDIO_SPEAKER_5POINT1;
    case 7:  return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
    case 8:  return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    }
    FIXME("Unknown speaker configuration: %u\n", channels);
    return 0;
}

/* find the nearest Windows-reportable channel configuration that is a
 * superset of the given speakers (transplant of pulse convert_channel_map) */
static void convert_channel_map(uint32_t channels, UINT mask, WAVEFORMATEXTENSIBLE *fmt)
{
    if (channels == 1)
    {
        fmt->Format.nChannels = 1;
        fmt->dwChannelMask = mask;
        return;
    }
    if (channels <= 2 && (mask & ~KSAUDIO_SPEAKER_STEREO) == 0)
    {
        fmt->Format.nChannels = 2;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
        return;
    }
    if (channels <= 4 && (mask & ~KSAUDIO_SPEAKER_QUAD) == 0)
    {
        fmt->Format.nChannels = 4;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_QUAD;
        return;
    }
    if (channels <= 4 && (mask & ~KSAUDIO_SPEAKER_SURROUND) == 0)
    {
        fmt->Format.nChannels = 4;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_SURROUND;
        return;
    }
    if (channels <= 6 && (mask & ~KSAUDIO_SPEAKER_5POINT1) == 0)
    {
        fmt->Format.nChannels = 6;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
        return;
    }
    if (channels <= 6 && (mask & ~KSAUDIO_SPEAKER_5POINT1_SURROUND) == 0)
    {
        fmt->Format.nChannels = 6;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_5POINT1_SURROUND;
        return;
    }
    if (channels <= 8 && (mask & ~KSAUDIO_SPEAKER_7POINT1) == 0)
    {
        fmt->Format.nChannels = 8;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_7POINT1;
        return;
    }
    if (channels <= 8 && (mask & ~KSAUDIO_SPEAKER_7POINT1_SURROUND) == 0)
    {
        fmt->Format.nChannels = 8;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
        return;
    }
    fmt->Format.nChannels = channels;
    fmt->dwChannelMask = mask;
}

static void build_format(WAVEFORMATEXTENSIBLE *fmt, uint32_t rate, uint32_t channels, UINT mask)
{
    WAVEFORMATEX *wfx = &fmt->Format;

    wfx->wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx->cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    convert_channel_map(channels, mask, fmt);
    wfx->wBitsPerSample = 32;
    wfx->nSamplesPerSec = rate ? rate : 48000;
    wfx->nBlockAlign = wfx->nChannels * wfx->wBitsPerSample / 8;
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
    fmt->Samples.wValidBitsPerSample = 32;
    fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

static struct pw_phys_device *add_device(struct list *list, const char *pw_name, const char *display,
                                         EndpointFormFactor form, uint32_t rate, uint32_t channels, UINT mask,
                                         REFERENCE_TIME min_period)
{
    size_t len = strlen(pw_name);
    struct pw_phys_device *dev = malloc(offsetof(struct pw_phys_device, pw_name) + len + 1);

    if (!dev)
        return NULL;
    if (!(dev->display = utf8_to_wstr(display)))
    {
        free(dev);
        return NULL;
    }
    dev->form = form;
    build_format(&dev->fmt, rate, channels, mask);
    dev->channel_mask = dev->fmt.dwChannelMask;
    dev->def_period = 100000;
    dev->min_period = min_period;
    memcpy(dev->pw_name, pw_name, len + 1);
    list_add_tail(list, &dev->entry);
    TRACE("%s (%s) channels=%u mask=%#x\n", debugstr_w(dev->display), pw_name,
          dev->fmt.Format.nChannels, dev->channel_mask);
    return dev;
}

static void free_device_lists(void)
{
    static struct list *const lists[] = { &g_render_devices, &g_capture_devices, NULL };
    struct list *const *list = lists;
    struct pw_phys_device *dev, *next;

    do {
        LIST_FOR_EACH_ENTRY_SAFE(dev, next, *list, struct pw_phys_device, entry)
        {
            list_remove(&dev->entry);
            free(dev->display);
            free(dev);
        }
    } while (*(++list));
}

/* ----------------------------------------------------------------------
 * Process attach / detach
 * ---------------------------------------------------------------------- */

/* True when dir holds a SPA support plugin of this process's architecture. */
static BOOL spa_plugin_dir_usable(const char *dir)
{
    char path[PATH_MAX + 64];
    unsigned char ident[5];
    int fd, n;

    snprintf(path, sizeof(path), "%s/support/libspa-support.so", dir);
    if ((fd = open(path, O_RDONLY)) < 0)
        return FALSE;
    n = read(fd, ident, sizeof(ident));
    close(fd);
    if (n != (int)sizeof(ident) || memcmp(ident, "\x7f""ELF", 4))
        return FALSE;
    return ident[4] == (sizeof(void *) == 8 ? 2 : 1); /* ELFCLASS64 : ELFCLASS32 */
}

/* Containers (Steam pressure-vessel) import libpipewire from the host but its
 * compiled-in SPA plugin path does not exist inside the container, so
 * pw_loop_new() fails with "can't make support.system handle".  The plugins
 * live in the same libdir as the loaded library and match its architecture, so
 * derive the path from the loaded object.  A different-architecture parent (a
 * 32-bit launcher spawning a 64-bit game) also leaks its own SPA_PLUGIN_DIR
 * through the environment; drop such an inherited value and re-derive ours. */
static void pipewire_set_plugin_dirs(void)
{
    Dl_info info;
    char libdir[PATH_MAX], path[PATH_MAX + 64], *sep;
    const char *existing = getenv("SPA_PLUGIN_DIR");

    if (existing)
    {
        if (spa_plugin_dir_usable(existing))
        {
            TRACE("keeping SPA_PLUGIN_DIR %s\n", existing);
            return;
        }
        WARN("inherited SPA_PLUGIN_DIR %s is not loadable by this process; re-deriving\n", existing);
        unsetenv("SPA_PLUGIN_DIR");
        unsetenv("PIPEWIRE_MODULE_DIR");
    }

    if (!dladdr((void *)pw_init, &info) || !info.dli_fname)
    {
        WARN("cannot locate the loaded libpipewire; leaving SPA_PLUGIN_DIR unset\n");
        return;
    }
    if (!realpath(info.dli_fname, libdir))
    {
        WARN("cannot resolve libpipewire path %s; leaving SPA_PLUGIN_DIR unset\n", info.dli_fname);
        return;
    }
    if (!(sep = strrchr(libdir, '/')))
        return;
    *sep = 0;

    snprintf(path, sizeof(path), "%s/spa-0.2/support/libspa-support.so", libdir);
    if (access(path, F_OK))
    {
        WARN("no SPA support plugin at %s; leaving SPA_PLUGIN_DIR unset\n", path);
        return;
    }

    snprintf(path, sizeof(path), "%s/spa-0.2", libdir);
    setenv("SPA_PLUGIN_DIR", path, 1);
    snprintf(path, sizeof(path), "%s/pipewire-0.3", libdir);
    if (!access(path, F_OK))
        setenv("PIPEWIRE_MODULE_DIR", path, 0);
    TRACE("derived SPA plugin dir from %s\n", libdir);
}

static NTSTATUS pipewire_process_attach(void *args)
{
    pipewire_set_plugin_dirs();
    pw_init(NULL, NULL);
    TRACE("PipeWire %s, header %s\n", pw_get_library_version(), pw_get_headers_version());
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_process_detach(void *args)
{
    /* May run at process exit (DLL_PROCESS_DETACH with lpvReserved set), with
     * the pw_thread_loop still running: it is a raw pthread that process
     * termination does not stop, and main_loop_stop is skipped on that path.
     * Do not take the loop lock (a killed thread may have held it) and do not
     * pw_deinit() here (it dlcloses the PipeWire modules under the live loop
     * thread).  Clean library teardown happens in pipewire_main_loop_stop on
     * the FreeLibrary path; at process exit the OS reclaims everything. */
    free_device_lists();
    return STATUS_SUCCESS;
}

/* ----------------------------------------------------------------------
 * Connection / main loop
 * ---------------------------------------------------------------------- */

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    /* Runs on the PipeWire loop thread: pw API only, no ntdll/Wine calls. */
    if (id == PW_ID_CORE)
    {
        core_dead = TRUE;
        if (pw_loop_global)
            pw_thread_loop_signal(pw_loop_global, false);
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

/* Called with the loop lock held. */
static HRESULT pipewire_connect(const WCHAR *appname)
{
    struct pw_properties *props = NULL;
    char *app;

    if (pw_core_global && !core_dead)
        return S_OK;

    if (pw_core_global)
    {
        if (core_listener_added)
        {
            spa_hook_remove(&core_listener);
            core_listener_added = FALSE;
        }
        pw_core_disconnect(pw_core_global);
        pw_core_global = NULL;
    }
    core_dead = FALSE;

    if ((app = app_name_from_wstr(appname)))
    {
        props = pw_properties_new(PW_KEY_APP_NAME, app, NULL);
        free(app);
    }
    if (!(pw_core_global = pw_context_connect(pw_ctx, props, 0)))
    {
        WARN("pw_context_connect failed\n");
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    }
    pw_core_add_listener(pw_core_global, &core_listener, &core_events, NULL);
    core_listener_added = TRUE;
    return S_OK;
}

static NTSTATUS pipewire_main_loop_start(void *args)
{
    pthread_mutex_lock(&pw_init_mutex);
    if (pw_loop_global)
        goto out;

    if (!(pw_loop_global = pw_thread_loop_new("winepipewire", NULL)))
    {
        ERR("pw_thread_loop_new failed; SPA plugins may be missing "
            "(set SPA_PLUGIN_DIR to the directory containing spa-0.2).\n");
        goto out;
    }
    if (!(pw_ctx = pw_context_new(pw_thread_loop_get_loop(pw_loop_global), NULL, 0)))
    {
        ERR("pw_context_new failed\n");
        pw_thread_loop_destroy(pw_loop_global);
        pw_loop_global = NULL;
        goto out;
    }
    if (pw_thread_loop_start(pw_loop_global) < 0)
    {
        ERR("pw_thread_loop_start failed\n");
        pw_context_destroy(pw_ctx);
        pw_ctx = NULL;
        pw_thread_loop_destroy(pw_loop_global);
        pw_loop_global = NULL;
    }

out:
    pthread_mutex_unlock(&pw_init_mutex);
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_main_loop_stop(void *args)
{
    pthread_mutex_lock(&pw_init_mutex);
    if (pw_loop_global)
    {
        pw_thread_loop_lock(pw_loop_global);
        if (pw_core_global)
        {
            if (core_listener_added)
            {
                spa_hook_remove(&core_listener);
                core_listener_added = FALSE;
            }
            pw_core_disconnect(pw_core_global);
            pw_core_global = NULL;
        }
        pw_thread_loop_unlock(pw_loop_global);
        pw_thread_loop_stop(pw_loop_global);
        if (pw_ctx)
        {
            pw_context_destroy(pw_ctx);
            pw_ctx = NULL;
        }
        pw_thread_loop_destroy(pw_loop_global);
        pw_loop_global = NULL;
    }
    pthread_mutex_unlock(&pw_init_mutex);
    pw_deinit();
    return STATUS_SUCCESS;
}

/* ----------------------------------------------------------------------
 * Probe: device enumeration, defaults, formats (test_connect)
 *
 * All registry / node / metadata / core callbacks below run on the probe
 * thread loop's own (foreign) thread.  They may only call libc, spa and pw
 * APIs -- never ntdll/Wine (no TRACE).  Strings are kept as UTF-8 and
 * converted to UTF-16 later, on the unix-call thread.
 * ---------------------------------------------------------------------- */

struct probe_node
{
    struct list entry;
    uint32_t id;
    EDataFlow flow;
    char *node_name;
    char *display;
    struct pw_node *proxy;
    struct spa_hook listener;
    uint32_t channels;
    uint32_t position[SPA_AUDIO_MAX_CHANNELS];
    int have_format;
};

struct probe
{
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook core_listener;
    struct spa_hook registry_listener;
    int sync_seq;
    struct list nodes;
    struct pw_metadata *meta_default;
    struct pw_metadata *meta_settings;
    struct spa_hook meta_default_listener;
    struct spa_hook meta_settings_listener;
    char default_sink[256];
    char default_source[256];
    uint32_t clock_rate;
    uint32_t min_quantum;
    BOOL core_error;
};

static void on_probe_node_param(void *data, int seq, uint32_t id, uint32_t index,
                                uint32_t next, const struct spa_pod *param)
{
    struct probe_node *pn = data;
    struct spa_audio_info_raw info;

    if (!param || id != SPA_PARAM_EnumFormat)
        return;
    spa_zero(info);
    if (spa_format_audio_raw_parse(param, &info) < 0)
        return;
    if (info.channels < 1 || info.channels > SPA_AUDIO_MAX_CHANNELS)
        return;
    /* Keep the configuration with the most channels: best surround coverage. */
    if (!pn->have_format || info.channels > pn->channels)
    {
        pn->channels = info.channels;
        memcpy(pn->position, info.position, sizeof(info.position));
        pn->have_format = 1;
    }
}

static const struct pw_node_events probe_node_events = {
    PW_VERSION_NODE_EVENTS,
    .param = on_probe_node_param,
};

static int on_probe_metadata_property(void *data, uint32_t subject, const char *key,
                                      const char *type, const char *value)
{
    struct probe *p = data;
    char *dst;

    if (!key || !value)
        return 0;
    if (!strcmp(key, "default.audio.sink"))
        dst = p->default_sink;
    else if (!strcmp(key, "default.audio.source"))
        dst = p->default_source;
    else
        return 0;
    parse_json_str_field(value, "name", dst, 256);
    return 0;
}

static const struct pw_metadata_events probe_metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_probe_metadata_property,
};

static int on_probe_settings_property(void *data, uint32_t subject, const char *key,
                                      const char *type, const char *value)
{
    struct probe *p = data;

    if (!key || !value)
        return 0;
    if (!strcmp(key, "clock.force-rate"))
    {
        uint32_t r = (uint32_t)strtoul(value, NULL, 10);
        if (r)
            p->clock_rate = r;
    }
    else if (!strcmp(key, "clock.rate") && !p->clock_rate)
    {
        p->clock_rate = (uint32_t)strtoul(value, NULL, 10);
    }
    else if (!strcmp(key, "clock.force-quantum"))
    {
        uint32_t q = (uint32_t)strtoul(value, NULL, 10);
        if (q)
            p->min_quantum = q;
    }
    else if (!strcmp(key, "clock.min-quantum") && !p->min_quantum)
    {
        p->min_quantum = (uint32_t)strtoul(value, NULL, 10);
    }
    return 0;
}

static const struct pw_metadata_events probe_settings_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_probe_settings_property,
};

static void on_probe_registry_global(void *data, uint32_t id, uint32_t permissions,
                                      const char *type, uint32_t version,
                                      const struct spa_dict *props)
{
    struct probe *p = data;

    if (!type || !props)
        return;

    if (!strcmp(type, PW_TYPE_INTERFACE_Node))
    {
        const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        const char *nick = spa_dict_lookup(props, PW_KEY_NODE_NICK);
        struct probe_node *pn;

        if (!media_class || !node_name)
            return;
        if (strcmp(media_class, "Audio/Sink") && strcmp(media_class, "Audio/Source"))
            return;

        if (!(pn = calloc(1, sizeof(*pn))))
            return;
        pn->id = id;
        pn->flow = !strcmp(media_class, "Audio/Sink") ? eRender : eCapture;
        pn->node_name = strdup(node_name);
        pn->display = strdup(desc ? desc : (nick ? nick : node_name));
        if (!pn->node_name || !pn->display)
        {
            free(pn->node_name);
            free(pn->display);
            free(pn);
            return;
        }
        list_add_tail(&p->nodes, &pn->entry);

        /* Bind the node and ask for its supported formats so we can report
         * the real channel layout.  The param events arrive during the sync
         * round-trips below. */
        pn->proxy = pw_registry_bind(p->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
        if (pn->proxy)
        {
            pw_node_add_listener(pn->proxy, &pn->listener, &probe_node_events, pn);
            pw_node_enum_params(pn->proxy, 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX, NULL);
        }
    }
    else if (!strcmp(type, PW_TYPE_INTERFACE_Metadata))
    {
        const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (!name)
            return;
        if (!strcmp(name, "default") && !p->meta_default)
        {
            p->meta_default = pw_registry_bind(p->registry, id, PW_TYPE_INTERFACE_Metadata,
                                               PW_VERSION_METADATA, 0);
            if (p->meta_default)
                pw_metadata_add_listener(p->meta_default, &p->meta_default_listener,
                                         &probe_metadata_events, p);
        }
        else if (!strcmp(name, "settings") && !p->meta_settings)
        {
            p->meta_settings = pw_registry_bind(p->registry, id, PW_TYPE_INTERFACE_Metadata,
                                                PW_VERSION_METADATA, 0);
            if (p->meta_settings)
                pw_metadata_add_listener(p->meta_settings, &p->meta_settings_listener,
                                         &probe_settings_events, p);
        }
    }
}

static void on_probe_registry_global_remove(void *data, uint32_t id)
{
}

static const struct pw_registry_events probe_registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_probe_registry_global,
    .global_remove = on_probe_registry_global_remove,
};

static void on_probe_core_done(void *data, uint32_t id, int seq)
{
    struct probe *p = data;
    if (id == PW_ID_CORE && seq == p->sync_seq)
        pw_thread_loop_signal(p->loop, false);
}

static void on_probe_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    /* Runs on the PipeWire loop thread: pw API only, no ntdll/Wine calls. */
    struct probe *p = data;
    if (id == PW_ID_CORE)
    {
        p->core_error = TRUE;
        pw_thread_loop_signal(p->loop, false);
    }
}

static const struct pw_core_events probe_core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_probe_core_done,
    .error = on_probe_core_error,
};

/* Round-trip the core; must be called with the loop lock held. */
static void probe_roundtrip(struct probe *p)
{
    if (p->core_error)
        return;
    p->sync_seq = pw_core_sync(p->core, PW_ID_CORE, p->sync_seq);
    pw_thread_loop_timed_wait(p->loop, 2);
}

/* Synthetic default endpoint at index 0 (empty pw_name -> session manager
 * default routing).  When the default node is known, mirror its format so
 * the default endpoint advertises the real speaker layout. */
static void add_default_device(struct list *list, EndpointFormFactor form, const char *match,
                               uint32_t rate, REFERENCE_TIME min_period)
{
    struct pw_phys_device *dev, *def_src = NULL, *def;

    if (match[0])
    {
        LIST_FOR_EACH_ENTRY(dev, list, struct pw_phys_device, entry)
            if (!strcmp(dev->pw_name, match)) { def_src = dev; break; }
    }
    if (!(def = malloc(offsetof(struct pw_phys_device, pw_name) + 1)))
        return;
    if (!(def->display = utf8_to_wstr("PipeWire")))
    {
        free(def);
        return;
    }
    def->pw_name[0] = '\0';
    def->form = form;
    def->def_period = 100000;
    def->min_period = min_period;
    if (def_src)
    {
        def->fmt = def_src->fmt;
        def->channel_mask = def_src->channel_mask;
    }
    else
    {
        build_format(&def->fmt, rate, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        def->channel_mask = def->fmt.dwChannelMask;
    }
    list_add_head(list, &def->entry);
}

static void build_device_cache(struct probe *p)
{
    struct probe_node *pn;
    uint32_t rate = p->clock_rate ? p->clock_rate : 48000;
    REFERENCE_TIME min_period = 30000;

    /* IAudioClient3 shared-mode floor.  The graph cannot deliver cycles
     * below clock.min-quantum (clock.force-quantum pins it outright), and
     * 128 frames (~2.7 ms at 48 kHz) matches the typical Windows engine
     * floor.  Clamp to the 3 ms winepulse-parity value so the advertised
     * minimum only ever improves; without settings metadata keep 3 ms. */
    if (p->min_quantum)
    {
        /* Floor division: mmdevapi converts back with a ceiling, so this
         * round-trips to the exact frame count. */
        REFERENCE_TIME q = (REFERENCE_TIME)p->min_quantum * 10000000 / rate;
        REFERENCE_TIME floor_rt = (REFERENCE_TIME)128 * 10000000 / rate;
        min_period = q > floor_rt ? q : floor_rt;
        if (min_period > 30000)
            min_period = 30000;
        TRACE("min_quantum=%u rate=%u -> min_period=%d hns\n",
              p->min_quantum, rate, (int)min_period);
    }

    lstrcpynA(g_default_sink, p->default_sink, sizeof(g_default_sink));
    lstrcpynA(g_default_source, p->default_source, sizeof(g_default_source));

    LIST_FOR_EACH_ENTRY(pn, &p->nodes, struct probe_node, entry)
    {
        struct list *list = (pn->flow == eRender) ? &g_render_devices : &g_capture_devices;
        EndpointFormFactor form = (pn->flow == eRender) ? Speakers : Microphone;
        uint32_t channels = pn->have_format ? pn->channels : 2;
        UINT mask = pn->have_format ? positions_to_mask(pn->position, pn->channels)
                                    : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        add_device(list, pn->node_name, pn->display, form, rate, channels, mask, min_period);
    }

    add_default_device(&g_render_devices, Speakers, g_default_sink, rate, min_period);
    add_default_device(&g_capture_devices, Microphone, g_default_source, rate, min_period);
}

static void probe_teardown(struct probe *p)
{
    struct probe_node *pn;

    LIST_FOR_EACH_ENTRY(pn, &p->nodes, struct probe_node, entry)
    {
        if (pn->proxy)
        {
            spa_hook_remove(&pn->listener);
            pw_proxy_destroy((struct pw_proxy *)pn->proxy);
            pn->proxy = NULL;
        }
    }
    if (p->meta_default)
    {
        spa_hook_remove(&p->meta_default_listener);
        pw_proxy_destroy((struct pw_proxy *)p->meta_default);
        p->meta_default = NULL;
    }
    if (p->meta_settings)
    {
        spa_hook_remove(&p->meta_settings_listener);
        pw_proxy_destroy((struct pw_proxy *)p->meta_settings);
        p->meta_settings = NULL;
    }
    if (p->registry)
    {
        spa_hook_remove(&p->registry_listener);
        pw_proxy_destroy((struct pw_proxy *)p->registry);
        p->registry = NULL;
    }
    if (p->core)
    {
        spa_hook_remove(&p->core_listener);
        pw_core_disconnect(p->core);
        p->core = NULL;
    }
}

static NTSTATUS pipewire_test_connect(void *args)
{
    struct test_connect_params *params = args;
    struct probe p;
    struct probe_node *pn, *next;

    free_device_lists();
    list_init(&g_render_devices);
    list_init(&g_capture_devices);
    g_default_sink[0] = g_default_source[0] = '\0';

    params->priority = Priority_Unavailable;

    memset(&p, 0, sizeof(p));
    list_init(&p.nodes);

    if (!(p.loop = pw_thread_loop_new("winepipewire-probe", NULL)))
    {
        ERR("Failed to create PipeWire probe loop; SPA plugins may be missing "
            "(set SPA_PLUGIN_DIR to the directory containing spa-0.2).\n");
        return STATUS_SUCCESS;
    }
    if (!(p.context = pw_context_new(pw_thread_loop_get_loop(p.loop), NULL, 0)))
    {
        ERR("Failed to create PipeWire probe context\n");
        pw_thread_loop_destroy(p.loop);
        return STATUS_SUCCESS;
    }

    if (pw_thread_loop_start(p.loop) < 0)
    {
        ERR("Failed to start PipeWire probe loop\n");
        pw_context_destroy(p.context);
        pw_thread_loop_destroy(p.loop);
        return STATUS_SUCCESS;
    }
    pw_thread_loop_lock(p.loop);

    if (!(p.core = pw_context_connect(p.context, NULL, 0)))
    {
        pw_thread_loop_unlock(p.loop);
        pw_thread_loop_stop(p.loop);
        pw_context_destroy(p.context);
        pw_thread_loop_destroy(p.loop);
        return STATUS_SUCCESS;
    }

    pw_core_add_listener(p.core, &p.core_listener, &probe_core_events, &p);
    p.registry = pw_core_get_registry(p.core, PW_VERSION_REGISTRY, 0);
    if (p.registry)
        pw_registry_add_listener(p.registry, &p.registry_listener, &probe_registry_events, &p);

    /* First round-trip: globals emitted, nodes/metadata bound, enum_params
     * issued.  Second: the param events and metadata properties land. */
    probe_roundtrip(&p);
    probe_roundtrip(&p);

    probe_teardown(&p);
    pw_thread_loop_unlock(p.loop);
    pw_thread_loop_stop(p.loop);
    pw_context_destroy(p.context);
    pw_thread_loop_destroy(p.loop);

    if (p.core_error && list_empty(&p.nodes))
    {
        WARN("PipeWire core reported an error during the probe\n");
        return STATUS_SUCCESS;
    }

    /* The pw loop is fully stopped: safe to do Wine string conversion. */
    build_device_cache(&p);

    LIST_FOR_EACH_ENTRY_SAFE(pn, next, &p.nodes, struct probe_node, entry)
    {
        list_remove(&pn->entry);
        free(pn->node_name);
        free(pn->display);
        free(pn);
    }

    TRACE("probe for %s: %u sinks default=%s, %u sources default=%s, rate=%u\n",
          debugstr_w(params->name), list_count(&g_render_devices), debugstr_a(g_default_sink),
          list_count(&g_capture_devices), debugstr_a(g_default_source), p.clock_rate);

    params->priority = Priority_Preferred;
    return STATUS_SUCCESS;
}

/* ----------------------------------------------------------------------
 * Endpoint enumeration / formats / periods / props
 * ---------------------------------------------------------------------- */

static NTSTATUS pipewire_get_endpoint_ids(void *args)
{
    struct get_endpoint_ids_params *params = args;
    struct list *list = (params->flow == eRender) ? &g_render_devices : &g_capture_devices;
    struct endpoint *endpoint = params->endpoints;
    size_t len, name_len, needed;
    unsigned int offset;
    struct pw_phys_device *dev;

    params->num = list_count(list);
    offset = needed = params->num * sizeof(*params->endpoints);

    LIST_FOR_EACH_ENTRY(dev, list, struct pw_phys_device, entry)
    {
        name_len = lstrlenW(dev->display) + 1;
        len = strlen(dev->pw_name) + 1;
        needed += name_len * sizeof(WCHAR) + ((len + 1) & ~1);

        if (needed <= params->size)
        {
            endpoint->name = offset;
            memcpy((char *)params->endpoints + offset, dev->display, name_len * sizeof(WCHAR));
            offset += name_len * sizeof(WCHAR);
            endpoint->device = offset;
            memcpy((char *)params->endpoints + offset, dev->pw_name, len);
            offset += (len + 1) & ~1;
            endpoint++;
        }
    }
    params->default_idx = 0;

    if (needed > params->size)
    {
        params->size = needed;
        params->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    else
        params->result = S_OK;
    return STATUS_SUCCESS;
}

/* Resolve a device string to its cache entry.  A loopback capture targets a
 * render sink, so an eCapture lookup that misses the capture list falls back
 * to the render list (the sink's format/period describe the loopback). */
static struct pw_phys_device *find_device(EDataFlow flow, const char *name)
{
    struct list *list = (flow == eRender) ? &g_render_devices : &g_capture_devices;
    struct pw_phys_device *dev;

    LIST_FOR_EACH_ENTRY(dev, list, struct pw_phys_device, entry)
        if (!strcmp(name, dev->pw_name))
            return dev;
    if (flow == eCapture)
    {
        LIST_FOR_EACH_ENTRY(dev, &g_render_devices, struct pw_phys_device, entry)
            if (!strcmp(name, dev->pw_name))
                return dev;
    }
    return NULL;
}

static NTSTATUS pipewire_get_mix_format(void *args)
{
    struct get_mix_format_params *params = args;
    struct pw_phys_device *dev = find_device(params->flow, params->device);

    if (dev)
    {
        *params->fmt = dev->fmt;
        params->result = S_OK;
    }
    else
    {
        WARN("device not found: flow %d %s.\n", params->flow, debugstr_a(params->device));
        params->result = E_FAIL;
    }
    return STATUS_SUCCESS;
}

static HRESULT get_device_period_helper(EDataFlow flow, const char *pw_name,
                                        REFERENCE_TIME *def, REFERENCE_TIME *min)
{
    struct pw_phys_device *dev;

    if (!def && !min)
        return E_POINTER;

    if (!(dev = find_device(flow, pw_name)))
        return E_FAIL;
    if (def)
        *def = dev->def_period;
    if (min)
        *min = dev->min_period;
    return S_OK;
}

static NTSTATUS pipewire_get_device_period(void *args)
{
    struct get_device_period_params *params = args;

    params->result = get_device_period_helper(params->flow, params->device,
                                               params->def_period, params->min_period);
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_is_format_supported(void *args)
{
    struct is_format_supported_params *params = args;

    /* Shared-mode format conversion/resampling is the adapter's job, so we
     * accept any format here (mirrors winepulse).  Exclusive mode is not
     * supported. */
    if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
        params->result = AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
    else
        params->result = S_OK;

    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_get_prop_value(void *args)
{
    static const GUID PKEY_AudioEndpoint_GUID = {
        0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}
    };
    static const PROPERTYKEY devicepath_key = {
        {0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 2
    };
    struct get_prop_value_params *params = args;
    struct pw_phys_device *dev = find_device(params->flow, params->device);

    if (!dev)
    {
        params->result = E_FAIL;
        return STATUS_SUCCESS;
    }
    if (IsEqualPropertyKey(*params->prop, devicepath_key))
    {
        /* PipeWire nodes do not reliably carry vendor/product ids. */
        params->result = E_NOTIMPL;
        return STATUS_SUCCESS;
    }
    if (IsEqualGUID(&params->prop->fmtid, &PKEY_AudioEndpoint_GUID))
    {
        switch (params->prop->pid)
        {
        case 0:   /* FormFactor */
            params->value->vt = VT_UI4;
            params->value->ulVal = dev->form;
            params->result = S_OK;
            return STATUS_SUCCESS;
        case 3:   /* PhysicalSpeakers */
            if (dev->channel_mask)
            {
                params->value->vt = VT_UI4;
                params->value->ulVal = dev->channel_mask;
                params->result = S_OK;
            }
            else
                params->result = E_FAIL;
            return STATUS_SUCCESS;
        }
    }
    params->result = E_NOTIMPL;
    return STATUS_SUCCESS;
}

/* ----------------------------------------------------------------------
 * Format mapping (pulse_spec_from_waveformat transplant)
 * ---------------------------------------------------------------------- */

static HRESULT pipewire_info_from_waveformat(struct pipewire_stream *stream, const WAVEFORMATEX *fmt)
{
    struct spa_audio_info_raw *info = &stream->info;
    enum spa_audio_format spafmt = SPA_AUDIO_FORMAT_UNKNOWN;
    UINT mask = 0, i = 0, j;

    memset(info, 0, sizeof(*info));
    info->rate = fmt->nSamplesPerSec;

    switch (fmt->wFormatTag)
    {
    case WAVE_FORMAT_IEEE_FLOAT:
        if (!fmt->nChannels || fmt->nChannels > 2 || fmt->wBitsPerSample != 32)
            break;
        spafmt = SPA_AUDIO_FORMAT_F32_LE;
        info->channels = fmt->nChannels;
        mask = get_channel_mask(fmt->nChannels);
        break;
    case WAVE_FORMAT_PCM:
        if (!fmt->nChannels || fmt->nChannels > 2)
            break;
        if (fmt->wBitsPerSample == 8)
            spafmt = SPA_AUDIO_FORMAT_U8;
        else if (fmt->wBitsPerSample == 16)
            spafmt = SPA_AUDIO_FORMAT_S16_LE;
        else if (fmt->wBitsPerSample == 24)
            spafmt = SPA_AUDIO_FORMAT_S24_LE;
        else if (fmt->wBitsPerSample == 32)
            spafmt = SPA_AUDIO_FORMAT_S32_LE;
        else
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        info->channels = fmt->nChannels;
        mask = get_channel_mask(fmt->nChannels);
        break;
    case WAVE_FORMAT_EXTENSIBLE:
    {
        WAVEFORMATEXTENSIBLE *wfe = (WAVEFORMATEXTENSIBLE *)fmt;
        if (fmt->cbSize < sizeof(*wfe) - sizeof(*fmt))
            break;
        mask = wfe->dwChannelMask;
        if (IsEqualGUID(&wfe->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
            (!wfe->Samples.wValidBitsPerSample || wfe->Samples.wValidBitsPerSample == 32) &&
            fmt->wBitsPerSample == 32)
            spafmt = SPA_AUDIO_FORMAT_F32_LE;
        else if (IsEqualGUID(&wfe->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))
        {
            DWORD valid = wfe->Samples.wValidBitsPerSample;
            if (!valid)
                valid = fmt->wBitsPerSample;
            if (!valid || valid > fmt->wBitsPerSample)
                break;
            switch (fmt->wBitsPerSample)
            {
            case 8:  if (valid == 8) spafmt = SPA_AUDIO_FORMAT_U8; break;
            case 16: if (valid == 16) spafmt = SPA_AUDIO_FORMAT_S16_LE; break;
            case 24: if (valid == 24) spafmt = SPA_AUDIO_FORMAT_S24_LE; break;
            case 32:
                if (valid == 32) spafmt = SPA_AUDIO_FORMAT_S32_LE;
                else if (valid == 24) spafmt = SPA_AUDIO_FORMAT_S24_32_LE;
                break;
            default:
                return AUDCLNT_E_UNSUPPORTED_FORMAT;
            }
        }
        info->channels = fmt->nChannels;
        if (!mask || (mask & (SPEAKER_ALL | SPEAKER_RESERVED)))
            mask = get_channel_mask(fmt->nChannels);
        for (j = 0; j < ARRAY_SIZE(spa_pos_from_wfx) && i < fmt->nChannels; ++j)
            if (mask & (1u << j))
                info->position[i++] = spa_pos_from_wfx[j];
        if (mask == SPEAKER_FRONT_CENTER)
            info->position[0] = SPA_AUDIO_CHANNEL_MONO;
        if (i < fmt->nChannels || (mask & SPEAKER_RESERVED))
        {
            ERR("Invalid channel mask: %u/%u and %#x\n", i, fmt->nChannels, mask);
            spafmt = SPA_AUDIO_FORMAT_UNKNOWN;
        }
        if (spafmt == SPA_AUDIO_FORMAT_UNKNOWN)
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        info->format = spafmt;
        return S_OK;
    }
    case WAVE_FORMAT_ALAW:
    case WAVE_FORMAT_MULAW:
        if (fmt->wBitsPerSample != 8)
        {
            FIXME("Unsupported bpp %u for LAW\n", fmt->wBitsPerSample);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        if (fmt->nChannels != 1 && fmt->nChannels != 2)
        {
            FIXME("Unsupported channels %u for LAW\n", fmt->nChannels);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        spafmt = fmt->wFormatTag == WAVE_FORMAT_MULAW ? SPA_AUDIO_FORMAT_ULAW : SPA_AUDIO_FORMAT_ALAW;
        info->channels = fmt->nChannels;
        mask = get_channel_mask(fmt->nChannels);
        break;
    default:
        WARN("Unhandled tag %x\n", fmt->wFormatTag);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    if (spafmt == SPA_AUDIO_FORMAT_UNKNOWN || !info->channels)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    info->format = spafmt;
    for (j = 0; j < ARRAY_SIZE(spa_pos_from_wfx) && i < info->channels; ++j)
        if (mask & (1u << j))
            info->position[i++] = spa_pos_from_wfx[j];
    if (mask == SPEAKER_FRONT_CENTER)
        info->position[0] = SPA_AUDIO_CHANNEL_MONO;
    return S_OK;
}

/* ----------------------------------------------------------------------
 * pw_stream callbacks (foreign thread, loop lock held: no ntdll/TRACE)
 * ---------------------------------------------------------------------- */

static void apply_volume(const struct pipewire_stream *stream, BYTE *buffer, UINT32 bytes)
{
    const float *vol = stream->vol;
    UINT32 i, channels = stream->info.channels, mute = 0;
    BOOL adjust = FALSE;
    BYTE *end;

    if (!bytes)
        return;

    for (i = 0; i < channels; i++)
    {
        adjust |= vol[i] != 1.0f;
        if (vol[i] == 0.0f)
            mute++;
    }
    if (mute == channels)
    {
        silence_buffer(stream->info.format, buffer, bytes);
        return;
    }
    if (!adjust)
        return;

    end = buffer + bytes;
    switch (stream->info.format)
    {
#ifndef WORDS_BIGENDIAN
#define PROCESS_BUFFER(type) do         \
{                                       \
    type *p = (type *)buffer;           \
    do                                  \
    {                                   \
        for (i = 0; i < channels; i++)  \
            p[i] = p[i] * vol[i];       \
        p += i;                         \
    } while ((BYTE *)p != end);         \
} while (0)
    case SPA_AUDIO_FORMAT_S16_LE:
        PROCESS_BUFFER(INT16);
        break;
    case SPA_AUDIO_FORMAT_S32_LE:
        PROCESS_BUFFER(INT32);
        break;
    case SPA_AUDIO_FORMAT_F32_LE:
        PROCESS_BUFFER(float);
        break;
#undef PROCESS_BUFFER
    case SPA_AUDIO_FORMAT_S24_32_LE:
    {
        UINT32 *p = (UINT32 *)buffer;
        do
        {
            for (i = 0; i < channels; i++)
            {
                p[i] = (INT32)((INT32)(p[i] << 8) * vol[i]);
                p[i] >>= 8;
            }
            p += i;
        } while ((BYTE *)p != end);
        break;
    }
    case SPA_AUDIO_FORMAT_S24_LE:
    {
        UINT32 *q = (UINT32 *)buffer;
        BYTE *p;

        i = 0;
        while (end - (BYTE *)q >= 12)
        {
            UINT32 v[4], k;
            v[0] = q[0] << 8;
            v[1] = q[1] << 16 | (q[0] >> 16 & ~0xff);
            v[2] = q[2] << 24 | (q[1] >> 8  & ~0xff);
            v[3] = q[2] & ~0xff;
            for (k = 0; k < 4; k++)
            {
                v[k] = (INT32)((INT32)v[k] * vol[i]);
                if (++i == channels) i = 0;
            }
            *q++ = v[0] >> 8  | (v[1] & ~0xff) << 16;
            *q++ = v[1] >> 16 | (v[2] & ~0xff) << 8;
            *q++ = v[2] >> 24 | (v[3] & ~0xff);
        }
        p = (BYTE *)q;
        while (p != end)
        {
            UINT32 v = (INT32)((INT32)(p[0] << 8 | p[1] << 16 | p[2] << 24) * vol[i]);
            *p++ = v >> 8  & 0xff;
            *p++ = v >> 16 & 0xff;
            *p++ = v >> 24;
            if (++i == channels) i = 0;
        }
        break;
    }
#endif
    case SPA_AUDIO_FORMAT_U8:
    {
        UINT8 *p = (UINT8 *)buffer;
        do
        {
            for (i = 0; i < channels; i++)
                p[i] = (int)((p[i] - 128) * vol[i]) + 128;
            p += i;
        } while ((BYTE *)p != end);
        break;
    }
    case SPA_AUDIO_FORMAT_ALAW:
    {
        UINT8 *p = (UINT8 *)buffer;
        do
        {
            for (i = 0; i < channels; i++)
                p[i] = mult_alaw_sample(p[i], vol[i]);
            p += i;
        } while ((BYTE *)p != end);
        break;
    }
    case SPA_AUDIO_FORMAT_ULAW:
    {
        UINT8 *p = (UINT8 *)buffer;
        do
        {
            for (i = 0; i < channels; i++)
                p[i] = mult_ulaw_sample(p[i], vol[i]);
            p += i;
        } while ((BYTE *)p != end);
        break;
    }
    default:
        break;
    }
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error)
{
    if (pw_loop_global)
        pw_thread_loop_signal(pw_loop_global, false);
}

static void on_stream_process(void *data)
{
    struct pipewire_stream *stream = data;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    struct spa_data *d;

    if (!(b = pw_stream_dequeue_buffer(stream->pw)))
        return;
    buf = b->buffer;
    if (!buf || !buf->n_datas || !buf->datas ||
        !(d = &buf->datas[0])->data || !d->chunk)
    {
        pw_stream_queue_buffer(stream->pw, b);
        return;
    }

    if (stream->dataflow == eRender)
    {
        UINT32 maxsize = d->maxsize;
        UINT32 req_frames, need_bytes, n;

        if (!b->requested || b->requested > maxsize / stream->frame_size)
            req_frames = maxsize / stream->frame_size;
        else
            req_frames = (UINT32)b->requested;
        need_bytes = req_frames * stream->frame_size;

        if (stream->started)
        {
            n = min(need_bytes, __atomic_load_n(&stream->pa_held_bytes, __ATOMIC_ACQUIRE));
            copy_from_ring(d->data, stream->local_buffer, stream->real_bufsize_bytes,
                           stream->pa_offs_bytes, n);
            apply_volume(stream, d->data, n);
            if (n < need_bytes)
            {
                silence_buffer(stream->info.format, (BYTE *)d->data + n, need_bytes - n);
                stream->underrun_count++;
            }
            stream->pa_offs_bytes = (stream->pa_offs_bytes + n) % stream->real_bufsize_bytes;
            __atomic_sub_fetch(&stream->pa_held_bytes, n, __ATOMIC_RELEASE);
        }
        else
            silence_buffer(stream->info.format, d->data, need_bytes);

        d->chunk->offset = 0;
        d->chunk->stride = stream->frame_size;
        d->chunk->size = need_bytes;
        b->size = req_frames;
    }
    else /* eCapture */
    {
        if (stream->started && stream->capture_ring)
        {
            UINT32 offs = min(d->chunk->offset, d->maxsize);
            UINT32 avail = min(d->chunk->size, d->maxsize - offs);
            const BYTE *src = (const BYTE *)d->data + offs;
            SIZE_T n = avail, cap_held;

            if (n > stream->capture_ring_size)
            {
                src += n - stream->capture_ring_size;
                n = stream->capture_ring_size;
            }
            cap_held = __atomic_load_n(&stream->cap_held_bytes, __ATOMIC_ACQUIRE);
            if (cap_held + n > stream->capture_ring_size)
            {
                SIZE_T drop = cap_held + n - stream->capture_ring_size;
                stream->cap_read_offs = (stream->cap_read_offs + drop) % stream->capture_ring_size;
                __atomic_sub_fetch(&stream->cap_held_bytes, drop, __ATOMIC_RELEASE);
                stream->overrun_count++;
            }
            if (n)
            {
                SIZE_T woff = (stream->cap_read_offs + stream->cap_held_bytes) % stream->capture_ring_size;
                SIZE_T first = min(n, stream->capture_ring_size - woff);
                memcpy(stream->capture_ring + woff, src, first);
                if (n > first)
                    memcpy(stream->capture_ring, src + first, n - first);
                __atomic_add_fetch(&stream->cap_held_bytes, n, __ATOMIC_RELEASE);
            }
        }
    }

    pw_stream_queue_buffer(stream->pw, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .process = on_stream_process,
};

/* ----------------------------------------------------------------------
 * Stream creation / release
 * ---------------------------------------------------------------------- */

static BOOL stream_valid(struct pipewire_stream *stream)
{
    enum pw_stream_state st;
    if (!stream || !stream->pw)
        return FALSE;
    st = pw_stream_get_state(stream->pw, NULL);
    return st == PW_STREAM_STATE_PAUSED || st == PW_STREAM_STATE_STREAMING;
}

static BOOL device_is_sink(const char *device)
{
    struct pw_phys_device *dev;
    LIST_FOR_EACH_ENTRY(dev, &g_render_devices, struct pw_phys_device, entry)
        if (!strcmp(device, dev->pw_name))
            return TRUE;
    return FALSE;
}

/* Called with the loop lock held. */
static HRESULT pipewire_stream_connect(struct pipewire_stream *stream, const char *device,
                                       const WCHAR *appname)
{
    struct pw_properties *props;
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    uint32_t period_frames = stream->period_bytes / stream->frame_size;
    enum pw_stream_state st;
    char *app;
    int tries;

    props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                              PW_KEY_MEDIA_CATEGORY,
                              stream->dataflow == eRender ? "Playback" : "Capture",
                              NULL);
    if (!props)
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;

    app = app_name_from_wstr(appname);
    if (app)
    {
        pw_properties_set(props, PW_KEY_APP_NAME, app);
        pw_properties_set(props, PW_KEY_NODE_NAME, app);
        pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, app);
    }
    else
        pw_properties_set(props, PW_KEY_NODE_NAME, "winepipewire");
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", period_frames, stream->info.rate);
    if (device && device[0])
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, device);
    if (stream->dataflow == eCapture &&
        ((stream->flags & AUDCLNT_STREAMFLAGS_LOOPBACK) ||
         (device && device[0] && device_is_sink(device))))
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");

    stream->pw = pw_stream_new(pw_core_global, app ? app : "winepipewire", props);
    free(app);
    if (!stream->pw)
    {
        WARN("pw_stream_new failed.\n");
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    }

    pw_stream_add_listener(stream->pw, &stream->stream_listener, &stream_events, stream);

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &stream->info);
    if (!params[0])
    {
        WARN("spa_format_audio_raw_build overflowed for stream %p.\n", stream);
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    }

    if (pw_stream_connect(stream->pw,
                          stream->dataflow == eRender ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
                          PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_INACTIVE,
                          params, 1) < 0)
    {
        WARN("pw_stream_connect failed.\n");
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    }

    for (tries = 0; tries < 10; tries++)
    {
        st = pw_stream_get_state(stream->pw, NULL);
        if (st == PW_STREAM_STATE_PAUSED || st == PW_STREAM_STATE_STREAMING)
            return S_OK;
        if (st == PW_STREAM_STATE_ERROR || st == PW_STREAM_STATE_UNCONNECTED)
        {
            const char *error = NULL;
            pw_stream_get_state(stream->pw, &error);
            WARN("stream %p connect failed: %s.\n", stream, debugstr_a(error));
            return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
        }
        if (pw_thread_loop_timed_wait(pw_loop_global, 1) != 0)
            break;
    }
    st = pw_stream_get_state(stream->pw, NULL);
    if (st == PW_STREAM_STATE_PAUSED || st == PW_STREAM_STATE_STREAMING)
        return S_OK;
    {
        const char *error = NULL;
        st = pw_stream_get_state(stream->pw, &error);
        WARN("stream %p connect timed out in state %d: %s.\n", stream, st, debugstr_a(error));
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    }
}

static NTSTATUS pipewire_create_stream(void *args)
{
    struct create_stream_params *params = args;
    struct pipewire_stream *stream;
    SIZE_T bufsize_bytes, size;
    UINT32 i;
    HRESULT hr;

    TRACE("flow %d share %#x flags %#x period %lld dur %lld fmt %uch/%uHz/%ubit dev %s name %s.\n",
          params->flow, params->share, params->flags, (long long)params->period,
          (long long)params->duration, params->fmt->nChannels, params->fmt->nSamplesPerSec,
          params->fmt->wBitsPerSample, debugstr_a(params->device), debugstr_w(params->name));
    if (!pw_loop_global)
    {
        WARN("PipeWire main loop not running.\n");
        params->result = AUDCLNT_E_ENDPOINT_CREATE_FAILED;
        return STATUS_SUCCESS;
    }

    if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
    {
        TRACE("Exclusive mode not supported.\n");
        params->result = AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
        return STATUS_SUCCESS;
    }

    pw_thread_loop_lock(pw_loop_global);

    if (FAILED(hr = pipewire_connect(params->name)))
    {
        params->result = hr;
        pw_thread_loop_unlock(pw_loop_global);
        return STATUS_SUCCESS;
    }

    if (!(stream = calloc(1, sizeof(*stream))))
    {
        params->result = E_OUTOFMEMORY;
        pw_thread_loop_unlock(pw_loop_global);
        return STATUS_SUCCESS;
    }

    stream->dataflow = params->flow;
    for (i = 0; i < ARRAY_SIZE(stream->vol); ++i)
        stream->vol[i] = 1.f;

    hr = pipewire_info_from_waveformat(stream, params->fmt);
    TRACE("Obtaining format returns %08x\n", (unsigned)hr);
    if (FAILED(hr))
        goto exit;

    stream->frame_size = spa_format_bytes(stream->info.format) * stream->info.channels;
    if (!stream->frame_size)
    {
        hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        goto exit;
    }

    stream->def_period = params->period;
    stream->duration = params->duration;
    stream->flags = params->flags;
    stream->share = params->share;
    stream->mmdev_period_usec = params->period / 10;

    if (!(stream->device = strdup(params->device ? params->device : "")))
    {
        hr = E_OUTOFMEMORY;
        goto exit;
    }

    stream->period_bytes = stream->frame_size * muldiv(params->period, stream->info.rate, 10000000);
    if (stream->period_bytes == 0)
    {
        WARN("Invalid period: %lld hns at %u Hz.\n", (long long)params->period, stream->info.rate);
        hr = E_INVALIDARG;
        goto exit;
    }

    stream->bufsize_frames =
        (SIZE_T)(((UINT64)params->duration * stream->info.rate + 9999999) / 10000000);
    bufsize_bytes = stream->bufsize_frames * stream->frame_size;

    hr = pipewire_stream_connect(stream, params->device, params->name);
    if (FAILED(hr))
        goto exit;

    stream->rate_connected = stream->info.rate;

    list_init(&stream->packet_free_head);
    list_init(&stream->packet_filled_head);
    if (stream->dataflow == eRender)
    {
        size = stream->real_bufsize_bytes = stream->bufsize_frames * 2 * stream->frame_size;
        if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer,
                                    zero_bits, &size, MEM_COMMIT, PAGE_READWRITE))
            hr = E_OUTOFMEMORY;
    }
    else
    {
        UINT32 capture_packets, unalign;

        if ((unalign = bufsize_bytes % stream->period_bytes))
            bufsize_bytes += stream->period_bytes - unalign;
        stream->bufsize_frames = bufsize_bytes / stream->frame_size;
        stream->real_bufsize_bytes = bufsize_bytes;
        capture_packets = stream->real_bufsize_bytes / stream->period_bytes;

        size = stream->real_bufsize_bytes + capture_packets * sizeof(ACPacket);
        if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer,
                                    zero_bits, &size, MEM_COMMIT, PAGE_READWRITE))
            hr = E_OUTOFMEMORY;
        else
        {
            ACPacket *cur_packet = (ACPacket *)((char *)stream->local_buffer + stream->real_bufsize_bytes);
            BYTE *data = stream->local_buffer;
            silence_buffer(stream->info.format, stream->local_buffer, stream->real_bufsize_bytes);
            for (i = 0; i < capture_packets; ++i, ++cur_packet)
            {
                list_add_tail(&stream->packet_free_head, &cur_packet->entry);
                cur_packet->data = data;
                data += stream->period_bytes;
            }

            /* staging ring fed by the process callback */
            size = stream->capture_ring_size = stream->real_bufsize_bytes;
            if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->capture_ring,
                                        zero_bits, &size, MEM_COMMIT, PAGE_READWRITE))
                hr = E_OUTOFMEMORY;
        }
    }

exit:
    if (FAILED(params->result = hr))
    {
        if (stream->pw)
        {
            pw_stream_destroy(stream->pw);
            stream->pw = NULL;
        }
        if (stream->local_buffer)
        {
            size = 0;
            NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, &size, MEM_RELEASE);
        }
        if (stream->capture_ring)
        {
            size = 0;
            NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->capture_ring, &size, MEM_RELEASE);
        }
        free(stream->device);
        free(stream);
    }

    if (SUCCEEDED(hr))
    {
        *params->channel_count = stream->info.channels;
        *params->stream = (stream_handle)(UINT_PTR)stream;
        TRACE("created stream %p, %u channels.\n", stream, stream->info.channels);
    }
    else
        WARN("failed: %#x.\n", (unsigned)hr);
    pw_thread_loop_unlock(pw_loop_global);
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_release_stream(void *args)
{
    struct release_stream_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);
    struct pipewire_period *dead_period = NULL;
    SIZE_T size;

    pw_thread_loop_lock(pw_loop_global);
    TRACE("stream %p.\n", stream);
    if (stream->underrun_count || stream->overrun_count)
        WARN("stream %p underran %u times, overran %u times.\n",
             stream, stream->underrun_count, stream->overrun_count);
    if (stream->period)
    {
        struct pipewire_period *period = stream->period;

        if (period->timer_stream == stream)
        {
            period->timer_stream = NULL;
            period->grid_valid = FALSE;
        }
        list_remove(&stream->period_entry);
        stream->period = NULL;
        if (list_empty(&period->streams))
        {
            list_remove(&period->entry);
            period->please_quit = TRUE;
            dead_period = period;
        }
    }
    if (stream->pw)
    {
        pw_stream_destroy(stream->pw);
        stream->pw = NULL;
    }
    pw_thread_loop_unlock(pw_loop_global);

    if (dead_period)
    {
        NtWaitForSingleObject(dead_period->timer_thread, FALSE, NULL);
        NtClose(dead_period->timer_thread);
        free(dead_period->device);
        free(dead_period);
    }

    if (stream->tmp_buffer)
    {
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE);
    }
    if (stream->local_buffer)
    {
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, &size, MEM_RELEASE);
    }
    if (stream->capture_ring)
    {
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->capture_ring, &size, MEM_RELEASE);
    }
    free(stream->device);
    free(stream);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* ----------------------------------------------------------------------
 * Capture slicing + period group timer (8628-style shared timer; one Wine
 * thread per (device, period) group)
 * ---------------------------------------------------------------------- */

/* Slice period-sized packets out of the staging ring.  Runs on the Wine
 * timer thread with the loop lock held: ntdll (QPC) is legal here. */
static void pipewire_read(struct pipewire_stream *stream)
{
    while (__atomic_load_n(&stream->cap_held_bytes, __ATOMIC_ACQUIRE) >= stream->period_bytes)
    {
        ACPacket *p, *next;
        LARGE_INTEGER stamp, freq;

        if (!(p = (ACPacket *)list_head(&stream->packet_free_head)))
        {
            p = (ACPacket *)list_head(&stream->packet_filled_head);
            if (!p) return;
            if (!p->discont)
            {
                next = (ACPacket *)p->entry.next;
                next->discont = 1;
            }
            else
                p = (ACPacket *)list_tail(&stream->packet_filled_head);
        }
        else
        {
            stream->held_bytes += stream->period_bytes;
        }
        NtQueryPerformanceCounter(&stamp, &freq);
        p->qpcpos = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
        p->discont = 0;
        list_remove(&p->entry);
        list_add_tail(&stream->packet_filled_head, &p->entry);

        copy_from_ring(p->data, stream->capture_ring, stream->capture_ring_size,
                       stream->cap_read_offs, stream->period_bytes);
        stream->cap_read_offs = (stream->cap_read_offs + stream->period_bytes) % stream->capture_ring_size;
        __atomic_sub_fetch(&stream->cap_held_bytes, stream->period_bytes, __ATOMIC_RELEASE);
    }
}

static void pipewire_period_timer_loop(void *args)
{
    struct pipewire_period *period = args;
    struct pipewire_stream *stream;
    LARGE_INTEGER delay;
    struct pw_time pwt;
    UINT64 now = 0;

    delay.QuadPart = -(INT64)period->period_usec * 10;

    while (!period->please_quit)
    {
        int have_now = 0;

        NtDelayExecution(FALSE, &delay);

        pw_thread_loop_lock(pw_loop_global);
        delay.QuadPart = -(INT64)period->period_usec * 10;

        if (period->timer_stream && !period->timer_stream->started)
        {
            period->timer_stream = NULL;
            period->grid_valid = FALSE;
        }
        if (!period->timer_stream)
        {
            LIST_FOR_EACH_ENTRY(stream, &period->streams, struct pipewire_stream, period_entry)
            {
                if (stream->started)
                {
                    period->timer_stream = stream;
                    period->grid_valid = FALSE;
                    break;
                }
            }
        }

        if (period->timer_stream && period->timer_stream->pw &&
            pw_stream_get_time_n(period->timer_stream->pw, &pwt, sizeof(pwt)) == 0 &&
            pwt.now && pwt.rate.denom)
        {
            struct timespec ts;
            UINT64 mono_ns;

            clock_gettime(CLOCK_MONOTONIC, &ts);
            mono_ns = (UINT64)ts.tv_sec * 1000000000 + ts.tv_nsec;

            /* pwt.now and pwt.ticks only advance once per graph cycle, so
             * comparing the continuous period grid against them directly
             * makes the grid chase quantum-sized steps with clamp-sized
             * corrections every tick (the wakeup cadence smears across
             * period +/- period/2 whenever the quantum does not divide the
             * period).  Extrapolate the graph clock to the sampling instant
             * instead, like winepulse's PA_STREAM_INTERPOLATE_TIMING, and
             * treat a graph that stopped updating as having no clock so the
             * grid free-runs at the nominal period and re-acquires on
             * resume. */
            if (mono_ns >= (UINT64)pwt.now && mono_ns - pwt.now < 1000000000)
            {
                now = pwt.ticks * (UINT64)pwt.rate.num * 1000000 / pwt.rate.denom
                      + (mono_ns - pwt.now) / 1000;
                have_now = 1;
            }
        }

        if (!have_now)
            period->grid_valid = FALSE;
        else if (!period->grid_valid)
        {
            /* (re)acquire the grid; start draining next tick, mirroring the
             * one-period start absorb of the old per-stream loop */
            period->last_time = now;
            period->grid_valid = TRUE;
        }
        else
        {
            INT64 adjust = (INT64)(period->last_time + period->period_usec) - (INT64)now;

            if (adjust > 1000000 || adjust < -1000000)
            {
                /* graph clock stalled or jumped: re-acquire the grid next tick */
                period->grid_valid = FALSE;
            }
            else
            {
                if (adjust > (INT64)(period->period_usec / 2))
                    adjust = period->period_usec / 2;
                else if (adjust < -(INT64)(period->period_usec / 2))
                    adjust = -(INT64)(period->period_usec / 2);

                delay.QuadPart = -((INT64)period->period_usec + adjust) * 10;
                period->last_time += period->period_usec;

                LIST_FOR_EACH_ENTRY(stream, &period->streams, struct pipewire_stream, period_entry)
                {
                    if (!stream->started)
                        continue;
                    if (stream->dataflow == eRender)
                    {
                        UINT32 adv = min(stream->period_bytes, stream->held_bytes);
                        stream->lcl_offs_bytes += adv;
                        stream->lcl_offs_bytes %= stream->real_bufsize_bytes;
                        stream->held_bytes -= adv;
                    }
                    else
                        pipewire_read(stream);
                }
            }
        }

        LIST_FOR_EACH_ENTRY(stream, &period->streams, struct pipewire_stream, period_entry)
            if (stream->event)
                NtSetEvent(stream->event, NULL);

        pw_thread_loop_unlock(pw_loop_global);
    }
    PsTerminateSystemThread(0);
}

/* Called with the loop lock held. */
static HRESULT pipewire_add_stream_to_period(struct pipewire_stream *stream)
{
    static const WCHAR name[] = {'w','i','n','e','p','i','p','e','w','i','r','e','_','t','i','m','e','r',0};
    struct pipewire_period *period;

    if (stream->period)
        return S_OK;

    LIST_FOR_EACH_ENTRY(period, &active_periods, struct pipewire_period, entry)
    {
        if (period->period_usec == stream->mmdev_period_usec && !strcmp(period->device, stream->device))
        {
            stream->period = period;
            list_add_tail(&period->streams, &stream->period_entry);
            TRACE("stream %p joins period %p (%llu us, dev %s).\n", stream, period,
                  (unsigned long long)period->period_usec, debugstr_a(period->device));
            return S_OK;
        }
    }

    if (!(period = calloc(1, sizeof(*period))))
        return E_OUTOFMEMORY;
    if (!(period->device = strdup(stream->device)))
    {
        free(period);
        return E_OUTOFMEMORY;
    }
    period->period_usec = stream->mmdev_period_usec;
    list_init(&period->streams);
    stream->period = period;
    list_add_tail(&period->streams, &stream->period_entry);
    list_add_tail(&active_periods, &period->entry);

    if (create_unix_thread(&period->timer_thread, name, pipewire_period_timer_loop, period))
    {
        ERR("Failed to create timer thread for period %llu us.\n",
            (unsigned long long)stream->mmdev_period_usec);
        list_remove(&stream->period_entry);
        list_remove(&period->entry);
        stream->period = NULL;
        free(period->device);
        free(period);
        return E_FAIL;
    }
    TRACE("stream %p created period %p (%llu us, dev %s).\n", stream, period,
          (unsigned long long)period->period_usec, debugstr_a(period->device));
    return S_OK;
}

static NTSTATUS pipewire_start(void *args)
{
    struct start_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);

    TRACE("stream %p.\n", stream);
    params->result = S_OK;
    pw_thread_loop_lock(pw_loop_global);
    if (!stream_valid(stream))
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    if ((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_EVENTHANDLE_NOT_SET;
        return STATUS_SUCCESS;
    }

    if (stream->started)
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_NOT_STOPPED;
        return STATUS_SUCCESS;
    }

    if (FAILED(params->result = pipewire_add_stream_to_period(stream)))
    {
        pw_thread_loop_unlock(pw_loop_global);
        return STATUS_SUCCESS;
    }

    if (pw_stream_set_active(stream->pw, true) < 0)
    {
        /* mirrors pulse_start's failed-uncork path */
        WARN("pw_stream_set_active failed for stream %p.\n", stream);
        params->result = E_FAIL;
        pw_thread_loop_unlock(pw_loop_global);
        return STATUS_SUCCESS;
    }
    stream->started = TRUE;
    pw_thread_loop_unlock(pw_loop_global);
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_stop(void *args)
{
    struct stop_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);

    TRACE("stream %p.\n", stream);
    pw_thread_loop_lock(pw_loop_global);
    if (!stream_valid(stream))
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    if (!stream->started)
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = S_FALSE;
        return STATUS_SUCCESS;
    }

    pw_stream_set_active(stream->pw, false);
    stream->started = FALSE;
    pw_thread_loop_unlock(pw_loop_global);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_reset(void *args)
{
    struct reset_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);

    TRACE("stream %p.\n", stream);
    pw_thread_loop_lock(pw_loop_global);
    if (!stream_valid(stream))
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    if (stream->started)
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_NOT_STOPPED;
        return STATUS_SUCCESS;
    }

    if (stream->locked)
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_BUFFER_OPERATION_PENDING;
        return STATUS_SUCCESS;
    }

    pw_stream_flush(stream->pw, false);

    if (stream->dataflow == eRender)
    {
        stream->clock_lastpos = stream->clock_written = 0;
        stream->pa_offs_bytes = stream->lcl_offs_bytes = 0;
        stream->held_bytes = 0;
        __atomic_store_n(&stream->pa_held_bytes, 0, __ATOMIC_RELEASE);
    }
    else
    {
        ACPacket *p;
        stream->clock_written += stream->held_bytes;
        stream->held_bytes = 0;
        stream->cap_read_offs = 0;
        __atomic_store_n(&stream->cap_held_bytes, 0, __ATOMIC_RELEASE);

        if ((p = stream->locked_ptr))
        {
            stream->locked_ptr = NULL;
            list_add_tail(&stream->packet_free_head, &p->entry);
        }
        list_move_tail(&stream->packet_free_head, &stream->packet_filled_head);
    }
    pw_thread_loop_unlock(pw_loop_global);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* ----------------------------------------------------------------------
 * Render / capture buffer transfer
 * ---------------------------------------------------------------------- */

static BOOL alloc_tmp_buffer(struct pipewire_stream *stream, SIZE_T bytes)
{
    SIZE_T size;

    if (stream->tmp_buffer_bytes >= bytes)
        return TRUE;

    if (stream->tmp_buffer)
    {
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE);
        stream->tmp_buffer = NULL;
        stream->tmp_buffer_bytes = 0;
    }
    if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer,
                                zero_bits, &bytes, MEM_COMMIT, PAGE_READWRITE))
        return FALSE;

    stream->tmp_buffer_bytes = bytes;
    return TRUE;
}

static UINT32 pipewire_render_padding(struct pipewire_stream *stream)
{
    return stream->held_bytes / stream->frame_size;
}

static UINT32 pipewire_capture_padding(struct pipewire_stream *stream)
{
    ACPacket *packet = stream->locked_ptr;
    if (!packet && !list_empty(&stream->packet_filled_head))
    {
        packet = (ACPacket *)list_head(&stream->packet_filled_head);
        stream->locked_ptr = packet;
        list_remove(&packet->entry);
    }
    return stream->held_bytes / stream->frame_size;
}

static NTSTATUS pipewire_get_render_buffer(void *args)
{
    struct get_render_buffer_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);
    size_t bytes;
    UINT32 wri_offs_bytes;

    pw_thread_loop_lock(pw_loop_global);
    if (!stream_valid(stream))
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    if (stream->locked)
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        return STATUS_SUCCESS;
    }

    if (!params->frames)
    {
        pw_thread_loop_unlock(pw_loop_global);
        *params->data = NULL;
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    if (stream->held_bytes / stream->frame_size + params->frames > stream->bufsize_frames)
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_BUFFER_TOO_LARGE;
        return STATUS_SUCCESS;
    }

    bytes = params->frames * stream->frame_size;
    wri_offs_bytes = (stream->lcl_offs_bytes + stream->held_bytes) % stream->real_bufsize_bytes;
    if (wri_offs_bytes + bytes > stream->real_bufsize_bytes)
    {
        if (!alloc_tmp_buffer(stream, bytes))
        {
            pw_thread_loop_unlock(pw_loop_global);
            params->result = E_OUTOFMEMORY;
            return STATUS_SUCCESS;
        }
        *params->data = stream->tmp_buffer;
        stream->locked = -bytes;
    }
    else
    {
        *params->data = stream->local_buffer + wri_offs_bytes;
        stream->locked = bytes;
    }

    silence_buffer(stream->info.format, *params->data, bytes);

    pw_thread_loop_unlock(pw_loop_global);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static void pipewire_wrap_buffer(struct pipewire_stream *stream, BYTE *buffer, UINT32 written_bytes)
{
    UINT32 wri_offs_bytes = (stream->lcl_offs_bytes + stream->held_bytes) % stream->real_bufsize_bytes;
    UINT32 chunk_bytes = stream->real_bufsize_bytes - wri_offs_bytes;

    if (written_bytes <= chunk_bytes)
    {
        memcpy(stream->local_buffer + wri_offs_bytes, buffer, written_bytes);
    }
    else
    {
        memcpy(stream->local_buffer + wri_offs_bytes, buffer, chunk_bytes);
        memcpy(stream->local_buffer, buffer + chunk_bytes, written_bytes - chunk_bytes);
    }
}

static NTSTATUS pipewire_release_render_buffer(void *args)
{
    struct release_render_buffer_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);
    UINT32 written_bytes;
    BYTE *buffer;

    pw_thread_loop_lock(pw_loop_global);
    if (!stream->locked || !params->written_frames)
    {
        stream->locked = 0;
        pw_thread_loop_unlock(pw_loop_global);
        params->result = params->written_frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
        return STATUS_SUCCESS;
    }

    if (params->written_frames * stream->frame_size >
        (stream->locked >= 0 ? stream->locked : -stream->locked))
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_INVALID_SIZE;
        return STATUS_SUCCESS;
    }

    if (stream->locked >= 0)
        buffer = stream->local_buffer + (stream->lcl_offs_bytes + stream->held_bytes) % stream->real_bufsize_bytes;
    else
        buffer = stream->tmp_buffer;

    written_bytes = params->written_frames * stream->frame_size;
    if (params->flags & AUDCLNT_BUFFERFLAGS_SILENT)
        silence_buffer(stream->info.format, buffer, written_bytes);

    if (stream->locked < 0)
        pipewire_wrap_buffer(stream, buffer, written_bytes);

    stream->held_bytes += written_bytes;
    if (__atomic_add_fetch(&stream->pa_held_bytes, written_bytes, __ATOMIC_RELEASE) > stream->real_bufsize_bytes)
    {
        WARN("%p PipeWire buffer overflow.\n", stream);
        stream->pa_offs_bytes = stream->lcl_offs_bytes;
        __atomic_store_n(&stream->pa_held_bytes, stream->held_bytes, __ATOMIC_RELEASE);
    }
    stream->clock_written += written_bytes;
    stream->locked = 0;

    pw_thread_loop_unlock(pw_loop_global);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_get_capture_buffer(void *args)
{
    struct get_capture_buffer_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);
    ACPacket *packet;

    pw_thread_loop_lock(pw_loop_global);
    if (!stream_valid(stream))
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }
    if (stream->locked)
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        return STATUS_SUCCESS;
    }

    pipewire_capture_padding(stream);
    if ((packet = stream->locked_ptr))
    {
        *params->frames = stream->period_bytes / stream->frame_size;
        *params->flags = 0;
        if (packet->discont)
            *params->flags |= AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
        if (params->devpos)
        {
            if (packet->discont)
                *params->devpos = (stream->clock_written + stream->period_bytes) / stream->frame_size;
            else
                *params->devpos = stream->clock_written / stream->frame_size;
        }
        if (params->qpcpos)
            *params->qpcpos = packet->qpcpos;
        *params->data = packet->data;
    }
    else
        *params->frames = 0;
    stream->locked = *params->frames;
    pw_thread_loop_unlock(pw_loop_global);
    params->result = *params->frames ? S_OK : AUDCLNT_S_BUFFER_EMPTY;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_release_capture_buffer(void *args)
{
    struct release_capture_buffer_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);

    pw_thread_loop_lock(pw_loop_global);
    if (!stream->locked && params->done)
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        return STATUS_SUCCESS;
    }
    if (params->done && stream->locked != params->done)
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_INVALID_SIZE;
        return STATUS_SUCCESS;
    }
    if (params->done)
    {
        ACPacket *packet = stream->locked_ptr;
        stream->locked_ptr = NULL;
        stream->held_bytes -= stream->period_bytes;
        if (packet->discont)
            stream->clock_written += 2 * stream->period_bytes;
        else
            stream->clock_written += stream->period_bytes;
        list_add_tail(&stream->packet_free_head, &packet->entry);
    }
    stream->locked = 0;
    pw_thread_loop_unlock(pw_loop_global);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* ----------------------------------------------------------------------
 * Clocks / sizes / volumes / events / rate
 * ---------------------------------------------------------------------- */

static NTSTATUS pipewire_get_current_padding(void *args)
{
    struct get_current_padding_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);

    pw_thread_loop_lock(pw_loop_global);
    if (!stream_valid(stream))
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    if (stream->dataflow == eRender)
        *params->padding = pipewire_render_padding(stream);
    else
        *params->padding = pipewire_capture_padding(stream);
    pw_thread_loop_unlock(pw_loop_global);

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_get_buffer_size(void *args)
{
    struct get_buffer_size_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);

    params->result = S_OK;
    pw_thread_loop_lock(pw_loop_global);
    if (!stream_valid(stream))
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else
        *params->frames = stream->bufsize_frames;
    pw_thread_loop_unlock(pw_loop_global);
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_get_latency(void *args)
{
    struct get_latency_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);
    REFERENCE_TIME lat;

    pw_thread_loop_lock(pw_loop_global);
    if (!stream_valid(stream))
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }
    lat = stream->period_bytes / stream->frame_size;
    *params->latency = (lat * 10000000) / stream->info.rate + stream->def_period;
    TRACE("stream %p latency %u ms.\n", stream, (unsigned)(*params->latency / 10000));
    pw_thread_loop_unlock(pw_loop_global);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_get_next_packet_size(void *args)
{
    struct get_next_packet_size_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);

    pw_thread_loop_lock(pw_loop_global);
    pipewire_capture_padding(stream);
    if (stream->locked_ptr)
        *params->frames = stream->period_bytes / stream->frame_size;
    else
        *params->frames = 0;
    pw_thread_loop_unlock(pw_loop_global);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_get_frequency(void *args)
{
    struct get_frequency_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);

    pw_thread_loop_lock(pw_loop_global);
    if (!stream_valid(stream))
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    *params->freq = stream->info.rate;
    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->freq *= stream->frame_size;
    pw_thread_loop_unlock(pw_loop_global);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_get_position(void *args)
{
    struct get_position_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);

    pw_thread_loop_lock(pw_loop_global);
    if (!stream_valid(stream))
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    *params->pos = stream->clock_written - stream->held_bytes;

    if (stream->share == AUDCLNT_SHAREMODE_EXCLUSIVE || params->device)
        *params->pos /= stream->frame_size;

    if (*params->pos < stream->clock_lastpos)
        *params->pos = stream->clock_lastpos;
    else
        stream->clock_lastpos = *params->pos;
    pw_thread_loop_unlock(pw_loop_global);

    if (params->qpctime)
    {
        LARGE_INTEGER stamp, freq;
        NtQueryPerformanceCounter(&stamp, &freq);
        *params->qpctime = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_set_volumes(void *args)
{
    struct set_volumes_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);
    unsigned int i;

    pw_thread_loop_lock(pw_loop_global);
    if (stream_valid(stream))
        for (i = 0; i < stream->info.channels; i++)
            stream->vol[i] = params->volumes[i] * params->master_volume *
                             params->session_volumes[i];
    pw_thread_loop_unlock(pw_loop_global);
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_set_event_handle(void *args)
{
    struct set_event_handle_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);
    HRESULT hr = S_OK;

    TRACE("stream %p event %p.\n", stream, params->event);
    pw_thread_loop_lock(pw_loop_global);
    if (!stream_valid(stream))
        hr = AUDCLNT_E_DEVICE_INVALIDATED;
    else if (!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        hr = AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
    else if (stream->event)
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    else
        stream->event = params->event;
    pw_thread_loop_unlock(pw_loop_global);

    params->result = hr;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_set_sample_rate(void *args)
{
    struct set_sample_rate_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);
    HRESULT hr = S_OK;
    float ratio;

    TRACE("stream %p rate %u.\n", stream, (unsigned)params->rate);
    pw_thread_loop_lock(pw_loop_global);
    if (!stream_valid(stream))
    {
        hr = AUDCLNT_E_DEVICE_INVALIDATED;
        goto exit;
    }
    if (stream->dataflow != eRender)
    {
        hr = E_NOTIMPL;
        goto exit;
    }

    /* PulseAudio rejects rates outside [1, 48000 * 8] in
     * pa_stream_update_sample_rate; mirror winepulse's observable failure
     * code for rates the resampler cannot honor (the negated comparison
     * also catches NaN). */
    if (!(params->rate >= 1.0f && params->rate <= 384000.0f))
    {
        WARN("Unsupported sample rate %u.\n", (unsigned)params->rate);
        hr = E_OUTOFMEMORY;
        goto exit;
    }

    ratio = params->rate / (float)stream->rate_connected;
    if (pw_stream_set_control(stream->pw, SPA_PROP_rate, 1, &ratio, 0) < 0)
    {
        /* needs PipeWire >= 1.2.6 and an active adaptive resampler */
        WARN("pw_stream_set_control(rate) failed for stream %p.\n", stream);
        hr = E_NOTIMPL;
        goto exit;
    }

    pw_stream_flush(stream->pw, false);

    stream->clock_lastpos = stream->clock_written = 0;
    stream->pa_offs_bytes = stream->lcl_offs_bytes = 0;
    stream->held_bytes = 0;
    __atomic_store_n(&stream->pa_held_bytes, 0, __ATOMIC_RELEASE);
    stream->period_bytes =
        stream->frame_size * muldiv(stream->mmdev_period_usec, params->rate, 1000000);
    stream->info.rate = params->rate;

    silence_buffer(stream->info.format, stream->local_buffer, stream->real_bufsize_bytes);

exit:
    pw_thread_loop_unlock(pw_loop_global);
    params->result = hr;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_is_started(void *args)
{
    struct is_started_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);

    pw_thread_loop_lock(pw_loop_global);
    params->result = stream_valid(stream) && stream->started ? S_OK : S_FALSE;
    pw_thread_loop_unlock(pw_loop_global);
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_get_loopback_capture_device(void *args)
{
    struct get_loopback_capture_device_params *params = args;
    const char *device = params->device;
    const char *sink;
    UINT32 needed;

    /* Resolve the render device string to a concrete sink node name; an
     * empty string means the session-manager default sink.  create_stream
     * then sets PW_KEY_STREAM_CAPTURE_SINK for that name. */
    if (device && device[0])
    {
        if (!device_is_sink(device))
        {
            WARN("loopback target %s is not a sink.\n", debugstr_a(device));
            params->result = E_FAIL;
            return STATUS_SUCCESS;
        }
        sink = device;
    }
    else
        sink = g_default_sink;

    needed = strlen(sink) + 1;
    if (params->ret_device_len < needed || !params->ret_device)
    {
        params->ret_device_len = needed;
        params->result = STATUS_BUFFER_TOO_SMALL;
        return STATUS_SUCCESS;
    }
    memcpy(params->ret_device, sink, needed);
    TRACE("loopback capture from sink %s.\n", debugstr_a(sink));
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* ----------------------------------------------------------------------
 * MIDI delegation + dispatch tables
 * ---------------------------------------------------------------------- */

static NTSTATUS pipewire_midi_get_driver(void *args)
{
    static const WCHAR driver[] = {'a','l','s','a',0};

    /* Delegate MIDI to winealsa, exactly as winepulse does. */
    memcpy(args, driver, sizeof(driver));
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_not_implemented(void *args)
{
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    pipewire_process_attach,
    pipewire_process_detach,
    pipewire_main_loop_start,
    pipewire_main_loop_stop,
    pipewire_get_endpoint_ids,
    pipewire_create_stream,
    pipewire_release_stream,
    pipewire_start,
    pipewire_stop,
    pipewire_reset,
    pipewire_get_render_buffer,
    pipewire_release_render_buffer,
    pipewire_get_capture_buffer,
    pipewire_release_capture_buffer,
    pipewire_is_format_supported,
    pipewire_get_loopback_capture_device,
    pipewire_get_mix_format,
    pipewire_get_device_period,
    pipewire_get_buffer_size,
    pipewire_get_latency,
    pipewire_get_current_padding,
    pipewire_get_next_packet_size,
    pipewire_get_frequency,
    pipewire_get_position,
    pipewire_set_volumes,
    pipewire_set_event_handle,
    pipewire_set_sample_rate,
    pipewire_test_connect,
    pipewire_is_started,
    pipewire_get_prop_value,
    pipewire_midi_get_driver,
    pipewire_not_implemented,
    pipewire_not_implemented,
    pipewire_not_implemented,
    pipewire_not_implemented,
    pipewire_not_implemented,
    pipewire_not_implemented,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == funcs_count);

#ifdef _WIN64

typedef UINT PTR32;

static NTSTATUS pipewire_wow64_process_attach(void *args)
{
    SYSTEM_BASIC_INFORMATION info;

    NtQuerySystemInformation(SystemEmulationBasicInformation, &info, sizeof(info), NULL);
    zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;

    return pipewire_process_attach( args );
}

static NTSTATUS pipewire_wow64_get_endpoint_ids(void *args)
{
    struct
    {
        EDataFlow flow;
        PTR32 endpoints;
        unsigned int size;
        HRESULT result;
        unsigned int num;
        unsigned int default_idx;
    } *params32 = args;
    struct get_endpoint_ids_params params =
    {
        .flow = params32->flow,
        .endpoints = ULongToPtr(params32->endpoints),
        .size = params32->size
    };
    pipewire_get_endpoint_ids(&params);
    params32->size = params.size;
    params32->result = params.result;
    params32->num = params.num;
    params32->default_idx = params.default_idx;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_create_stream(void *args)
{
    struct
    {
        PTR32 name;
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        DWORD flags;
        REFERENCE_TIME duration;
        REFERENCE_TIME period;
        PTR32 fmt;
        HRESULT result;
        PTR32 channel_count;
        PTR32 stream;
    } *params32 = args;
    struct create_stream_params params =
    {
        .name = ULongToPtr(params32->name),
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .flags = params32->flags,
        .duration = params32->duration,
        .period = params32->period,
        .fmt = ULongToPtr(params32->fmt),
        .channel_count = ULongToPtr(params32->channel_count),
        .stream = ULongToPtr(params32->stream)
    };
    pipewire_create_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_release_stream(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
    } *params32 = args;
    struct release_stream_params params =
    {
        .stream = params32->stream,
    };
    pipewire_release_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_get_render_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        UINT32 frames;
        HRESULT result;
        PTR32 data;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_render_buffer_params params =
    {
        .stream = params32->stream,
        .frames = params32->frames,
        .data = &data
    };
    pipewire_get_render_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_get_capture_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 data;
        PTR32 frames;
        PTR32 flags;
        PTR32 devpos;
        PTR32 qpcpos;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_capture_buffer_params params =
    {
        .stream = params32->stream,
        .data = &data,
        .frames = ULongToPtr(params32->frames),
        .flags = ULongToPtr(params32->flags),
        .devpos = ULongToPtr(params32->devpos),
        .qpcpos = ULongToPtr(params32->qpcpos)
    };
    pipewire_get_capture_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
};

static NTSTATUS pipewire_wow64_is_format_supported(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        PTR32 fmt_in;
        HRESULT result;
    } *params32 = args;
    struct is_format_supported_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .fmt_in = ULongToPtr(params32->fmt_in),
    };
    pipewire_is_format_supported(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_get_loopback_capture_device(void *args)
{
    struct
    {
        PTR32 name;
        PTR32 device;
        PTR32 ret_device;
        UINT32 ret_device_len;
        HRESULT result;
    } *params32 = args;

    struct get_loopback_capture_device_params params =
    {
        .name = ULongToPtr(params32->name),
        .device = ULongToPtr(params32->device),
        .ret_device = ULongToPtr(params32->ret_device),
        .ret_device_len = params32->ret_device_len,
    };

    pipewire_get_loopback_capture_device(&params);
    params32->result = params.result;
    params32->ret_device_len = params.ret_device_len;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_get_mix_format(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 fmt;
        HRESULT result;
    } *params32 = args;
    struct get_mix_format_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .fmt = ULongToPtr(params32->fmt),
    };
    pipewire_get_mix_format(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_get_device_period(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        HRESULT result;
        PTR32 def_period;
        PTR32 min_period;
    } *params32 = args;
    struct get_device_period_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .def_period = ULongToPtr(params32->def_period),
        .min_period = ULongToPtr(params32->min_period),
    };
    pipewire_get_device_period(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_get_buffer_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_buffer_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    pipewire_get_buffer_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_get_latency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 latency;
    } *params32 = args;
    struct get_latency_params params =
    {
        .stream = params32->stream,
        .latency = ULongToPtr(params32->latency)
    };
    pipewire_get_latency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_get_current_padding(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 padding;
    } *params32 = args;
    struct get_current_padding_params params =
    {
        .stream = params32->stream,
        .padding = ULongToPtr(params32->padding)
    };
    pipewire_get_current_padding(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_get_next_packet_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_next_packet_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    pipewire_get_next_packet_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_get_frequency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 freq;
    } *params32 = args;
    struct get_frequency_params params =
    {
        .stream = params32->stream,
        .freq = ULongToPtr(params32->freq)
    };
    pipewire_get_frequency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_get_position(void *args)
{
    struct
    {
        stream_handle stream;
        BOOL device;
        HRESULT result;
        PTR32 pos;
        PTR32 qpctime;
    } *params32 = args;
    struct get_position_params params =
    {
        .stream = params32->stream,
        .device = params32->device,
        .pos = ULongToPtr(params32->pos),
        .qpctime = ULongToPtr(params32->qpctime)
    };
    pipewire_get_position(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_set_volumes(void *args)
{
    struct
    {
        stream_handle stream;
        float master_volume;
        PTR32 volumes;
        PTR32 session_volumes;
    } *params32 = args;
    struct set_volumes_params params =
    {
        .stream = params32->stream,
        .master_volume = params32->master_volume,
        .volumes = ULongToPtr(params32->volumes),
        .session_volumes = ULongToPtr(params32->session_volumes),
    };
    return pipewire_set_volumes(&params);
}

static NTSTATUS pipewire_wow64_set_event_handle(void *args)
{
    struct
    {
        stream_handle stream;
        PTR32 event;
        HRESULT result;
    } *params32 = args;
    struct set_event_handle_params params =
    {
        .stream = params32->stream,
        .event = ULongToHandle(params32->event)
    };
    pipewire_set_event_handle(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_test_connect(void *args)
{
    struct
    {
        PTR32 name;
        enum driver_priority priority;
    } *params32 = args;
    struct test_connect_params params =
    {
        .name = ULongToPtr(params32->name),
    };
    pipewire_test_connect(&params);
    params32->priority = params.priority;
    return STATUS_SUCCESS;
}

static NTSTATUS pipewire_wow64_get_prop_value(void *args)
{
    struct propvariant32
    {
        WORD vt;
        WORD pad1, pad2, pad3;
        union
        {
            ULONG ulVal;
            PTR32 ptr;
            ULARGE_INTEGER uhVal;
        };
    } *value32;
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 guid;
        PTR32 prop;
        HRESULT result;
        PTR32 value;
        PTR32 buffer; /* caller allocated buffer to hold value's strings */
        PTR32 buffer_size;
    } *params32 = args;
    PROPVARIANT value;
    struct get_prop_value_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .guid = ULongToPtr(params32->guid),
        .prop = ULongToPtr(params32->prop),
        .value = &value,
        .buffer = ULongToPtr(params32->buffer),
        .buffer_size = ULongToPtr(params32->buffer_size)
    };
    pipewire_get_prop_value(&params);
    params32->result = params.result;
    if (SUCCEEDED(params.result))
    {
        value32 = UlongToPtr(params32->value);
        value32->vt = value.vt;
        switch (value.vt)
        {
        case VT_UI4:
            value32->ulVal = value.ulVal;
            break;
        case VT_LPWSTR:
            value32->ptr = params32->buffer;
            break;
        default:
            FIXME("Unhandled vt %04x\n", value.vt);
        }
    }
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    pipewire_wow64_process_attach,
    pipewire_process_detach,
    pipewire_main_loop_start,
    pipewire_main_loop_stop,
    pipewire_wow64_get_endpoint_ids,
    pipewire_wow64_create_stream,
    pipewire_wow64_release_stream,
    pipewire_start,
    pipewire_stop,
    pipewire_reset,
    pipewire_wow64_get_render_buffer,
    pipewire_release_render_buffer,
    pipewire_wow64_get_capture_buffer,
    pipewire_release_capture_buffer,
    pipewire_wow64_is_format_supported,
    pipewire_wow64_get_loopback_capture_device,
    pipewire_wow64_get_mix_format,
    pipewire_wow64_get_device_period,
    pipewire_wow64_get_buffer_size,
    pipewire_wow64_get_latency,
    pipewire_wow64_get_current_padding,
    pipewire_wow64_get_next_packet_size,
    pipewire_wow64_get_frequency,
    pipewire_wow64_get_position,
    pipewire_wow64_set_volumes,
    pipewire_wow64_set_event_handle,
    pipewire_set_sample_rate,
    pipewire_wow64_test_connect,
    pipewire_is_started,
    pipewire_wow64_get_prop_value,
    pipewire_midi_get_driver,
    pipewire_not_implemented,
    pipewire_not_implemented,
    pipewire_not_implemented,
    pipewire_not_implemented,
    pipewire_not_implemented,
    pipewire_not_implemented,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == funcs_count);

#endif /* _WIN64 */
