#ifndef READ_HACK_INCLUDED
#define READ_HACK_INCLUDED

#include "mfobjects.h"
#include "winternl.h"

struct read_hack_callback
{
    IMFAsyncCallback IMFAsyncCallback_iface;
    LONG refcount;
    HANDLE event;
    DWORD param;
    IMFMediaEvent *media_event;
    IMFAsyncResult *result;
};

static struct read_hack_callback *read_hack_callback_from_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct read_hack_callback, IMFAsyncCallback_iface);
}

static HRESULT WINAPI read_hack_callback_QueryInterface(IMFAsyncCallback *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFAsyncCallback) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFAsyncCallback_AddRef(iface);
        return S_OK;
    }

    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI read_hack_callback_AddRef(IMFAsyncCallback *iface)
{
    struct read_hack_callback *callback;

    callback = read_hack_callback_from_IMFAsyncCallback(iface);
    return InterlockedIncrement(&callback->refcount);
}

static ULONG WINAPI read_hack_callback_Release(IMFAsyncCallback *iface)
{
    struct read_hack_callback *callback;
    ULONG refcount;

    callback = read_hack_callback_from_IMFAsyncCallback(iface);
    refcount = InterlockedDecrement(&callback->refcount);

    if (!refcount)
    {
        CloseHandle(callback->event);
        free(callback);
    }

    return refcount;
}

static HRESULT WINAPI read_hack_callback_GetParameters(IMFAsyncCallback *iface, DWORD *flags, DWORD *queue)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI read_hack_async_callback_result_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct read_hack_callback *callback;

    callback = read_hack_callback_from_IMFAsyncCallback(iface);

    callback->result = result;
    IMFAsyncResult_AddRef(callback->result);
    SetEvent(callback->event);

    return S_OK;
}

static const IMFAsyncCallbackVtbl read_hack_async_callback_result_vtbl =
{
    read_hack_callback_QueryInterface,
    read_hack_callback_AddRef,
    read_hack_callback_Release,
    read_hack_callback_GetParameters,
    read_hack_async_callback_result_Invoke,
};

static struct read_hack_callback * create_read_hack_callback(const IMFAsyncCallbackVtbl *vtbl)
{
    struct read_hack_callback *callback;

    callback = calloc(1, sizeof(*callback));

    callback->IMFAsyncCallback_iface.lpVtbl = vtbl;
    callback->refcount = 1;
    callback->event = CreateEventA(NULL, FALSE, FALSE, NULL);

    return callback;
}

static inline HRESULT IMFByteStream_Read_Hack(IMFByteStream *stream, BYTE *pb, ULONG cb, ULONG *pcbRead) {
    /* HACK: Use the async BeginRead/EndRead interface due to buggy 
     * sync Read implementation on the game's end. */
    static int cached = -1;
    HRESULT hr;

    if (cached == -1)
    {
        const char *sgi;
        WARN("HACK: Enabled using async BeginRead instead of sync Read.\n");
        sgi = getenv("SteamGameId");
        cached = !strcmp(sgi, "333980");
    }

    hr = IMFByteStream_Read(stream, pb, cb, pcbRead);

    if (cached && *pcbRead == 0xffffffff)
    {
        DWORD res;
        IMFAsyncResult *result = NULL;
        struct read_hack_callback *read_callback;

        TRACE("HACK: Using async BeginRead instead of sync Read.\n");

        read_callback = create_read_hack_callback(&read_hack_async_callback_result_vtbl);

        if (FAILED(hr = IMFByteStream_BeginRead(stream, pb, cb, &read_callback->IMFAsyncCallback_iface, NULL)))
        {
            WARN("BeginRead failed with %#lx, falling back to Read.\n", hr);
            hr = IMFByteStream_Read(stream, pb, cb, pcbRead);
            goto end;
        }

        if ((res = WaitForSingleObject(read_callback->event, 500)) != WAIT_OBJECT_0)
        {
            ERR("Waiting for BeginRead result failed with %#lx.\n", res);
            hr = E_INVALIDARG;
            goto end;
        }

        result = read_callback->result;
        read_callback->result = NULL;

        if (FAILED(hr = IMFByteStream_EndRead(stream, result, pcbRead)))
        {
            WARN("EndRead failed with %#lx.\n", hr);
        }

end:
        if (result)
            IMFAsyncResult_Release(result);
        IMFAsyncCallback_Release(&read_callback->IMFAsyncCallback_iface);
    }

    return hr;
}

#endif /* READ_HACK_INCLUDED */
