/*
 * atidxx64 implementation
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
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"

#include "wine/vulkan.h"
#include "wine/asm.h"

#define COBJMACROS
#include "initguid.h"
#include "d3d11.h"
#include "d3d12.h"

#include "dxgi1_6.h"

#include "dxvk_interfaces.h"
#include "atidxx.h"

#include <wingdi.h>

WINE_DEFAULT_DEBUG_CHANNEL(atidxx);

static HMODULE d3d11_module;
static HRESULT (WINAPI *pD3D11CreateDevice)(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type,
        HMODULE swrast, UINT flags, const D3D_FEATURE_LEVEL *feature_levels, UINT levels,
        UINT sdk_version, ID3D11Device **device_out, D3D_FEATURE_LEVEL *obtained_feature_level,
        ID3D11DeviceContext **immediate_context);

typedef void (*vtable_ptr)(void);

static HRESULT load_d3d11(void)
{
    if (!d3d11_module)
        d3d11_module = LoadLibraryA("d3d11.dll");

    if (!d3d11_module)
    {
        ERR("Failed to load d3d11.dll\n");
        return E_FAIL;
    }

    return S_OK;
}


HRESULT WINAPI AmdD3D11CreateDeviceExt(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE swrast, UINT flags,
        const D3D_FEATURE_LEVEL *feature_levels, UINT levels, UINT sdk_version, ID3D11Device **device_out,
        D3D_FEATURE_LEVEL *obtained_feature_level, ID3D11DeviceContext **immediate_context, void *unk)
{
    HRESULT ret;
    FIXME("%p semi-stub\n", unk);

    if ((ret = load_d3d11()))
        return ret;

    if (!pD3D11CreateDevice)
        pD3D11CreateDevice = (void*)GetProcAddress(d3d11_module, "D3D11CreateDevice");

    return pD3D11CreateDevice(adapter, driver_type, swrast, flags, feature_levels, levels, sdk_version,
            device_out, obtained_feature_level, immediate_context);
}

/*
    Ext Ifaces to implement:
    field_0x170 = 0x11
    field_0x190 = 0x17
    field_0x188 = 0x15
    field_0x178 = 0x14
    field_0x1a0 = 0x1d
    field_0x168 = 0xb
    field_0x160 = 0xf
    field_0x198 = 0x17
 */

/* field_0x160 (0xf)

    0x0 = AddRef
    0x8 = Release
    0x10 = virtual destructor
    0x18 = BeginUAVOverlap
    0x20 = EndUAVOverlap
    0x28 = GetVersion (called on init, prob some kind of version getter)

*/

/* field_0x168 (0xb)
    0x0 = AddRef
    0x8 = Release
    0x10 = virtual destructor
    0x18 = SetDepthBounds
    0x20 = GetVersion
*/

/* field_0x170 (0x11)
    0x0 = AddRef
    0x8 = Release
    0x10 = virtual destructor
    0x18 = GetVersion
    0x20 = MultiDrawIndirect
    0x28 = MultiDrawIndexedIndirect
    0x30 = MultiDrawIndirectCount
    0x38 = MultiDrawIndexedIndirectCount
*/

typedef struct
{
    IAmdDxExt IAmdDxExt_iface;
    IAmdDxExtUAVOverlap IAmdDxExtUAVOverlap_iface;
    IAmdDxExtQuadBufferStereo IAmdDxExtQuadBufferStereo_iface;
    IAmdDxExtDepthBounds IAmdDxExtDepthBounds_iface;
    IAmdDxExtMultidrawIndirect IAmdDxExtMultidrawIndirect_iface;
    LONG ref;
    //could also be a d3d10 device, just cast
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11VkExtContext *ext_context;
    BOOL is_d3d11;
    BOOL uav_overlap;
    BOOL depth_bounds;
    BOOL multi_draw_indirect;
    BOOL multi_draw_indirect_count;
} AmdDxExt;

static inline AmdDxExt *impl_from_IAmdDxExt(IAmdDxExt *iface)
{
    return CONTAINING_RECORD(iface, AmdDxExt, IAmdDxExt_iface);
}

static inline AmdDxExt *impl_from_IAmdDxExtUAVOverlap(IAmdDxExtUAVOverlap *iface)
{
    return CONTAINING_RECORD(iface, AmdDxExt, IAmdDxExtUAVOverlap_iface);
}

static inline AmdDxExt *impl_from_IAmdDxExtQuadBufferStereo(IAmdDxExtQuadBufferStereo *iface)
{
    return CONTAINING_RECORD(iface, AmdDxExt, IAmdDxExtQuadBufferStereo_iface);
}

static inline AmdDxExt *impl_from_IAmdDxExtDepthBounds(IAmdDxExtDepthBounds *iface)
{
    return CONTAINING_RECORD(iface, AmdDxExt, IAmdDxExtDepthBounds_iface);
}

static inline AmdDxExt *impl_from_IAmdDxExtMultidrawIndirect(IAmdDxExtMultidrawIndirect *iface)
{
    return CONTAINING_RECORD(iface, AmdDxExt, IAmdDxExtMultidrawIndirect_iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExt_AddRef, 4)
unsigned int __thiscall AmdDxExt_AddRef(IAmdDxExt *iface)
{
    AmdDxExt *This = impl_from_IAmdDxExt(iface);
    return InterlockedIncrement(&This->ref);
}

DEFINE_THISCALL_WRAPPER(AmdDxExt_Release, 4)
unsigned int __thiscall AmdDxExt_Release(IAmdDxExt *iface)
{
    AmdDxExt *This = impl_from_IAmdDxExt(iface);
    LONG ref = InterlockedDecrement(&This->ref);
    if(ref == 0)
    {
        if(This->is_d3d11)
        {
            ID3D11Device_Release(This->device);
            ID3D11DeviceContext_Release(This->context);
            ID3D11VkExtContext_Release(This->ext_context);
        }
        free(This);
    }
    return ref;
}

DEFINE_THISCALL_WRAPPER(AmdDxExt_Release, 4)
void __thiscall AmdDxExt_Destroy(IAmdDxExt *iface)
{
    FIXME("%p stub!\n", iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExt_GetVersion, 8)
HRESULT __thiscall AmdDxExt_GetVersion(IAmdDxExt *ext, AmdDxExtVersion *version)
{
    FIXME("%p %p\n", ext, version);

    version->majorVersion = 1;
    version->minorVersion = 0;

    return S_OK;
}

DEFINE_THISCALL_WRAPPER(AmdDxExt_GetExtInterface, 8)
IAmdDxExtInterface* __thiscall AmdDxExt_GetExtInterface(IAmdDxExt *ext, unsigned int iface)
{
    AmdDxExt *This = impl_from_IAmdDxExt(ext);
    IAmdDxExtInterface *ret = NULL;
    TRACE("%p %x\n", ext, iface);

    switch(iface)
    {
        case 0x2:
            ret = (IAmdDxExtInterface *)&This->IAmdDxExtQuadBufferStereo_iface;
            break;
        case 0xb:
            ret = (IAmdDxExtInterface *)&This->IAmdDxExtDepthBounds_iface;
            break;
        case 0xf:
            ret = (IAmdDxExtInterface *)&This->IAmdDxExtUAVOverlap_iface;
            break;
        case 0x11:
            ret = (IAmdDxExtInterface *)&This->IAmdDxExtMultidrawIndirect_iface;
            break;
        default:
        {
            FIXME("Unknown interface %x\n", iface);
            break;
        }
    }

    if (ret) AmdDxExt_AddRef(ext);

    return ret;
}

DEFINE_THISCALL_WRAPPER(AmdDxExt_IaSetPrimitiveTopology, 8)
HRESULT __thiscall AmdDxExt_IaSetPrimitiveTopology(IAmdDxExt *ext, D3D_PRIMITIVE_TOPOLOGY topology)
{
    AmdDxExt *This = impl_from_IAmdDxExt(ext);
    TRACE("%p %u\n", ext, topology);

    if(This->is_d3d11)
    {
        ID3D11DeviceContext_IASetPrimitiveTopology(This->context, topology);
    }

    return S_OK;
}

DEFINE_THISCALL_WRAPPER(AmdDxExt_IaGetPrimitiveTopology, 8)
HRESULT __thiscall AmdDxExt_IaGetPrimitiveTopology(IAmdDxExt *ext, AmdDxExtPrimitiveTopology *topology)
{
    FIXME("%p %p stub\n", ext, topology);

    return E_NOTIMPL;
}

DEFINE_THISCALL_WRAPPER(AmdDxExt_SetSingleSampleRead, 12)
HRESULT __thiscall AmdDxExt_SetSingleSampleRead(IAmdDxExt *iface, ID3D10Resource *res, BOOL single_sample)
{
    FIXME("%p %p %u stub\n", iface, res, single_sample);

    return E_NOTIMPL;
}

DEFINE_THISCALL_WRAPPER(AmdDxExt_SetSingleSampleRead11, 12)
HRESULT __thiscall AmdDxExt_SetSingleSampleRead11(IAmdDxExt *iface, ID3D11Resource *res, BOOL single_sample)
{
    FIXME("%p %p %u stub\n", iface, res, single_sample);

    return E_NOTIMPL;
}

DEFINE_THISCALL_WRAPPER(AmdDxExt_QueryFeatureSupport, 16)
HRESULT __thiscall AmdDxExt_QueryFeatureSupport(IAmdDxExt *iface, unsigned int feature_token, void *data, unsigned int data_size)
{
    FIXME("%p %u %p %u stub\n", iface, feature_token, data, data_size);

    return E_NOTIMPL;
}

DEFINE_THISCALL_WRAPPER(AmdDxExt_IaSetPrimitiveTopologyCtx, 12)
HRESULT __thiscall AmdDxExt_IaSetPrimitiveTopologyCtx(IAmdDxExt *iface, unsigned int topology, ID3D11DeviceContext *ctx)
{
    FIXME("%p %u %p stub\n", iface, topology, ctx);

    return E_NOTIMPL;
}

DEFINE_THISCALL_WRAPPER(AmdDxExt_IaGetPrimitiveTopologyCtx, 12)
HRESULT __thiscall AmdDxExt_IaGetPrimitiveTopologyCtx(IAmdDxExt *iface, AmdDxExtPrimitiveTopology *topology, ID3D11DeviceContext *ctx)
{
    FIXME("%p %p %p stub\n", iface, topology, ctx);

    return E_NOTIMPL;
}

/* our release method handles destruction */
DEFINE_THISCALL_WRAPPER(AmdDxExtUAVOverlap_Destroy, 4)
void __thiscall AmdDxExtUAVOverlap_Destroy(IAmdDxExtUAVOverlap *iface)
{
    FIXME("%p stub!\n", iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExtUAVOverlap_AddRef, 4)
unsigned int __thiscall AmdDxExtUAVOverlap_AddRef(IAmdDxExtUAVOverlap *iface)
{
    AmdDxExt *This = impl_from_IAmdDxExtUAVOverlap(iface);
    return AmdDxExt_AddRef(&This->IAmdDxExt_iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExtUAVOverlap_Release, 4)
unsigned int __thiscall AmdDxExtUAVOverlap_Release(IAmdDxExtUAVOverlap *iface)
{
    AmdDxExt *This = impl_from_IAmdDxExtUAVOverlap(iface);
    return AmdDxExt_Release(&This->IAmdDxExt_iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExtUAVOverlap_BeginUAVOverlap, 4)
HRESULT __thiscall AmdDxExtUAVOverlap_BeginUAVOverlap(IAmdDxExtUAVOverlap *iface)
{
    AmdDxExt *This = impl_from_IAmdDxExtUAVOverlap(iface);
    TRACE("%p\n", iface);

    if (!This->ext_context) return E_FAIL;

    if (!This->uav_overlap) return E_FAIL;

    ID3D11VkExtContext_SetBarrierControl(This->ext_context, D3D11_VK_BARRIER_CONTROL_IGNORE_WRITE_AFTER_WRITE);

    return S_OK;
}

DEFINE_THISCALL_WRAPPER(AmdDxExtUAVOverlap_EndUAVOverlap, 4)
HRESULT __thiscall AmdDxExtUAVOverlap_EndUAVOverlap(IAmdDxExtUAVOverlap *iface)
{
    AmdDxExt *This = impl_from_IAmdDxExtUAVOverlap(iface);
    TRACE("%p\n", iface);

    if (!This->ext_context) return E_FAIL;

    if (!This->uav_overlap) return E_FAIL;

    ID3D11VkExtContext_SetBarrierControl(This->ext_context, 0);

    return S_OK;
}

DEFINE_THISCALL_WRAPPER(AmdDxExtUAVOverlap_GetVersion, 8)
void __thiscall AmdDxExtUAVOverlap_GetVersion(IAmdDxExtUAVOverlap *iface, AmdDxExtVersion* version)
{
    FIXME("%p %p stub!\n", iface, version);
    version->majorVersion = 1;
    version->minorVersion = 0;
}

DEFINE_THISCALL_WRAPPER(AmdDxExtQuadBufferStereo_AddRef, 4)
unsigned int __thiscall AmdDxExtQuadBufferStereo_AddRef(IAmdDxExtQuadBufferStereo *iface)
{
    AmdDxExt *This = impl_from_IAmdDxExtQuadBufferStereo(iface);
    return AmdDxExt_AddRef(&This->IAmdDxExt_iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExtQuadBufferStereo_Release, 4)
unsigned int __thiscall AmdDxExtQuadBufferStereo_Release(IAmdDxExtQuadBufferStereo *iface)
{
    AmdDxExt *This = impl_from_IAmdDxExtQuadBufferStereo(iface);
    return AmdDxExt_Release(&This->IAmdDxExt_iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExtQuadBufferStereo_Destroy, 4)
void __thiscall AmdDxExtQuadBufferStereo_Destroy(IAmdDxExtQuadBufferStereo *iface)
{
    FIXME("%p stub!\n", iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExtQuadBufferStereo_EnableQuadBufferStereo, 8)
HRESULT __thiscall AmdDxExtQuadBufferStereo_EnableQuadBufferStereo(IAmdDxExtQuadBufferStereo *iface, BOOL enable)
{
    FIXME("%p %u stub\n", iface, enable);

    return E_NOTIMPL;
}

DEFINE_THISCALL_WRAPPER(AmdDxExtQuadBufferStereo_GetDisplayModeList, 20)
HRESULT __thiscall AmdDxExtQuadBufferStereo_GetDisplayModeList(IAmdDxExtQuadBufferStereo *iface, DXGI_FORMAT format, UINT flags, UINT *num_modes, DXGI_MODE_DESC *desc)
{
    FIXME("%p %u %u %p %p stub\n", iface, format, flags, num_modes, desc);

    if (!num_modes) return E_INVALIDARG;

    *num_modes = 0;

    return S_OK;
}

DEFINE_THISCALL_WRAPPER(AmdDxExtQuadBufferStereo_GetLineOffset, 8)
UINT __thiscall AmdDxExtQuadBufferStereo_GetLineOffset(IAmdDxExtQuadBufferStereo *iface, IDXGISwapChain *swapchain)
{
    FIXME("%p %p stub\n", iface, swapchain);

    return 0;
}

DEFINE_THISCALL_WRAPPER(AmdDxExtDepthBounds_AddRef, 4)
unsigned int __thiscall AmdDxExtDepthBounds_AddRef(IAmdDxExtDepthBounds *iface)
{
    AmdDxExt *This = impl_from_IAmdDxExtDepthBounds(iface);
    return AmdDxExt_AddRef(&This->IAmdDxExt_iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExtDepthBounds_Release, 4)
unsigned int __thiscall AmdDxExtDepthBounds_Release(IAmdDxExtDepthBounds *iface)
{
    AmdDxExt *This = impl_from_IAmdDxExtDepthBounds(iface);
    return AmdDxExt_Release(&This->IAmdDxExt_iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExtDepthBounds_GetVersion, 8)
void __thiscall AmdDxExtDepthBounds_GetVersion(IAmdDxExtDepthBounds *iface, AmdDxExtVersion* version)
{
    FIXME("%p %p stub!\n", iface, version);
    version->majorVersion = 1;
    version->minorVersion = 0;
}

DEFINE_THISCALL_WRAPPER(AmdDxExtDepthBounds_SetDepthBounds, 16)
HRESULT __thiscall AmdDxExtDepthBounds_SetDepthBounds(IAmdDxExtDepthBounds *iface, BOOL enabled, float min, float max)
{
    AmdDxExt *This = impl_from_IAmdDxExtDepthBounds(iface);

    TRACE("%p %u %f %f\n", iface, enabled, min, max);

    if (!This->ext_context) return E_FAIL;

    if (!This->depth_bounds) return E_FAIL;

    ID3D11VkExtContext_SetDepthBoundsTest(This->ext_context, enabled, min, max);

    return S_OK;
}

DEFINE_THISCALL_WRAPPER(AmdDxExtDepthBounds_Destroy, 4)
void __thiscall AmdDxExtDepthBounds_Destroy(IAmdDxExtDepthBounds *iface)
{
    FIXME("%p stub\n", iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExtMultidrawIndirect_AddRef, 4)
unsigned int __thiscall AmdDxExtMultidrawIndirect_AddRef(IAmdDxExtMultidrawIndirect *iface)
{
    AmdDxExt *This = impl_from_IAmdDxExtMultidrawIndirect(iface);
    return AmdDxExt_AddRef(&This->IAmdDxExt_iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExtMultidrawIndirect_Release, 4)
unsigned int __thiscall AmdDxExtMultidrawIndirect_Release(IAmdDxExtMultidrawIndirect *iface)
{
    AmdDxExt *This = impl_from_IAmdDxExtMultidrawIndirect(iface);
    return AmdDxExt_Release(&This->IAmdDxExt_iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExtMultidrawIndirect_Destroy, 4)
void __thiscall AmdDxExtMultidrawIndirect_Destroy(IAmdDxExtMultidrawIndirect *iface)
{
    FIXME("%p stub\n", iface);
}

DEFINE_THISCALL_WRAPPER(AmdDxExtMultidrawIndirect_GetVersion, 8)
void __thiscall AmdDxExtMultidrawIndirect_GetVersion(IAmdDxExtMultidrawIndirect *iface, AmdDxExtVersion* version)
{
    AmdDxExt *This = impl_from_IAmdDxExtMultidrawIndirect(iface);
    FIXME("%p %p semi-stub!\n", iface, version);
    version->majorVersion = 1;
    /* needed for indirect count */
    version->minorVersion = This->multi_draw_indirect_count ? 2 : 0;
}

DEFINE_THISCALL_WRAPPER(AmdDxExtMultidrawIndirect_MultiDrawIndirect, 20)
HRESULT __thiscall AmdDxExtMultidrawIndirect_MultiDrawIndirect(IAmdDxExtMultidrawIndirect *iface, unsigned int draw_count, ID3D11Buffer *buffer, unsigned int byte_offset, unsigned int byte_stride)
{
    AmdDxExt *This = impl_from_IAmdDxExtMultidrawIndirect(iface);
    TRACE("%p %u %p %u %u\n", iface, draw_count, buffer, byte_offset, byte_stride);

    if (!This->ext_context) return E_FAIL;

    if (!This->multi_draw_indirect) return E_FAIL;

    ID3D11VkExtContext_MultiDrawIndirect(This->ext_context, draw_count, buffer, byte_offset, byte_stride);

    return S_OK;
}

DEFINE_THISCALL_WRAPPER(AmdDxExtMultidrawIndirect_MultiDrawIndexedIndirect, 20)
HRESULT __thiscall AmdDxExtMultidrawIndirect_MultiDrawIndexedIndirect(IAmdDxExtMultidrawIndirect *iface, unsigned int draw_count, ID3D11Buffer *buffer, unsigned int byte_offset, unsigned int byte_stride)
{
    AmdDxExt *This = impl_from_IAmdDxExtMultidrawIndirect(iface);
    TRACE("%p %u %p %u %u\n", iface, draw_count, buffer, byte_offset, byte_stride);

    if (!This->ext_context) return E_FAIL;

    if (!This->multi_draw_indirect) return E_FAIL;

    ID3D11VkExtContext_MultiDrawIndexedIndirect(This->ext_context, draw_count, buffer, byte_offset, byte_stride);

    return S_OK;
}

static unsigned int get_max_draw_count(ID3D11Buffer *buffer, unsigned int offset, unsigned int stride, unsigned int size)
{
    D3D11_BUFFER_DESC desc;
    unsigned int count;

    ID3D11Buffer_GetDesc(buffer, &desc);

    if(offset >= desc.ByteWidth)
    {
        WARN("Offset %u, buffer size %u.", offset, desc.ByteWidth);
        return 0;
    }

    count = (desc.ByteWidth - offset) / stride;
    if (desc.ByteWidth - offset - count * stride >= size)
        ++count;

    if (!count)
        WARN("zero count, buffer size %u, offset %u, stride %u, size %u.\n", desc.ByteWidth, offset, stride, size);

    return count;
}

DEFINE_THISCALL_WRAPPER(AmdDxextMultiDrawIndirect_MultiDrawIndirectCount, 24)
HRESULT __thiscall AmdDxextMultiDrawIndirect_MultiDrawIndirectCount(IAmdDxExtMultidrawIndirect *iface, ID3D11Buffer *buffer_for_count, unsigned int byte_offset_for_count, ID3D11Buffer *buffer, unsigned int byte_offset, unsigned int byte_stride)
{
    AmdDxExt *This = impl_from_IAmdDxExtMultidrawIndirect(iface);
    unsigned int max_draw_count;
    TRACE("%p %p %u %p %u %u\n", iface, buffer_for_count, byte_offset_for_count, buffer, byte_offset, byte_stride);

    if (!This->ext_context) return E_FAIL;

    if (!This->multi_draw_indirect_count) return E_FAIL;

    max_draw_count = get_max_draw_count(buffer, byte_offset, byte_stride, sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS));

    ID3D11VkExtContext_MultiDrawIndirectCount(This->ext_context, max_draw_count, buffer_for_count, byte_offset_for_count, buffer, byte_offset, byte_stride);

    return S_OK;
}

DEFINE_THISCALL_WRAPPER(AmdDxextMultiDrawIndirect_MultiDrawIndexedIndirectCount, 24)
HRESULT __thiscall AmdDxextMultiDrawIndirect_MultiDrawIndexedIndirectCount(IAmdDxExtMultidrawIndirect *iface, ID3D11Buffer *buffer_for_count, unsigned int byte_offset_for_count, ID3D11Buffer *buffer, unsigned int byte_offset, unsigned int byte_stride)
{
    AmdDxExt *This = impl_from_IAmdDxExtMultidrawIndirect(iface);
    unsigned int max_draw_count;
    TRACE("%p %p %u %p %u %u\n", iface, buffer_for_count, byte_offset_for_count, buffer, byte_offset, byte_stride);

    if (!This->ext_context) return E_FAIL;

    if (!This->multi_draw_indirect_count) return E_FAIL;

    max_draw_count = get_max_draw_count(buffer, byte_offset, byte_stride, sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS));

    ID3D11VkExtContext_MultiDrawIndexedIndirectCount(This->ext_context, max_draw_count, buffer_for_count, byte_offset_for_count, buffer, byte_offset, byte_stride);

    return S_OK;
}

static const IAmdDxExtUAVOverlapVtbl amddxext_uav_vtable =
{
    THISCALL(AmdDxExtUAVOverlap_AddRef),
    THISCALL(AmdDxExtUAVOverlap_Release),
    THISCALL(AmdDxExtUAVOverlap_Destroy),
    THISCALL(AmdDxExtUAVOverlap_BeginUAVOverlap),
    THISCALL(AmdDxExtUAVOverlap_EndUAVOverlap),
    THISCALL(AmdDxExtUAVOverlap_GetVersion)
};

static const IAmdDxExtVtbl AmdDxExt_vtable =
{
    THISCALL(AmdDxExt_AddRef), //0
    THISCALL(AmdDxExt_Release), //0x8
    THISCALL(AmdDxExt_Destroy), //0x10
    THISCALL(AmdDxExt_GetVersion), //0x18
    THISCALL(AmdDxExt_GetExtInterface), //0x20
    THISCALL(AmdDxExt_IaSetPrimitiveTopology), //0x28
    THISCALL(AmdDxExt_IaGetPrimitiveTopology), // 0x30
    THISCALL(AmdDxExt_SetSingleSampleRead), //0x38 ??
    THISCALL(AmdDxExt_SetSingleSampleRead11), //0x40 ??
    THISCALL(AmdDxExt_QueryFeatureSupport), //0x48
    THISCALL(AmdDxExt_IaSetPrimitiveTopologyCtx), //0x50 ??
    THISCALL(AmdDxExt_IaGetPrimitiveTopologyCtx) //0x58 ??
};

static const IAmdDxExtQuadBufferStereoVtbl quadbufstereo_vtable =
{
    THISCALL(AmdDxExtQuadBufferStereo_AddRef),
    THISCALL(AmdDxExtQuadBufferStereo_Release),
    THISCALL(AmdDxExtQuadBufferStereo_Destroy),
    THISCALL(AmdDxExtQuadBufferStereo_EnableQuadBufferStereo),
    THISCALL(AmdDxExtQuadBufferStereo_GetLineOffset),
    THISCALL(AmdDxExtQuadBufferStereo_GetDisplayModeList),
};

static const IAmdDxExtDepthBoundsVtbl amddxext_depth_vtable =
{
    THISCALL(AmdDxExtDepthBounds_AddRef),
    THISCALL(AmdDxExtDepthBounds_Release),
    THISCALL(AmdDxExtDepthBounds_Destroy),
    THISCALL(AmdDxExtDepthBounds_SetDepthBounds),
    THISCALL(AmdDxExtDepthBounds_GetVersion)
};

static const IAmdDxExtMultidrawIndirectVtbl amddxext_multidraw_vtable =
{
    THISCALL(AmdDxExtMultidrawIndirect_AddRef),
    THISCALL(AmdDxExtMultidrawIndirect_Release),
    THISCALL(AmdDxExtMultidrawIndirect_Destroy),
    THISCALL(AmdDxExtMultidrawIndirect_GetVersion),
    THISCALL(AmdDxExtMultidrawIndirect_MultiDrawIndirect),
    THISCALL(AmdDxExtMultidrawIndirect_MultiDrawIndexedIndirect),
    THISCALL(AmdDxextMultiDrawIndirect_MultiDrawIndirectCount),
    THISCALL(AmdDxextMultiDrawIndirect_MultiDrawIndexedIndirectCount)
};

HRESULT CDECL AmdDxExtCreate11(ID3D11Device *device, IAmdDxExt **ext)
{
    UINT64 id;
    HRESULT ret;
    AmdDxExt *obj;
    ID3D11VkExtDevice *ext_device;
    TRACE("%p %p\n", device, ext);

    if (!ext) return E_INVALIDARG;

    id = (ULONG_PTR)*ext;

    if (id == 0xbf380ebc5ab4d0a6ull)
    {
        ERR("D3D11 Anti-Lag 2 is not supported!\n");
        return E_NOTIMPL;
    }

    if ((ret = load_d3d11()))
        return ret;

    if (!(obj = malloc(sizeof(AmdDxExt))))
        return E_OUTOFMEMORY;

    obj->device = device;
    ID3D11Device_AddRef(device);
    ID3D11Device_GetImmediateContext(device, &obj->context);

    if(FAILED(ret = ID3D11DeviceContext_QueryInterface(obj->context, &IID_ID3D11VkExtContext, (void**)&obj->ext_context)))
    {
        ERR("Failed to get ID3D11VkExtContext\n");
        return ret;
    }

    if(FAILED(ret = ID3D11Device_QueryInterface(device, &IID_ID3D11VkExtDevice, (void**)&ext_device)))
    {
        ERR("Failed to get ID3D11VkExtDevice\n");
        return ret;
    }

    obj->IAmdDxExt_iface.lpVtbl = &AmdDxExt_vtable;
    obj->IAmdDxExtUAVOverlap_iface.lpVtbl = &amddxext_uav_vtable;
    obj->IAmdDxExtQuadBufferStereo_iface.lpVtbl = &quadbufstereo_vtable;
    obj->IAmdDxExtDepthBounds_iface.lpVtbl = &amddxext_depth_vtable;
    obj->IAmdDxExtMultidrawIndirect_iface.lpVtbl = &amddxext_multidraw_vtable;
    obj->is_d3d11 = TRUE;
    obj->ref = 1;

    obj->depth_bounds = !!ID3D11VkExtDevice_GetExtensionSupport(ext_device, D3D11_VK_EXT_DEPTH_BOUNDS);
    obj->uav_overlap = !!ID3D11VkExtDevice_GetExtensionSupport(ext_device, D3D11_VK_EXT_BARRIER_CONTROL);
    obj->multi_draw_indirect = !!ID3D11VkExtDevice_GetExtensionSupport(ext_device, D3D11_VK_EXT_MULTI_DRAW_INDIRECT);
    obj->multi_draw_indirect_count = !!ID3D11VkExtDevice_GetExtensionSupport(ext_device, D3D11_VK_EXT_MULTI_DRAW_INDIRECT_COUNT);

    TRACE("Supported extensions:\n DepthBounds: %d, UAVOverlap: %d, MultiDrawIndirect: %d, MultiDrawIndirectCount: %d\n",
            obj->depth_bounds, obj->uav_overlap, obj->multi_draw_indirect, obj->multi_draw_indirect_count);

    *ext = &obj->IAmdDxExt_iface;

    ID3D11VkExtDevice_Release(ext_device);

    return S_OK;
}
