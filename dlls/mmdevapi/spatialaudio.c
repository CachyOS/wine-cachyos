/*
 * Copyright 2020 Andrew Eikum for CodeWeavers
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

#define COBJMACROS

#include <stdarg.h>
#include <math.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "ole2.h"
#include "mmdeviceapi.h"
#include "mmsystem.h"
#include "audioclient.h"
#include "endpointvolume.h"
#include "audiopolicy.h"
#include "spatialaudioclient.h"

#include "mmdevapi_private.h"

#include "wine/unixlib.h"
#include "unix/unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(mmdevapi);
WINE_DECLARE_DEBUG_CHANNEL(spatial);

#define SPATIAL_MAX_DYNAMIC_OBJECTS 112  /* matches Windows Sonic for Headphones */

static BOOL spatial_option_enabled(const WCHAR *value)
{
    return *value == 'y' || *value == 'Y' || *value == 't' || *value == 'T' || *value == '1';
}

static BOOL spatial_unix_init(void)
{
    static LONG status = -1;
    if (status == -1) InterlockedExchange(&status, __wine_init_unix_call());
    return !status;
}

static UINT get_spatial_dynamic_budget(void)
{
    /* an over-long env value or an empty registry value leaves buf untouched
     * while still reporting success */
    WCHAR buf[16] = {0};
    DWORD size = sizeof(buf), type = REG_NONE;
    HKEY key;
    BOOL enabled = FALSE;
    const char *source = "registry";

    if(GetEnvironmentVariableW(L"WINE_SPATIAL_SOUND", buf, ARRAY_SIZE(buf))){
        enabled = spatial_option_enabled(buf);
        source = "environment";
    }else if(RegOpenKeyW(HKEY_CURRENT_USER, L"Software\\Wine\\mmdevapi", &key) == ERROR_SUCCESS){
        if(RegQueryValueExW(key, L"SpatialSound", 0, &type, (BYTE*)buf, &size) == ERROR_SUCCESS){
            if(type == REG_SZ || type == REG_EXPAND_SZ){
                enabled = spatial_option_enabled(buf);
            }else if(type == REG_DWORD && size == sizeof(DWORD)){
                /* a DWORD reads as L"\x0001" as a string, which is neither
                 * "1" nor "y", so it would otherwise silently mean "off" */
                DWORD value;
                memcpy(&value, buf, sizeof(value));
                enabled = value != 0;
            }else
                WARN("Ignoring SpatialSound of type %lu.\n", type);
        }
        RegCloseKey(key);
    }

    TRACE_(spatial)("Spatial sound %s (%s), dynamic object budget %u.\n",
            enabled ? "enabled" : "disabled", source,
            enabled ? SPATIAL_MAX_DYNAMIC_OBJECTS : 0);

    return enabled ? SPATIAL_MAX_DYNAMIC_OBJECTS : 0;
}

/* Index of a static bed channel in static_object_map[], or ~0 for anything
 * that has no channel: None, Dynamic, a multi-bit value, or a bit above
 * BackCenter. The argument is app-supplied and AudioObjectType is signed. */
static UINT32 AudioObjectType_to_index(AudioObjectType type)
{
    UINT32 bits = type, o = 0;

    if(bits < AudioObjectType_FrontLeft || bits > AudioObjectType_BackCenter ||
            (bits & (bits - 1)))
        return ~0;
    while(bits >>= 1)
        ++o;
    return o - 1;
}

static const char *debugstr_fmtex(const WAVEFORMATEX *fmt)
{
    static char buf[2048];

    if (!fmt)
    {
        strcpy(buf, "(null)");
    }
    else if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;
        snprintf(buf, sizeof(buf), "tag: 0x%x (%s), ch: %u (mask: 0x%lx), rate: %lu, depth: %u",
                fmt->wFormatTag, debugstr_guid(&fmtex->SubFormat),
                fmt->nChannels, fmtex->dwChannelMask, fmt->nSamplesPerSec,
                fmt->wBitsPerSample);
    }
    else
    {
        snprintf(buf, sizeof(buf), "tag: 0x%x, ch: %u, rate: %lu, depth: %u",
                fmt->wFormatTag, fmt->nChannels, fmt->nSamplesPerSec,
                fmt->wBitsPerSample);
    }
    return buf;
}

/* Windows treats WAVE_FORMAT_EXTENSIBLE and the plain tag as the same format. */
static WORD object_format_tag(const WAVEFORMATEX *fmt)
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;

    if(fmt->wFormatTag != WAVE_FORMAT_EXTENSIBLE)
        return fmt->wFormatTag;
    if(fmt->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        return WAVE_FORMAT_UNKNOWN;
    if(IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
        return WAVE_FORMAT_IEEE_FLOAT;
    if(IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))
        return WAVE_FORMAT_PCM;
    return WAVE_FORMAT_UNKNOWN;
}

static BOOL object_formats_compatible(const WAVEFORMATEX *fmt1, const WAVEFORMATEX *fmt2)
{
    WORD tag = object_format_tag(fmt1);

    /* Shape is validated first, by validate_wave_format_ex: Windows reports a
     * malformed format and an unsupported one with different HRESULTs. */
    return tag != WAVE_FORMAT_UNKNOWN &&
           tag == object_format_tag(fmt2) &&
           fmt1->nChannels == fmt2->nChannels &&
           fmt1->nSamplesPerSec == fmt2->nSamplesPerSec &&
           fmt1->wBitsPerSample == fmt2->wBitsPerSample;
}

/* Windows' shape validator, run before any support test: only the extensible
 * arm checks wBitsPerSample and nBlockAlign; dwChannelMask is never read. */
static HRESULT validate_wave_format_ex(const WAVEFORMATEX *fmt)
{
    UINT32 prod;

    if(!fmt)
        return E_POINTER;

    if(!fmt->nChannels || !fmt->nSamplesPerSec || !fmt->nAvgBytesPerSec ||
            !fmt->nBlockAlign || fmt->cbSize > 0x400)
        return E_INVALIDARG;

    /* 32-bit wrapping product, positive for every depth the arms below
     * accept, so the divide needs no sign handling. */
    prod = (UINT32)fmt->nChannels * fmt->wBitsPerSample;

    if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE){
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)fmt;
        BOOL pcm;

        if(fmt->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
            return E_INVALIDARG;

        /* Only the two known subformats are examined; anything else is
         * accepted on the prologue alone, so none of the tests below run for
         * it. */
        pcm = IsEqualGUID(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM);
        if(!pcm && !IsEqualGUID(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            return S_OK;

        if(pcm){
            if(fmt->wBitsPerSample != 8 && fmt->wBitsPerSample != 16 &&
                    fmt->wBitsPerSample != 24 && fmt->wBitsPerSample != 32)
                return E_INVALIDARG;
        }else if(fmt->wBitsPerSample != 32 && fmt->wBitsPerSample != 64)
            return E_INVALIDARG;

        if(!ext->Samples.wValidBitsPerSample ||
                fmt->wBitsPerSample < ext->Samples.wValidBitsPerSample)
            return E_INVALIDARG;

        if(fmt->nAvgBytesPerSec != ((prod * fmt->nSamplesPerSec) >> 3))
            return E_INVALIDARG;

        if(fmt->nBlockAlign != prod / 8)
            return E_INVALIDARG;

        return S_OK;
    }

    if(fmt->wFormatTag != WAVE_FORMAT_PCM && fmt->wFormatTag != WAVE_FORMAT_IEEE_FLOAT)
        return S_OK;

    if(fmt->cbSize)
        return E_INVALIDARG;

    if(fmt->wBitsPerSample & 7)
        return E_INVALIDARG;

    if(fmt->nChannels > 2)
        return E_INVALIDARG;

    if(fmt->nAvgBytesPerSec != ((prod * fmt->nSamplesPerSec) >> 3))
        return E_INVALIDARG;

    return S_OK;
}

typedef struct SpatialAudioImpl SpatialAudioImpl;
typedef struct SpatialAudioStreamImpl SpatialAudioStreamImpl;
typedef struct SpatialAudioObjectImpl SpatialAudioObjectImpl;

struct SpatialAudioObjectImpl {
    ISpatialAudioObject ISpatialAudioObject_iface;
    LONG ref;

    SpatialAudioStreamImpl *sa_stream;
    AudioObjectType type;
    UINT32 static_idx;

    float *buf;

    BOOL invalidated;
    BOOL updated;
    /* Missing a pass only ends an object after its first GetBuffer, so a
     * pre-activated pool of unwritten objects stays valid. */
    BOOL started;
    /* SetEndOfStream invalidates the object for the caller at once, but its
     * last buffer still owes one pass through the mixer */
    BOOL eos_pending;
    UINT32 eos_frames;

    float pos[3];
    float volume;
    UINT engine_slot;

    struct list entry;
};

struct SpatialAudioStreamImpl {
    ISpatialAudioObjectRenderStream ISpatialAudioObjectRenderStream_iface;
    LONG ref;
    CRITICAL_SECTION lock;

    SpatialAudioImpl *sa_client;
    SpatialAudioObjectRenderStreamActivationParams params;

    IAudioClient *client;
    IAudioRenderClient *render;

    UINT32 period_frames, update_frames;
    WAVEFORMATEXTENSIBLE stream_fmtex;

    float *buf;

    UINT32 static_object_map[17];
    UINT32 dyn_max, dyn_live;
    BOOL virtualize_bed;
    UINT32 dyn_left, dyn_right;

    UINT64 engine;
    float *hrtf_buf;

    struct list objects;
};

struct SpatialAudioImpl {
    ISpatialAudioClient ISpatialAudioClient_iface;
    IAudioFormatEnumerator IAudioFormatEnumerator_iface;
    IMMDevice *mmdev;
    LONG ref;
    WAVEFORMATEXTENSIBLE object_fmtex;
    UINT dyn_budget;
};

static inline SpatialAudioObjectImpl *impl_from_ISpatialAudioObject(ISpatialAudioObject *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioObjectImpl, ISpatialAudioObject_iface);
}

static inline SpatialAudioStreamImpl *impl_from_ISpatialAudioObjectRenderStream(ISpatialAudioObjectRenderStream *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioStreamImpl, ISpatialAudioObjectRenderStream_iface);
}

static inline SpatialAudioImpl *impl_from_ISpatialAudioClient(ISpatialAudioClient *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioImpl, ISpatialAudioClient_iface);
}

static inline SpatialAudioImpl *impl_from_IAudioFormatEnumerator(IAudioFormatEnumerator *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioImpl, IAudioFormatEnumerator_iface);
}

static HRESULT WINAPI SAO_QueryInterface(ISpatialAudioObject *iface,
        REFIID riid, void **ppv)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectBase) ||
            IsEqualIID(riid, &IID_ISpatialAudioObject)) {
        *ppv = &This->ISpatialAudioObject_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAO_AddRef(ISpatialAudioObject *iface)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAO_Release(ISpatialAudioObject *iface)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if(!ref){
        EnterCriticalSection(&This->sa_stream->lock);
        list_remove(&This->entry);
        if(This->type == AudioObjectType_Dynamic)
            This->sa_stream->dyn_live--;
        /* bed channels take a slot too when the bed is virtualized */
        if(This->sa_stream->engine && This->engine_slot != ~0){
            struct spatial_object_remove_params params;
            params.handle = This->sa_stream->engine;
            params.slot = This->engine_slot;
            WINE_UNIX_CALL(unix_spatial_object_remove, &params);
        }
        LeaveCriticalSection(&This->sa_stream->lock);

        ISpatialAudioObjectRenderStream_Release(&This->sa_stream->ISpatialAudioObjectRenderStream_iface);
        free(This->buf);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAO_GetBuffer(ISpatialAudioObject *iface,
        BYTE **buffer, UINT32 *bytes)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p, %p)\n", This, buffer, bytes);

    if(!buffer || !bytes)
        return E_POINTER;

    EnterCriticalSection(&This->sa_stream->lock);

    if(This->sa_stream->update_frames == ~0){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if(This->invalidated){
        *buffer = NULL;
        *bytes = 0;
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }

    This->started = This->updated = TRUE;

    *buffer = (BYTE *)This->buf;
    *bytes = This->sa_stream->update_frames *
        This->sa_stream->sa_client->object_fmtex.Format.nBlockAlign;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_SetEndOfStream(ISpatialAudioObject *iface, UINT32 frames)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%u)\n", This, frames);

    EnterCriticalSection(&This->sa_stream->lock);

    if(This->sa_stream->update_frames == ~0){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if(frames > This->sa_stream->update_frames){
        LeaveCriticalSection(&This->sa_stream->lock);
        /* Windows rejects an over-long count and keeps the object active. */
        WARN("End of stream past the buffer: %u of %u frames.\n",
                frames, This->sa_stream->update_frames);
        return E_INVALIDARG;
    }

    if(!This->invalidated){
        This->eos_frames = frames;
        This->eos_pending = TRUE;
    }
    This->invalidated = TRUE;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_IsActive(ISpatialAudioObject *iface, BOOL *active)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p)\n", This, active);

    if(!active)
        return E_POINTER;

    EnterCriticalSection(&This->sa_stream->lock);
    *active = !This->invalidated;
    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_GetAudioObjectType(ISpatialAudioObject *iface,
        AudioObjectType *type)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p)\n", This, type);

    if(!type)
        return E_POINTER;

    *type = This->type;

    return S_OK;
}

static HRESULT WINAPI SAO_SetPosition(ISpatialAudioObject *iface, float x,
        float y, float z)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%f, %f, %f)\n", This, x, y, z);

    if(This->type != AudioObjectType_Dynamic)
        return SPTLAUDCLNT_E_PROPERTY_NOT_SUPPORTED;

    EnterCriticalSection(&This->sa_stream->lock);

    if(This->sa_stream->update_frames == ~0){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if(This->invalidated){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }

    This->pos[0] = x;
    This->pos[1] = y;
    This->pos[2] = z;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_SetVolume(ISpatialAudioObject *iface, float vol)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%f)\n", This, vol);

    /* Windows checks the range first, before the destroyed test, and applies
     * volume to static objects too. NaN is unordered against both bounds. */
    if(vol < 0.0f || vol > 1.0f)
        return E_INVALIDARG;

    EnterCriticalSection(&This->sa_stream->lock);

    if(This->sa_stream->update_frames == ~0){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if(This->invalidated){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }

    /* NaN passes the range test; keep the gain finite. */
    This->volume = isfinite(vol) ? vol : 0.0f;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static ISpatialAudioObjectVtbl ISpatialAudioObject_vtbl = {
    SAO_QueryInterface,
    SAO_AddRef,
    SAO_Release,
    SAO_GetBuffer,
    SAO_SetEndOfStream,
    SAO_IsActive,
    SAO_GetAudioObjectType,
    SAO_SetPosition,
    SAO_SetVolume,
};

static HRESULT WINAPI SAORS_QueryInterface(ISpatialAudioObjectRenderStream *iface,
        REFIID riid, void **ppv)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStreamBase) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStream)) {
        *ppv = &This->ISpatialAudioObjectRenderStream_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAORS_AddRef(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAORS_Release(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if(!ref){
        IAudioClient_Stop(This->client);
        if(This->engine){
            struct spatial_release_params params;
            params.handle = This->engine;
            WINE_UNIX_CALL(unix_spatial_release, &params);
        }
        free(This->hrtf_buf);
        if(This->update_frames != ~0 && This->update_frames > 0)
            IAudioRenderClient_ReleaseBuffer(This->render, This->update_frames, 0);
        IAudioRenderClient_Release(This->render);
        IAudioClient_Release(This->client);
        if(This->params.NotifyObject)
            ISpatialAudioObjectRenderStreamNotify_Release(This->params.NotifyObject);
        free((void*)This->params.ObjectFormat);
        CloseHandle(This->params.EventHandle);
        DeleteCriticalSection(&This->lock);
        ISpatialAudioClient_Release(&This->sa_client->ISpatialAudioClient_iface);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAORS_GetAvailableDynamicObjectCount(
        ISpatialAudioObjectRenderStream *iface, UINT32 *count)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    TRACE("(%p)->(%p)\n", This, count);

    if(!count)
        return E_POINTER;

    EnterCriticalSection(&This->lock);
    /* Same source as Begin's out parameter, which Windows requires. */
    *count = This->dyn_max;
    LeaveCriticalSection(&This->lock);
    return S_OK;
}

static HRESULT WINAPI SAORS_GetService(ISpatialAudioObjectRenderStream *iface,
        REFIID riid, void **service)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), service);

    if(!service)
        return E_POINTER;

    *service = NULL;

    /* Windows serves these four from the stream's IAudioClient and answers
     * E_NOINTERFACE otherwise. IAudioRenderClient is withheld: the stream owns
     * the render packet during a pass. */
    if(IsEqualIID(riid, &IID_IAudioClock) ||
            IsEqualIID(riid, &IID_IAudioStreamVolume) ||
            IsEqualIID(riid, &IID_IAudioSessionControl) ||
            IsEqualIID(riid, &IID_ISimpleAudioVolume))
        return IAudioClient_GetService(This->client, riid, service);

    WARN("Unsupported service %s.\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static HRESULT WINAPI SAORS_Start(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    hr = IAudioClient_Start(This->client);
    if(FAILED(hr)){
        WARN("IAudioClient::Start failed: %08lx\n", hr);
        if(hr == AUDCLNT_E_NOT_STOPPED)
            return SPTLAUDCLNT_E_STREAM_NOT_STOPPED;
        return hr;
    }

    TRACE_(spatial)("stream %p started, rendering through %s.\n", This,
            This->engine ? "the Steam Audio HRTF engine" :
            (This->virtualize_bed ? "stereo panning (HRTF unavailable)"
                                  : "multichannel passthrough"));
    return S_OK;
}

static HRESULT WINAPI SAORS_Stop(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    /* S_FALSE for an already-stopped stream, which is not FAILED, so the
     * driver's answer has to be forwarded rather than replaced by S_OK. */
    hr = IAudioClient_Stop(This->client);
    if(FAILED(hr))
        WARN("IAudioClient::Stop failed: %08lx\n", hr);

    return hr;
}

static HRESULT WINAPI SAORS_Reset(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    BOOL pass_open;
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    EnterCriticalSection(&This->lock);

    /* Windows' Reset leaves an open pass open. IAudioClient::Reset refuses
     * while a packet is held, so drop it and reacquire; mixing only happens
     * in End. */
    pass_open = This->update_frames != ~0 && This->update_frames > 0;
    if(pass_open)
        IAudioRenderClient_ReleaseBuffer(This->render, 0, 0);

    hr = IAudioClient_Reset(This->client);

    if(pass_open){
        HRESULT buf_hr = IAudioRenderClient_GetBuffer(This->render, This->update_frames,
                (BYTE **)&This->buf);
        if(FAILED(buf_hr)){
            WARN("could not reacquire the render buffer: %08lx\n", buf_hr);
            This->update_frames = ~0;
            if(SUCCEEDED(hr))
                hr = buf_hr;
        }
    }

    LeaveCriticalSection(&This->lock);

    if(hr == AUDCLNT_E_NOT_STOPPED)
        return SPTLAUDCLNT_E_STREAM_NOT_STOPPED;
    return hr;
}

static HRESULT WINAPI SAORS_BeginUpdatingAudioObjects(ISpatialAudioObjectRenderStream *iface,
        UINT32 *dyn_count, UINT32 *frames)
{
    static BOOL fixme_once = FALSE;
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object;
    HRESULT hr;

    TRACE("(%p)->(%p, %p)\n", This, dyn_count, frames);

    if(!dyn_count || !frames)
        return E_POINTER;

    /* Windows zeroes both before any test can fail, so a caller that ignores
     * the HRESULT reads 0 rather than the previous pass's values. */
    *dyn_count = 0;
    *frames = 0;

    EnterCriticalSection(&This->lock);

    if(This->update_frames != ~0){
        LeaveCriticalSection(&This->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    This->update_frames = This->period_frames;

    if(This->update_frames > 0){
        hr = IAudioRenderClient_GetBuffer(This->render, This->update_frames, (BYTE **)&This->buf);
        if(FAILED(hr)){
            WARN("GetBuffer failed: %08lx\n", hr);
            This->update_frames = ~0;
            LeaveCriticalSection(&This->lock);
            /* No free packet, i.e. the caller is late.  Windows reports the
             * buffer-size code for the same state. */
            if(hr == AUDCLNT_E_BUFFER_TOO_LARGE || hr == AUDCLNT_E_OUT_OF_ORDER)
                return AUDCLNT_E_BUFFER_SIZE_ERROR;
            return hr;
        }

        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
            /* An unwritten object is still all zeroes (calloc'd, GetBuffer is
             * the only writer), so a pre-activated pool skips this loop. */
            if(object->started)
                memset(object->buf, 0, This->update_frames *
                        This->sa_client->object_fmtex.Format.nBlockAlign);
            object->updated = FALSE;
        }
    }else if (!fixme_once){
        fixme_once = TRUE;
        FIXME("Zero frame update.\n");
    }

    /* Windows reports the per-pass grant, not the remaining capacity;
     * activation is the gate. */
    *dyn_count = This->dyn_max;
    *frames = This->update_frames;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

/* canonical 7.1.4 bed directions plus the bottom quad and back-center, in Steam
 * Audio listener space (+x right, +y up, -z ahead); fronts and backs sit 30
 * degrees off centre (0.5, 0.866), the height layer 45 degrees up (0.707);
 * returns FALSE for the only non-directional channel (LFE), which bypasses the
 * HRTF and sums equally to both ears. */
static BOOL bed_object_position(AudioObjectType type, float pos[3])
{
    switch(type){
    case AudioObjectType_FrontLeft:     pos[0]=-0.5f; pos[1]=0.0f;   pos[2]=-0.866f; return TRUE;
    case AudioObjectType_FrontRight:    pos[0]= 0.5f; pos[1]=0.0f;   pos[2]=-0.866f; return TRUE;
    case AudioObjectType_FrontCenter:   pos[0]= 0.0f; pos[1]=0.0f;   pos[2]=-1.0f;   return TRUE;
    case AudioObjectType_SideLeft:      pos[0]=-1.0f; pos[1]=0.0f;   pos[2]= 0.0f;   return TRUE;
    case AudioObjectType_SideRight:     pos[0]= 1.0f; pos[1]=0.0f;   pos[2]= 0.0f;   return TRUE;
    case AudioObjectType_BackLeft:      pos[0]=-0.5f; pos[1]=0.0f;   pos[2]= 0.866f; return TRUE;
    case AudioObjectType_BackRight:     pos[0]= 0.5f; pos[1]=0.0f;   pos[2]= 0.866f; return TRUE;
    case AudioObjectType_TopFrontLeft:  pos[0]=-0.5f; pos[1]=0.707f; pos[2]=-0.5f;   return TRUE;
    case AudioObjectType_TopFrontRight: pos[0]= 0.5f; pos[1]=0.707f; pos[2]=-0.5f;   return TRUE;
    case AudioObjectType_TopBackLeft:   pos[0]=-0.5f; pos[1]=0.707f; pos[2]= 0.5f;   return TRUE;
    case AudioObjectType_TopBackRight:  pos[0]= 0.5f; pos[1]=0.707f; pos[2]= 0.5f;   return TRUE;
    case AudioObjectType_BottomFrontLeft:  pos[0]=-0.5f; pos[1]=-0.707f; pos[2]=-0.5f;  return TRUE;
    case AudioObjectType_BottomFrontRight: pos[0]= 0.5f; pos[1]=-0.707f; pos[2]=-0.5f;  return TRUE;
    case AudioObjectType_BottomBackLeft:   pos[0]=-0.5f; pos[1]=-0.707f; pos[2]= 0.5f;  return TRUE;
    case AudioObjectType_BottomBackRight:  pos[0]= 0.5f; pos[1]=-0.707f; pos[2]= 0.5f;  return TRUE;
    case AudioObjectType_BackCenter:    pos[0]= 0.0f; pos[1]=0.0f;   pos[2]= 1.0f;   return TRUE;
    default:                            pos[0]= 0.0f; pos[1]=0.0f;   pos[2]= 0.0f;   return FALSE;
    }
}

static void mix_lfe_object(SpatialAudioStreamImpl *stream, SpatialAudioObjectImpl *object)
{
    float *in = object->buf, *out = stream->buf;
    float g = 0.5f * object->volume;
    UINT32 nch = stream->stream_fmtex.Format.nChannels, i;
    for(i = 0; i < stream->update_frames; ++i){
        out[stream->dyn_left]  += in[i] * g;
        out[stream->dyn_right] += in[i] * g;
        out += nch;
    }
}

/* No SPEAKER_BOTTOM_* bits exist; Windows folds each bottom channel into its
 * nearest neighbour at -3 dB. */
#define SPATIAL_BOTTOM_FOLD 0.70794576f

static float static_fold_gain(AudioObjectType type)
{
    switch(type){
    case AudioObjectType_BottomFrontLeft:
    case AudioObjectType_BottomFrontRight:
    case AudioObjectType_BottomBackLeft:
    case AudioObjectType_BottomBackRight:
        return SPATIAL_BOTTOM_FOLD;
    default:
        return 1.0f;
    }
}

static void mix_static_object(SpatialAudioStreamImpl *stream, SpatialAudioObjectImpl *object)
{
    float *in = object->buf, *out, vol;
    UINT32 i;
    if(object->static_idx == ~0 ||
            stream->static_object_map[object->static_idx] == ~0){
        WARN("Got unmapped static object?! Not mixing. Type: 0x%x\n", object->type);
        return;
    }
    out = stream->buf + stream->static_object_map[object->static_idx];
    vol = object->volume * static_fold_gain(object->type);
    for(i = 0; i < stream->update_frames; ++i){
        *out += *in * vol;
        ++in;
        out += stream->stream_fmtex.Format.nChannels;
    }
}

/* lateral equal-power pan; front/back and elevation collapse onto the x axis,
 * pi/4 maps p in [-1, 1] onto [0, pi/2]; the HRTF backend replaces this mixer */
static void mix_dynamic_object(SpatialAudioStreamImpl *stream, SpatialAudioObjectImpl *object)
{
    float len, p, gl, gr;
    float *in = object->buf, *out = stream->buf;
    UINT32 nch = stream->stream_fmtex.Format.nChannels, i;

    len = sqrtf(object->pos[0] * object->pos[0] +
                object->pos[1] * object->pos[1] +
                object->pos[2] * object->pos[2]);
    p = len > 0.0f ? object->pos[0] / len : 0.0f;
    gl = cosf((p + 1.0f) * 0.78539816f) * object->volume;
    gr = sinf((p + 1.0f) * 0.78539816f) * object->volume;

    for(i = 0; i < stream->update_frames; ++i){
        out[stream->dyn_left] += in[i] * gl;
        out[stream->dyn_right] += in[i] * gr;
        out += nch;
    }
}

/* Compensates the HRTF stage's per-object power gain, about +2.85 dB against
 * the reference renderer; applied only to engine-bound objects. */
#define SPATIAL_HRTF_PAD 0.72027775f

static HRESULT WINAPI SAORS_EndUpdatingAudioObjects(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object;
    HRESULT hr = S_OK;

    TRACE("(%p)->()\n", This);

    EnterCriticalSection(&This->lock);

    if(This->update_frames == ~0){
        LeaveCriticalSection(&This->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if(This->update_frames > 0){
        struct spatial_mix_object mix_objs[SPATIAL_MAX_SLOTS];
        UINT32 mix_count = 0, i;
        /* Set by every mixer that writes to dyn_left/dyn_right; only
         * mix_static_object writes anywhere else. */
        BOOL bus_dirty = FALSE;

        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
            if(object->invalidated && !object->eos_pending)
                continue;
            if(object->eos_pending && object->eos_frames < This->update_frames)
                memset(object->buf + object->eos_frames, 0,
                        (This->update_frames - object->eos_frames) * sizeof(float));
            if(This->engine && object->engine_slot != ~0 &&
                    mix_count < SPATIAL_MAX_SLOTS){
                mix_objs[mix_count].buffer = (UINT_PTR)object->buf;
                mix_objs[mix_count].slot = object->engine_slot;
                memcpy(mix_objs[mix_count].pos, object->pos, sizeof(object->pos));
                mix_objs[mix_count].volume = object->volume * SPATIAL_HRTF_PAD;
                mix_count++;
            }else if(object->type == AudioObjectType_Dynamic){
                mix_dynamic_object(This, object);
                bus_dirty = TRUE;
            }else if(This->virtualize_bed){
                if(object->type == AudioObjectType_LowFrequency)
                    mix_lfe_object(This, object);
                else
                    mix_dynamic_object(This, object);
                bus_dirty = TRUE;
            }else{
                mix_static_object(This, object);
            }
        }

        if(mix_count){
            struct spatial_mix_params params;
            memset(This->hrtf_buf, 0, 2 * This->update_frames * sizeof(float));
            params.handle = This->engine;
            params.frames = This->update_frames;
            params.count = mix_count;
            params.objects = (UINT_PTR)mix_objs;
            params.out_l = (UINT_PTR)This->hrtf_buf;
            params.out_r = (UINT_PTR)(This->hrtf_buf + This->update_frames);
            if(!WINE_UNIX_CALL(unix_spatial_mix, &params)){
                UINT32 nch = This->stream_fmtex.Format.nChannels;
                for(i = 0; i < This->update_frames; ++i){
                    This->buf[i * nch + This->dyn_left] += This->hrtf_buf[i];
                    This->buf[i * nch + This->dyn_right] += This->hrtf_buf[This->update_frames + i];
                }
                bus_dirty = TRUE;
            }else{
                LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
                    if((!object->invalidated || object->eos_pending) &&
                            object->engine_slot != ~0)
                        mix_dynamic_object(This, object);
                }
                bus_dirty = TRUE;
            }
        }

        /* Hard-clip the two bus channels when something summed into them this
         * tick. The static path is one-to-one and stays untouched, so
         * passthrough remains transparent. */
        if(bus_dirty){
            UINT32 nch = This->stream_fmtex.Format.nChannels;

            for(i = 0; i < This->update_frames; ++i){
                float *l = &This->buf[i * nch + This->dyn_left];
                float *r = &This->buf[i * nch + This->dyn_right];

                /* Zero a non-finite sample: the driver's f32-to-int is
                 * fminf(fmaxf(v, lo), hi), which sends a NaN out as
                 * full-scale negative. */
                if(!isfinite(*l)) *l = 0.0f;
                else if(*l > 1.0f) *l = 1.0f;
                else if(*l < -1.0f) *l = -1.0f;
                if(!isfinite(*r)) *r = 0.0f;
                else if(*r > 1.0f) *r = 1.0f;
                else if(*r < -1.0f) *r = -1.0f;
            }
        }

        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
            if(object->started && !object->updated)
                object->invalidated = TRUE;
            object->eos_pending = FALSE;
        }

        hr = IAudioRenderClient_ReleaseBuffer(This->render, This->update_frames, 0);
        if(FAILED(hr))
            WARN("ReleaseBuffer failed: %08lx\n", hr);
    }

    /* Cleared on every path as on Windows; the ReleaseBuffer failure is
     * returned so an endpoint loss is reported here. */
    This->update_frames = ~0;

    LeaveCriticalSection(&This->lock);

    return hr;
}

static HRESULT WINAPI SAORS_ActivateSpatialAudioObject(ISpatialAudioObjectRenderStream *iface,
        AudioObjectType type, ISpatialAudioObject **object)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *obj;
    HRESULT hr = S_OK;

    TRACE("(%p)->(0x%x, %p)\n", This, type, object);

    /* The admission tests run under the lock: SAO_Release mutates dyn_live
     * and the object list. */
    if(!(obj = calloc(1, sizeof(*obj))))
        return E_OUTOFMEMORY;
    if(!(obj->buf = calloc(This->period_frames,
            This->sa_client->object_fmtex.Format.nBlockAlign))){
        free(obj);
        return E_OUTOFMEMORY;
    }
    obj->ISpatialAudioObject_iface.lpVtbl = &ISpatialAudioObject_vtbl;
    obj->ref = 1;
    obj->type = type;
    obj->volume = 1.0f;
    obj->engine_slot = ~0;
    if(type == AudioObjectType_None){
        FIXME("AudioObjectType_None not implemented yet!\n");
        obj->static_idx = ~0;
    }else{
        obj->static_idx = AudioObjectType_to_index(type);
    }
    obj->sa_stream = This;

    EnterCriticalSection(&This->lock);

    if(type == AudioObjectType_Dynamic){
        if(This->dyn_live >= This->dyn_max){
            WARN("No dynamic object slots available (%u live, %u max, spatial sound %s).\n",
                    This->dyn_live, This->dyn_max, This->sa_client->dyn_budget ? "on" : "off");
            hr = SPTLAUDCLNT_E_NO_MORE_OBJECTS;
        }
    }else if(type & ~This->params.StaticObjectTypeMask){
        hr = SPTLAUDCLNT_E_STATIC_OBJECT_NOT_AVAILABLE;
    }else if(type != AudioObjectType_None){
        SpatialAudioObjectImpl *other;

        /* StaticObjectTypeMask is app-supplied and unfiltered, so it can admit
         * a type that maps to no bed channel */
        if(obj->static_idx == ~0)
            hr = SPTLAUDCLNT_E_STATIC_OBJECT_NOT_AVAILABLE;
        else LIST_FOR_EACH_ENTRY(other, &This->objects, SpatialAudioObjectImpl, entry){
            if(other->static_idx == obj->static_idx){
                hr = SPTLAUDCLNT_E_OBJECT_ALREADY_ACTIVE;
                break;
            }
        }
    }

    if(FAILED(hr)){
        LeaveCriticalSection(&This->lock);
        free(obj->buf);
        free(obj);
        return hr;
    }

    if(type == AudioObjectType_Dynamic){
        This->dyn_live++;
        if(This->engine){
            struct spatial_object_add_params params;
            params.handle = This->engine;
            if(!WINE_UNIX_CALL(unix_spatial_object_add, &params))
                obj->engine_slot = params.slot;
            else
                WARN("No HRTF effect slot for dynamic object %p, will use panning.\n", obj);
        }
    }else if(This->virtualize_bed){
        if(bed_object_position(type, obj->pos) && This->engine){
            struct spatial_object_add_params params;
            params.handle = This->engine;
            if(!WINE_UNIX_CALL(unix_spatial_object_add, &params))
                obj->engine_slot = params.slot;
            else
                WARN("No HRTF effect slot for bed channel 0x%x, will use panning.\n", type);
        }
    }
    list_add_tail(&This->objects, &obj->entry);
    SAORS_AddRef(&This->ISpatialAudioObjectRenderStream_iface);

    LeaveCriticalSection(&This->lock);

    *object = &obj->ISpatialAudioObject_iface;

    return S_OK;
}

static ISpatialAudioObjectRenderStreamVtbl ISpatialAudioObjectRenderStream_vtbl = {
    SAORS_QueryInterface,
    SAORS_AddRef,
    SAORS_Release,
    SAORS_GetAvailableDynamicObjectCount,
    SAORS_GetService,
    SAORS_Start,
    SAORS_Stop,
    SAORS_Reset,
    SAORS_BeginUpdatingAudioObjects,
    SAORS_EndUpdatingAudioObjects,
    SAORS_ActivateSpatialAudioObject,
};

static HRESULT WINAPI SAC_QueryInterface(ISpatialAudioClient *iface, REFIID riid, void **ppv)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioClient)) {
        *ppv = &This->ISpatialAudioClient_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAC_AddRef(ISpatialAudioClient *iface)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAC_Release(ISpatialAudioClient *iface)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if (!ref) {
        IMMDevice_Release(This->mmdev);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAC_GetStaticObjectPosition(ISpatialAudioClient *iface,
        AudioObjectType type, float *x, float *y, float *z)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    float pos[3];

    TRACE("(%p)->(0x%x, %p, %p, %p)\n", This, type, x, y, z);

    if(!x || !y || !z)
        return E_INVALIDARG;

    /* The same directions the HRTF mixer renders each bed channel to;
     * non-directional types (LFE) report the listener origin. */
    bed_object_position(type, pos);
    *x = pos[0];
    *y = pos[1];
    *z = pos[2];

    return S_OK;
}

static HRESULT WINAPI SAC_GetNativeStaticObjectTypeMask(ISpatialAudioClient *iface,
        AudioObjectType *mask)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, mask);

    if(!mask)
        return E_INVALIDARG;

    /* Every static type the bed mixer can place. 0x3fffe is also what Windows
     * reports for an unrecognised encoder. */
    *mask = AudioObjectType_FrontLeft | AudioObjectType_FrontRight |
            AudioObjectType_FrontCenter | AudioObjectType_LowFrequency |
            AudioObjectType_SideLeft | AudioObjectType_SideRight |
            AudioObjectType_BackLeft | AudioObjectType_BackRight |
            AudioObjectType_TopFrontLeft | AudioObjectType_TopFrontRight |
            AudioObjectType_TopBackLeft | AudioObjectType_TopBackRight |
            AudioObjectType_BottomFrontLeft | AudioObjectType_BottomFrontRight |
            AudioObjectType_BottomBackLeft | AudioObjectType_BottomBackRight |
            AudioObjectType_BackCenter;

    return S_OK;
}

static HRESULT WINAPI SAC_GetMaxDynamicObjectCount(ISpatialAudioClient *iface,
        UINT32 *value)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, value);

    if(!value)
        return E_INVALIDARG;

    *value = This->dyn_budget;

    return S_OK;
}

static HRESULT WINAPI SAC_GetSupportedAudioObjectFormatEnumerator(
        ISpatialAudioClient *iface, IAudioFormatEnumerator **enumerator)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, enumerator);

    if(!enumerator)
        return E_POINTER;

    *enumerator = &This->IAudioFormatEnumerator_iface;
    SAC_AddRef(iface);

    return S_OK;
}

static HRESULT WINAPI SAC_IsAudioObjectFormatSupported(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format);

static HRESULT WINAPI SAC_GetMaxFrameCount(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format, UINT32 *count)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    HRESULT hr;

    /* FIXME: should get device period from the device */
    static const REFERENCE_TIME period = 100000;

    TRACE("(%p)->(%p, %p)\n", This, format, count);

    if(!count)
        return E_POINTER;

    *count = 0;

    /* Windows runs the support test, including the NULL check, before it
     * reaches the period. */
    if((hr = SAC_IsAudioObjectFormatSupported(iface, format)) != S_OK)
        return hr;

    *count = MulDiv(period, format->nSamplesPerSec, 10000000);

    return S_OK;
}

static HRESULT WINAPI SAC_IsAudioObjectFormatSupported(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format)
{
    SpatialAudioImpl *sac = impl_from_ISpatialAudioClient(iface);
    HRESULT hr;

    TRACE("sac %p, format %s.\n", sac, debugstr_fmtex(format));

    if (!format)
        return E_POINTER;

    if ((hr = validate_wave_format_ex(format)) != S_OK)
        return hr;

    if (!object_formats_compatible(&sac->object_fmtex.Format, format))
    {
        TRACE("Reporting format %s as unsupported.\n", debugstr_fmtex(format));
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    return S_OK;
}

static HRESULT WINAPI SAC_IsSpatialAudioStreamAvailable(ISpatialAudioClient *iface,
        REFIID stream_uuid, const PROPVARIANT *info)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(stream_uuid), info);

    /* Windows answers this with no device or budget test. */
    if(IsEqualIID(stream_uuid, &IID_ISpatialAudioObjectRenderStream))
        return S_OK;

    WARN("Unsupported spatial stream %s.\n", debugstr_guid(stream_uuid));
    return SPTLAUDCLNT_E_STREAM_NOT_AVAILABLE;
}

static WAVEFORMATEX *clone_fmtex(const WAVEFORMATEX *src)
{
    WAVEFORMATEX *r = malloc(sizeof(WAVEFORMATEX) + src->cbSize);
    memcpy(r, src, sizeof(WAVEFORMATEX) + src->cbSize);
    return r;
}

static void static_mask_to_channels(AudioObjectType static_mask, WORD *count, DWORD *mask, UINT32 *map)
{
    UINT32 out_chan = 0, map_idx = 0;
    *count = 0;
    *mask = 0;
#define CONVERT_MASK(f, t) \
    if(static_mask & f){ \
        *count += 1; \
        *mask |= t; \
        map[map_idx++] = out_chan++; \
        TRACE("mapping 0x%x to %u\n", f, out_chan - 1); \
    }else{ \
        map[map_idx++] = ~0; \
    }
    /* The fold target is always mapped before its bottom channel, because both
     * loops run in AudioObjectType bit order.  A target missing from the mask
     * leaves ~0, which mix_static_object already reports and drops. */
#define FOLD_MASK(f, t) \
    if(static_mask & f){ \
        map[map_idx++] = map[AudioObjectType_to_index(t)]; \
        TRACE("folding 0x%x into 0x%x\n", f, t); \
    }else{ \
        map[map_idx++] = ~0; \
    }
    CONVERT_MASK(AudioObjectType_FrontLeft, SPEAKER_FRONT_LEFT);
    CONVERT_MASK(AudioObjectType_FrontRight, SPEAKER_FRONT_RIGHT);
    CONVERT_MASK(AudioObjectType_FrontCenter, SPEAKER_FRONT_CENTER);
    CONVERT_MASK(AudioObjectType_LowFrequency, SPEAKER_LOW_FREQUENCY);
    CONVERT_MASK(AudioObjectType_SideLeft, SPEAKER_SIDE_LEFT);
    CONVERT_MASK(AudioObjectType_SideRight, SPEAKER_SIDE_RIGHT);
    CONVERT_MASK(AudioObjectType_BackLeft, SPEAKER_BACK_LEFT);
    CONVERT_MASK(AudioObjectType_BackRight, SPEAKER_BACK_RIGHT);
    CONVERT_MASK(AudioObjectType_TopFrontLeft, SPEAKER_TOP_FRONT_LEFT);
    CONVERT_MASK(AudioObjectType_TopFrontRight, SPEAKER_TOP_FRONT_RIGHT);
    CONVERT_MASK(AudioObjectType_TopBackLeft, SPEAKER_TOP_BACK_LEFT);
    CONVERT_MASK(AudioObjectType_TopBackRight, SPEAKER_TOP_BACK_RIGHT);
    /* A bottom channel shares its fold target's endpoint channel, so
     * nChannels == popcount(*mask). */
    FOLD_MASK(AudioObjectType_BottomFrontLeft, AudioObjectType_FrontLeft);
    FOLD_MASK(AudioObjectType_BottomFrontRight, AudioObjectType_FrontRight);
    FOLD_MASK(AudioObjectType_BottomBackLeft, AudioObjectType_BackLeft);
    FOLD_MASK(AudioObjectType_BottomBackRight, AudioObjectType_BackRight);
    CONVERT_MASK(AudioObjectType_BackCenter, SPEAKER_BACK_CENTER);
}

static HRESULT activate_stream(SpatialAudioStreamImpl *stream)
{
    WAVEFORMATEXTENSIBLE *object_fmtex = (WAVEFORMATEXTENSIBLE *)stream->params.ObjectFormat;
    HRESULT hr;
    REFERENCE_TIME period;
    UINT32 i;
    WAVEFORMATEX *mix_fmt = NULL;
    WORD bed_ch = 0;
    DWORD bed_mask = 0;
    UINT32 dev_ch = 0;
    AudioObjectType effective_mask;

    if(!(object_fmtex->Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                (object_fmtex->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                 IsEqualGUID(&object_fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)))){
        FIXME("Only float formats are supported for now\n");
        return E_INVALIDARG;
    }

    hr = IMMDevice_Activate(stream->sa_client->mmdev, &IID_IAudioClient,
            CLSCTX_INPROC_SERVER, NULL, (void**)&stream->client);
    if(FAILED(hr)){
        WARN("Activate failed: %08lx\n", hr);
        return hr;
    }

    hr = IAudioClient_GetDevicePeriod(stream->client, &period, NULL);
    if(FAILED(hr)){
        WARN("GetDevicePeriod failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    effective_mask = stream->params.StaticObjectTypeMask ? stream->params.StaticObjectTypeMask :
            (AudioObjectType_FrontLeft | AudioObjectType_FrontRight);
    static_mask_to_channels(effective_mask, &bed_ch, &bed_mask, stream->static_object_map);

    if(SUCCEEDED(IAudioClient_GetMixFormat(stream->client, &mix_fmt))){
        dev_ch = mix_fmt->nChannels;
        CoTaskMemFree(mix_fmt);
    }

    /* HRTF-virtualize a surround bed only into a stereo endpoint (a headphone
     * target); real multichannel endpoints keep the passthrough downmix. */
    stream->virtualize_bed = stream->sa_client->dyn_budget && dev_ch && dev_ch <= 2 && bed_ch > 2;

    stream->stream_fmtex.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    if(stream->virtualize_bed){
        stream->stream_fmtex.Format.nChannels = 2;
        stream->stream_fmtex.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        stream->dyn_left = 0;
        stream->dyn_right = 1;
        TRACE("Bed virtualization requested: %u channels to stereo.\n", bed_ch);
    }else{
        /* The binaural bus needs real front-left/right channels: with a
         * dynamic budget, widen a bed that lacks either so the bus never
         * lands on another channel index. The app never sees this format and
         * the graph downmixes to the device. */
        if(stream->sa_client->dyn_budget &&
                (~bed_mask & (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT))){
            effective_mask |= AudioObjectType_FrontLeft | AudioObjectType_FrontRight;
            static_mask_to_channels(effective_mask, &bed_ch, &bed_mask,
                    stream->static_object_map);
            TRACE_(spatial)("bed lacks the front pair, widened to %u channels "
                    "(mask 0x%lx) so the binaural bus has somewhere to go.\n",
                    bed_ch, bed_mask);
        }
        stream->stream_fmtex.Format.nChannels = bed_ch;
        stream->stream_fmtex.dwChannelMask = bed_mask;
        i = stream->static_object_map[AudioObjectType_to_index(AudioObjectType_FrontLeft)];
        stream->dyn_left = i != ~0 ? i : 0;
        i = stream->static_object_map[AudioObjectType_to_index(AudioObjectType_FrontRight)];
        stream->dyn_right = i != ~0 ? i : (stream->stream_fmtex.Format.nChannels > 1 ? 1 : 0);
    }
    stream->stream_fmtex.Format.nSamplesPerSec = stream->params.ObjectFormat->nSamplesPerSec;
    stream->stream_fmtex.Format.wBitsPerSample = stream->params.ObjectFormat->wBitsPerSample;
    stream->stream_fmtex.Format.nBlockAlign = (stream->stream_fmtex.Format.nChannels * stream->stream_fmtex.Format.wBitsPerSample) / 8;
    stream->stream_fmtex.Format.nAvgBytesPerSec = stream->stream_fmtex.Format.nSamplesPerSec * stream->stream_fmtex.Format.nBlockAlign;
    stream->stream_fmtex.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    stream->stream_fmtex.Samples.wValidBitsPerSample = stream->stream_fmtex.Format.wBitsPerSample;
    stream->stream_fmtex.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    hr = IAudioClient_Initialize(stream->client, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
            period, 0, &stream->stream_fmtex.Format, NULL);
    if(FAILED(hr)){
        WARN("Initialize failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    hr = IAudioClient_SetEventHandle(stream->client, stream->params.EventHandle);
    if(FAILED(hr)){
        WARN("SetEventHandle failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    hr = IAudioClient_GetService(stream->client, &IID_IAudioRenderClient, (void**)&stream->render);
    if(FAILED(hr)){
        WARN("GetService(AudioRenderClient) failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    stream->period_frames = MulDiv(period, stream->stream_fmtex.Format.nSamplesPerSec, 10000000);

    TRACE_(spatial)("stream %p configured: %u-channel bed -> %u-channel endpoint, %s, "
            "static mask 0x%x, dynamic budget %u.\n",
            stream, bed_ch, stream->stream_fmtex.Format.nChannels,
            stream->virtualize_bed ? "bed virtualization requested, backend chosen at activation"
                                   : "multichannel passthrough",
            effective_mask, stream->dyn_max);
    return S_OK;
}

static HRESULT WINAPI SAC_ActivateSpatialAudioStream(ISpatialAudioClient *iface,
        const PROPVARIANT *prop, REFIID riid, void **stream)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    SpatialAudioObjectRenderStreamActivationParams *params;
    HRESULT hr;

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), stream);

    /* Windows checks these before the IID: E_INVALIDARG for a NULL
     * PROPVARIANT, E_POINTER for a NULL out pointer. */
    if(!prop)
        return E_INVALIDARG;
    if(!stream)
        return E_POINTER;
    *stream = NULL;

    if(IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStream)){
        SpatialAudioStreamImpl *obj;

        /* Windows accepts the v1 struct or v1 plus the UINT32 that
         * SpatialAudioObjectRenderStreamActivationParams2 adds; the v2 tail
         * is not parsed. */
        if(prop->vt != VT_BLOB || !prop->blob.pBlobData ||
                (prop->blob.cbSize != sizeof(SpatialAudioObjectRenderStreamActivationParams) &&
                 prop->blob.cbSize != sizeof(SpatialAudioObjectRenderStreamActivationParams)
                                      + sizeof(UINT32))){
            WARN("Got invalid params\n");
            *stream = NULL;
            return E_INVALIDARG;
        }

        params = (SpatialAudioObjectRenderStreamActivationParams*) prop->blob.pBlobData;

        if(params->StaticObjectTypeMask & AudioObjectType_Dynamic){
            *stream = NULL;
            return E_INVALIDARG;
        }

        if(params->EventHandle == INVALID_HANDLE_VALUE ||
                params->EventHandle == 0){
            *stream = NULL;
            return E_INVALIDARG;
        }

        if(!(params->ObjectFormat &&
             object_formats_compatible(params->ObjectFormat, &This->object_fmtex.Format))) {
            *stream = NULL;
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }

        if(params->MinDynamicObjectCount > This->dyn_budget){
            WARN("MinDynamicObjectCount %u exceeds the budget of %u.\n",
                    params->MinDynamicObjectCount, This->dyn_budget);
            *stream = NULL;
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }

        obj = calloc(1, sizeof(SpatialAudioStreamImpl));

        obj->ISpatialAudioObjectRenderStream_iface.lpVtbl = &ISpatialAudioObjectRenderStream_vtbl;
        obj->ref = 1;
        memcpy(&obj->params, params, sizeof(obj->params));

        obj->update_frames = ~0;
        obj->dyn_max = params->MaxDynamicObjectCount < This->dyn_budget ?
                params->MaxDynamicObjectCount : This->dyn_budget;

        InitializeCriticalSection(&obj->lock);
        list_init(&obj->objects);

        obj->sa_client = This;
        SAC_AddRef(&This->ISpatialAudioClient_iface);

        obj->params.ObjectFormat = clone_fmtex(obj->params.ObjectFormat);

        DuplicateHandle(GetCurrentProcess(), obj->params.EventHandle,
                GetCurrentProcess(), &obj->params.EventHandle, 0, FALSE,
                DUPLICATE_SAME_ACCESS);

        if(obj->params.NotifyObject)
            ISpatialAudioObjectRenderStreamNotify_AddRef(obj->params.NotifyObject);

        if(TRACE_ON(mmdevapi)){
            TRACE("ObjectFormat: {%s}\n", debugstr_fmtex(obj->params.ObjectFormat));
            TRACE("StaticObjectTypeMask: 0x%x\n", obj->params.StaticObjectTypeMask);
            TRACE("MinDynamicObjectCount: 0x%x\n", obj->params.MinDynamicObjectCount);
            TRACE("MaxDynamicObjectCount: 0x%x\n", obj->params.MaxDynamicObjectCount);
            TRACE("Category: 0x%x\n", obj->params.Category);
            TRACE("EventHandle: %p\n", obj->params.EventHandle);
            TRACE("NotifyObject: %p\n", obj->params.NotifyObject);
        }

        hr = activate_stream(obj);
        if(FAILED(hr)){
            if(obj->params.NotifyObject)
                ISpatialAudioObjectRenderStreamNotify_Release(obj->params.NotifyObject);
            DeleteCriticalSection(&obj->lock);
            free((void*)obj->params.ObjectFormat);
            CloseHandle(obj->params.EventHandle);
            ISpatialAudioClient_Release(&obj->sa_client->ISpatialAudioClient_iface);
            free(obj);
            *stream = NULL;
            return hr;
        }

        if(obj->dyn_max || obj->virtualize_bed){
            struct spatial_init_params init_params;
            init_params.rate = obj->stream_fmtex.Format.nSamplesPerSec;
            init_params.frames = obj->period_frames;
            init_params.handle = 0;
            if(spatial_unix_init() && !WINE_UNIX_CALL(unix_spatial_init, &init_params) &&
                    (obj->hrtf_buf = calloc(2 * obj->period_frames, sizeof(float)))){
                obj->engine = init_params.handle;
                TRACE_(spatial)("Using the Steam Audio HRTF engine "
                        "(bed virtualization %s, up to %u dynamic objects).\n",
                        obj->virtualize_bed ? "on" : "off", obj->dyn_max);
            }
            else if(init_params.handle){
                struct spatial_release_params release_params;
                release_params.handle = init_params.handle;
                WINE_UNIX_CALL(unix_spatial_release, &release_params);
            }
            if(!obj->engine)
                WARN_(spatial)("HRTF engine unavailable, %s will use stereo panning.\n",
                        obj->virtualize_bed ? "the bed and any dynamic objects"
                                            : "dynamic objects");
        }

        *stream = &obj->ISpatialAudioObjectRenderStream_iface;
    }else{
        FIXME("Unsupported audio stream IID: %s\n", debugstr_guid(riid));
        *stream = NULL;
        return E_NOTIMPL;
    }

    return S_OK;
}

static ISpatialAudioClientVtbl ISpatialAudioClient_vtbl = {
    SAC_QueryInterface,
    SAC_AddRef,
    SAC_Release,
    SAC_GetStaticObjectPosition,
    SAC_GetNativeStaticObjectTypeMask,
    SAC_GetMaxDynamicObjectCount,
    SAC_GetSupportedAudioObjectFormatEnumerator,
    SAC_GetMaxFrameCount,
    SAC_IsAudioObjectFormatSupported,
    SAC_IsSpatialAudioStreamAvailable,
    SAC_ActivateSpatialAudioStream,
};

static HRESULT WINAPI SAOFE_QueryInterface(IAudioFormatEnumerator *iface,
        REFIID riid, void **ppvObject)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_QueryInterface(&This->ISpatialAudioClient_iface, riid, ppvObject);
}

static ULONG WINAPI SAOFE_AddRef(IAudioFormatEnumerator *iface)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_AddRef(&This->ISpatialAudioClient_iface);
}

static ULONG WINAPI SAOFE_Release(IAudioFormatEnumerator *iface)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_Release(&This->ISpatialAudioClient_iface);
}

static HRESULT WINAPI SAOFE_GetCount(IAudioFormatEnumerator *iface, UINT32 *count)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);

    TRACE("(%p)->(%p)\n", This, count);

    if(!count)
        return E_POINTER;

    *count = 1;

    return S_OK;
}

static HRESULT WINAPI SAOFE_GetFormat(IAudioFormatEnumerator *iface,
        UINT32 index, WAVEFORMATEX **format)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);

    TRACE("(%p)->(%u, %p)\n", This, index, format);

    if(!format)
        return E_POINTER;

    if(index > 0)
        return E_INVALIDARG;

    *format = &This->object_fmtex.Format;

    return S_OK;
}

static IAudioFormatEnumeratorVtbl IAudioFormatEnumerator_vtbl = {
    SAOFE_QueryInterface,
    SAOFE_AddRef,
    SAOFE_Release,
    SAOFE_GetCount,
    SAOFE_GetFormat,
};

HRESULT SpatialAudioClient_Create(IMMDevice *mmdev, ISpatialAudioClient **out)
{
    SpatialAudioImpl *obj;

    obj = calloc(1, sizeof(*obj));

    obj->ref = 1;
    obj->ISpatialAudioClient_iface.lpVtbl = &ISpatialAudioClient_vtbl;
    obj->IAudioFormatEnumerator_iface.lpVtbl = &IAudioFormatEnumerator_vtbl;
    obj->dyn_budget = get_spatial_dynamic_budget();

    obj->object_fmtex.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    obj->object_fmtex.Format.nChannels = 1;
    obj->object_fmtex.Format.nSamplesPerSec = 48000;
    obj->object_fmtex.Format.wBitsPerSample = sizeof(float) * 8;
    obj->object_fmtex.Format.nBlockAlign = (obj->object_fmtex.Format.nChannels * obj->object_fmtex.Format.wBitsPerSample) / 8;
    obj->object_fmtex.Format.nAvgBytesPerSec = obj->object_fmtex.Format.nSamplesPerSec * obj->object_fmtex.Format.nBlockAlign;
    obj->object_fmtex.Format.cbSize = 0;

    obj->mmdev = mmdev;
    IMMDevice_AddRef(mmdev);

    *out = &obj->ISpatialAudioClient_iface;

    TRACE_(spatial)("client %p created (spatial sound %s, dynamic object budget %u).\n",
            obj, obj->dyn_budget ? "enabled" : "disabled", obj->dyn_budget);
    return S_OK;
}
