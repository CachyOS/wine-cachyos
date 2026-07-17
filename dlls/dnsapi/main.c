/*
 * DNS support
 *
 * Copyright (C) 2006 Matthew Kehrer
 * Copyright (C) 2006 Hans Leidekker
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
#include <stdlib.h>

#include "windef.h"
#include "winternl.h"
#include "winbase.h"
#include "winerror.h"
#include "windns.h"
#include "dnsapi.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dnsapi);

BOOL WINAPI DllMain( HINSTANCE hinst, DWORD reason, LPVOID reserved )
{
    TRACE( "(%p, %lu, %p)\n", hinst, reason, reserved );

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls( hinst );
        if (__wine_init_unix_call())
            ERR( "No libresolv support, expect problems\n" );
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

/******************************************************************************
 * DnsAcquireContextHandle_A              [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsAcquireContextHandle_A( DWORD flags, void *cred, HANDLE *context )
{
    FIXME( "(%#lx, %p, %p) stub\n", flags, cred, context );

    *context = (HANDLE)0xdeadbeef;
    return ERROR_SUCCESS;
}

/******************************************************************************
 * DnsAcquireContextHandle_UTF8              [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsAcquireContextHandle_UTF8( DWORD flags, void *cred, HANDLE *context )
{
    FIXME( "(%#lx, %p, %p) stub\n", flags, cred, context );

    *context = (HANDLE)0xdeadbeef;
    return ERROR_SUCCESS;
}

/******************************************************************************
 * DnsAcquireContextHandle_W              [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsAcquireContextHandle_W( DWORD flags, void *cred, HANDLE *context )
{
    FIXME( "(%#lx, %p, %p) stub\n", flags, cred, context );

    *context = (HANDLE)0xdeadbeef;
    return ERROR_SUCCESS;
}

/******************************************************************************
 * DnsFlushResolverCache               [DNSAPI.@]
 *
 */
VOID WINAPI DnsFlushResolverCache(void)
{
    FIXME(": stub\n");
}

/******************************************************************************
 * DnsFlushResolverCacheEntry_A               [DNSAPI.@]
 *
 */
BOOL WINAPI DnsFlushResolverCacheEntry_A( PCSTR entry )
{
    FIXME( "%s: stub\n", debugstr_a(entry) );
    if (!entry) return FALSE;
    return TRUE;
}

/******************************************************************************
 * DnsFlushResolverCacheEntry_UTF8               [DNSAPI.@]
 *
 */
BOOL WINAPI DnsFlushResolverCacheEntry_UTF8( PCSTR entry )
{
    FIXME( "%s: stub\n", debugstr_a(entry) );
    if (!entry) return FALSE;
    return TRUE;
}

/******************************************************************************
 * DnsFlushResolverCacheEntry_W               [DNSAPI.@]
 *
 */
BOOL WINAPI DnsFlushResolverCacheEntry_W( PCWSTR entry )
{
    FIXME( "%s: stub\n", debugstr_w(entry) );
    if (!entry) return FALSE;
    return TRUE;
}

/******************************************************************************
 * DnsGetCacheDataTable                    [DNSAPI.@]
 *
 */
BOOL WINAPI DnsGetCacheDataTable( PDNS_CACHE_ENTRY* entry )
{
    FIXME( "(%p) stub\n", entry );
    return FALSE;
}

/******************************************************************************
 * DnsReleaseContextHandle                [DNSAPI.@]
 *
 */
VOID WINAPI DnsReleaseContextHandle( HANDLE context )
{
    FIXME( "(%p) stub\n", context );
}

/******************************************************************************
 * DnsModifyRecordsInSet_A                 [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsModifyRecordsInSet_A( DNS_RECORDA *add, DNS_RECORDA *delete, DWORD options, HANDLE context,
                                           void *servers, void *reserved )
{
    FIXME( "(%p, %p, %#lx, %p, %p, %p) stub\n", add, delete, options, context, servers, reserved );
    return ERROR_SUCCESS;
}

/******************************************************************************
 * DnsModifyRecordsInSet_UTF8              [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsModifyRecordsInSet_UTF8( DNS_RECORDA *add, DNS_RECORDA *delete, DWORD options, HANDLE context,
                                              void *servers, void *reserved )
{
    FIXME( "(%p, %p, %#lx, %p, %p, %p) stub\n", add, delete, options, context, servers, reserved );
    return ERROR_SUCCESS;
}

/******************************************************************************
 * DnsModifyRecordsInSet_W                 [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsModifyRecordsInSet_W( DNS_RECORDW *add, DNS_RECORDW *delete, DWORD options, HANDLE context,
                                           void *servers, void *reserved )
{
    FIXME( "(%p, %p, %#lx, %p, %p, %p) stub\n", add, delete, options, context, servers, reserved );
    return ERROR_SUCCESS;
}

/******************************************************************************
 * DnsWriteQuestionToBuffer_UTF8          [DNSAPI.@]
 *
 */
BOOL WINAPI DnsWriteQuestionToBuffer_UTF8( DNS_MESSAGE_BUFFER *buffer, DWORD *size, const char *name, WORD type,
                                           WORD xid, BOOL recurse )
{
    FIXME( "(%p, %p, %s, %d, %d, %d) stub\n", buffer, size, debugstr_a(name), type, xid, recurse );
    return FALSE;
}

/******************************************************************************
 * DnsWriteQuestionToBuffer_W              [DNSAPI.@]
 *
 */
BOOL WINAPI DnsWriteQuestionToBuffer_W( DNS_MESSAGE_BUFFER *buffer, DWORD *size, const WCHAR *name, WORD type,
                                        WORD xid, BOOL recurse )
{
    FIXME( "(%p, %p, %s, %d, %d, %d) stub\n", buffer, size, debugstr_w(name), type, xid, recurse );
    return FALSE;
}

/******************************************************************************
 * DnsReplaceRecordSetA                    [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsReplaceRecordSetA( DNS_RECORDA *set, DWORD options, HANDLE context, void *servers,
                                        void *reserved )
{
    FIXME( "(%p, %#lx, %p, %p, %p) stub\n", set, options, context, servers, reserved );
    return ERROR_SUCCESS;
}

/******************************************************************************
 * DnsReplaceRecordSetUTF8                 [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsReplaceRecordSetUTF8( DNS_RECORDA *set, DWORD options, HANDLE context, void *servers,
                                           void *reserved )
{
    FIXME( "(%p, %#lx, %p, %p, %p) stub\n", set, options, context, servers, reserved );
    return ERROR_SUCCESS;
}

/******************************************************************************
 * DnsReplaceRecordSetW                    [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsReplaceRecordSetW( DNS_RECORDW *set, DWORD options, HANDLE context, void *servers,
                                        void *reserved )
{
    FIXME( "(%p, %#lx, %p, %p, %p) stub\n", set, options, context, servers, reserved );
    return ERROR_SUCCESS;
}

/******************************************************************************
 * DnsServiceBrowse                        [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsServiceBrowse( PDNS_SERVICE_BROWSE_REQUEST request, PDNS_SERVICE_CANCEL cancel)
{
    FIXME( "(%p, %p) stub\n", request, cancel );
    return ERROR_SUCCESS;
}

/******************************************************************************
 * DnsStartMulticastQuery                  [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsStartMulticastQuery(MDNS_QUERY_REQUEST *request, MDNS_QUERY_HANDLE *handle)
{
    FIXME( "(%p, %p) stub\n", request, handle );
    return ERROR_SUCCESS;
}

/******************************************************************************
 * DnsStopMulticastQuery                   [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsStopMulticastQuery(MDNS_QUERY_HANDLE *handle)
{
    FIXME( "(%p) stub\n", handle );
    return ERROR_SUCCESS;
}

/******************************************************************************
 * DnsServiceConstructInstance        [DNSAPI.@]
 *
 */
PDNS_SERVICE_INSTANCE WINAPI DnsServiceConstructInstance( PCWSTR service_name, PCWSTR host_name,
                                                           PIP4_ADDRESS ip4, PIP6_ADDRESS ip6,
                                                           WORD port, WORD priority, WORD weight,
                                                           DWORD property_count, PWSTR *keys,
                                                           PWSTR *values )
{
    DNS_SERVICE_INSTANCE *instance;
    DWORD i;

    FIXME( "(%s, %s, %p, %p, %u, %u, %u, %lu, %p, %p) stub\n",
           debugstr_w(service_name), debugstr_w(host_name), ip4, ip6,
           port, priority, weight, property_count, keys, values );

    if (!(instance = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*instance) )))
        return NULL;

    if (service_name && !(instance->pszInstanceName = wcsdup( service_name )))
        goto fail;
    if (host_name && !(instance->pszHostName = wcsdup( host_name )))
        goto fail;

    if (ip4)
    {
        if (!(instance->ip4Address = HeapAlloc( GetProcessHeap(), 0, sizeof(*instance->ip4Address) )))
            goto fail;
        *instance->ip4Address = *ip4;
    }
    if (ip6)
    {
        if (!(instance->ip6Address = HeapAlloc( GetProcessHeap(), 0, sizeof(*instance->ip6Address) )))
            goto fail;
        *instance->ip6Address = *ip6;
    }

    instance->wPort     = port;
    instance->wPriority = priority;
    instance->wWeight   = weight;

    if (property_count && keys && values)
    {
        instance->dwPropertyCount = property_count;
        if (!(instance->keys = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                          property_count * sizeof(*instance->keys) )))
            goto fail;
        if (!(instance->values = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                            property_count * sizeof(*instance->values) )))
            goto fail;
        for (i = 0; i < property_count; i++)
        {
            if (keys[i] && !(instance->keys[i] = wcsdup( keys[i] )))
                goto fail;
            if (values[i] && !(instance->values[i] = wcsdup( values[i] )))
                goto fail;
        }
    }

    return instance;

fail:
    DnsServiceFreeInstance( instance );
    return NULL;
}

/******************************************************************************
 * DnsServiceFreeInstance             [DNSAPI.@]
 *
 */
VOID WINAPI DnsServiceFreeInstance( PDNS_SERVICE_INSTANCE instance )
{
    DWORD i;

    FIXME( "(%p) stub\n", instance );

    if (!instance) return;

    free( instance->pszInstanceName );
    free( instance->pszHostName );
    HeapFree( GetProcessHeap(), 0, instance->ip4Address );
    HeapFree( GetProcessHeap(), 0, instance->ip6Address );

    if (instance->keys && instance->values)
    {
        for (i = 0; i < instance->dwPropertyCount; i++)
        {
            free( instance->keys[i] );
            free( instance->values[i] );
        }
    }
    HeapFree( GetProcessHeap(), 0, instance->keys );
    HeapFree( GetProcessHeap(), 0, instance->values );
    HeapFree( GetProcessHeap(), 0, instance );
}

/******************************************************************************
 * DnsServiceRegister                 [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsServiceRegister( PDNS_SERVICE_REGISTER_REQUEST request, PDNS_SERVICE_CANCEL cancel )
{
    FIXME( "(%p, %p) stub\n", request, cancel );

    if (!request) return ERROR_INVALID_PARAMETER;

    /* Invoke completion callback immediately with a not-implemented status so
     * callers do not block waiting for an async result that will never arrive. */
    if (request->pRegisterCompletionCallback)
        request->pRegisterCompletionCallback( ERROR_CALL_NOT_IMPLEMENTED,
                                              request->pQueryContext,
                                              request->pServiceInstance );
    return DNS_REQUEST_PENDING;
}

/******************************************************************************
 * DnsServiceDeRegister               [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsServiceDeRegister( PDNS_SERVICE_REGISTER_REQUEST request, PDNS_SERVICE_CANCEL cancel )
{
    FIXME( "(%p, %p) stub\n", request, cancel );

    if (!request) return ERROR_INVALID_PARAMETER;

    if (request->pRegisterCompletionCallback)
        request->pRegisterCompletionCallback( ERROR_CALL_NOT_IMPLEMENTED,
                                              request->pQueryContext,
                                              request->pServiceInstance );
    return DNS_REQUEST_PENDING;
}

/******************************************************************************
 * DnsServiceRegisterCancel           [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsServiceRegisterCancel( PDNS_SERVICE_CANCEL cancel )
{
    FIXME( "(%p) stub\n", cancel );
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/******************************************************************************
 * DnsServiceResolve                  [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsServiceResolve( PDNS_SERVICE_RESOLVE_REQUEST request, PDNS_SERVICE_CANCEL cancel )
{
    FIXME( "(%p, %p) stub\n", request, cancel );

    if (!request) return ERROR_INVALID_PARAMETER;

    if (request->pResolveCompletionCallback)
        request->pResolveCompletionCallback( ERROR_CALL_NOT_IMPLEMENTED,
                                             request->pQueryContext,
                                             NULL );
    return DNS_REQUEST_PENDING;
}

/******************************************************************************
 * DnsServiceResolveCancel            [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsServiceResolveCancel( PDNS_SERVICE_CANCEL cancel )
{
    FIXME( "(%p) stub\n", cancel );
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/******************************************************************************
 * DnsServiceBrowseCancel             [DNSAPI.@]
 *
 */
DNS_STATUS WINAPI DnsServiceBrowseCancel( PDNS_SERVICE_CANCEL cancel )
{
    FIXME( "(%p) stub\n", cancel );
    return ERROR_CALL_NOT_IMPLEMENTED;
}
