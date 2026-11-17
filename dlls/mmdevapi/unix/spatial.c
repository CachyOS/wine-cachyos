/*
 * Spatial audio HRTF rendering through Steam Audio (libphonon)
 *
 * Copyright 2026 M0n7y5
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

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <dlfcn.h>
#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/unixlib.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(spatial);

/* Minimal Steam Audio 4.x C API declarations, from the documented interface
 * (github.com/ValveSoftware/steam-audio, Apache-2.0).  The library is
 * dlopen'd; phonon.h is not shipped. */

typedef void *IPLContext;
typedef void *IPLHRTF;
typedef void *IPLBinauralEffect;

typedef struct {
    unsigned int version;          /* STEAMAUDIO_VERSION: (major << 16) | (minor << 8) | patch */
    void *logCallback;
    void *allocateCallback;
    void *freeCallback;
    int simdLevel;                 /* max allowed: 0 SSE2 .. 4 AVX512 */
    int flags;
} IPLContextSettings;

typedef struct {
    int samplingRate;
    int frameSize;
} IPLAudioSettings;

typedef struct {
    int type;                      /* 0 = IPL_HRTFTYPE_DEFAULT */
    const char *sofaFileName;
    const unsigned char *sofaData;
    int sofaDataSize;
    float volume;
    int normType;                  /* 0 = IPL_HRTFNORMTYPE_NONE */
} IPLHRTFSettings;

typedef struct {
    int numChannels;
    int numSamples;
    float **data;                  /* deinterleaved */
} IPLAudioBuffer;

typedef struct {
    float x, y, z;
} IPLVector3;

typedef struct {
    IPLHRTF hrtf;
} IPLBinauralEffectSettings;

typedef struct {
    IPLVector3 direction;          /* unit vector, listener space: +x right, +y up, -z ahead */
    int interpolation;             /* 0 = IPL_HRTFINTERPOLATION_NEAREST */
    float spatialBlend;
    IPLHRTF hrtf;
    float *peakDelays;
} IPLBinauralEffectParams;

#define STEAMAUDIO_VERSION ((4u << 16) | (8u << 8) | 1u)

static int (*p_iplContextCreate)(IPLContextSettings *, IPLContext *);
static void (*p_iplContextRelease)(IPLContext *);
static int (*p_iplHRTFCreate)(IPLContext, IPLAudioSettings *, IPLHRTFSettings *, IPLHRTF *);
static void (*p_iplHRTFRelease)(IPLHRTF *);
static int (*p_iplBinauralEffectCreate)(IPLContext, IPLAudioSettings *,
                                        IPLBinauralEffectSettings *, IPLBinauralEffect *);
static int (*p_iplBinauralEffectApply)(IPLBinauralEffect, IPLBinauralEffectParams *,
                                       IPLAudioBuffer *, IPLAudioBuffer *);
static int (*p_iplBinauralEffectGetTailSize)(IPLBinauralEffect);
static void (*p_iplBinauralEffectRelease)(IPLBinauralEffect *);

static pthread_once_t phonon_once = PTHREAD_ONCE_INIT;
static void *phonon_handle;

/* phonon.h documents iplHRTFCreate as not thread-safe, and two streams can
 * initialise at once. Held across the paired release too, since the same
 * objects are being torn down. */
static pthread_mutex_t engine_lock = PTHREAD_MUTEX_INITIALIZER;

static void phonon_load(void)
{
    const char *path = getenv("WINE_SPATIAL_PHONON");

    if (path && path[0])
    {
        if (!(phonon_handle = dlopen(path, RTLD_NOW)))
            /* the override may name the wrong ELF class in a split wow64 setup */
            WARN("Could not load %s: %s\n", path, dlerror());
        else
            TRACE("Loaded %s.\n", path);
    }
    if (!phonon_handle)
    {
        if (!(phonon_handle = dlopen("libphonon.so", RTLD_NOW)))
        {
            WARN("Could not load libphonon.so: %s\n", dlerror());
            return;
        }
        TRACE("Loaded libphonon.so.\n");
    }

#define LOAD_FUNC(f) \
    if (!(p_##f = dlsym(phonon_handle, #f))) goto fail
    LOAD_FUNC(iplContextCreate);
    LOAD_FUNC(iplContextRelease);
    LOAD_FUNC(iplHRTFCreate);
    LOAD_FUNC(iplHRTFRelease);
    LOAD_FUNC(iplBinauralEffectCreate);
    LOAD_FUNC(iplBinauralEffectApply);
    LOAD_FUNC(iplBinauralEffectRelease);
#undef LOAD_FUNC

    /* Not in the list above, which fails the whole load on a missing symbol:
     * without it the tail countdown falls back to a fixed two blocks. */
    p_iplBinauralEffectGetTailSize = dlsym(phonon_handle, "iplBinauralEffectGetTailSize");

    return;

fail:
    WARN("Incomplete libphonon.so, disabling HRTF.\n");
    dlclose(phonon_handle);
    phonon_handle = NULL;
}

/* One engine per stream; the PE side holds the stream lock across every call
 * that touches it, so nothing here locks.  Streams run concurrently, so this
 * struct holds no shared state.  init and release are exclusive by
 * construction: the stream is unpublished or already at refcount zero. */
struct spatial_engine
{
    IPLContext ctx;
    IPLHRTF hrtf;
    IPLAudioSettings audio;
    IPLBinauralEffect effects[SPATIAL_MAX_SLOTS];
    /* per-slot count of silent blocks still to convolve; skipping a live tail clicks */
    unsigned char tail_pending[SPATIAL_MAX_SLOTS];
    unsigned char tail_blocks;  /* tail_pending reload, 0 until libphonon can report it */
    float *scratch;     /* 2 * frames, deinterleaved L then R */
};

static NTSTATUS spatial_init(void *args)
{
    struct spatial_init_params *params = args;
    IPLContextSettings ctx_settings;
    IPLHRTFSettings hrtf_settings;
    struct spatial_engine *engine;

    pthread_once(&phonon_once, phonon_load);
    if (!phonon_handle) return STATUS_NOT_SUPPORTED;

    /* IPLAudioSettings takes both as int, and scratch is 2 * frames floats;
     * the size_t cast below is not enough on a 32-bit unix side */
    if (!params->frames || !params->rate || params->rate > INT_MAX ||
        params->frames > INT_MAX / (UINT)(2 * sizeof(float)))
        return STATUS_INVALID_PARAMETER;

    if (!(engine = calloc(1, sizeof(*engine)))) return STATUS_NO_MEMORY;
    if (!(engine->scratch = calloc(2 * (size_t)params->frames, sizeof(float))))
    {
        free(engine);
        return STATUS_NO_MEMORY;
    }

    engine->audio.samplingRate = params->rate;
    engine->audio.frameSize = params->frames;

    memset(&ctx_settings, 0, sizeof(ctx_settings));
    ctx_settings.version = STEAMAUDIO_VERSION;
    /* AVX512 measures no faster than AVX2 here and can downclock the core. */
    ctx_settings.simdLevel = 3; /* IPL_SIMDLEVEL_AVX2 */
    pthread_mutex_lock(&engine_lock);
    if (p_iplContextCreate(&ctx_settings, &engine->ctx))
    {
        pthread_mutex_unlock(&engine_lock);
        WARN("iplContextCreate failed.\n");
        goto fail;
    }

    memset(&hrtf_settings, 0, sizeof(hrtf_settings));
    hrtf_settings.type = 0;
    hrtf_settings.volume = 1.0f;
    if (p_iplHRTFCreate(engine->ctx, &engine->audio, &hrtf_settings, &engine->hrtf))
    {
        pthread_mutex_unlock(&engine_lock);
        WARN("iplHRTFCreate failed.\n");
        goto fail;
    }
    pthread_mutex_unlock(&engine_lock);

    TRACE("engine %p: %u Hz, %u frames.\n", engine, params->rate, params->frames);
    params->handle = (UINT_PTR)engine;
    return STATUS_SUCCESS;

fail:
    if (engine->ctx)
    {
        pthread_mutex_lock(&engine_lock);
        p_iplContextRelease(&engine->ctx);
        pthread_mutex_unlock(&engine_lock);
    }
    free(engine->scratch);
    free(engine);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS spatial_release(void *args)
{
    struct spatial_release_params *params = args;
    struct spatial_engine *engine = (struct spatial_engine *)(UINT_PTR)params->handle;
    unsigned int i;

    for (i = 0; i < SPATIAL_MAX_SLOTS; i++)
        if (engine->effects[i]) p_iplBinauralEffectRelease(&engine->effects[i]);
    pthread_mutex_lock(&engine_lock);
    p_iplHRTFRelease(&engine->hrtf);
    p_iplContextRelease(&engine->ctx);
    pthread_mutex_unlock(&engine_lock);
    free(engine->scratch);
    free(engine);
    return STATUS_SUCCESS;
}

static NTSTATUS spatial_object_add(void *args)
{
    struct spatial_object_add_params *params = args;
    struct spatial_engine *engine = (struct spatial_engine *)(UINT_PTR)params->handle;
    IPLBinauralEffectSettings settings;
    unsigned int i;

    for (i = 0; i < SPATIAL_MAX_SLOTS; i++)
        if (!engine->effects[i]) break;
    if (i == SPATIAL_MAX_SLOTS) return STATUS_INSUFFICIENT_RESOURCES;

    settings.hrtf = engine->hrtf;
    if (p_iplBinauralEffectCreate(engine->ctx, &engine->audio, &settings, &engine->effects[i]))
    {
        WARN("iplBinauralEffectCreate failed.\n");
        return STATUS_NOT_SUPPORTED;
    }

    engine->tail_pending[i] = 0;
    params->slot = i;
    return STATUS_SUCCESS;
}

static NTSTATUS spatial_object_remove(void *args)
{
    struct spatial_object_remove_params *params = args;
    struct spatial_engine *engine = (struct spatial_engine *)(UINT_PTR)params->handle;

    if (params->slot >= SPATIAL_MAX_SLOTS || !engine->effects[params->slot])
        return STATUS_INVALID_PARAMETER;
    p_iplBinauralEffectRelease(&engine->effects[params->slot]);
    engine->effects[params->slot] = NULL;
    return STATUS_SUCCESS;
}

static int buffer_is_silent(const float *p, UINT frames)
{
    UINT i;

    for (i = 0; i < frames; i++)
        if (p[i] != 0.0f) return 0;
    return 1;
}

/* iplBinauralEffectApply never stops returning TAILREMAINING, so the tail length
 * comes from iplBinauralEffectGetTailSize; 2 blocks covers the 544-sample tail at
 * 480 frames when the size is unavailable. */
#define SPATIAL_TAIL_BLOCKS 2

static unsigned char tail_block_count(IPLBinauralEffect effect, int frames)
{
    int tail;

    if (!p_iplBinauralEffectGetTailSize) return SPATIAL_TAIL_BLOCKS;
    tail = p_iplBinauralEffectGetTailSize(effect);
    /* the countdown is one byte per slot, and 255 blocks is not an HRTF tail */
    if (tail <= 0 || tail / frames > 254) return SPATIAL_TAIL_BLOCKS;
    return (tail + frames - 1) / frames;
}

static NTSTATUS spatial_mix(void *args)
{
    struct spatial_mix_params *params = args;
    struct spatial_engine *engine = (struct spatial_engine *)(UINT_PTR)params->handle;
    const struct spatial_mix_object *objs =
            (const struct spatial_mix_object *)(UINT_PTR)params->objects;
    float *out_l = (float *)(UINT_PTR)params->out_l;
    float *out_r = (float *)(UINT_PTR)params->out_r;
    unsigned int i, f;

    if (params->frames != (UINT)engine->audio.frameSize ||
        params->count > SPATIAL_MAX_SLOTS)
        return STATUS_INVALID_PARAMETER;

    for (i = 0; i < params->count; i++)
    {
        IPLBinauralEffectParams effect_params;
        IPLAudioBuffer in, out;
        float *in_data = (float *)(UINT_PTR)objs[i].buffer;
        float *out_data[2];
        float len;
        int silent;

        if (objs[i].slot >= SPATIAL_MAX_SLOTS || !engine->effects[objs[i].slot])
            continue;

        silent = buffer_is_silent(in_data, params->frames);
        if (silent && !engine->tail_pending[objs[i].slot])
            continue;

        len = sqrtf(objs[i].pos[0] * objs[i].pos[0] +
                    objs[i].pos[1] * objs[i].pos[1] +
                    objs[i].pos[2] * objs[i].pos[2]);
        if (len > 0.0f)
        {
            /* ISpatialAudioObject::SetPosition and Steam Audio use the same
             * listener-space convention (+x right, +y up, -z ahead) */
            effect_params.direction.x = objs[i].pos[0] / len;
            effect_params.direction.y = objs[i].pos[1] / len;
            effect_params.direction.z = objs[i].pos[2] / len;
        }
        else
        {
            effect_params.direction.x = 0.0f;
            effect_params.direction.y = 0.0f;
            effect_params.direction.z = -1.0f;
        }
        effect_params.interpolation = 0;
        effect_params.spatialBlend = 1.0f;
        effect_params.hrtf = engine->hrtf;
        effect_params.peakDelays = NULL;

        in.numChannels = 1;
        in.numSamples = params->frames;
        in.data = &in_data;

        /* Steam Audio effects cannot process in place; the output is always
         * the separate scratch buffer. */
        out_data[0] = engine->scratch;
        out_data[1] = engine->scratch + params->frames;
        out.numChannels = 2;
        out.numSamples = params->frames;
        out.data = out_data;

        p_iplBinauralEffectApply(engine->effects[objs[i].slot], &effect_params, &in, &out);
        if (silent)
            engine->tail_pending[objs[i].slot]--;
        else
        {
            /* the size reads 0 until the effect has convolved a block, so it
             * cannot be asked for at spatial_object_add time */
            if (!engine->tail_blocks)
                engine->tail_blocks = tail_block_count(engine->effects[objs[i].slot],
                                                       engine->audio.frameSize);
            engine->tail_pending[objs[i].slot] = engine->tail_blocks;
        }

        for (f = 0; f < params->frames; f++)
        {
            out_l[f] += out_data[0][f] * objs[i].volume;
            out_r[f] += out_data[1][f] * objs[i].volume;
        }
    }

    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    spatial_init,
    spatial_release,
    spatial_object_add,
    spatial_object_remove,
    spatial_mix,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == spatial_funcs_count);

#ifdef _WIN64

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    spatial_init,
    spatial_release,
    spatial_object_add,
    spatial_object_remove,
    spatial_mix,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == spatial_funcs_count);

#endif /* _WIN64 */
