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

/* Requires libpipewire-0.3 >= 1.2.0: pw_stream_get_data_loop() and struct
 * pw_loop::name.  The floor is the minimum API this file consumes, not the
 * version the ship vehicle carries; configure.ac records what it excludes.
 * An older library disables the driver there rather than failing the build,
 * so do not paper one over with version conditionals here.
 *
 * pw_loop_locked() arrived in 1.6.0, above the 1.4.2 steamrt4 ships, so
 * stream_loop_locked() below reconstructs it.  Check any newer entry point
 * exists in the runtime, not merely in the headers: most are static inlines
 * dispatching through a SPA interface, and spa_api_method_r returns its
 * default when the method is missing, so the call compiles, links, runs and
 * silently does nothing.  The i386 unixlib compounds this, building against
 * the container's own PipeWire while resolving against the runtime's. */

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
#include <errno.h>
#include <sys/random.h>
#include <sys/resource.h>

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
#include "devpkey.h"

#include "wine/debug.h"
#include "wine/list.h"
#include "wine/unixlib.h"

#include "../mmdevapi/unixlib.h"

#include "mult.h"

WINE_DEFAULT_DEBUG_CHANNEL(pipewire);

#define PW_CHANNELS_MAX 64

/* Bounds for sample rates and daemon-supplied clock settings.  The rate
 * ceiling matches winepulse's effective limit (pa_sample_spec_valid rejects
 * anything above PA_RATE_MAX): the rate reaches nSamplesPerSec in the mix
 * format applications hand back to Initialize, where it scales every buffer
 * allocation and wraps nAvgBytesPerSec.  The quantum only feeds the
 * advertised IAudioClient3 minimum, which build_device_cache clamps again. */
#define PW_MIN_CLOCK_RATE 8000
#define PW_MAX_RATE       384000
#define PW_MAX_QUANTUM    65536

/* Constrains driver allocations to the 32-bit address space for WoW64
 * callers; left 0 for native 64-bit processes. */
static ULONG_PTR zero_bits = 0;

/* ----------------------------------------------------------------------
 * Stream and device structures (struct pulse_stream transplant)
 * ---------------------------------------------------------------------- */

/* Ownership token for one period-sized capture slot; see the capture ring
 * comment in struct pipewire_stream.
 *
 * State and publication sequence share ONE atomic word so every transition
 * compare-exchanges the exact value it observed.  On the state alone, a slot
 * evicted and republished between a scan and its claim would read FULL both
 * times and be delivered ahead of older slots; folding the sequence in makes
 * any recycle change the compared word, so the exchange fails and the caller
 * rescans.  30 bits of sequence wrap after about 128 days of continuous
 * capture at a 10 ms period, and an ABA needs that wrap inside one
 * scan-to-claim window. */
enum cap_slot_state
{
    CAP_FREE = 0,   /* nobody owns the bytes */
    CAP_FILLING,    /* producer owns them */
    CAP_FULL,       /* published, unclaimed */
    CAP_INUSE,      /* consumer owns them */
};

#define CAP_STATE_BITS 2
#define CAP_STATE_MASK 3u
#define CAP_STATE(w)   ((w) & CAP_STATE_MASK)
#define CAP_SEQ(w)     ((w) >> CAP_STATE_BITS)
#define CAP_SEQ_MAX    (~0u >> CAP_STATE_BITS)
#define CAP_WORD(s, st) ((((s) & CAP_SEQ_MAX) << CAP_STATE_BITS) | (st))

struct cap_slot
{
    UINT32 word;    /* CAP_WORD(sequence, state); only changed by exchange */
    BOOL disc;      /* audio was lost immediately before this slot; written
                     * by whoever owns the slot, so it needs no atomicity */
};

/* An aligned base only keeps every element aligned if the stride is a whole
 * number of alignments; pin that so a later field cannot break it silently. */
C_ASSERT(sizeof(struct cap_slot) % _Alignof(struct cap_slot) == 0);

/* Publication order of two slot words, wraparound safe.  Masking off the
 * state leaves the sequence already shifted, so a signed difference gives the
 * right answer across a 30-bit wrap. */
static BOOL cap_seq_before(UINT32 a, UINT32 b)
{
    return (INT32)((a & ~CAP_STATE_MASK) - (b & ~CAP_STATE_MASK)) < 0;
}

/* Per-stream state-transition journal.  pw_stream_state_changed fires on a
 * foreign PipeWire thread with no Wine TEB, so it only records here and Wine
 * threads drain and log later.  The ring is lock-free and drop-oldest:
 * overflow loses the oldest unread transitions rather than blocking. */
#define STATE_JOURNAL_LEN 8

struct stream_journal_entry
{
    UINT32 seq;         /* atomic: 0 = empty, else write index + 1 */
    int old_state;
    int new_state;
    char error[96];
};

struct pipewire_stream
{
    EDataFlow dataflow;

    struct pw_stream *pw;
    struct spa_hook stream_listener;
    struct spa_audio_info_raw info;
    UINT32 frame_size;
    UINT32 rate_connected; /* negotiated stream rate; SPA_PROP_rate is absolute vs this */
    char last_error[128]; /* set on ERROR callback; emitted once from Wine path */
    BOOL pending_error;

    DWORD flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;
    float vol[PW_CHANNELS_MAX];

    REFERENCE_TIME def_period;

    INT32 locked;
    BOOL started; /* atomic: control release-stores, process callback load-acquires */
    SIZE_T bufsize_frames, real_bufsize_bytes, period_bytes;
    /* Render ring bookkeeping.  lcl_offs/held are the application side,
     * pa_offs/pa_held the process-callback reader (names kept from the
     * pulse.c transplant).  pa_held_bytes crosses to the callback through
     * atomics; pa_offs_bytes is the callback's own cursor and control paths
     * reach it only through the data loop. */
    SIZE_T lcl_offs_bytes, pa_offs_bytes;
    SIZE_T tmp_buffer_bytes, held_bytes, pa_held_bytes;
    BYTE *local_buffer, *tmp_buffer;
    void *locked_ptr;
    UINT64 mmdev_period_usec;

    /* Capture staging ring, the analogue of PulseAudio's internal record
     * buffer: the process callback appends raw bytes, the Wine timer thread
     * slices period-sized ACPackets out with QPC timestamps.
     *
     * Ownership of a slot's BYTES follows ownership of its state word, which
     * only changes by compare-exchange (FREE, FILLING, FULL, INUSE), so
     * neither side can enter a slot without taking it out of the other's
     * reach.  That is also what keeps drop-oldest, which a plain SPSC ring
     * cannot: with nothing free the producer reclaims the oldest FULL slot,
     * an exchange that can only succeed if the consumer has not claimed it.
     * Dropping the newest would hand the application a stale backlog after a
     * stall.  Slots are published in fill order via seq, compared with
     * wraparound safe signed differences. */
    BYTE *capture_ring;
    SIZE_T capture_ring_size;
    struct cap_slot *cap_slots;
    UINT32 cap_n_slots;
    UINT32 cap_w_slot;    /* producer-private: slot being filled */
    SIZE_T cap_w_fill;    /* producer-private: bytes already in it */
    UINT32 cap_w_seq;     /* producer-private: next publication sequence */
    BOOL cap_filling;     /* producer-private: cap_w_slot is held FILLING */
    BOOL cap_lost;        /* producer-private: loss with no sequence gap,
                           * charged to the next slot published */
    UINT32 cap_next_seq;  /* consumer-private: sequence expected next */

    INT64 clock_lastpos, clock_written;
    /* atomic: process callback relaxed-increments, timer/control relaxed-load */
    UINT32 underrun_count, overrun_count, bad_buffer_count;
    UINT32 ring_warned;   /* RING_OP_* bits already reported for this stream */
    UINT32 cb_seq;        /* callback-private: callbacks entered */
    UINT32 cb_mark;       /* diagnostic breadcrumb, never read by the driver */
    BOOL underrun_logged, overrun_logged, bad_buffer_logged;

    /* journal_w: producer counter, atomic, bumped from the PW callback.
     * journal_r: consumer cursor, Wine threads only, under the loop lock. */
    struct stream_journal_entry journal[STATE_JOURNAL_LEN];
    UINT32 journal_w;
    UINT32 journal_r;

    struct list packet_free_head;
    struct list packet_filled_head;

    char *device;
    struct list entry;          /* g_streams */
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

/* As for the slot array: an aligned base only keeps every element aligned if
 * the stride is a whole number of alignments. */
C_ASSERT(sizeof(ACPacket) % _Alignof(ACPacket) == 0);

/* PipeWire node.bus / device.bus values we translate into a Windows device
 * path (mirrors winepulse's phys_device_bus_type). */
enum pw_device_bus
{
    PW_BUS_OTHER = 0,
    PW_BUS_PCI,
    PW_BUS_USB,
};

struct pw_phys_device
{
    struct list entry;
    WCHAR *display;
    EndpointFormFactor form;
    UINT channel_mask;
    REFERENCE_TIME min_period, def_period;
    WAVEFORMATEXTENSIBLE fmt;
    /* Device-path inputs (winepulse get_device_path parity).  index runs
     * across both lists and the synthetic defaults, because a bus-less
     * device path is {1}.ROOT\MEDIA\<index> and nothing else in it varies. */
    enum pw_device_bus bus;
    UINT16 vendor_id, product_id;
    UINT index;
    GUID container_id;
    char pw_name[];
};

struct pipewire_period
{
    struct list entry;
    char *device;
    UINT64 period_usec;
    struct list streams;
    HANDLE timer_thread;
    LONG please_quit; /* atomic: release_stream store-release, timer load-acquire */
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
static int core_last_res;
static char core_last_message[128];
static BOOL core_error_logged;

static struct list g_streams = LIST_INIT(g_streams); /* loop-lock protected */

static pthread_mutex_t pw_init_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct list g_render_devices = LIST_INIT(g_render_devices);
static struct list g_capture_devices = LIST_INIT(g_capture_devices);
static struct list active_periods = LIST_INIT(active_periods);
static char g_default_sink[256];
static char g_default_source[256];

/* ----------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

/* Safe for TEB-less PW callback threads (no ntdll). */
static void copy_cstr(char *dst, size_t dst_size, const char *src)
{
    size_t i;

    if (!dst_size)
        return;
    if (!src)
    {
        dst[0] = '\0';
        return;
    }
    for (i = 0; i + 1 < dst_size && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* frames = round(period * rate / denom); bytes = frames * frame_size.
 * period/rate/denom are unsigned so callers can pass REFERENCE_TIME (hns)
 * or usec without intermediate int narrowing. */
static BOOL calc_period_bytes(UINT64 period, UINT32 rate, UINT32 denom,
                              UINT32 frame_size, SIZE_T *out_bytes)
{
    UINT64 frames;

    if (!period || !rate || !denom || !frame_size)
        return FALSE;
    if (period > (UINT64_MAX - denom / 2) / rate)
        return FALSE;

    frames = (period * (UINT64)rate + denom / 2) / denom;
    if (!frames || frames > (UINT64)SIZE_MAX / frame_size)
        return FALSE;

    *out_bytes = (SIZE_T)(frames * frame_size);
    return TRUE;
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

#define MAX_DEVICE_NAME_LEN 62

/* Mirror winepulse get_device_name: some broken apps (Split/Second with
 * fmodex) crash on endpoint names longer than 62 chars, even on native.  The
 * cap is on UTF-16 length, so it is checked here on the unix-call thread
 * where ntdll_umbstowcs is legal, not on the probe thread.  When no shorter
 * candidate fits, keep the long name rather than truncate, matching pulse. */
static WCHAR *utf8_to_wstr_capped(const char *desc, const char *nick, const char *node_name)
{
    const char *chosen = desc ? desc : (nick ? nick : node_name);
    WCHAR *w = utf8_to_wstr(chosen);

    if (w && lstrlenW(w) > MAX_DEVICE_NAME_LEN)
    {
        WCHAR *alt = NULL;

        if (nick && nick[0])
            alt = utf8_to_wstr(nick);
        if (!alt || lstrlenW(alt) > MAX_DEVICE_NAME_LEN)
        {
            const char *tail = node_name ? strrchr(node_name, '.') : NULL;

            free(alt);
            alt = NULL;
            tail = tail ? tail + 1 : node_name;
            if (tail && tail[0])
                alt = utf8_to_wstr(tail);
        }
        if (alt && lstrlenW(alt) <= MAX_DEVICE_NAME_LEN)
        {
            free(w);
            w = alt;
        }
        else
            free(alt);
    }
    return w;
}

/* winepulse prefixes monitor source descriptions with an unlocalized
 * "Monitor of ", so match it.  The result may exceed MAX_DEVICE_NAME_LEN,
 * the same non-guarantee as utf8_to_wstr_capped. */
static WCHAR *monitor_name_from(const WCHAR *sink_display)
{
    static const WCHAR monitor_of[] = {'M','o','n','i','t','o','r',' ','o','f',' '};
    size_t pre = ARRAY_SIZE(monitor_of), len = lstrlenW(sink_display);
    WCHAR *out = malloc((pre + len + 1) * sizeof(WCHAR));

    if (!out)
        return NULL;
    memcpy(out, monitor_of, sizeof(monitor_of));
    memcpy(out + pre, sink_display, (len + 1) * sizeof(WCHAR));
    return out;
}

/* Post-mortem breadcrumb recovered from a core file; the driver never reads
 * it.  Zero means the callback has never run for this stream, otherwise the
 * low bits give the phase and the rest a callback count.  The stores are
 * relaxed, so a mark can be published either side of the code it brackets:
 * read it as "roughly here", not as proof. */
#define CB_ENTER 1  /* in the callback, buffer not yet validated */
#define CB_BODY  2  /* buffer validated, moving audio */
#define CB_DONE  3  /* buffer queued back, callback returning */
#define CB_MARK(s, ph) \
    __atomic_store_n(&(s)->cb_mark, ((s)->cb_seq << 2) | (ph), __ATOMIC_RELAXED)

/* Render dispatch mode, read from the environment once per process.  With
 * PW_STREAM_FLAG_RT_PROCESS the callback runs on PipeWire's realtime data
 * thread rather than the thread loop, which removes the lock that serialized
 * it against the control paths; what that is worth in latency is the graph's
 * decision.  Capture never sets it: its source is its own driver, so it has
 * nothing to win for the same exposure. */
static BOOL rt_render;

/* Realtime CPU budget below which arming RT_PROCESS is a process kill rather
 * than a latency choice.  RLIMIT_RTTIME counts only CPU burned without a
 * blocking syscall and an audio callback blocks every quantum, so the floor
 * sits well above any sane callback and well below the 200 ms a working rtkit
 * grants.  module-rt itself defaults rt.time.soft/hard to unlimited, so a
 * small value here is always something the environment imposed. */
#define RT_TIME_FLOOR_USEC 20000

/* The process-wide budget every SCHED_FIFO/RR thread here is judged against.
 * Either pointer may be NULL.  A failed read reports unlimited: not knowing
 * the budget is no reason to refuse the mode. */
static void rt_time_limit(UINT64 *soft, UINT64 *hard)
{
    struct rlimit rl;

    if (getrlimit(RLIMIT_RTTIME, &rl) < 0)
        rl.rlim_cur = rl.rlim_max = RLIM_INFINITY;
    if (soft)
        *soft = (UINT64)rl.rlim_cur;
    if (hard)
        *hard = (UINT64)rl.rlim_max;
}

static const char *rt_time_str(char *buf, size_t len, UINT64 usec)
{
    if (usec == (UINT64)RLIM_INFINITY)
        snprintf(buf, len, "unlimited");
    else
        snprintf(buf, len, "%llu us", (unsigned long long)usec);
    return buf;
}

/* Identifies one process run, so a PROTON_LOG and a core dump can be shown to
 * describe the same run instead of assumed to: every dispatch line carries
 * the value and the global is readable out of a core by name.  Zero is the
 * never-initialised sentinel and the live value is forced off it.  Entropy is
 * getrandom mixed with the pid and a monotonic count, since pids recycle and
 * a collision would certify a mismatched pairing.  Treat it as opaque. */
static UINT64 dispatch_token;

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

/* Take ownership of the slot the producer is due to fill next.  A FREE slot
 * is taken outright; otherwise the oldest FULL slot is evicted, which is what
 * makes the policy drop-oldest.  Either transition is a compare-exchange, so
 * a slot the consumer has already claimed can never be taken from under it,
 * and the producer moves on to the next one.  Runs on the process
 * callback: no allocation, no logging, no Wine calls. */
static BOOL cap_acquire_slot(struct pipewire_stream *stream)
{
    UINT32 tries;

    for (tries = 0; tries < stream->cap_n_slots; tries++)
    {
        struct cap_slot *slot = &stream->cap_slots[stream->cap_w_slot];
        UINT32 w = __atomic_load_n(&slot->word, __ATOMIC_ACQUIRE);
        UINT32 st = CAP_STATE(w);

        /* Exchange the whole observed word, not just the state, so a slot
         * that was recycled since the load fails here rather than being
         * silently taken in its new generation. */
        if ((st == CAP_FREE || st == CAP_FULL) &&
            __atomic_compare_exchange_n(&slot->word, &w, CAP_WORD(CAP_SEQ(w), CAP_FILLING),
                                        FALSE, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
        {
            if (st == CAP_FULL)
            {
                /* Evicted unread audio.  Do NOT flag here: eviction removes a
                 * sequence number, and the consumer turns that missing
                 * sequence into the discontinuity on exactly the packet that
                 * gapped.  Flagging the slot we are about to publish would
                 * put it on a later packet instead. */
                __atomic_add_fetch(&stream->overrun_count, 1, __ATOMIC_RELAXED);
            }
            stream->cap_filling = TRUE;
            stream->cap_w_fill = 0;
            return TRUE;
        }
        /* CAP_FILLING, CAP_INUSE, or the consumer won the race: move on. */
        stream->cap_w_slot = (stream->cap_w_slot + 1) % stream->cap_n_slots;
    }
    return FALSE;
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
    int len, res;

    spa_json_init(&it[0], json, strlen(json));
    if (spa_json_enter_object(&it[0], &it[1]) <= 0)
        return -1;
    /* -ENOSPC means the key token did not fit key[], not that the object
     * ended: consume its value and keep scanning, or one long key hides
     * every field behind it.  Mirrors spa_json_object_next. */
    while ((res = spa_json_get_string(&it[1], key, sizeof(key))) != 0)
    {
        if (res < 0 && res != -ENOSPC)
            return -1;
        if (res > 0 && !strcmp(key, field))
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

/* Takes ownership of display, already converted on the unix-call thread, and
 * frees it on failure. */
static struct pw_phys_device *add_device(struct list *list, const char *pw_name, WCHAR *display,
                                         EndpointFormFactor form, uint32_t rate, uint32_t channels, UINT mask,
                                         REFERENCE_TIME min_period)
{
    size_t len = strlen(pw_name);
    struct pw_phys_device *dev;

    if (!display)
        return NULL;
    if (!(dev = malloc(offsetof(struct pw_phys_device, pw_name) + len + 1)))
    {
        free(display);
        return NULL;
    }
    dev->display = display;
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
    /* O_CLOEXEC: process_attach can run with application threads already
     * fork+exec'ing.  O_NONBLOCK: the path comes from the environment, and
     * opening a FIFO would otherwise block the first mmdevapi unix call
     * forever instead of failing closed. */
    if ((fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK)) < 0)
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
    /* Overwrite: reaching here means no usable SPA_PLUGIN_DIR was inherited,
     * so an inherited module dir is unverified and may be foreign-arch.  Both
     * must come from the same libdir.  An explicitly configured pair keeps
     * its values through the early return above. */
    if (!access(path, F_OK))
        setenv("PIPEWIRE_MODULE_DIR", path, 1);
    TRACE("derived SPA plugin dir from %s\n", libdir);
}

static NTSTATUS pipewire_process_attach(void *args)
{
    const char *rt = getenv("WINEPIPEWIRE_RT");
    struct timespec ts;
    UINT64 rnd = 0;

    pipewire_set_plugin_dirs();
    pw_init(NULL, NULL);
    TRACE("PipeWire %s, header %s\n", pw_get_library_version(), pw_get_headers_version());

    /* First unix call the driver receives (mmdevapi/main.c:94), so the token
     * is set before any stream can connect and no dispatch line can be
     * emitted without one. */
    if (getrandom(&rnd, sizeof(rnd), 0) != (ssize_t)sizeof(rnd))
        rnd = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    dispatch_token = rnd ^ ((UINT64)(getpid() & 0xffff) << 48) ^
                     ((((UINT64)ts.tv_sec * 1000000000 + ts.tv_nsec)) & 0xffffffffffffull);
    if (!dispatch_token)
        dispatch_token = 1;

    rt_render = rt && !strcmp(rt, "1");
    /* Only what was asked for.  What actually happens is not known until a
     * stream connects and the data loop can be compared, so the line that a
     * crash report is read against is emitted there, not here. */
    TRACE("WINEPIPEWIRE_RT=%d requested, session=%016llx\n", rt_render,
          (unsigned long long)dispatch_token);
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
        core_last_res = res;
        copy_cstr(core_last_message, sizeof(core_last_message), message);
        core_error_logged = FALSE;
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
        /* Existing streams still hold pw_stream objects on this core.
         * Destroy those objects under the loop lock and mark the Wine-side
         * streams invalidated so a fresh core can be opened for new clients.
         * The app recovers by releasing the old IAudioClient after creating
         * its replacement (standard device-lost order). */
        if (!list_empty(&g_streams))
        {
            struct pipewire_stream *stream;
            unsigned int n = 0;

            LIST_FOR_EACH_ENTRY(stream, &g_streams, struct pipewire_stream, entry)
                n++;
            if (!core_error_logged)
            {
                WARN("core dead (res %d: %s): invalidating %u live stream(s) and reconnecting.\n",
                     core_last_res, debugstr_a(core_last_message[0] ? core_last_message : NULL),
                     n);
                core_error_logged = TRUE;
            }
            LIST_FOR_EACH_ENTRY(stream, &g_streams, struct pipewire_stream, entry)
            {
                if (!stream->pw)
                    continue;
                spa_hook_remove(&stream->stream_listener);
                pw_stream_destroy(stream->pw);
                stream->pw = NULL;
                copy_cstr(stream->last_error, sizeof(stream->last_error), "core connection lost");
                stream->pending_error = TRUE;
            }
        }
        else if (!core_error_logged && core_dead)
        {
            WARN("reconnecting after core error (res %d: %s).\n",
                 core_last_res, debugstr_a(core_last_message[0] ? core_last_message : NULL));
            core_error_logged = TRUE;
        }

        if (core_listener_added)
        {
            spa_hook_remove(&core_listener);
            core_listener_added = FALSE;
        }
        pw_core_disconnect(pw_core_global);
        pw_core_global = NULL;
    }
    core_dead = FALSE;
    core_last_res = 0;
    core_last_message[0] = '\0';
    core_error_logged = FALSE;

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
    /* Inside the mutex: a concurrent main_loop_start would otherwise be in
     * pw_thread_loop_new/pw_context_new while this unloads the SPA plugins. */
    pw_deinit();
    pthread_mutex_unlock(&pw_init_mutex);
    return STATUS_SUCCESS;
}

/* ----------------------------------------------------------------------
 * Probe: device enumeration, defaults, formats (test_connect)
 *
 * All registry / node / metadata / core callbacks below run on the probe
 * thread loop's own (foreign) thread.  They may only call libc, spa and pw
 * APIs, never ntdll/Wine (no TRACE).  Strings are kept as UTF-8 and
 * converted to UTF-16 later, on the unix-call thread.
 * ---------------------------------------------------------------------- */

struct probe_node
{
    struct list entry;
    uint32_t id;
    EDataFlow flow;
    char *node_name;
    char *display;         /* raw UTF-8 description candidate (probe thread) */
    char *nick;
    struct pw_node *proxy;
    struct spa_hook listener;
    uint32_t channels;
    uint32_t position[SPA_AUDIO_MAX_CHANNELS];
    int have_format;
    uint32_t device_id;    /* parent device.id, or SPA_ID_INVALID */
    enum pw_device_bus bus;
};

/* Vendor/product ids live on the Device object, not the node.  Registry
 * globals omit them, so the probe binds each device and reads them from its
 * info event. */
struct probe_device
{
    struct list entry;
    uint32_t id;
    enum pw_device_bus bus;
    UINT16 vendor_id, product_id;
    struct pw_device *proxy;
    struct spa_hook listener;
    BOOL listener_added;
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
    struct list devices;
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
    size_t dst_size;

    if (!key)
        return 0;
    if (!strcmp(key, "default.audio.sink"))
    {
        dst = p->default_sink;
        dst_size = sizeof(p->default_sink);
    }
    else if (!strcmp(key, "default.audio.source"))
    {
        dst = p->default_source;
        dst_size = sizeof(p->default_source);
    }
    else
        return 0;
    /* A cleared default arrives as a NULL value, and a malformed one must
     * not leave the previous name standing either. */
    if (!value || parse_json_str_field(value, "name", dst, dst_size) < 0)
        dst[0] = '\0';
    return 0;
}

static const struct pw_metadata_events probe_metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_probe_metadata_property,
};

/* Daemon-supplied properties are free-form text: accept only a number in the
 * given base, fully consumed, inside the range the driver can honour.  A bare
 * strtoul takes "abc" as 0, silently truncates out-of-range values on 64-bit
 * and saturates to ULONG_MAX on 32-bit, so one string can mean two different
 * things across the two unixlib builds. */
static BOOL parse_u32(const char *value, int base, uint32_t lo, uint32_t hi, uint32_t *out)
{
    unsigned long v;
    char *end;

    if (!value)
        return FALSE;
    errno = 0;
    v = strtoul(value, &end, base);
    if (errno || end == value || *end || v < lo || v > hi)
        return FALSE;
    *out = (uint32_t)v;
    return TRUE;
}

static int on_probe_settings_property(void *data, uint32_t subject, const char *key,
                                      const char *type, const char *value)
{
    struct probe *p = data;
    uint32_t v;

    if (!key || !value)
        return 0;
    if (!strcmp(key, "clock.force-rate"))
    {
        if (parse_u32(value, 10, PW_MIN_CLOCK_RATE, PW_MAX_RATE, &v))
            p->clock_rate = v;
    }
    else if (!strcmp(key, "clock.rate") && !p->clock_rate)
    {
        if (parse_u32(value, 10, PW_MIN_CLOCK_RATE, PW_MAX_RATE, &v))
            p->clock_rate = v;
    }
    else if (!strcmp(key, "clock.force-quantum"))
    {
        if (parse_u32(value, 10, 1, PW_MAX_QUANTUM, &v))
            p->min_quantum = v;
    }
    else if (!strcmp(key, "clock.min-quantum") && !p->min_quantum)
    {
        if (parse_u32(value, 10, 1, PW_MAX_QUANTUM, &v))
            p->min_quantum = v;
    }
    return 0;
}

static const struct pw_metadata_events probe_settings_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_probe_settings_property,
};

static enum pw_device_bus parse_device_bus(const char *bus)
{
    if (!bus)
        return PW_BUS_OTHER;
    if (!strcmp(bus, "pci"))
        return PW_BUS_PCI;
    if (!strcmp(bus, "usb"))
        return PW_BUS_USB;
    return PW_BUS_OTHER;
}

/* Vendor/product ids arrive as 0x-prefixed hex strings. */
static void on_probe_device_info(void *data, const struct pw_device_info *info)
{
    struct probe_device *pd = data;
    uint32_t v;

    if (!info || !info->props)
        return;
    pd->bus = parse_device_bus(spa_dict_lookup(info->props, PW_KEY_DEVICE_BUS));
    pd->vendor_id = parse_u32(spa_dict_lookup(info->props, PW_KEY_DEVICE_VENDOR_ID),
                              16, 0, 0xffff, &v) ? (UINT16)v : 0;
    pd->product_id = parse_u32(spa_dict_lookup(info->props, PW_KEY_DEVICE_PRODUCT_ID),
                               16, 0, 0xffff, &v) ? (UINT16)v : 0;
}

static const struct pw_device_events probe_device_events = {
    PW_VERSION_DEVICE_EVENTS,
    .info = on_probe_device_info,
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
        const char *dev_id = spa_dict_lookup(props, PW_KEY_DEVICE_ID);
        struct probe_node *pn, *dup;
        EDataFlow flow;

        /* An empty node.name aliases the synthetic default endpoint's
         * placeholder, so both would enumerate with an empty device string
         * and find_device would only ever reach the first.  winepulse rejects
         * the same case (pulse.c:757). */
        if (!media_class || !node_name || !node_name[0])
            return;
        if (!strcmp(media_class, "Audio/Sink"))
            flow = eRender;
        else if (!strcmp(media_class, "Audio/Source"))
            flow = eCapture;
        else
            return;

        /* find_device returns the first match in a list, so a second node
         * with this direction and name would enumerate but never be
         * reachable.  Any local client can register one. */
        LIST_FOR_EACH_ENTRY(dup, &p->nodes, struct probe_node, entry)
            if (dup->flow == flow && !strcmp(dup->node_name, node_name))
                return;

        if (!(pn = calloc(1, sizeof(*pn))))
            return;
        pn->id = id;
        pn->flow = flow;
        /* SPA_ID_INVALID is the "no parent device" sentinel, so it must not
         * also be what an unparsable id decays to. */
        if (!parse_u32(dev_id, 10, 0, SPA_ID_INVALID - 1, &pn->device_id))
            pn->device_id = SPA_ID_INVALID;
        pn->bus = parse_device_bus(spa_dict_lookup(props, "device.bus"));
        /* Probe thread: keep everything UTF-8, no ntdll/WCHAR work.  The name
         * cap is applied later, in utf8_to_wstr_capped on the unix-call thread. */
        pn->node_name = strdup(node_name);
        pn->display = strdup(desc ? desc : (nick ? nick : node_name));
        pn->nick = nick ? strdup(nick) : NULL;
        if (!pn->node_name || !pn->display)
        {
            free(pn->node_name);
            free(pn->display);
            free(pn->nick);
            free(pn);
            return;
        }
        list_add_tail(&p->nodes, &pn->entry);

        /* Bind the node and ask for its supported formats, which carry the
         * real channel layout.  The param events arrive during the sync
         * round-trips below. */
        pn->proxy = pw_registry_bind(p->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
        if (pn->proxy)
        {
            pw_node_add_listener(pn->proxy, &pn->listener, &probe_node_events, pn);
            pw_node_enum_params(pn->proxy, 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX, NULL);
        }
    }
    else if (!strcmp(type, PW_TYPE_INTERFACE_Device))
    {
        struct probe_device *pd;

        /* Failed bind keeps PW_BUS_OTHER and zero ids.  The path falls back
         * to ROOT\MEDIA. */
        if (!(pd = calloc(1, sizeof(*pd))))
            return;
        pd->id = id;
        pd->bus = PW_BUS_OTHER;
        list_add_tail(&p->devices, &pd->entry);

        pd->proxy = pw_registry_bind(p->registry, id, PW_TYPE_INTERFACE_Device,
                                     PW_VERSION_DEVICE, 0);
        if (pd->proxy)
        {
            pw_device_add_listener(pd->proxy, &pd->listener, &probe_device_events, pd);
            pd->listener_added = TRUE;
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
    struct probe *p = data;
    struct probe_device *pd, *pdnext;
    struct probe_node *pn, *pnnext;

    /* A global that disappears mid-probe is gone from the graph: destroy its
     * proxy and drop its entry, so the cache does not enumerate a node that
     * no longer exists and device lookups do not see stale data. */
    LIST_FOR_EACH_ENTRY_SAFE(pn, pnnext, &p->nodes, struct probe_node, entry)
    {
        if (pn->id != id)
            continue;
        if (pn->proxy)
        {
            spa_hook_remove(&pn->listener);
            pw_proxy_destroy((struct pw_proxy *)pn->proxy);
            pn->proxy = NULL;
        }
        list_remove(&pn->entry);
        free(pn->node_name);
        free(pn->display);
        free(pn->nick);
        free(pn);
        return;
    }
    LIST_FOR_EACH_ENTRY_SAFE(pd, pdnext, &p->devices, struct probe_device, entry)
    {
        if (pd->id != id)
            continue;
        if (pd->listener_added)
        {
            spa_hook_remove(&pd->listener);
            pd->listener_added = FALSE;
        }
        if (pd->proxy)
        {
            pw_proxy_destroy((struct pw_proxy *)pd->proxy);
            pd->proxy = NULL;
        }
        list_remove(&pd->entry);
        free(pd);
        return;
    }
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

/* Synthetic default endpoint (empty pw_name -> session manager default
 * routing).  When the default node is known, mirror its format so the
 * default endpoint advertises the real speaker layout. */
static void add_default_device(struct list *list, EndpointFormFactor form, const char *match,
                               uint32_t rate, REFERENCE_TIME min_period, UINT index)
{
    struct pw_phys_device *dev, *def_src = NULL, *def;

    if (match[0])
    {
        LIST_FOR_EACH_ENTRY(dev, list, struct pw_phys_device, entry)
            if (!strcmp(dev->pw_name, match)) { def_src = dev; break; }
    }
    /* calloc a full struct, +1 for the empty pw_name: the placeholder has no
     * bus and no vendor ids, and the full size keeps -Warray-bounds quiet on
     * the flexible array member. */
    if (!(def = calloc(1, sizeof(*def) + 1)))
        return;
    /* Burnout Paradise Remastered crashes on a device name with no space,
     * so mirror winepulse's "PulseAudio Output" placeholder (pulse.c). */
    if (!(def->display = utf8_to_wstr(form == Speakers ? "PipeWire Output" : "PipeWire Input")))
    {
        free(def);
        return;
    }
    def->pw_name[0] = '\0';
    def->form = form;
    def->index = index;
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

static struct probe_device *probe_find_device(struct probe *p, uint32_t device_id)
{
    struct probe_device *pd;

    if (device_id == SPA_ID_INVALID)
        return NULL;
    LIST_FOR_EACH_ENTRY(pd, &p->devices, struct probe_device, entry)
        if (pd->id == device_id)
            return pd;
    return NULL;
}

/* A ContainerId stable across reboots.  winepulse derives it from udev sysfs,
 * which we lack, so hash the vendor/product ids plus the reboot-stable
 * PipeWire node name.  A bus-less device gets a zero GUID, as pulse leaves it
 * without sysfs. */
static GUID make_container_id(enum pw_device_bus bus, UINT16 vendor_id, UINT16 product_id,
                              const char *node_name)
{
    GUID id = {0};
    UINT64 h = 1469598103934665603ull; /* FNV-1a offset basis */
    const char *c;

    if (bus == PW_BUS_OTHER)
        return id;
    for (c = node_name; c && *c; c++)
        h = (h ^ (unsigned char)*c) * 1099511628211ull;
    id.Data1 = MAKELONG(vendor_id, product_id);
    id.Data2 = (UINT16)(h >> 48);
    id.Data3 = (UINT16)(h >> 32);
    id.Data4[0] = (BYTE)(h >> 24);
    id.Data4[1] = (BYTE)(h >> 16);
    id.Data4[2] = (BYTE)(h >> 8);
    id.Data4[3] = (BYTE)h;
    id.Data4[4] = (BYTE)(vendor_id >> 8);
    id.Data4[5] = (BYTE)vendor_id;
    id.Data4[6] = (BYTE)(product_id >> 8);
    id.Data4[7] = (BYTE)product_id;
    return id;
}

static void fill_device_path_info(struct pw_phys_device *dev, struct probe *p,
                                  struct probe_node *pn, UINT index)
{
    struct probe_device *pd = probe_find_device(p, pn->device_id);

    if (!dev)
        return;
    dev->bus = pd ? pd->bus : pn->bus;
    dev->vendor_id = pd ? pd->vendor_id : 0;
    dev->product_id = pd ? pd->product_id : 0;
    dev->index = index;
    dev->container_id = make_container_id(dev->bus, dev->vendor_id, dev->product_id, pn->node_name);
}

static void build_device_cache(struct probe *p)
{
    struct probe_node *pn;
    uint32_t rate = p->clock_rate ? p->clock_rate : 48000;
    REFERENCE_TIME min_period = 30000;
    UINT index = 0;

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
        struct pw_phys_device *dev;
        WCHAR *name = utf8_to_wstr_capped(pn->display, pn->nick, pn->node_name);

        dev = add_device(list, pn->node_name, name, form, rate, channels, mask, min_period);
        fill_device_path_info(dev, p, pn, index++);

        /* PipeWire has no separate monitor nodes, so synthesize one capture
         * endpoint per sink under the sink's own name, with the LineLevel
         * form winepulse gives monitor sources.  Capture streams that name a
         * sink already connect through PW_KEY_STREAM_CAPTURE_SINK (see
         * pipewire_stream_connect). */
        if (pn->flow == eRender && dev)
        {
            WCHAR *mon = monitor_name_from(dev->display);
            if (mon)
            {
                struct pw_phys_device *md = add_device(&g_capture_devices, pn->node_name, mon,
                                                       LineLevel, rate, channels, mask, min_period);
                fill_device_path_info(md, p, pn, index++);
            }
        }
    }

    add_default_device(&g_render_devices, Speakers, g_default_sink, rate, min_period, index++);
    add_default_device(&g_capture_devices, Microphone, g_default_source, rate, min_period, index++);
}

static void probe_teardown(struct probe *p)
{
    struct probe_node *pn;
    struct probe_device *pd;

    LIST_FOR_EACH_ENTRY(pn, &p->nodes, struct probe_node, entry)
    {
        if (pn->proxy)
        {
            spa_hook_remove(&pn->listener);
            pw_proxy_destroy((struct pw_proxy *)pn->proxy);
            pn->proxy = NULL;
        }
    }
    LIST_FOR_EACH_ENTRY(pd, &p->devices, struct probe_device, entry)
    {
        if (pd->listener_added)
        {
            spa_hook_remove(&pd->listener);
            pd->listener_added = FALSE;
        }
        if (pd->proxy)
        {
            pw_proxy_destroy((struct pw_proxy *)pd->proxy);
            pd->proxy = NULL;
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
    struct probe_device *pd, *pdnext;
    BOOL failed;

    free_device_lists();
    list_init(&g_render_devices);
    list_init(&g_capture_devices);
    g_default_sink[0] = g_default_source[0] = '\0';

    params->priority = Priority_Unavailable;

    memset(&p, 0, sizeof(p));
    list_init(&p.nodes);
    list_init(&p.devices);

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
        ERR("pw_context_connect failed during probe.\n");
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

    /* Both probe lists are freed on every path: the error return below used
     * to sit above the cleanup and leak p.devices. */
    failed = p.core_error && list_empty(&p.nodes);
    if (failed)
        WARN("PipeWire core reported an error during the probe\n");
    else
        /* The pw loop is fully stopped: safe to do Wine string conversion. */
        build_device_cache(&p);

    LIST_FOR_EACH_ENTRY_SAFE(pn, next, &p.nodes, struct probe_node, entry)
    {
        list_remove(&pn->entry);
        free(pn->node_name);
        free(pn->display);
        free(pn->nick);
        free(pn);
    }
    LIST_FOR_EACH_ENTRY_SAFE(pd, pdnext, &p.devices, struct probe_device, entry)
    {
        list_remove(&pd->entry);
        free(pd);
    }

    if (failed)
        return STATUS_SUCCESS;

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

/* Synthesize a Windows device path, mirroring winepulse get_device_path.
 * Most audio devices have no serial number, so substitute the low 8 hex
 * digits of the endpoint GUID, which mmdevapi derives from the device name
 * and is stable per device. */
static void get_device_path(struct pw_phys_device *dev, struct get_prop_value_params *params)
{
    const GUID *guid = params->guid;
    PROPVARIANT *out = params->value;
    UINT serial_number;
    char path[128];
    int len;

    serial_number = (guid->Data4[4] << 24) | (guid->Data4[5] << 16) |
                    (guid->Data4[6] << 8) | guid->Data4[7];

    switch (dev->bus)
    {
    case PW_BUS_PCI:
        len = sprintf(path, "{1}.HDAUDIO\\FUNC_01&VEN_%04X&DEV_%04X\\%u&%08X",
                      dev->vendor_id, dev->product_id, dev->index, serial_number);
        break;
    case PW_BUS_USB:
        len = sprintf(path, "{1}.USB\\VID_%04X&PID_%04X\\%u&%08X",
                      dev->vendor_id, dev->product_id, dev->index, serial_number);
        break;
    default:
        len = sprintf(path, "{1}.ROOT\\MEDIA\\%04u", dev->index);
        break;
    }

    if (*params->buffer_size < ++len * sizeof(WCHAR))
    {
        params->result = E_NOT_SUFFICIENT_BUFFER;
        *params->buffer_size = len * sizeof(WCHAR);
        return;
    }
    out->vt = VT_LPWSTR;
    out->pwszVal = params->buffer;
    ntdll_umbstowcs(path, len, out->pwszVal, len);
    params->result = S_OK;
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
        get_device_path(dev, params);
        return STATUS_SUCCESS;
    }
    if (IsEqualGUID(&params->prop->fmtid, &DEVPKEY_Device_ContainerId))
    {
        if (!params->buffer || *params->buffer_size < sizeof(*params->value->puuid))
        {
            *params->buffer_size = sizeof(*params->value->puuid);
            params->result = E_NOT_SUFFICIENT_BUFFER;
        }
        else
        {
            params->value->vt = VT_CLSID;
            params->value->puuid = params->buffer;
            *params->value->puuid = dev->container_id;
            params->result = S_OK;
        }
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
    if (!fmt->nSamplesPerSec || fmt->nSamplesPerSec > PW_MAX_RATE)
    {
        WARN("Unsupported sample rate %u.\n", fmt->nSamplesPerSec);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
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
                /* A 32-bit container carries its wValidBitsPerSample valid
                 * bits left-aligned, with the unused low bits zero (WDK,
                 * WAVEFORMATEXTENSIBLE), so the container already holds a
                 * valid 32-bit sample and S32 is bit-exact for 24 valid bits
                 * as well as 32.  SPA_AUDIO_FORMAT_S24_32_LE is the opposite
                 * ALSA layout, 24 bits in the low end of the word, and reads
                 * such a container 8 bits down with the top 8 bits wrapped.
                 * Upstream drops the pair instead (e320cfd50bd); we cannot, since
                 * mmdevapi accepts it: IsFormatSupported says S_OK, Initialize fails. */
                if (valid == 32 || valid == 24) spafmt = SPA_AUDIO_FORMAT_S32_LE;
                break;
            default:
                WARN("Unsupported PCM container %u valid %lu.\n",
                     fmt->wBitsPerSample, (unsigned long)valid);
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
        {
            WARN("Unsupported extensible format: tag=%u ch=%u rate=%u bits=%u mask=%#x.\n",
                 fmt->wFormatTag, fmt->nChannels, fmt->nSamplesPerSec,
                 fmt->wBitsPerSample, mask);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
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
        WARN("Unhandled tag %#x ch=%u rate=%u bits=%u.\n",
             fmt->wFormatTag, fmt->nChannels, fmt->nSamplesPerSec, fmt->wBitsPerSample);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    if (spafmt == SPA_AUDIO_FORMAT_UNKNOWN || !info->channels)
    {
        WARN("Rejected format: tag=%#x ch=%u rate=%u bits=%u mask=%#x.\n",
             fmt->wFormatTag, fmt->nChannels, fmt->nSamplesPerSec, fmt->wBitsPerSample, mask);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
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

/* vol is a caller-owned snapshot: the process callback must not re-read
 * stream->vol, which SetVolumes mutates concurrently. */
static void apply_volume(const struct pipewire_stream *stream, const float *vol,
                         BYTE *buffer, UINT32 bytes)
{
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

/* Foreign PipeWire thread: no Wine calls.  The release store to seq publishes
 * the slot, so the consumer never sees a half-filled entry. */
static void stream_journal_record(struct pipewire_stream *stream, enum pw_stream_state old,
                                  enum pw_stream_state state, const char *error)
{
    UINT32 w = __atomic_fetch_add(&stream->journal_w, 1, __ATOMIC_RELAXED);
    struct stream_journal_entry *e = &stream->journal[w % STATE_JOURNAL_LEN];

    e->old_state = old;
    e->new_state = state;
    copy_cstr(e->error, sizeof(e->error), error);
    __atomic_store_n(&e->seq, w + 1, __ATOMIC_RELEASE);
}

/* Wine threads only, loop lock held, so logging is safe here. */
static void stream_journal_flush(struct pipewire_stream *stream)
{
    UINT32 w = __atomic_load_n(&stream->journal_w, __ATOMIC_ACQUIRE);
    UINT32 r = stream->journal_r;

    if (w - r > STATE_JOURNAL_LEN)
        r = w - STATE_JOURNAL_LEN;  /* drop-oldest: producer outran us */

    for (; r != w; r++)
    {
        struct stream_journal_entry *e = &stream->journal[r % STATE_JOURNAL_LEN];
        UINT32 seq = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
        const char *msg;

        if (seq != r + 1)
            continue;  /* producer lapped this slot mid-read */
        msg = e->error[0] ? e->error : NULL;
        if (e->new_state == PW_STREAM_STATE_ERROR)
            WARN("stream %p state %s -> %s: %s.\n", stream,
                 pw_stream_state_as_string(e->old_state),
                 pw_stream_state_as_string(e->new_state), debugstr_a(msg));
        else
            TRACE("stream %p state %s -> %s%s%s.\n", stream,
                  pw_stream_state_as_string(e->old_state),
                  pw_stream_state_as_string(e->new_state),
                  msg ? ": " : "", msg ? msg : "");
    }
    stream->journal_r = w;
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error)
{
    struct pipewire_stream *stream = data;

    /* PW loop thread, lock held: record only, no Wine logging. */
    stream_journal_record(stream, old, state, error);
    if (state == PW_STREAM_STATE_ERROR)
    {
        copy_cstr(stream->last_error, sizeof(stream->last_error), error);
        stream->pending_error = TRUE;
    }
    if (pw_loop_global)
        pw_thread_loop_signal(pw_loop_global, false);
}

static void on_stream_process(void *data)
{
    struct pipewire_stream *stream = data;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    struct spa_data *d;

    stream->cb_seq++;
    CB_MARK(stream, CB_ENTER);

    if (!(b = pw_stream_dequeue_buffer(stream->pw)))
        return;
    buf = b->buffer;
    if (!buf || !buf->n_datas || !buf->datas ||
        !(d = &buf->datas[0])->data || !d->chunk)
    {
        __atomic_add_fetch(&stream->bad_buffer_count, 1, __ATOMIC_RELAXED);
        if (stream->dataflow == eCapture)
        {
            /* The buffer is discarded, so audio was lost.  Charge it to the
             * next slot published, the same as a clamp or an all-slots-held
             * drop; without this the application sees the gap with nothing to
             * explain it.  Producer-private, so a plain store is right. */
            stream->cap_lost = TRUE;
        }
        pw_stream_queue_buffer(stream->pw, b);
        return;
    }
    CB_MARK(stream, CB_BODY);

    if (stream->dataflow == eRender)
    {
        UINT32 maxsize = d->maxsize;
        UINT32 req_frames, need_bytes, n, c;
        float vol[PW_CHANNELS_MAX];

        if (!b->requested || b->requested > maxsize / stream->frame_size)
            req_frames = maxsize / stream->frame_size;
        else
            req_frames = (UINT32)b->requested;
        need_bytes = req_frames * stream->frame_size;

        if (__atomic_load_n(&stream->started, __ATOMIC_ACQUIRE))
        {
            /* copy_from_ring wraps once, so a count above the ring size
             * would read past the allocation. */
            n = min(need_bytes, __atomic_load_n(&stream->pa_held_bytes, __ATOMIC_ACQUIRE));
            n = min(n, stream->real_bufsize_bytes);
            copy_from_ring(d->data, stream->local_buffer, stream->real_bufsize_bytes,
                           stream->pa_offs_bytes, n);
            for (c = 0; c < stream->info.channels; c++)
                __atomic_load(&stream->vol[c], &vol[c], __ATOMIC_ACQUIRE);
            apply_volume(stream, vol, d->data, n);
            if (n < need_bytes)
            {
                silence_buffer(stream->info.format, (BYTE *)d->data + n, need_bytes - n);
                __atomic_add_fetch(&stream->underrun_count, 1, __ATOMIC_RELAXED);
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
        if (__atomic_load_n(&stream->started, __ATOMIC_ACQUIRE) && stream->capture_ring)
        {
            UINT32 offs = min(d->chunk->offset, d->maxsize);
            UINT32 avail = min(d->chunk->size, d->maxsize - offs);
            const BYTE *src = (const BYTE *)d->data + offs;
            SIZE_T n = avail;

            /* Framing is relative to the start of the valid region, so only
             * the length matters.  cap_w_fill is producer-private and carried
             * across callbacks, so one chunk that is not a whole number of
             * frames would rotate every later frame across channels for the
             * life of the stream.  Drop the partial tail and report it rather
             * than silently rotating.  Must precede the ring clamp below,
             * which subtracts a length from src. */
            if (n % stream->frame_size)
            {
                n -= n % stream->frame_size;
                __atomic_add_fetch(&stream->bad_buffer_count, 1, __ATOMIC_RELAXED);
                stream->cap_lost = TRUE;
            }

            /* A chunk larger than the whole ring can only be represented by
             * its tail, which is the newest audio in it. */
            if (n > stream->capture_ring_size)
            {
                src += n - stream->capture_ring_size;
                n = stream->capture_ring_size;
                __atomic_add_fetch(&stream->overrun_count, 1, __ATOMIC_RELAXED);
                stream->cap_lost = TRUE;
            }

            while (n)
            {
                struct cap_slot *slot;
                SIZE_T take;

                if (!stream->cap_filling && !cap_acquire_slot(stream))
                {
                    /* Every slot is held by the consumer: the rest of this
                     * chunk is dropped, and a drop must be reported like any
                     * other, otherwise the application sees a gap with no
                     * discontinuity to explain it. */
                    __atomic_add_fetch(&stream->overrun_count, 1, __ATOMIC_RELAXED);
                    stream->cap_lost = TRUE;
                    break;
                }
                slot = &stream->cap_slots[stream->cap_w_slot];
                take = min(n, stream->period_bytes - stream->cap_w_fill);
                memcpy(stream->capture_ring + stream->cap_w_slot * stream->period_bytes +
                       stream->cap_w_fill, src, take);
                stream->cap_w_fill += take;
                src += take;
                n -= take;
                if (stream->cap_w_fill == stream->period_bytes)
                {
                    /* We own the slot as CAP_FILLING, so nobody else can
                     * transition it and a release store suffices.  The
                     * release also publishes slot->disc. */
                    slot->disc = stream->cap_lost;
                    stream->cap_lost = FALSE;
                    __atomic_store_n(&slot->word,
                                     CAP_WORD(stream->cap_w_seq, CAP_FULL), __ATOMIC_RELEASE);
                    stream->cap_w_seq = (stream->cap_w_seq + 1) & CAP_SEQ_MAX;
                    stream->cap_filling = FALSE;
                    stream->cap_w_slot = (stream->cap_w_slot + 1) % stream->cap_n_slots;
                    stream->cap_w_fill = 0;
                }
            }
        }
    }

    pw_stream_queue_buffer(stream->pw, b);
    CB_MARK(stream, CB_DONE);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .process = on_stream_process,
};

/* ----------------------------------------------------------------------
 * Stream creation / release
 * ---------------------------------------------------------------------- */

/* Called with the loop lock held. Emits one WARN per pending ERROR episode. */
static BOOL stream_valid(struct pipewire_stream *stream)
{
    enum pw_stream_state st;

    if (!stream)
        return FALSE;
    stream_journal_flush(stream);
    if (stream->pending_error)
    {
        if (stream->pw)
            st = pw_stream_get_state(stream->pw, NULL);
        else
            st = PW_STREAM_STATE_ERROR;
        WARN("stream %p saw error (now %s): %s.\n", stream,
             pw_stream_state_as_string(st),
             debugstr_a(stream->last_error[0] ? stream->last_error : NULL));
        stream->pending_error = FALSE;
    }
    if (!stream->pw)
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

/* Called with the loop lock held.  Waits out CONNECTING so the caller sees a
 * stream that has either negotiated or failed, never one still in flight. */
static HRESULT stream_await_ready(struct pipewire_stream *stream)
{
    enum pw_stream_state st;
    const char *error = NULL;
    int tries;

    for (tries = 0; tries < 10; tries++)
    {
        st = pw_stream_get_state(stream->pw, NULL);
        if (st == PW_STREAM_STATE_PAUSED || st == PW_STREAM_STATE_STREAMING)
            return S_OK;
        if (st == PW_STREAM_STATE_ERROR || st == PW_STREAM_STATE_UNCONNECTED)
        {
            pw_stream_get_state(stream->pw, &error);
            WARN("stream %p connect failed: %s.\n", stream, debugstr_a(error));
            return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
        }
        if (pw_thread_loop_timed_wait(pw_loop_global, 1) != 0)
            break;
    }
    st = pw_stream_get_state(stream->pw, &error);
    if (st == PW_STREAM_STATE_PAUSED || st == PW_STREAM_STATE_STREAMING)
        return S_OK;
    WARN("stream %p connect timed out in state %d: %s.\n", stream, st, debugstr_a(error));
    return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
}

/* At ERR because the alternative is a process the kernel kills without a
 * diagnostic, and the reporter will not have set WINEDEBUG. */
static void rt_gate_report(UINT64 soft, UINT64 hard, BOOL late)
{
    char buf[32];

    ERR("audio dispatch: realtime CPU budget is %s, under the %u us floor, so a "
        "realtime data thread would be killed by the kernel on its first "
        "quantum.  %s  This process keeps the driver loop for every stream from "
        "here.  session=%016llx\n",
        rt_time_str(buf, sizeof(buf), soft), RT_TIME_FLOOR_USEC,
        late ? "The budget was written while the stream was connecting, so it is "
               "being reconnected without RT_PROCESS."
             : "Connecting without RT_PROCESS.",
        (unsigned long long)dispatch_token);

    if (hard < RT_TIME_FLOOR_USEC)
        ERR("audio dispatch: the hard limit is %s, so nothing unprivileged can "
            "raise it.  Install rtkit and restart xdg-desktop-portal so it "
            "re-reads the provider, or launch with WINEPIPEWIRE_RT=0.  "
            "session=%016llx\n", rt_time_str(buf, sizeof(buf), hard),
            (unsigned long long)dispatch_token);
}

/* Certify where the process callback actually ended up.  RT_PROCESS asks for
 * the data thread, but node.loop.class from PIPEWIRE_PROPS or a client.conf
 * stream.rules entry decides independently, in both directions, so report the
 * measured loop rather than the request.  node.async is reported beside it
 * because RT_PROCESS suppresses it while rules are applied afterwards and can
 * restore it alone.  This certifies dispatch, not latency, and not the graph's
 * scheduling mode, which the link derives from both nodes.  rttime= is the
 * process-wide RLIMIT_RTTIME soft limit after connect: the same "data-loop.0"
 * is a latency win with a 200 ms budget and a process kill with none.
 *
 * Once per distinct (dataflow, requested, effective, async) combination.  The
 * matched case is WARN so the A/B recipe can grep it without full tracing;
 * anything needing action is at ERR.  Called with the loop lock held. */
static void report_dispatch_mode(struct pipewire_stream *stream, BOOL rt_render)
{
    struct pw_loop *dl = pw_stream_get_data_loop(stream->pw);
    const struct pw_properties *sprops = pw_stream_get_properties(stream->pw);
    const char *async = sprops ? pw_properties_get(sprops, PW_KEY_NODE_ASYNC) : NULL;
    const BOOL want_data = rt_render && stream->dataflow == eRender;
    const BOOL got_data = dl != pw_thread_loop_get_loop(pw_loop_global);
    const BOOL async_on = async && pw_properties_parse_bool(async);
    const UINT32 bit = 1u << ((stream->dataflow == eRender ? 8 : 0) |
                              (want_data ? 4 : 0) | (got_data ? 2 : 0) |
                              (async_on ? 1 : 0));
    static UINT32 reported;
    const char *flow, *req, *eff, *name;
    UINT64 rt_soft;
    char rt_buf[32];

    if (reported & bit)
        return;
    reported |= bit;

    rt_time_limit(&rt_soft, NULL);
    flow = stream->dataflow == eRender ? "render" : "capture";
    req = want_data ? "data-thread" : "driver-loop";
    eff = got_data ? "data-thread" : "driver-loop";
    name = dl && dl->name ? dl->name : "?";

    if (want_data != got_data)
    {
        ERR("audio dispatch: %s requested %s, effective %s, node.async=%s, "
            "loop \"%s\", rttime=%s - MISMATCH, the requested mode is NOT "
            "in force, session=%016llx\n", flow, req, eff,
            async ? async : "unset", name,
            rt_time_str(rt_buf, sizeof(rt_buf), rt_soft),
            (unsigned long long)dispatch_token);

        if (want_data)
            ERR("audio dispatch: PW_STREAM_FLAG_RT_PROCESS did not take effect.  "
                "node.loop.class is pinned to the main loop, so this run does "
                "not exercise the realtime path and must not be reported as "
                "one.  session=%016llx\n", (unsigned long long)dispatch_token);
        else
            ERR("audio dispatch: processing was redirected off the driver loop "
                "by node.loop.class; this configuration is not validated and "
                "can corrupt audio.  session=%016llx\n",
                (unsigned long long)dispatch_token);
        return;
    }

    WARN("audio dispatch: %s requested %s, effective %s, node.async=%s, "
         "loop \"%s\", rttime=%s, session=%016llx\n", flow, req, eff,
         async ? async : "unset", name,
         rt_time_str(rt_buf, sizeof(rt_buf), rt_soft),
         (unsigned long long)dispatch_token);

    if (want_data && async_on)
        ERR("audio dispatch: RT_PROCESS suppresses node.async, so the "
            "property being set means a stream.rules entry or "
            "PIPEWIRE_PROPS put it back.  An override is fighting the "
            "flag: the callback is on the data thread, but the scheduling "
            "the flag asks for must not be assumed for this run.  "
            "session=%016llx\n", (unsigned long long)dispatch_token);
    else if (!want_data && !async_on)
        ERR("audio dispatch: libpipewire sets node.async for a stream "
            "without RT_PROCESS, so the property being %s means an "
            "override cleared it.  The callback runs on the driver loop, "
            "which is not realtime scheduled, with nothing asking the "
            "graph for slack in front of it.  session=%016llx\n",
            async ? async : "absent", (unsigned long long)dispatch_token);
}

/* Called with the loop lock held. */
static HRESULT pipewire_stream_connect(struct pipewire_stream *stream, const char *device,
                                       const WCHAR *appname)
{
    struct pw_properties *props;
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    SIZE_T period_frames;
    char *app;
    static LONG stream_number;
    char media_name[32];

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
    /* calc_period_bytes already rejects a period_bytes that cannot fit
     * SIZE_T, which on 32-bit also covers the UINT32 PW_KEY_NODE_LATENCY
     * bound.  On 64-bit the frame count can still exceed UINT32_MAX, so
     * refuse rather than narrow. */
    period_frames = stream->period_bytes / stream->frame_size;
    if (period_frames > UINT32_MAX)
    {
        free(app);
        pw_properties_free(props);
        return E_INVALIDARG;
    }
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u",
                       (UINT32)period_frames, stream->info.rate);
    if (device && device[0])
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, device);
    if (stream->dataflow == eCapture &&
        ((stream->flags & AUDCLNT_STREAMFLAGS_LOOPBACK) ||
         (device && device[0] && device_is_sink(device))))
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");

    /* libpipewire copies this into media.name, which applets print next to
     * application.name, so the app name here would show up twice. */
    snprintf(media_name, sizeof(media_name), "audio stream #%d",
             (int)InterlockedIncrement(&stream_number));
    stream->pw = pw_stream_new(pw_core_global, media_name, props);
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

    {
        enum pw_stream_flags flags = PW_STREAM_FLAG_AUTOCONNECT |
                                     PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_INACTIVE;
        enum pw_direction dir = stream->dataflow == eRender ?
                                PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT;
        UINT64 rt_soft, rt_hard;
        HRESULT hr;
        int rc;

        /* Arming RT_PROCESS asks module-rt to promote the data loop thread to
         * a realtime policy, after which the kernel judges it against the
         * process-wide RLIMIT_RTTIME.  A zero budget kills that thread on its
         * first quantum: the kernel tests the hard limit first and sends
         * SIGKILL outright, never reaching the catchable SIGXCPU
         * (posix-cpu-timers.c, check_thread_timers), and module-rt writes the
         * advertised value to soft and hard alike.  No core, no handler,
         * nothing in a crash report.
         *
         * Zero arrives by inheritance.  module-rt on the RTKit path clamps
         * rt.time.soft and rt.time.hard to what the desktop portal
         * advertises, and a portal exporting Realtime with no rtkit behind it
         * advertises zero.  The clamp is process-wide, lowers the hard limit
         * and survives exec, so a launcher that opened audio first hands
         * every process it spawns a budget of zero.
         *
         * Read before arming, not after: the loop runs during negotiation, so
         * a stream checked once it has settled can be killed before it can be
         * reconnected, while a thread that was never promoted cannot. */
        rt_time_limit(&rt_soft, &rt_hard);
        if (rt_render && stream->dataflow == eRender)
        {
            if (rt_soft < RT_TIME_FLOOR_USEC)
            {
                rt_gate_report(rt_soft, rt_hard, FALSE);
                rt_render = FALSE;
            }
            else
                flags |= PW_STREAM_FLAG_RT_PROCESS;
        }

        rc = pw_stream_connect(stream->pw, dir, PW_ID_ANY, flags, params, 1);
        if (rc < 0)
        {
            WARN("pw_stream_connect failed for stream %p: %d.\n", stream, rc);
            return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
        }

        if (FAILED(hr = stream_await_ready(stream)))
            return hr;

        /* Covers the process that started with a healthy budget and was
         * clamped by module-rt while this connect was in flight.  Best
         * effort: the promotion has already happened, so this loses the same
         * race described above, but it bounds the damage to one stream, and
         * clearing rt_render latches the decision for every later stream in
         * the process.  Read and written under the loop lock. */
        rt_time_limit(&rt_soft, &rt_hard);
        if ((flags & PW_STREAM_FLAG_RT_PROCESS) && rt_soft < RT_TIME_FLOOR_USEC)
        {
            rt_gate_report(rt_soft, rt_hard, TRUE);
            rt_render = FALSE;
            rc = pw_stream_disconnect(stream->pw);
            if (rc < 0)
            {
                WARN("pw_stream_disconnect failed for stream %p: %d.\n", stream, rc);
                return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
            }
            rc = pw_stream_connect(stream->pw, dir, PW_ID_ANY,
                                   flags & ~PW_STREAM_FLAG_RT_PROCESS, params, 1);
            if (rc < 0)
            {
                WARN("pw_stream_connect failed for stream %p: %d.\n", stream, rc);
                return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
            }
            if (FAILED(hr = stream_await_ready(stream)))
                return hr;
        }
    }

    report_dispatch_mode(stream, rt_render);

    return S_OK;
}

/* Checked SIZE_T arithmetic for the buffer sizing below.  mmdevapi's
 * adjust_timing never clamps the requested duration down, so the derived
 * byte sizes can overflow SIZE_T, which is 32-bit under WoW64 and faulted
 * the heap on i386.  On overflow the caller rejects the stream with
 * E_OUTOFMEMORY, like winealsa and winepulse. */
static BOOL size_mul(SIZE_T a, SIZE_T b, SIZE_T *out)
{
    if (b && a > SIZE_MAX / b)
        return FALSE;
    *out = a * b;
    return TRUE;
}

static BOOL size_add(SIZE_T a, SIZE_T b, SIZE_T *out)
{
    if (a > SIZE_MAX - b)
        return FALSE;
    *out = a + b;
    return TRUE;
}

/* Size an allocation whose tail array follows head_bytes of audio.  The array
 * must start aligned: head_bytes is a multiple of period_bytes, which is a
 * frame count times a frame size that can be 1, 3 or 6 bytes for packed 8 and
 * 24 bit formats (packed 24 bit stereo at 30 ms gives 7938), and both
 * ACPacket's list pointers and cap_slot's atomics are undefined when
 * misaligned.  Returns the aligned tail offset and the total size, or FALSE
 * on overflow. */
static BOOL aligned_tail(SIZE_T head_bytes, SIZE_T align, SIZE_T count, SIZE_T elem_size,
                         SIZE_T *tail_offs, SIZE_T *total)
{
    SIZE_T tail_bytes;

    if (!size_add(head_bytes, align - 1, tail_offs))
        return FALSE;
    *tail_offs &= ~(align - 1);
    if (!size_mul(count, elem_size, &tail_bytes))
        return FALSE;
    return size_add(*tail_offs, tail_bytes, total);
}

/* Every buffer the stream owns, then the stream.  Shared by the create error
 * path and release so a buffer added later cannot be freed in only one of
 * them; tmp_buffer is allocated lazily by get_render_buffer, so it is always
 * NULL on the create path. */
static void free_stream(struct pipewire_stream *stream)
{
    SIZE_T size;

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
        WARN("Out of memory allocating stream.\n");
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
    stream->flags = params->flags;
    stream->share = params->share;
    stream->mmdev_period_usec = params->period / 10;

    if (!(stream->device = strdup(params->device ? params->device : "")))
    {
        WARN("Out of memory duplicating device name.\n");
        hr = E_OUTOFMEMORY;
        goto exit;
    }

    if (!calc_period_bytes(params->period, stream->info.rate, 10000000,
                           stream->frame_size, &stream->period_bytes))
    {
        WARN("Invalid period: %lld hns at %u Hz.\n", (long long)params->period, stream->info.rate);
        hr = E_INVALIDARG;
        goto exit;
    }

    {
        UINT64 frames;
        BOOL ok = params->duration >= 0 && stream->info.rate &&
                  (UINT64)params->duration <= (UINT64_MAX - 9999999) / stream->info.rate;

        /* Ceiling of duration*rate/1e7, safe only after the check above.  A
         * wrapped product used to yield a tiny frame count and let Initialize
         * succeed with a corrupt buffer. */
        if (ok)
        {
            frames = ((UINT64)params->duration * stream->info.rate + 9999999) / 10000000;
            /* get_buffer_size and the ring cursors narrow the frame count to
             * UINT32, so a buffer only fits if the count does too.  Zero is
             * rejected here too: it would leave real_bufsize_bytes at 0 and
             * the ring cursors divide by that. */
            ok = frames && frames <= UINT32_MAX;
        }
        if (ok)
            ok = size_mul((SIZE_T)frames, stream->frame_size, &bufsize_bytes);
        if (!ok)
        {
            WARN("Buffer duration %lld hns at %u Hz is out of range.\n",
                 (long long)params->duration, stream->info.rate);
            hr = E_OUTOFMEMORY;
            goto exit;
        }
        stream->bufsize_frames = (SIZE_T)frames;
    }

    hr = pipewire_stream_connect(stream, params->device, params->name);
    if (FAILED(hr))
        goto exit;

    stream->rate_connected = stream->info.rate;

    list_init(&stream->packet_free_head);
    list_init(&stream->packet_filled_head);
    if (stream->dataflow == eRender)
    {
        if (!size_mul(bufsize_bytes, 2, &stream->real_bufsize_bytes) ||
            stream->real_bufsize_bytes > UINT32_MAX)
        {
            WARN("Render buffer size too large (%lu frames).\n",
                 (unsigned long)stream->bufsize_frames);
            hr = E_OUTOFMEMORY;
            goto exit;
        }
        size = stream->real_bufsize_bytes;
        if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer,
                                    zero_bits, &size, MEM_COMMIT, PAGE_READWRITE))
        {
            WARN("Out of memory allocating render buffer (%lu bytes).\n", (unsigned long)size);
            hr = E_OUTOFMEMORY;
        }
    }
    else
    {
        UINT32 capture_packets;
        SIZE_T slots_offs, packets_offs, unalign;

        if ((unalign = bufsize_bytes % stream->period_bytes))
        {
            if (!size_add(bufsize_bytes, stream->period_bytes - unalign, &bufsize_bytes))
            {
                WARN("Capture buffer size overflows aligning to the period.\n");
                hr = E_OUTOFMEMORY;
                goto exit;
            }
        }
        stream->bufsize_frames = bufsize_bytes / stream->frame_size;
        stream->real_bufsize_bytes = bufsize_bytes;
        if (stream->real_bufsize_bytes > UINT32_MAX)
        {
            WARN("Capture buffer size too large (%lu frames).\n",
                 (unsigned long)stream->bufsize_frames);
            hr = E_OUTOFMEMORY;
            goto exit;
        }
        capture_packets = stream->real_bufsize_bytes / stream->period_bytes;

        if (!aligned_tail(stream->real_bufsize_bytes, _Alignof(ACPacket),
                          capture_packets, sizeof(ACPacket), &packets_offs, &size))
        {
            WARN("Capture buffer size overflows (%u packets).\n", capture_packets);
            hr = E_OUTOFMEMORY;
            goto exit;
        }
        if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer,
                                    zero_bits, &size, MEM_COMMIT, PAGE_READWRITE))
        {
            WARN("Out of memory allocating capture buffer (%lu bytes).\n", (unsigned long)size);
            hr = E_OUTOFMEMORY;
        }
        else if ((UINT_PTR)((char *)stream->local_buffer + packets_offs) & (_Alignof(ACPacket) - 1))
        {
            WARN("Capture packet array misaligned at %p.\n",
                 (char *)stream->local_buffer + packets_offs);
            hr = E_FAIL;
        }
        else
        {
            ACPacket *cur_packet = (ACPacket *)((char *)stream->local_buffer + packets_offs);
            BYTE *data = stream->local_buffer;
            silence_buffer(stream->info.format, stream->local_buffer, stream->real_bufsize_bytes);
            for (i = 0; i < capture_packets; ++i, ++cur_packet)
            {
                list_add_tail(&stream->packet_free_head, &cur_packet->entry);
                cur_packet->data = data;
                data += stream->period_bytes;
            }

            /* Staging ring carved into period-sized slots with the state
             * array appended. */
            stream->cap_n_slots = stream->real_bufsize_bytes / stream->period_bytes;
            stream->capture_ring_size = stream->real_bufsize_bytes;
            if (!aligned_tail(stream->capture_ring_size, _Alignof(struct cap_slot),
                              stream->cap_n_slots, sizeof(*stream->cap_slots),
                              &slots_offs, &size))
            {
                WARN("Capture ring size overflows (%u slots).\n", stream->cap_n_slots);
                hr = E_OUTOFMEMORY;
                goto exit;
            }
            if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->capture_ring,
                                        zero_bits, &size, MEM_COMMIT, PAGE_READWRITE))
            {
                WARN("Out of memory allocating capture ring (%lu bytes).\n", (unsigned long)size);
                hr = E_OUTOFMEMORY;
            }
            else
            {
                stream->cap_slots = (struct cap_slot *)(stream->capture_ring + slots_offs);
                /* The allocator hands back page-aligned memory and the offset
                 * above is rounded, so this cannot trip; refuse the stream
                 * rather than run the atomics on a misaligned word if it
                 * ever does. */
                if ((UINT_PTR)stream->cap_slots & (_Alignof(struct cap_slot) - 1))
                {
                    WARN("Capture slot array misaligned at %p.\n", stream->cap_slots);
                    hr = E_FAIL;
                }
            }
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
        free_stream(stream);
    }

    if (SUCCEEDED(hr))
    {
        list_add_tail(&g_streams, &stream->entry);
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

    pw_thread_loop_lock(pw_loop_global);
    TRACE("stream %p.\n", stream);
    {
        UINT32 under = __atomic_load_n(&stream->underrun_count, __ATOMIC_RELAXED);
        UINT32 over = __atomic_load_n(&stream->overrun_count, __ATOMIC_RELAXED);
        UINT32 bad = __atomic_load_n(&stream->bad_buffer_count, __ATOMIC_RELAXED);
        UINT32 mark = __atomic_load_n(&stream->cb_mark, __ATOMIC_RELAXED);
        const char *err = stream->last_error[0] ? stream->last_error : NULL;

        stream_journal_flush(stream);
        TRACE("stream %p final cb_seq %u last phase %u last_error %s.\n", stream,
              stream->cb_seq, mark & 3, debugstr_a(err));
        if (under || over || bad)
            WARN("stream %p underran %u times, overran %u times, bad buffers %u, "
                 "cb_seq %u last phase %u last_error %s.\n", stream,
                 under, over, bad, stream->cb_seq, mark & 3, debugstr_a(err));
    }
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
            __atomic_store_n(&period->please_quit, 1, __ATOMIC_RELEASE);
            dead_period = period;
        }
    }
    if (stream->pw)
    {
        spa_hook_remove(&stream->stream_listener);
        pw_stream_destroy(stream->pw);
        stream->pw = NULL;
    }
    list_remove(&stream->entry);
    pw_thread_loop_unlock(pw_loop_global);

    if (dead_period)
    {
        NtWaitForSingleObject(dead_period->timer_thread, FALSE, NULL);
        NtClose(dead_period->timer_thread);
        free(dead_period->device);
        free(dead_period);
    }

    free_stream(stream);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* ----------------------------------------------------------------------
 * Capture slicing + period group timer (8628-style shared timer; one Wine
 * thread per (device, period) group)
 * ---------------------------------------------------------------------- */

/* Claim the oldest published slot, if any, and return it owned by us.  Runs
 * on the Wine timer thread.  Slots are ordered by publication sequence with a
 * wraparound safe comparison, and the claim exchanges the whole state and
 * sequence word, so the slot we end up owning is exactly the one the scan
 * chose rather than whatever the producer put there since. */
static struct cap_slot *cap_claim_slot(struct pipewire_stream *stream, UINT32 *out_idx,
                                      UINT32 *out_seq)
{
    UINT32 pass;

    for (pass = 0; pass < stream->cap_n_slots; pass++)
    {
        UINT32 best = stream->cap_n_slots, best_word = 0, i;

        for (i = 0; i < stream->cap_n_slots; i++)
        {
            UINT32 w = __atomic_load_n(&stream->cap_slots[i].word, __ATOMIC_ACQUIRE);

            if (CAP_STATE(w) != CAP_FULL)
                continue;
            if (best == stream->cap_n_slots || cap_seq_before(w, best_word))
            {
                best = i;
                best_word = w;
            }
        }
        if (best == stream->cap_n_slots)
            return NULL;

        /* Exchange the exact word the scan saw.  If the producer evicted and
         * republished this slot in between, its sequence changed and this
         * fails, so we rescan instead of delivering out of order. */
        if (__atomic_compare_exchange_n(&stream->cap_slots[best].word, &best_word,
                                        CAP_WORD(CAP_SEQ(best_word), CAP_INUSE),
                                        FALSE, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
        {
            *out_idx = best;
            *out_seq = CAP_SEQ(best_word);
            return &stream->cap_slots[best];
        }
        /* The producer evicted it first; rescan. */
    }
    return NULL;
}

/* Slice period-sized packets out of the staging ring.  Runs on the Wine timer
 * thread with the loop lock held: ntdll (QPC) is legal here.  Only ever
 * touches slots it has taken to CAP_INUSE. */
static void pipewire_read(struct pipewire_stream *stream)
{
    struct cap_slot *slot;
    UINT32 idx, seq;

    while ((slot = cap_claim_slot(stream, &idx, &seq)))
    {
        ACPacket *p, *next;
        LARGE_INTEGER stamp, freq;

        if (!(p = (ACPacket *)list_head(&stream->packet_free_head)))
        {
            p = (ACPacket *)list_head(&stream->packet_filled_head);
            if (!p)
            {
                __atomic_store_n(&slot->word, CAP_WORD(seq, CAP_FULL), __ATOMIC_RELEASE);
                return;
            }
            /* Recycle the oldest packet, matching the ring below, and move
             * the discontinuity onto its successor, which is the packet that
             * gaps.  Recycling the newest instead would discard the freshest
             * audio, leave the gap unmarked and clear a flag the application
             * has not read.  The free list is empty here, so the successor
             * always exists. */
            next = (ACPacket *)p->entry.next;
            next->discont = 1;
        }
        else
        {
            stream->held_bytes += stream->period_bytes;
        }
        NtQueryPerformanceCounter(&stamp, &freq);
        p->qpcpos = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
        /* Two kinds of loss, each attributed to the packet that follows it.
         * An eviction removes a sequence number, so an unexpected sequence
         * means audio was lost immediately before this packet.  A clamp or an
         * all-slots-held drop loses audio without removing a sequence, so the
         * producer marks the next slot it publishes.
         *
         * The expected sequence starts at zero, not at whatever arrives
         * first, so opening slots evicted before the timer's first claim are
         * reported too; a first-claim baseline would hide a loss bounded only
         * by how long that claim is delayed. */
        p->discont = slot->disc || seq != stream->cap_next_seq;
        stream->cap_next_seq = (seq + 1) & CAP_SEQ_MAX;
        list_remove(&p->entry);
        list_add_tail(&stream->packet_filled_head, &p->entry);

        memcpy(p->data, stream->capture_ring + (SIZE_T)idx * stream->period_bytes,
               stream->period_bytes);
        __atomic_store_n(&slot->word, CAP_WORD(seq, CAP_FREE), __ATOMIC_RELEASE);
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

    while (!__atomic_load_n(&period->please_quit, __ATOMIC_ACQUIRE))
    {
        INT64 adjust = 0;
        int have_now = 0;

        NtDelayExecution(FALSE, &delay);

        pw_thread_loop_lock(pw_loop_global);

        if (period->timer_stream &&
            (!period->timer_stream->started || !period->timer_stream->pw))
        {
            period->timer_stream = NULL;
            period->grid_valid = FALSE;
        }
        if (!period->timer_stream)
        {
            LIST_FOR_EACH_ENTRY(stream, &period->streams, struct pipewire_stream, period_entry)
            {
                if (stream->started && stream->pw)
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
             * instead, like winepulse's PA_STREAM_INTERPOLATE_TIMING.  A
             * graph that stopped updating is treated as having no clock, so
             * the wakeup free-runs at the nominal period and the grid
             * re-acquires on resume. */
            if (mono_ns >= (UINT64)pwt.now && mono_ns - pwt.now < 1000000000)
            {
                now = pwt.ticks * (UINT64)pwt.rate.num * 1000000 / pwt.rate.denom
                      + (mono_ns - pwt.now) / 1000;
                have_now = 1;
            }
        }

        /* The graph clock steers the wakeup only.  Audio moves on every tick
         * regardless of what the clock reports: gating the drain on clock
         * validity lets a graph that stops updating pwt pin held_bytes
         * forever, so GetCurrentPadding reports a permanently full buffer
         * and get_render_buffer fails with AUDCLNT_E_BUFFER_TOO_LARGE while
         * the process callback drains the ring to silence.  winepulse
         * likewise advances one period per tick regardless of what the
         * server reports (pulse.c:1836). */
        if (!have_now)
            period->grid_valid = FALSE;
        else if (!period->grid_valid)
        {
            period->last_time = now;
            period->grid_valid = TRUE;
        }
        else
        {
            adjust = (INT64)(period->last_time + period->period_usec) - (INT64)now;

            if (adjust > 1000000 || adjust < -1000000)
            {
                /* graph clock stalled or jumped: re-acquire the grid next tick */
                period->grid_valid = FALSE;
                adjust = 0;
            }
            else
            {
                if (adjust > (INT64)(period->period_usec / 2))
                    adjust = period->period_usec / 2;
                else if (adjust < -(INT64)(period->period_usec / 2))
                    adjust = -(INT64)(period->period_usec / 2);

                period->last_time += period->period_usec;
            }
        }
        delay.QuadPart = -((INT64)period->period_usec + adjust) * 10;

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

        LIST_FOR_EACH_ENTRY(stream, &period->streams, struct pipewire_stream, period_entry)
        {
            UINT32 n;

            /* Drain each tick so an error surfaces promptly even when no
             * control op touches this stream. */
            stream_journal_flush(stream);

            if ((n = __atomic_load_n(&stream->underrun_count, __ATOMIC_RELAXED)) &&
                !stream->underrun_logged)
            {
                WARN("stream %p first underrun (count %u).\n", stream, n);
                stream->underrun_logged = TRUE;
            }
            if ((n = __atomic_load_n(&stream->overrun_count, __ATOMIC_RELAXED)) &&
                !stream->overrun_logged)
            {
                WARN("stream %p first overrun (count %u).\n", stream, n);
                stream->overrun_logged = TRUE;
            }
            if ((n = __atomic_load_n(&stream->bad_buffer_count, __ATOMIC_RELAXED)) &&
                !stream->bad_buffer_logged)
            {
                WARN("stream %p first bad process buffer (count %u).\n", stream, n);
                stream->bad_buffer_logged = TRUE;
            }
            if (stream->event)
                NtSetEvent(stream->event, NULL);
        }

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

    /* Publish started before activating: once the node is active the process
     * callback may run, and it must not see a started=FALSE stream that is
     * already producing.  set_active still precedes add_stream_to_period so a
     * failed activation cannot leave the stream linked into the period. */
    __atomic_store_n(&stream->started, TRUE, __ATOMIC_RELEASE);

    if (pw_stream_set_active(stream->pw, true) < 0)
    {
        /* mirrors pulse_start's failed-uncork path */
        __atomic_store_n(&stream->started, FALSE, __ATOMIC_RELEASE);
        WARN("pw_stream_set_active failed for stream %p.\n", stream);
        params->result = E_FAIL;
        pw_thread_loop_unlock(pw_loop_global);
        return STATUS_SUCCESS;
    }

    if (FAILED(params->result = pipewire_add_stream_to_period(stream)))
    {
        if (pw_stream_set_active(stream->pw, false) < 0)
            WARN("pw_stream_set_active(false) rollback failed for stream %p.\n", stream);
        __atomic_store_n(&stream->started, FALSE, __ATOMIC_RELEASE);
        pw_thread_loop_unlock(pw_loop_global);
        return STATUS_SUCCESS;
    }

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

    if (pw_stream_set_active(stream->pw, false) < 0)
        WARN("pw_stream_set_active(false) failed for stream %p.\n", stream);
    /* after set_active(false): its data-loop barrier has drained any
     * in-flight callback, so no reader can still observe started=TRUE */
    __atomic_store_n(&stream->started, FALSE, __ATOMIC_RELEASE);
    pw_thread_loop_unlock(pw_loop_global);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* Run fn with the stream's process callback excluded, from a control path that
 * already holds the thread loop lock.  pw_loop_locked() does this in one call
 * but needs 1.6.0.
 *
 * When the data loop IS the thread loop (every stream without RT_PROCESS) the
 * caller already holds the mutex the callback is dispatched under, so fn runs
 * inline.  A blocking invoke would also complete, but it releases that lock
 * around its ack wait, breaking the operation's atomicity; below 1.6 it
 * releases only one recursion level, so it would hang a nested caller.
 * Otherwise the callback is on a data thread the lock does not hold off, and
 * fn is marshalled to run between dispatches.
 *
 * The invoke can fail on queue allocation.  A negative return means the
 * cursors were not touched, so no caller may publish state assuming the step
 * ran.  -ENODEV is a pw_stream already destroyed by a core reconnect, which
 * callers map to a different HRESULT. */
static int stream_loop_locked(struct pipewire_stream *stream, spa_invoke_func_t fn)
{
    struct pw_loop *data_loop;

    if (!stream->pw || !(data_loop = pw_stream_get_data_loop(stream->pw)))
        return -ENODEV;

    if (data_loop == pw_thread_loop_get_loop(pw_loop_global))
        return fn(data_loop->loop, false, 0, NULL, 0, stream);

    return pw_loop_invoke(data_loop, fn, 0, NULL, 0, true, stream);
}

/* The device is gone only when the stream object itself is; a loop that could
 * not be reached is a transient service failure and the ring is intact. */
static HRESULT ring_op_hresult(int res)
{
    return res == -ENODEV ? AUDCLNT_E_DEVICE_INVALIDATED : AUDCLNT_E_SERVICE_NOT_RUNNING;
}

/* Operations that reach the render ring's read cursor.  Used to report a
 * failed marshal once per stream per operation, so a loop that cannot take an
 * invoke cannot flood the log. */
#define RING_OP_RESET   0
#define RING_OP_RESYNC  1
#define RING_OP_RATE    2

/* Runs on a Wine control thread under the loop lock, so logging is legal. */
static void ring_op_failed(struct pipewire_stream *stream, unsigned int op,
                           const char *what, int res)
{
    if (stream->ring_warned & (1u << op))
        return;
    stream->ring_warned |= 1u << op;
    WARN("stream %p: %s could not be run against the data loop (%d); the "
         "operation was abandoned and the ring is unchanged.\n", stream, what, res);
}

/* The render ring's read cursor belongs to the process callback; these run
 * through stream_loop_locked so the callback cannot be in flight. */
static int do_reset_ring(struct spa_loop *loop, bool async, uint32_t seq,
                         const void *data, size_t size, void *user_data)
{
    struct pipewire_stream *stream = user_data;

    stream->pa_offs_bytes = 0;
    __atomic_store_n(&stream->pa_held_bytes, 0, __ATOMIC_RELEASE);
    return 0;
}

/* Republish the writer's cursor after an overflow.  Reads the Wine-side
 * fields, which the caller holds the thread loop lock over. */
static int do_resync_ring(struct spa_loop *loop, bool async, uint32_t seq,
                          const void *data, size_t size, void *user_data)
{
    struct pipewire_stream *stream = user_data;

    stream->pa_offs_bytes = stream->lcl_offs_bytes;
    __atomic_store_n(&stream->pa_held_bytes, stream->held_bytes, __ATOMIC_RELEASE);
    return 0;
}

static NTSTATUS pipewire_reset(void *args)
{
    struct reset_params *params = args;
    struct pipewire_stream *stream = handle_get_stream(params->stream);
    UINT32 i;

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

    if (pw_stream_flush(stream->pw, false) < 0)
        WARN("pw_stream_flush failed for stream %p.\n", stream);

    if (stream->dataflow == eRender)
    {
        /* Clear the read cursor before the cursors that describe it, so a
         * failed marshal leaves every one of them as it was rather than a
         * zeroed half. */
        int res = stream_loop_locked(stream, do_reset_ring);

        if (res < 0)
        {
            ring_op_failed(stream, RING_OP_RESET, "the render ring reset", res);
            pw_thread_loop_unlock(pw_loop_global);
            params->result = ring_op_hresult(res);
            return STATUS_SUCCESS;
        }
        stream->clock_lastpos = stream->clock_written = 0;
        stream->lcl_offs_bytes = 0;
        stream->held_bytes = 0;
    }
    else
    {
        ACPacket *p;
        stream->clock_written += stream->held_bytes;
        stream->held_bytes = 0;
        /* Stop's set_active(false) barrier drained the callback, so the slots
         * are quiescent here and can be reset without an exchange. */
        for (i = 0; i < stream->cap_n_slots; i++)
        {
            stream->cap_slots[i].word = CAP_WORD(0, CAP_FREE);
            stream->cap_slots[i].disc = FALSE;
        }
        stream->cap_w_slot = 0;
        stream->cap_w_fill = 0;
        stream->cap_w_seq = 0;
        stream->cap_filling = FALSE;
        stream->cap_lost = FALSE;
        stream->cap_next_seq = 0;

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
    {
        WARN("Out of memory allocating tmp buffer (%lu bytes).\n", (unsigned long)bytes);
        return FALSE;
    }

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

    /* UINT64: SIZE_T is 32 bits on the i386 unixlib, where this sum wraps and
     * lets an oversized frame count past the guard. */
    if ((UINT64)(stream->held_bytes / stream->frame_size) + params->frames > stream->bufsize_frames)
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
            WARN("Out of memory for render wrap buffer, stream %p.\n", stream);
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

    /* UINT64: the product is UINT32 x UINT32 and a wrapped value would pass
     * the guard, so a huge written_frames could commit a tiny byte count. */
    if ((UINT64)params->written_frames * stream->frame_size >
        (UINT64)(stream->locked >= 0 ? stream->locked : -stream->locked))
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
    /* Resync before publishing.  Adding first would briefly expose an
     * overfull pa_held_bytes, and the process callback would consume that
     * many bytes against a stale pa_offs_bytes before the repair landed. */
    if (__atomic_load_n(&stream->pa_held_bytes, __ATOMIC_RELAXED) + written_bytes >
        stream->real_bufsize_bytes)
    {
        int res;

        WARN("%p PipeWire buffer overflow.\n", stream);
        if ((res = stream_loop_locked(stream, do_resync_ring)) < 0)
        {
            /* Without the repair the read cursor does not describe the ring,
             * so publishing would hand the callback a byte count its offset
             * does not match.  Take the write back instead: the bytes stay
             * unpublished for the next GetBuffer to overwrite, which loses
             * this buffer but leaves every cursor as it was on entry. */
            stream->held_bytes -= written_bytes;
            ring_op_failed(stream, RING_OP_RESYNC, "the render ring overflow repair", res);
            stream->locked = 0;
            pw_thread_loop_unlock(pw_loop_global);
            params->result = ring_op_hresult(res);
            return STATUS_SUCCESS;
        }
    }
    else
        __atomic_add_fetch(&stream->pa_held_bytes, written_bytes, __ATOMIC_RELEASE);
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
    if (!stream_valid(stream))
    {
        pw_thread_loop_unlock(pw_loop_global);
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }
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
        {
            float v = params->volumes[i] * params->master_volume *
                      params->session_volumes[i];
            __atomic_store(&stream->vol[i], &v, __ATOMIC_RELEASE);
        }
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
    SIZE_T period_bytes;
    int rc;

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
    if (stream->locked)
    {
        hr = AUDCLNT_E_BUFFER_OPERATION_PENDING;
        goto exit;
    }

    /* PulseAudio rejects rates outside [1, 48000 * 8] in
     * pa_stream_update_sample_rate; mirror winepulse's observable failure
     * code for rates the resampler cannot honor (the negated comparison
     * also catches NaN). */
    if (!(params->rate >= 1.0f && params->rate <= (float)PW_MAX_RATE))
    {
        WARN("Unsupported sample rate %u.\n", (unsigned)params->rate);
        hr = E_OUTOFMEMORY;
        goto exit;
    }

    if (!calc_period_bytes(stream->mmdev_period_usec, (UINT32)params->rate, 1000000,
                           stream->frame_size, &period_bytes))
    {
        WARN("Invalid period after rate change: %llu us at %u Hz.\n",
             (unsigned long long)stream->mmdev_period_usec, (unsigned)params->rate);
        hr = E_INVALIDARG;
        goto exit;
    }

    /* Empty the ring before changing the rate.  The control cannot be put
     * back once set, so the step that can fail comes first: if the read
     * cursor cannot be reached, nothing has moved and the stream is still
     * coherent at the old rate.
     *
     * The storage is deliberately not cleared: occupancy zero already makes
     * the callback emit silence and GetBuffer silences what it hands out, so
     * clearing bytes the callback may be reading would race for no gain. */
    if ((rc = stream_loop_locked(stream, do_reset_ring)) < 0)
    {
        ring_op_failed(stream, RING_OP_RATE, "the render ring reset for a rate change", rc);
        hr = ring_op_hresult(rc);
        goto exit;
    }
    stream->clock_lastpos = stream->clock_written = 0;
    stream->lcl_offs_bytes = 0;
    stream->held_bytes = 0;

    ratio = params->rate / (float)stream->rate_connected;
    if (pw_stream_set_control(stream->pw, SPA_PROP_rate, 1, &ratio, 0) < 0)
    {
        /* The ring is already empty.  On failure the cursors still describe
         * the old rate and the buffered audio is gone. */
        WARN("pw_stream_set_control(rate) failed for stream %p.\n", stream);
        hr = E_NOTIMPL;
        goto exit;
    }

    pw_stream_flush(stream->pw, false);

    stream->period_bytes = period_bytes;
    stream->info.rate = params->rate;

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
}

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
        case VT_CLSID:
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
