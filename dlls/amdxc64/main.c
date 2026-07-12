/*
 * amdxc implementation
 *
 * Copyright 2023 Etaash Mathamsetty
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

#include <stdarg.h>

#include "ntstatus.h"
#include "winerror.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/vulkan.h"

#define COBJMACROS
#include "initguid.h"
#include "d3d12.h"

#include "amdxc_interfaces.h"
#include "vkd3d_device_vkd3d_ext.h"
#include "vkd3d_vk_includes.h"

WINE_DEFAULT_DEBUG_CHANNEL(amdxc);

static void check_fsr4_supported(ID3D12Device *device, BOOL *fp8, BOOL *p_wmma)
{
    ID3D12DeviceExt3 *ext;
    const char *e;
    BOOL wmma;
    static int once;

    if (FAILED(ID3D12Device_QueryInterface(device, &IID_ID3D12DeviceExt3, (void **)&ext))) return;

    wmma = ID3D12DeviceExt3_SupportsAGSExtension(ext, D3D12_AGS_EXTENSION_WMMA_FP8);
    *fp8 = ID3D12DeviceExt3_SupportsAGSExtension(ext, D3D12_AGS_EXTENSION_WMMA_FP8_NATIVE);

    if (*fp8) TRACE("FSR4 FP8 supported!\n");
    if ((e = getenv("DXIL_SPIRV_CONFIG")) && !strcmp(e, "wmma_rdna3_workaround"))
    {
        if ((*fp8 = wmma) && !once++)
            FIXME("FSR4 FP16 emulation is not recommended, please use FSR 4.1.1\n");
    }
    if (p_wmma) *p_wmma = wmma;

    ID3D12DeviceExt3_Release(ext);
}

static BOOL is_rdna2(ID3D12Device *device)
{
    ID3D12DXVKInteropDevice *interop;
    VkInstance instance;
    VkDevice vk_device;
    VkPhysicalDevice phys_device;
    VkPhysicalDeviceProperties2 prop = {0};
    VkPhysicalDeviceDriverProperties driver_prop = {0};
    const char **extensions = NULL;
    UINT extension_count = 0;
    BOOL ret = FALSE;

    if (FAILED(ID3D12Device_QueryInterface(device, &IID_ID3D12DXVKInteropDevice, (void **)&interop)))
        return FALSE;

    if (FAILED(ID3D12DXVKInteropDevice_GetVulkanHandles(interop, &instance, &phys_device, &vk_device)))
        goto fail;

    prop.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    prop.pNext = &driver_prop;
    driver_prop.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

    vkGetPhysicalDeviceProperties2(phys_device, &prop);

    if (prop.properties.vendorID != 0x1002) goto fail;
    /* Do not enable by default on iGPUs. They are not powerful enough.
     * Still can be enabled manually through FSR4_UPGRADE=1. */
    if (prop.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) goto fail;

    if (FAILED(ID3D12DXVKInteropDevice_GetDeviceExtensions(interop, &extension_count, NULL)))
        goto fail;

    extensions = malloc(sizeof(*extensions) * extension_count);

    if (FAILED(ID3D12DXVKInteropDevice_GetDeviceExtensions(interop, &extension_count, extensions)))
        goto fail;

    for (UINT i = 0; i < extension_count; i++)
    {
        if (!strcmp("VK_KHR_fragment_shading_rate", extensions[i])) ret = TRUE;
    }

fail:
    if (extensions) free(extensions);
    ID3D12DXVKInteropDevice_Release(interop);

    return ret;
}

struct AMDFSR4FFX
{
    IAmdExtFfxApi IAmdExtFfxApi_iface;
    LONG ref;
    BOOL fp8_supported, rdna2;
};

static struct AMDFSR4FFX* impl_from_IAmdExtFfxApi(IAmdExtFfxApi* iface)
{
    return CONTAINING_RECORD(iface, struct AMDFSR4FFX, IAmdExtFfxApi_iface);
}

ULONG STDMETHODCALLTYPE AMDFSR4FFX_AddRef(IAmdExtFfxApi *iface)
{
    struct AMDFSR4FFX* data = impl_from_IAmdExtFfxApi(iface);
    return InterlockedIncrement(&data->ref);
}

ULONG STDMETHODCALLTYPE AMDFSR4FFX_Release(IAmdExtFfxApi *iface)
{
    struct AMDFSR4FFX* data = impl_from_IAmdExtFfxApi(iface);
    ULONG ret = InterlockedDecrement(&data->ref);
    if (!ret) free(data);
    return ret;
}

HRESULT STDMETHODCALLTYPE AMDFSR4FFX_QueryInterface(IAmdExtFfxApi *iface, REFIID iid, void **obj)
{
    FIXME("%p %s %p", iface, debugstr_guid(iid), obj);

    return E_NOINTERFACE;
}

/* maintain compat with older SDK 2.0.0 and older */
typedef HRESULT (__stdcall *updateffxapi_pfn)(void*, unsigned int);
/* SDK 2.1.0 requires this */
typedef HRESULT (__stdcall *updateffxapi_pfn_ex)(void*, unsigned int, void*);

typedef ULONG (*pfnCanProvide)(ULONG64 typeId);
typedef ULONG (*pfnCreateContext)(void* context, void* desc, const void* allocator);
typedef ULONG (*pfnDestroyContext)(void* context, const void* allocator);
typedef ULONG (*pfnConfigure)(void* context, const void* desc);
typedef ULONG (*pfnQuery)(void* context, void* desc);
typedef ULONG (*pfnDispatch)(void* context, const void* desc);

struct ffxExternalProvider
{
    ULONG structVersion; /* 0x0 */
    ULONG64 descType; /* 0x8 */
    ULONG64 versionId; /* 0x10 */
    const char* versionName; /* 0x18 */
    pfnCanProvide canProvide; /* 0x20 */
    pfnCreateContext createContext; /* 0x28 */
    pfnDestroyContext destroyContext; /* 0x30 */
    pfnConfigure configure; /* 0x38 */
    pfnQuery query; /* 0x40 */
    pfnDispatch dispatch; /* 0x48 */
};

static void dump_provider(struct ffxExternalProvider *provider)
{
    if (!provider) return;

    TRACE("returned provider: %lx %I64x %s\n",
          provider->structVersion, provider->descType,
          debugstr_a(provider->versionName));
}

struct unk_data {
    int unk[4];
    struct unk_data *next;
};

HRESULT STDMETHODCALLTYPE AMDFSR4FFX_UpdateFfxApiProvider(IAmdExtFfxApi *iface, void *_data, unsigned int size)
{
    struct AMDFSR4FFX *this = impl_from_IAmdExtFfxApi(iface);
    struct ffxExternalProvider *data = _data;
    /* required to expose MLFG support */
    struct unk_data unk_data[1] = {{{0, 1, 0, 0}, NULL}};
    const char *env;
    updateffxapi_pfn_ex pfn_ex;
    updateffxapi_pfn pfn;
    HMODULE amdffx;
    BOOL fsr4;

    TRACE("%p %p %u\n", iface, data, size);

    if (!data) return E_INVALIDARG;

    env = getenv("MLFG_UPGRADE");
    if (this->fp8_supported || (env && !strcmp(env, "1")))
        unk_data->unk[2] = !env || strcmp(env, "0");

    fsr4 = (env = getenv("FSR4_UPGRADE")) && !strcmp(env, "1");
    if (!fsr4 && !this->rdna2) return E_NOTIMPL;
    /* explicitly disabled */
    if (env && !fsr4) return E_NOTIMPL;

    if (!(amdffx = LoadLibraryA("amdxcffx64")))
    {
        ERR("Failed to load FSR4 dll (amdxcffx64)!\n");
        return E_NOINTERFACE;
    }

    pfn_ex = (updateffxapi_pfn_ex)GetProcAddress(amdffx, "UpdateFfxApiProviderEx");
    pfn = (updateffxapi_pfn)GetProcAddress(amdffx, "UpdateFfxApiProvider");

    if (pfn_ex)
    {
        HRESULT ret = pfn_ex(data, size, unk_data);

        TRACE("status: %lx\n", ret);
        dump_provider(data);

        return ret;
    }

    if (pfn)
    {
        HRESULT ret;

        /* ensure user doesn't do dumb things on legacy amdxcffx64 */
        if (!this->fp8_supported)
        {
            ERR("FSR4 not supported on this system!\n");
            return E_NOINTERFACE;
        }

        if (!fsr4) return E_NOINTERFACE;

        ret = pfn(data, size);

        TRACE("status: %lx\n", ret);
        dump_provider(data);

        return ret;
    }

    ERR("UpdateFfxApiProvider[Ex] symbol not found!\n");
    return E_NOINTERFACE;
}

static const struct IAmdExtFfxApiVtbl AMDFSR4FFX_vtable = {
    AMDFSR4FFX_QueryInterface,
    AMDFSR4FFX_AddRef,
    AMDFSR4FFX_Release,
    AMDFSR4FFX_UpdateFfxApiProvider
};

struct AmdExtD3DShaderIntrinsics
{
    IAmdExtD3DShaderIntrinsics IAmdExtD3DShaderIntrinsics_iface;
    LONG ref;
    BOOL supports_wmma;
    BOOL supports_fp8;
};

struct AmdExtD3DShaderIntrinsics* impl_from_IAmdExtD3DShaderIntrinsics(IAmdExtD3DShaderIntrinsics *iface)
{
    return CONTAINING_RECORD(iface, struct AmdExtD3DShaderIntrinsics, IAmdExtD3DShaderIntrinsics_iface);
}

ULONG STDMETHODCALLTYPE AmdExtD3DShaderIntrinsics_AddRef(IAmdExtD3DShaderIntrinsics *iface)
{
    struct AmdExtD3DShaderIntrinsics *this = impl_from_IAmdExtD3DShaderIntrinsics(iface);
    return InterlockedIncrement(&this->ref);
}

ULONG STDMETHODCALLTYPE AmdExtD3DShaderIntrinsics_Release(IAmdExtD3DShaderIntrinsics *iface)
{
    struct AmdExtD3DShaderIntrinsics *this = impl_from_IAmdExtD3DShaderIntrinsics(iface);
    ULONG ret = InterlockedDecrement(&this->ref);
    if (!ret) free(this);
    return ret;
}

HRESULT STDMETHODCALLTYPE AmdExtD3DShaderIntrinsics_QueryInterface(IAmdExtD3DShaderIntrinsics *iface, REFIID iid, void **out)
{
    FIXME("%p %s %p stub!\n", iface, debugstr_guid(iid), out);
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE AmdExtD3DShaderIntrinsics_GetInfo(IAmdExtD3DShaderIntrinsics *iface,
                                                            AmdExtD3DShaderIntrinsicsInfo *info)
{
    FIXME("%p %p stub!\n", iface, info);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AmdExtD3DShaderIntrinsics_CheckSupport(IAmdExtD3DShaderIntrinsics *iface,
                                                                 AmdExtD3DShaderIntrinsicsSupport opcode)
{
    struct AmdExtD3DShaderIntrinsics *this = impl_from_IAmdExtD3DShaderIntrinsics(iface);
    if (opcode == AmdExtD3DShaderIntrinsicsSupport_Float8Conversion)
        return this->supports_fp8 ? S_OK : E_NOTIMPL;
    if (opcode == AmdExtD3DShaderIntrinsicsSupport_WaveMatrix)
        return this->supports_wmma ? S_OK : E_NOTIMPL;

    FIXME("%p %u stub!\n", iface, opcode);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AmdExtD3DShaderIntrinsics_Enable(IAmdExtD3DShaderIntrinsics *iface)
{
    TRACE("%p\n", iface);
    /* shader intrinsics are always handled by vkd3d-proton */
    return S_OK;
}

const static struct IAmdExtD3DShaderIntrinsicsVtbl AmdExtD3DShaderIntrinsics_vtable = {
    AmdExtD3DShaderIntrinsics_QueryInterface,
    AmdExtD3DShaderIntrinsics_AddRef,
    AmdExtD3DShaderIntrinsics_Release,
    AmdExtD3DShaderIntrinsics_GetInfo,
    AmdExtD3DShaderIntrinsics_CheckSupport,
    AmdExtD3DShaderIntrinsics_Enable
};

struct AmdExtD3DDevice8
{
    IAmdExtD3DDevice8 IAmdExtD3DDevice8_iface;
    LONG ref;
    BOOL fp8_supported;
};

struct AmdExtD3DDevice8 *impl_from_IAmdExtD3DDevice8(IAmdExtD3DDevice8 *iface)
{
    return CONTAINING_RECORD(iface, struct AmdExtD3DDevice8, IAmdExtD3DDevice8_iface);
}

HRESULT STDMETHODCALLTYPE AmdExtD3DDevice8_QueryInterface(IAmdExtD3DDevice8 *iface, REFIID iid, void **out)
{
    TRACE("%p %s %p\n", iface, debugstr_guid(iid), out);
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE AmdExtD3DDevice8_AddRef(IAmdExtD3DDevice8 *iface)
{
    struct AmdExtD3DDevice8* this = impl_from_IAmdExtD3DDevice8(iface);
    return InterlockedIncrement(&this->ref);
}

ULONG STDMETHODCALLTYPE AmdExtD3DDevice8_Release(IAmdExtD3DDevice8 *iface)
{
    struct AmdExtD3DDevice8* this = impl_from_IAmdExtD3DDevice8(iface);
    ULONG ret = InterlockedDecrement(&this->ref);
    if (!ret) free(this);
    return ret;
}

HRESULT STDMETHODCALLTYPE AmdExtD3DDevice8_CreateGraphicsPipelineState(IAmdExtD3DDevice8 *iface,
                                                                       const AmdExtD3DCreateInfo *pCreateInfo,
                                                                       const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc,
                                                                       REFIID iid, void **ppPipelineState)
{
    FIXME("%p %p %p %s %p stub!\n", iface, pCreateInfo, pDesc, debugstr_guid(iid), ppPipelineState);
    return E_NOTIMPL;
}

void STDMETHODCALLTYPE AmdExtD3DDevice8_PushMarker(IAmdExtD3DDevice8 *iface, ID3D12GraphicsCommandList *pGfxCmdList,
                                                   const char *pMarkerData)
{
    FIXME("%p %p %s stub!\n", iface, pGfxCmdList, pMarkerData);
}

void STDMETHODCALLTYPE AmdExtD3DDevice8_PopMarker(IAmdExtD3DDevice8 *iface, ID3D12GraphicsCommandList *pGfxCmdList)
{
    FIXME("%p %p stub!\n", iface, pGfxCmdList);
}

void STDMETHODCALLTYPE AmdExtD3DDevice8_SetMarker(IAmdExtD3DDevice8 *iface, ID3D12GraphicsCommandList *pGfxCmdList,
                                                  const char *pMarkerData)
{
    FIXME("%p %p %s stub!\n", iface, pGfxCmdList, pMarkerData);
}

HRESULT STDMETHODCALLTYPE AmdExtD3DDevice8_CheckExtFeatureSupport(IAmdExtD3DDevice8 *iface, AmdExtD3DCheckFeatureSupportType type,
                                                                  void *data, SIZE_T size)
{
    FIXME("%p %u %p %lu stub!\n", iface, type, data, (ULONG)size);
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE AmdExtD3DDevice8_CreateComputePipelineState(IAmdExtD3DDevice8 *iface,
                                                                      const AmdExtD3DCreateInfo* pAmdExtCreateInfo,
                                                                      const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
                                                                      REFIID iid, void **ppPipelineState)
{
    FIXME("%p %p %p %s %p stub!\n", iface, pAmdExtCreateInfo, pDesc, debugstr_guid(iid), ppPipelineState);
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE AmdExtD3DDevice8_CreatePipelineState(IAmdExtD3DDevice8 *iface,
                                                               const AmdExtD3DCreateInfo *pAmdExtCreateInfo,
                                                               const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc,
                                                               REFIID iid, void **ppPipelineState)
{
    FIXME("%p %p %p %s %p stub!\n", iface, pAmdExtCreateInfo, pDesc, debugstr_guid(iid), ppPipelineState);
    return E_NOTIMPL;
}

void STDMETHODCALLTYPE AmdExtD3DDevice8_SetPrimitiveTopology(IAmdExtD3DDevice8 *iface,
                                                             ID3D12GraphicsCommandList *pGfxCmdList,
                                                             AmdExtD3DPrimitiveTopology topology)
{
    FIXME("%p %p %u stub!\n", iface, pGfxCmdList, topology);
}

HRESULT STDMETHODCALLTYPE AmdExtD3DDevice8_CreateComputePipelineFromElf(IAmdExtD3DDevice8 *iface,
                                                                        AmdExtD3DPipelineElfInfo *pAmdExtCreateInfo,
                                                                        REFIID iid, void **ppPipelineState)
{
    FIXME("%p %p %s %p stub!\n", iface, pAmdExtCreateInfo, debugstr_guid(iid), ppPipelineState);
    return E_NOTIMPL;
}

void STDMETHODCALLTYPE AmdExtD3DDevice8_SetKernelArguments(IAmdExtD3DDevice8 *iface,
                                                           ID3D12GraphicsCommandList *pCmdList,
                                                           ULONG first, ULONG count, const void *ppValues)
{
    FIXME("%p %p %lu %lu %p stub!\n", iface, pCmdList, first, count, ppValues);
}

void STDMETHODCALLTYPE AmdExtD3DDevice8_GetGpuRtInterfaceVersion(IAmdExtD3DDevice8 *iface,
                                                                 AmdExtD3DGpuRtVersion *pInterfaceVersion)
{
    FIXME("%p %p stub!\n", iface, pInterfaceVersion);
}

void STDMETHODCALLTYPE AmdExtD3DDevice8_GetGpuRtBinaryVersion(IAmdExtD3DDevice8 *iface,
                                                              AmdExtD3DGpuRtVersion *pBinaryVersion)
{
    FIXME("%p %p stub!\n", iface, pBinaryVersion);
}

HRESULT STDMETHODCALLTYPE AmdExtD3DDevice8_CreateComputePipelineCrossCompile(IAmdExtD3DDevice8 *iface,
                                                                             const AmdExtD3DPipelineCrossCompileInfo* pAmdExtCreateInfo,
                                                                             REFIID iid, void **ppPipelineState)
{
    FIXME("%p %p %s %p stub!\n", iface, pAmdExtCreateInfo, debugstr_guid(iid), ppPipelineState);
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE AmdExtD3DDevice8_GetWaveMatrixProperties(IAmdExtD3DDevice8 *iface,
                                                                   SIZE_T *pCount, AmdExtWaveMatrixProperties *pProperties)
{
    struct AmdExtD3DDevice8 *this = impl_from_IAmdExtD3DDevice8(iface);
    static AmdExtWaveMatrixProperties prop[1] =
    {
        {
            16, 16, 16, AMD_EXT_WMMA_TYPE_FP8, AMD_EXT_WMMA_TYPE_FP8,
            AMD_EXT_WMMA_TYPE_FP32, AMD_EXT_WMMA_TYPE_FP32, FALSE
        }
    };

    TRACE("%p %p %p\n", iface, pCount, pProperties);

    if (!pCount) return E_INVALIDARG;

    if (!this->fp8_supported)
    {
        *pCount = 0;
        return S_OK;
    }

    if (*pCount >= sizeof(prop)/sizeof(prop[0]))
    {
        *pCount = sizeof(prop)/sizeof(prop[0]);
        memcpy(pProperties, prop, sizeof(prop));
        return S_OK;
    }

    return E_NOT_SUFFICIENT_BUFFER;
}

static const struct IAmdExtD3DDevice8Vtbl AmdExtD3DDevice8_vtable = {
    AmdExtD3DDevice8_QueryInterface,
    AmdExtD3DDevice8_AddRef,
    AmdExtD3DDevice8_Release,
    AmdExtD3DDevice8_CreateGraphicsPipelineState,
    AmdExtD3DDevice8_PushMarker,
    AmdExtD3DDevice8_PopMarker,
    AmdExtD3DDevice8_SetMarker,
    AmdExtD3DDevice8_CheckExtFeatureSupport,
    AmdExtD3DDevice8_CreateComputePipelineState,
    AmdExtD3DDevice8_CreatePipelineState,
    AmdExtD3DDevice8_SetPrimitiveTopology,
    AmdExtD3DDevice8_CreateComputePipelineFromElf,
    AmdExtD3DDevice8_SetKernelArguments,
    AmdExtD3DDevice8_GetGpuRtInterfaceVersion,
    AmdExtD3DDevice8_GetGpuRtBinaryVersion,
    AmdExtD3DDevice8_CreateComputePipelineCrossCompile,
    AmdExtD3DDevice8_GetWaveMatrixProperties
};

struct AmdExtD3DFactory
{
    IAmdExtD3DFactory IAmdExtD3DFactory_iface;
};

struct AmdExtD3DFactory* impl_from_IAmdExtD3DFactory(IAmdExtD3DFactory *iface)
{
    return CONTAINING_RECORD(iface, struct AmdExtD3DFactory, IAmdExtD3DFactory_iface);
}

ULONG STDMETHODCALLTYPE AmdExtD3DFactory_AddRef(IAmdExtD3DFactory *iface)
{
    return 2;
}

ULONG STDMETHODCALLTYPE AmdExtD3DFactory_Release(IAmdExtD3DFactory *iface)
{
    return 1;
}

HRESULT STDMETHODCALLTYPE AmdExtD3DFactory_CreateInterface(IAmdExtD3DFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
    TRACE("%p %p %s %p\n", iface, outer, debugstr_guid(iid), out);

    if (!out) return E_INVALIDARG;
    *out = NULL;

    if (IsEqualGUID(iid, &IID_IAmdExtD3DShaderIntrinsics))
    {
        struct AmdExtD3DShaderIntrinsics *this = calloc(1, sizeof(struct AmdExtD3DShaderIntrinsics));
        this->IAmdExtD3DShaderIntrinsics_iface.lpVtbl = &AmdExtD3DShaderIntrinsics_vtable;
        this->ref = 1;
        check_fsr4_supported((ID3D12Device *)outer, &this->supports_fp8, &this->supports_wmma);
        *out = &this->IAmdExtD3DShaderIntrinsics_iface;
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IAmdExtD3DDevice8))
    {
        struct AmdExtD3DDevice8 *this = calloc(1, sizeof(struct AmdExtD3DDevice8));
        this->IAmdExtD3DDevice8_iface.lpVtbl = &AmdExtD3DDevice8_vtable;
        this->ref = 1;
        check_fsr4_supported((ID3D12Device *)outer, &this->fp8_supported, NULL);
        *out = &this->IAmdExtD3DDevice8_iface;
        return S_OK;
    }
    /* some apps will try to query IAmdExtD3DDevice1 for debugging markers,
     * prevent that from causing spam in proton logs */
    else if (IsEqualGUID(iid, &IID_IAmdExtD3DDevice) ||
             IsEqualGUID(iid, &IID_IAmdExtD3DDevice1))
        return E_NOINTERFACE;
    else
    {
        FIXME("unknown guid %s\n", debugstr_guid(iid));
    }

    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE AmdExtD3DFactory_QueryInterface(IAmdExtD3DFactory *iface, REFIID iid, void **out)
{
    TRACE("%p %s %p\n", iface, debugstr_guid(iid), out);
    return E_NOINTERFACE;
}

static const struct IAmdExtD3DFactoryVtbl AmdExtD3DFactory_vtable = {
    AmdExtD3DFactory_QueryInterface,
    AmdExtD3DFactory_AddRef,
    AmdExtD3DFactory_Release,
    AmdExtD3DFactory_CreateInterface
};

static const struct AmdExtD3DFactory amd_d3d_factory = {
    .IAmdExtD3DFactory_iface = { &AmdExtD3DFactory_vtable },
};

HRESULT CDECL AmdExtD3DCreateInterface(IUnknown *outer, REFIID iid, void **obj)
{
    TRACE("outer %p, iid %s, obj %p\n", outer, debugstr_guid(iid), obj);

    if (IsEqualGUID(iid, &IID_IAmdExtFfxApi))
    {
        struct AMDFSR4FFX* ffx = calloc(1, sizeof(struct AMDFSR4FFX));
        ffx->IAmdExtFfxApi_iface.lpVtbl = &AMDFSR4FFX_vtable;
        ffx->ref = 1;
        ffx->rdna2 = is_rdna2((ID3D12Device *)outer);
        check_fsr4_supported((ID3D12Device *)outer, &ffx->fp8_supported, NULL);
        *obj = &ffx->IAmdExtFfxApi_iface;
        return S_OK;
    } else if (IsEqualGUID(iid, &IID_IAmdExtAntiLagApi)) {
        return ID3D12Device_QueryInterface((ID3D12Device *)outer, &IID_IAmdExtAntiLagApi, obj);
    } else if (IsEqualGUID(iid, &IID_IAmdExtD3DFactory)) {
        *obj = (void *)&amd_d3d_factory.IAmdExtD3DFactory_iface;
        return S_OK;
    } else {
        FIXME("unknown guid: %s\n", debugstr_guid(iid));
    }

    return E_NOINTERFACE;
}

HMODULE WINAPI AmdGetDxcModuleHandle(void)
{
    return GetModuleHandleA(NULL);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    const char *env;

    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
        {
            /* this creates a watermark for driver side FSR3 as well */
            if ((env = getenv("FSR_WATERMARK")) && !strcmp(env, "1"))
                _putenv("MLSR-WATERMARK=1");
            /* same here */
            if ((env = getenv("FSR_FG_WATERMARK")) && !strcmp(env, "1"))
                _putenv("MLFI-WATERMARK=1");
            break;
        }
        default: break;
    }

    return TRUE;
}
