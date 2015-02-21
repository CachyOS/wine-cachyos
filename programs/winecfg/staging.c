/*
 * WineCfg Staging panel
 *
 * Copyright 2014 Michael Müller
 * Copyright 2015 Sebastian Lackner
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
 *
 */

#define COBJMACROS

#include <windows.h>

#include "resource.h"
#include "winecfg.h"

/*
 * Command stream multithreading
 */
static BOOL csmt_get(void)
{
    WCHAR *buf = get_reg_key(config_key, L"Direct3D", L"csmt", NULL);
    BOOL ret = buf ? !!*buf : TRUE;
    free(buf);
    return ret;
}
static void csmt_set(BOOL status)
{
    set_reg_key_dword(config_key, L"Direct3D", L"csmt", status);
}

/*
 * DXVA2
 */
static BOOL vaapi_get(void)
{
    BOOL ret;
    WCHAR *value = get_reg_key(config_key, keypath(L"DXVA2"), L"backend", NULL);
    ret = (value && !wcscmp(value, L"va"));
    free(value);
    return ret;
}
static void vaapi_set(BOOL status)
{
    set_reg_key(config_key, keypath(L"DXVA2"), L"backend", status ? L"va" : NULL);
}

static void load_staging_settings(HWND dialog)
{
    CheckDlgButton(dialog, IDC_ENABLE_CSMT, csmt_get() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_ENABLE_VAAPI, vaapi_get() ? BST_CHECKED : BST_UNCHECKED);
}

INT_PTR CALLBACK StagingDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
        break;

    case WM_NOTIFY:
        if (((LPNMHDR)lParam)->code == PSN_SETACTIVE)
            load_staging_settings(hDlg);
        break;

    case WM_SHOWWINDOW:
        set_window_title(hDlg);
        break;

    case WM_DESTROY:
        break;

    case WM_COMMAND:
        if (HIWORD(wParam) != BN_CLICKED) break;
        switch (LOWORD(wParam))
        {
        case IDC_ENABLE_CSMT:
            csmt_set(IsDlgButtonChecked(hDlg, IDC_ENABLE_CSMT) == BST_CHECKED);
            SendMessageW(GetParent(hDlg), PSM_CHANGED, 0, 0);
            return TRUE;
        case IDC_ENABLE_VAAPI:
            vaapi_set(IsDlgButtonChecked(hDlg, IDC_ENABLE_VAAPI) == BST_CHECKED);
            SendMessageW(GetParent(hDlg), PSM_CHANGED, 0, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}
