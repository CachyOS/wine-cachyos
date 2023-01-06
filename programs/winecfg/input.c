/*
 * Copyright 2023 Rémi Bernon for CodeWeavers
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

#include <stdarg.h>
#include <stddef.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"

#include "winecfg.h"
#include "resource.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(winecfg);

static BOOL updating_ui;

static void init_dialog( HWND dialog )
{
    WCHAR auto_detect_layout[256];
    WCHAR *buffer, *layout;
    HWND layouts;

    convert_x11_desktop_key();

    updating_ui = TRUE;

    buffer = get_reg_key( config_key, keypath( L"X11 Driver" ), L"GrabFullscreen", L"N" );
    if (IS_OPTION_TRUE( *buffer )) CheckDlgButton( dialog, IDC_FULLSCREEN_GRAB, BST_CHECKED );
    else CheckDlgButton( dialog, IDC_FULLSCREEN_GRAB, BST_UNCHECKED );
    free( buffer );

    layouts = GetDlgItem( dialog, IDC_KEYBOARD_LAYOUT );
    LoadStringW( GetModuleHandleW( NULL ), IDS_INPUT_AUTO_DETECT_LAYOUT, auto_detect_layout,
                 ARRAY_SIZE(auto_detect_layout) );

    SendMessageW( layouts, CB_RESETCONTENT, 0, 0 );
    SendMessageW( layouts, CB_ADDSTRING, 0, (LPARAM)auto_detect_layout );

    buffer = get_reg_key( config_key, keypath( L"X11 Driver" ), L"KeyboardLayoutList", L"" );
    for (layout = buffer; *layout; layout += wcslen( layout ) + 1)
        SendMessageW( layouts, CB_ADDSTRING, 0, (LPARAM)layout );
    free( buffer );

    buffer = get_reg_key( config_key, keypath( L"X11 Driver" ), L"KeyboardLayout", L"" );
    if (!buffer || !buffer[0]) SendMessageW( layouts, CB_SETCURSEL, 0, 0 );
    else SendMessageW( layouts, CB_SELECTSTRING, -1, (LPARAM)buffer );
    free( buffer );

    buffer = get_reg_key( config_key, keypath( L"X11 Driver" ), L"KeyboardScancodeDetect", L"Y" );
    if (IS_OPTION_TRUE( *buffer )) CheckDlgButton( dialog, IDC_KEYBOARD_SCANCODE_DETECT, BST_CHECKED );
    else CheckDlgButton( dialog, IDC_KEYBOARD_SCANCODE_DETECT, BST_UNCHECKED );
    free( buffer );

    updating_ui = FALSE;
}

static void on_fullscreen_grab_clicked( HWND dialog )
{
    BOOL checked = IsDlgButtonChecked( dialog, IDC_FULLSCREEN_GRAB ) == BST_CHECKED;
    if (checked) set_reg_key( config_key, keypath( L"X11 Driver" ), L"GrabFullscreen", L"Y" );
    else set_reg_key( config_key, keypath( L"X11 Driver" ), L"GrabFullscreen", L"N" );
}

static void on_keyboard_layout_changed( HWND dialog )
{
    int len, index;
    WCHAR *buffer;

    if (!(index = SendMessageW( GetDlgItem( dialog, IDC_KEYBOARD_LAYOUT ), CB_GETCURSEL, 0, 0 )))
        set_reg_key( config_key, keypath( L"X11 Driver" ), L"KeyboardLayout", L"" );
    else
    {
        len = SendMessageW( GetDlgItem( dialog, IDC_KEYBOARD_LAYOUT ), CB_GETLBTEXTLEN, index, 0 ) + 1;
        if (!(buffer = malloc( len * sizeof(WCHAR) ))) return;

        SendMessageW( GetDlgItem( dialog, IDC_KEYBOARD_LAYOUT ), CB_GETLBTEXT, index, (LPARAM)buffer );
        set_reg_key( config_key, keypath( L"X11 Driver" ), L"KeyboardLayout", buffer );
        free( buffer );
    }
}

static void on_keyboard_scancode_detect_clicked( HWND dialog )
{
    BOOL checked = IsDlgButtonChecked( dialog, IDC_KEYBOARD_SCANCODE_DETECT ) == BST_CHECKED;
    if (checked) set_reg_key( config_key, keypath( L"X11 Driver" ), L"KeyboardScancodeDetect", L"Y" );
    else set_reg_key( config_key, keypath( L"X11 Driver" ), L"KeyboardScancodeDetect", L"N" );
}

INT_PTR CALLBACK InputDlgProc( HWND dialog, UINT message, WPARAM wparam, LPARAM lparam )
{
    TRACE( "dialog %p, message %#x, wparam %#Ix, lparam %#Ix\n", dialog, message, wparam, lparam );

    switch (message)
    {
    case WM_SHOWWINDOW:
        set_window_title( dialog );
        break;

    case WM_COMMAND:
        switch (HIWORD(wparam))
        {
        case BN_CLICKED:
            if (updating_ui) break;
            SendMessageW( GetParent( dialog ), PSM_CHANGED, 0, 0 );
            switch (LOWORD(wparam))
            {
            case IDC_FULLSCREEN_GRAB: on_fullscreen_grab_clicked( dialog ); break;
            case IDC_KEYBOARD_SCANCODE_DETECT: on_keyboard_scancode_detect_clicked( dialog ); break;
            }
            break;

        case CBN_SELCHANGE:
            if (updating_ui) break;
            SendMessageW( GetParent( dialog ), PSM_CHANGED, 0, 0 );
            switch (LOWORD(wparam))
            {
            case IDC_KEYBOARD_LAYOUT: on_keyboard_layout_changed( dialog ); break;
            }
            break;
        }
        break;

    case WM_NOTIFY:
        switch (((LPNMHDR)lparam)->code)
        {
        case PSN_KILLACTIVE:
            SetWindowLongPtrW( dialog, DWLP_MSGRESULT, FALSE );
            break;
        case PSN_APPLY:
            apply();
            SetWindowLongPtrW( dialog, DWLP_MSGRESULT, PSNRET_NOERROR );
            break;
        case PSN_SETACTIVE:
            init_dialog( dialog );
            break;
        case LVN_ITEMCHANGED:
            break;
        }
        break;
    case WM_INITDIALOG:
        break;
    }

    return FALSE;
}
