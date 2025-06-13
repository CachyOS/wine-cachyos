#include "objbase.h"

typedef struct
{
    unsigned int        majorVersion;
    unsigned int        minorVersion;
} AmdDxExtVersion;

typedef enum
{
                                                       // D3D10_DDI_PRIMITIVE_TOPOLOGY_* values
    AmdDxExtPrimitiveTopology_Undefined          = 0,  // D3D10 UNDEFINED
    AmdDxExtPrimitiveTopology_PointList          = 1,  // D3D10 POINTLIST
    AmdDxExtPrimitiveTopology_LineList           = 2,  // D3D10 LINELIST
    AmdDxExtPrimitiveTopology_LineStrip          = 3,  // D3D10 LINESTRIP
    AmdDxExtPrimitiveTopology_TriangleList       = 4,  // D3D10 TRIANGLELIST
    AmdDxExtPrimitiveTopology_TriangleStrip      = 5,  // D3D10 TRIANGLESTRIP
                                                       // 6 is reserved for legacy triangle fans
    AmdDxExtPrimitiveTopology_ExtQuadList        = 7,  // No D3D10 equivalent
    AmdDxExtPrimitiveTopology_ExtPatch           = 8,  // No D3D10 equivalent
    AmdDxExtPrimitiveTopology_ExtScreenRectList  = 9,  // No D3D10 equivalent
    AmdDxExtPrimitiveTopology_LineListAdj        = 10, // D3D10 LINELIST_ADJ
    AmdDxExtPrimitiveTopology_LineStripAdj       = 11, // D3D10 LINESTRIP_ADJ
    AmdDxExtPrimitiveTopology_TriangleListAdj    = 12, // D3D10 TRIANGLELIST_ADJ
    AmdDxExtPrimitiveTopology_TriangleStripAdj   = 13, // D3D10 TRIANGLESTRIP_ADJ
    AmdDxExtPrimitiveTopology_Max                = 14
} AmdDxExtPrimitiveTopology;

typedef enum
{
    AmdDxExtFeature_ScreenRectSupport           = 1, // Screen Rect supported - data is BOOL
    AmdDxExtFeature_DeviceHp3d                  = 2, // HP3D support
    AmdDxExtFeature_DeviceCtxSupport            = 3, // Device Ctx Support
} AmdDxExtFeatureToken;

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
#define THISCALLMETHOD_(type,method)  virtual type __thiscall method
#else
#define THISCALLMETHOD_(type,method)  type (__thiscall *method)
#endif

#define INTERFACE IAmdDxExtInterface
DECLARE_INTERFACE(IAmdDxExtInterface)
{
    THISCALLMETHOD_(unsigned int, AddRef)(THIS) PURE;
    THISCALLMETHOD_(unsigned int, Release)(THIS) PURE;
    /* virtual destructor, somehow missed this originally... im so dumb */
    THISCALLMETHOD_(void, Destroy)(THIS) PURE;
};
#undef INTERFACE

#define INTERFACE IAmdDxExt
DECLARE_INTERFACE_(IAmdDxExt, IAmdDxExtInterface)
{
    /*** IAmdDxExtInterface methods ***/
    THISCALLMETHOD_(unsigned int, AddRef)(THIS) PURE;
    THISCALLMETHOD_(unsigned int, Release)(THIS) PURE;
    THISCALLMETHOD_(void, Destroy)(THIS) PURE;

    /*** IAmdDxExt methods ***/
    THISCALLMETHOD_(HRESULT, GetVersion)(THIS_ AmdDxExtVersion *version) PURE;
    THISCALLMETHOD_(IAmdDxExtInterface*,GetExtInterface)(THIS_ unsigned int iface) PURE;


    THISCALLMETHOD_(HRESULT, IaSetPrimitiveTopology)(THIS_ D3D_PRIMITIVE_TOPOLOGY topology) PURE;
    THISCALLMETHOD_(HRESULT, IaGetPrimitiveTopology)(THIS_ AmdDxExtPrimitiveTopology *topology) PURE;
    THISCALLMETHOD_(HRESULT, SetSingleSampleRead)(THIS_ ID3D10Resource *res, BOOL single_sample) PURE;
    THISCALLMETHOD_(HRESULT, SetSingleSampleRead11)(THIS_ ID3D11Resource *res, BOOL single_sample) PURE;
    THISCALLMETHOD_(HRESULT, QueryFeatureSupport)(THIS_ unsigned int feature_token, void *data, unsigned int data_size) PURE;
    THISCALLMETHOD_(HRESULT, IaSetPrimitiveTopologyCtx)(THIS_ unsigned int topology, ID3D11DeviceContext *ctx) PURE;
    THISCALLMETHOD_(HRESULT, IaGetPrimitiveTopologyCtx)(THIS_ AmdDxExtPrimitiveTopology *topology, ID3D11DeviceContext *ctx) PURE;
};
#undef INTERFACE

#define INTERFACE IAmdDxExtUAVOverlap
DECLARE_INTERFACE_(IAmdDxExtUAVOverlap, IAmdDxExtInterface)
{
    /*** IAmdDxExtInterface methods ***/
    THISCALLMETHOD_(unsigned int, AddRef)(THIS) PURE;
    THISCALLMETHOD_(unsigned int, Release)(THIS) PURE;
    THISCALLMETHOD_(void, Destroy)(THIS) PURE;

    /*** IAmdDxExtUAVOverlap methods ***/
    THISCALLMETHOD_(HRESULT, BeginUAVOverlap)(THIS) PURE;
    THISCALLMETHOD_(HRESULT, EndUAVOverlap)(THIS) PURE;
    THISCALLMETHOD_(void, GetVersion)(THIS, AmdDxExtVersion* version) PURE;
};
#undef INTERFACE

#define INTERFACE IAmdDxExtQuadBufferStereo
DECLARE_INTERFACE_(IAmdDxExtQuadBufferStereo, IAmdDxExtInterface)
{
    /*** IAmdDxExtInterface methods ***/
    THISCALLMETHOD_(unsigned int, AddRef)(THIS) PURE;
    THISCALLMETHOD_(unsigned int, Release)(THIS) PURE;
    THISCALLMETHOD_(void, Destroy)(THIS) PURE;

    /*** IAmdDxExtQuadBufferStereo methods ***/
    THISCALLMETHOD_(HRESULT, EnableQuadBufferStereo)(THIS, BOOL enable) PURE;
    THISCALLMETHOD_(UINT, GetLineOffset)(THIS, IDXGISwapChain *swapchain) PURE;
    THISCALLMETHOD_(HRESULT, GetDisplayModeList)(THIS, DXGI_FORMAT format, unsigned int flags, unsigned int *num_modes, DXGI_MODE_DESC *desc) PURE;
};
#undef INTERFACE

#define INTERFACE IAmdDxExtDepthBounds
DECLARE_INTERFACE_(IAmdDxExtDepthBounds, IAmdDxExtInterface)
{
    /*** IAmdDxExtInterface methods ***/
    THISCALLMETHOD_(unsigned int, AddRef)(THIS) PURE;
    THISCALLMETHOD_(unsigned int, Release)(THIS) PURE;
    THISCALLMETHOD_(void, Destroy)(THIS) PURE;

    /*** IAmdDxExtDepthBounds methods ***/
    THISCALLMETHOD_(HRESULT, SetDepthBounds)(THIS, BOOL enabled, float min, float max) PURE;
    THISCALLMETHOD_(void, GetVersion)(THIS, AmdDxExtVersion *version) PURE;
};
#undef INTERFACE

#define INTERFACE IAmdDxExtMultidrawIndirect
DECLARE_INTERFACE_(IAmdDxExtMultidrawIndirect, IAmdDxExtInterface)
{
    /*** IAmdDxExtInterface methods ***/
    THISCALLMETHOD_(unsigned int, AddRef)(THIS) PURE;
    THISCALLMETHOD_(unsigned int, Release)(THIS) PURE;
    THISCALLMETHOD_(void, Destroy)(THIS) PURE;

    /*** IAmdDxExtMultidrawIndirect methods ***/
    THISCALLMETHOD_(void, GetVersion)(THIS, AmdDxExtVersion *version) PURE;
    THISCALLMETHOD_(HRESULT, MultiDrawIndirect)(THIS, unsigned int draw_count, ID3D11Buffer *buffer, unsigned int byte_offset, unsigned int byte_stride) PURE;
    THISCALLMETHOD_(HRESULT, MultiDrawIndexedIndirect)(THIS, unsigned int draw_count, ID3D11Buffer *buffer, unsigned int byte_offset, unsigned int byte_stride) PURE;
    THISCALLMETHOD_(HRESULT, MultiDrawIndirectCount)(THIS, ID3D11Buffer *buffer_for_count, unsigned int byte_offset_for_count, ID3D11Buffer *buffer, unsigned int byte_offset, unsigned int byte_stride) PURE;
    THISCALLMETHOD_(HRESULT, MultiDrawIndexedIndirectCount)(THIS, ID3D11Buffer *buffer_for_count, unsigned int byte_offset_for_count, ID3D11Buffer *buffer, unsigned int byte_offset, unsigned int byte_stride) PURE;
};

#ifdef __cplusplus
}
#endif
