/*
 * Copyright 2026 bluechxin
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wdfldr);

typedef struct _WDF_VERSION {
    ULONG Major;
    ULONG Minor;
    ULONG Build;
} WDF_VERSION;

typedef struct _WDF_BIND_INFO {
    ULONG Size;
    WCHAR *Component;
    WDF_VERSION Version;
    ULONG FuncCount;
    void **FuncTable;
    void *Module;
} WDF_BIND_INFO, *PWDF_BIND_INFO;

typedef struct _WDF_COMPONENT_GLOBALS {
    ULONG Size;
    void *DriverObject;
    void *RegistryPath;
    void *FuncTable;
    ULONG Reserved[16];
} WDF_COMPONENT_GLOBALS, *PWDF_COMPONENT_GLOBALS;

typedef struct _WDFLDR_CLIENT_INFO {
    struct list entry;
    DRIVER_OBJECT *driver;
    UNICODE_STRING registry_path;
    WDF_COMPONENT_GLOBALS globals;
    void *func_table;
} WDFLDR_CLIENT_INFO;

static struct list client_list = LIST_INIT(client_list);
static CRITICAL_SECTION client_cs;
static CRITICAL_SECTION_DEBUG client_cs_debug =
{
    0, 0, &client_cs,
    { &client_cs_debug.ProcessLocksList, &client_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": client_cs") }
};
static CRITICAL_SECTION client_cs = { &client_cs_debug, -1, 0, 0, 0, 0 };

static inline LPCSTR debugstr_us( const UNICODE_STRING *us )
{
    if (!us) return "<null>";
    return debugstr_wn( us->Buffer, us->Length / sizeof(WCHAR) );
}

NTSTATUS WINAPI WdfVersionBind(DRIVER_OBJECT *driver, UNICODE_STRING *reg_path, 
                                WDF_BIND_INFO *bind_info, PWDF_COMPONENT_GLOBALS *component_globals)
{
    FIXME("%p %s %p %p stub!\n", driver, debugstr_us(reg_path), bind_info, component_globals);
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI WdfVersionUnbind(UNICODE_STRING *reg_path, WDF_BIND_INFO *bind_info, 
                                  WDF_COMPONENT_GLOBALS *component_globals)
{
    FIXME("%s %p %p stub!\n", debugstr_us(reg_path), bind_info, component_globals);
    return STATUS_SUCCESS;
}
