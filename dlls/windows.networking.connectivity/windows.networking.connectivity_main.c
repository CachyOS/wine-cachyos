#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"
#include "wine/debug.h"

#include "objbase.h"
#include "initguid.h"

#include "activation.h"

WINE_DEFAULT_DEBUG_CHANNEL(network);

struct windows_networking_connectivity
{
    IActivationFactory IActivationFactory_iface;
    LONG refcount;
};

static inline struct windows_networking_connectivity *impl_from_IActivationFactory(IActivationFactory *iface)
{
    return CONTAINING_RECORD(iface, struct windows_networking_connectivity, IActivationFactory_iface);
}

static HRESULT STDMETHODCALLTYPE windows_networking_connectivity_QueryInterface(
        IActivationFactory *iface, REFIID iid, void **object)
{
    TRACE("iface %p, iid %s, object %p stub!\n", iface, debugstr_guid(iid), object);

    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject) ||
        IsEqualGUID(iid, &IID_IActivationFactory))
    {
        IUnknown_AddRef(iface);
        *object = &impl->INetworkInformationStatics_iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE windows_networking_connectivity_AddRef(
        IActivationFactory *iface)
{
    struct windows_networking_connectivity *impl = impl_from_IActivationFactory(iface);
    ULONG rc = InterlockedIncrement(&impl->refcount);
    TRACE("%p increasing refcount to %lu.\n", impl, rc);
    return rc;
}

static ULONG STDMETHODCALLTYPE windows_networking_connectivity_Release(
        IActivationFactory *iface)
{
    struct windows_networking_connectivity *impl = impl_from_IActivationFactory(iface);
    ULONG rc = InterlockedDecrement(&impl->refcount);
    TRACE("%p decreasing refcount to %lu.\n", impl, rc);
    return rc;
}

static HRESULT STDMETHODCALLTYPE windows_networking_connectivity_GetIids(
        IActivationFactory *iface, ULONG *iid_count, IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE windows_networking_connectivity_GetRuntimeClassName(
        IActivationFactory *iface, HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE windows_networking_connectivity_GetTrustLevel(
        IActivationFactory *iface, TrustLevel *trust_level)
{
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE windows_networking_connectivity_ActivateInstance(
        IActivationFactory *iface, IInspectable **instance)
{
    FIXME("iface %p, instance %p stub!\n", iface, instance);
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl activation_factory_vtbl =
{
    windows_networking_connectivity_QueryInterface,
    windows_networking_connectivity_AddRef,
    windows_networking_connectivity_Release,
    /* IInspectable methods */
    windows_networking_connectivity_GetIids,
    windows_networking_connectivity_GetRuntimeClassName,
    windows_networking_connectivity_GetTrustLevel,
    /* IActivationFactory methods */
    windows_networking_connectivity_ActivateInstance,
};

static struct windows_networking_connectivity windows_networking_connectivity =
{
    {&activation_factory_vtbl},
    1
};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *object)
{
    FIXME("clsid %s, riid %s, object %p stub!\n", debugstr_guid(clsid), debugstr_guid(riid), object);
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    TRACE("classid %s, factory %p.\n", debugstr_hstring(classid), factory);
    *factory = &windows_networking_connectivity.IActivationFactory_iface;
    IUnknown_AddRef(*factory);
    return S_OK;
}
