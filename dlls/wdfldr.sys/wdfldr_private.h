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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "ddk/wdm.h"
#include "wine/list.h"

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
