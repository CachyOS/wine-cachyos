/*
 * Wayland touch handling
 *
 * Copyright 2026 Etaash Mathamsetty
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "waylanddrv.h"
#include "wine/debug.h"

#include <stdlib.h>

WINE_DEFAULT_DEBUG_CHANNEL(cursor);

static LPARAM map_point_vscreen(double x, double y)
{
    RECT rect = NtUserGetVirtualScreenRect(MDT_RAW_DPI);
    int v_x, v_y;

    /* account for vscreen top left not being at 0,0 */
    v_x = round((x - rect.left) * (65535.0 / (rect.right - rect.left)));
    v_y = round((y - rect.top) * (65535.0 / (rect.bottom - rect.top)));

    /* due to rounding the point may be outside the 0-65535 range */
    if (v_x < 0) v_x = 0;
    else if (v_x > 65535) v_x = 65535;
    if (v_y < 0) v_y = 0;
    else if (v_y > 65535) v_y = 65535;

    return MAKELPARAM(v_x, v_y);
}

static void wayland_touch_point_to_window(struct wayland_surface *surface,
                                          double surface_x, double surface_y,
                                          double *window_x, double *window_y)
{
    *window_x = surface_x * surface->window.scale + surface->window.rect.left;
    *window_y = surface_y * surface->window.scale + surface->window.rect.top;
}

static void touch_handle_down(void *private, struct wl_touch *wl_touch,
		                      uint32_t serial, uint32_t time,
					          struct wl_surface *wl_surface, int32_t id,
					          wl_fixed_t x, wl_fixed_t y)
{
    struct wayland_touch *touch = &process_wayland.touch;
    struct wayland_touch_point *point = NULL;
    struct wayland_win_data *data = NULL;
    struct wayland_surface *surface;
    double touch_x, touch_y;
    INPUT input = {0};
    HWND hwnd;

    if (!wl_surface) return;

    InterlockedExchange(&process_wayland.input_serial, serial);

    if (!(hwnd = wl_surface_get_user_data(wl_surface))) goto err;
    if (!(data = wayland_win_data_get(hwnd))) goto err;
    if (!(surface = data->wayland_surface)) goto err;
    if (!(point = calloc(1, sizeof(*point)))) goto err;

    point->id = id;
    point->focused_hwnd = hwnd;
    wl_list_init(&point->link);
    wayland_touch_point_to_window(surface, wl_fixed_to_double(x),
                                  wl_fixed_to_double(y),
                                  &touch_x, &touch_y);
    point->xy = map_point_vscreen(touch_x, touch_y);
    wayland_win_data_release(data);

    TRACE("hwnd=%p id=%d pos=(%lf,%lf)\n", hwnd, id, touch_x, touch_y);

    input.type = INPUT_HARDWARE;
    input.hi.uMsg = WM_POINTERDOWN;
    input.hi.wParamL = id;
    input.hi.wParamH = POINTER_MESSAGE_FLAG_INRANGE |
                       POINTER_MESSAGE_FLAG_INCONTACT |
                       POINTER_MESSAGE_FLAG_NEW;

    NtUserSendHardwareInput(hwnd, 0, &input, point->xy);
    wl_list_insert(&touch->touch_points, &point->link);

    return;
err:
    if (data) wayland_win_data_release(data);
}

static struct wayland_touch_point *find_touch_point(int32_t id)
{
    struct wayland_touch *touch = &process_wayland.touch;
    struct wayland_touch_point *point;

    wl_list_for_each(point, &touch->touch_points, link)
        if (point->id == id) return point;

    return NULL;
}

static void touch_handle_up(void *private, struct wl_touch *wl_touch,
		                    uint32_t serial, uint32_t time, int32_t id)
{
    struct wayland_touch_point *point = NULL;
    INPUT input = {0};

    InterlockedExchange(&process_wayland.input_serial, serial);

    if (!(point = find_touch_point(id)))
    {
        ERR("Invalid or stale id=%d\n", id);
        return;
    }

    wl_list_remove(&point->link);

    input.type = INPUT_HARDWARE;
    input.hi.uMsg = WM_POINTERUP;
    input.hi.wParamL = id;
    input.hi.wParamH = POINTER_MESSAGE_FLAG_INRANGE |
                       POINTER_MESSAGE_FLAG_INCONTACT;

    TRACE("hwnd=%p id=%d\n", point->focused_hwnd, id);

    NtUserSendHardwareInput(point->focused_hwnd, 0, &input, point->xy);
    free(point);
}

static void touch_handle_motion(void *private, struct wl_touch *wl_touch,
                                uint32_t time, int32_t id,
                                wl_fixed_t x, wl_fixed_t y)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    struct wayland_touch_point *point;
    double touch_x, touch_y;
    LPARAM old_xy;
    INPUT input = {0};

    if (!(point = find_touch_point(id)))
    {
        ERR("Invalid or stale id=%d\n", id);
        return;
    }

    old_xy = point->xy;

    if (!(data = wayland_win_data_get(point->focused_hwnd))) return;
    if (!(surface = data->wayland_surface))
    {
        wayland_win_data_release(data);
        return;
    }
    wayland_touch_point_to_window(surface, wl_fixed_to_double(x),
                                  wl_fixed_to_double(y),
                                  &touch_x, &touch_y);
    point->xy = map_point_vscreen(touch_x, touch_y);
    wayland_win_data_release(data);

    if (old_xy != point->xy) return;

    TRACE("hwnd=%p pos=(%lf,%lf)\n", point->focused_hwnd, touch_x, touch_y);

    input.type = INPUT_HARDWARE;
    input.hi.uMsg = WM_POINTERUPDATE;
    input.hi.wParamL = id;
    input.hi.wParamH = POINTER_MESSAGE_FLAG_INRANGE |
                       POINTER_MESSAGE_FLAG_INCONTACT;

    NtUserSendHardwareInput(point->focused_hwnd, 0, &input, point->xy);
}

static void touch_handle_frame(void *private, struct wl_touch *wl_touch) { }

static void touch_handle_cancel(void *private, struct wl_touch *wl_touch)
{
    struct wayland_touch *touch = &process_wayland.touch;
    struct wayland_touch_point *point, *next;
    INPUT input = {0};

    TRACE("\n");

    input.type = INPUT_HARDWARE;
    input.hi.uMsg = WM_POINTERUP;
    input.hi.wParamH = POINTER_MESSAGE_FLAG_INRANGE |
                       POINTER_MESSAGE_FLAG_INCONTACT;

    wl_list_for_each_safe(point, next, &touch->touch_points, link)
    {
        input.hi.wParamL = point->id;

        NtUserSendHardwareInput(point->focused_hwnd, 0, &input, point->xy);
        wl_list_remove(&point->link);
        free(point);
    }
}

static const struct wl_touch_listener wl_touch_listener = {
    touch_handle_down,
    touch_handle_up,
    touch_handle_motion,
    touch_handle_frame,
    touch_handle_cancel,
};

void wayland_touch_init(struct wl_touch *wl_touch)
{
    struct wayland_touch *touch = &process_wayland.touch;

    touch->wl_touch = wl_touch;
    wl_list_init(&touch->touch_points);
    wl_touch_add_listener(touch->wl_touch, &wl_touch_listener, NULL);
}

void wayland_touch_deinit(void)
{
    struct wayland_touch_point *point, *next;
    struct wayland_touch *touch = &process_wayland.touch;

    wl_touch_release(touch->wl_touch);
    touch->wl_touch = NULL;
    wl_list_for_each_safe(point, next, &touch->touch_points, link)
    {
        wl_list_remove(&point->link);
        free(point);
    }
}
