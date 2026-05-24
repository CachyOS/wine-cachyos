/*
 * Wayland output handling
 *
 * Copyright (c) 2020 Alexandros Frantzis for Collabora Ltd
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

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static const int32_t default_refresh = 60000;
static uint32_t next_output_id = 0;

#define WAYLAND_OUTPUT_CHANGED_MODES        0x01
#define WAYLAND_OUTPUT_CHANGED_NAME         0x02
#define WAYLAND_OUTPUT_CHANGED_LOGICAL_XY   0x04
#define WAYLAND_OUTPUT_CHANGED_LOGICAL_WH   0x08
#define WAYLAND_OUTPUT_CHANGED_GEOMETRY     0x10
#define WAYLAND_OUTPUT_CHANGED_PRIMARIES    0x20
#define WAYLAND_OUTPUT_CHANGED_FALL         0x40
#define WAYLAND_OUTPUT_CHANGED_CLL          0x80
#define WAYLAND_OUTPUT_CHANGED_REF_L        0x100
#define WAYLAND_OUTPUT_CHANGED_MAX_TARGET_L 0x200

/**********************************************************************
 *          Output handling
 */

/* Compare a mode rb_tree key with the provided mode rb_entry and return -1 if
 * the key compares less than the entry, 0 if the key compares equal to the
 * entry, and 1 if the key compares greater than the entry.
 *
 * The comparison is based on comparing the width, height and refresh in that
 * order. */
static int wayland_output_mode_cmp_rb(const void *key,
                                      const struct rb_entry *entry)
{
    const struct wayland_output_mode *key_mode = key;
    const struct wayland_output_mode *entry_mode =
        RB_ENTRY_VALUE(entry, const struct wayland_output_mode, entry);

    if (key_mode->width < entry_mode->width) return -1;
    if (key_mode->width > entry_mode->width) return 1;
    if (key_mode->height < entry_mode->height) return -1;
    if (key_mode->height > entry_mode->height) return 1;
    if (key_mode->refresh < entry_mode->refresh) return -1;
    if (key_mode->refresh > entry_mode->refresh) return 1;

    return 0;
}

static void wayland_output_state_add_mode(struct wayland_output_state *state,
                                          int32_t width, int32_t height,
                                          int32_t refresh, BOOL current)
{
    struct rb_entry *mode_entry;
    struct wayland_output_mode *mode;
    struct wayland_output_mode key =
    {
        .width = width,
        .height = height,
        .refresh = refresh,
    };

    mode_entry = rb_get(&state->modes, &key);
    if (mode_entry)
    {
        mode = RB_ENTRY_VALUE(mode_entry, struct wayland_output_mode, entry);
    }
    else
    {
        mode = calloc(1, sizeof(*mode));
        if (!mode)
        {
            ERR("Failed to allocate space for wayland_output_mode\n");
            return;
        }
        mode->width = width;
        mode->height = height;
        mode->refresh = refresh;
        rb_put(&state->modes, mode, &mode->entry);
        state->modes_count++;
    }

    if (current) state->current_mode = mode;
}

static void maybe_init_display_devices(void)
{
    DWORD desktop_pid = 0;
    HWND desktop_hwnd;

    /* Right after process init we initialize all the display devices, so there
     * is no need to react to each individual event at that time. This check
     * also helps us avoid calling NtUserGetDesktopWindow() (see below) at
     * process init time, since it may not be safe. */
    if (!process_wayland.initialized) return;

    desktop_hwnd = NtUserGetDesktopWindow();
    NtUserGetWindowThread(desktop_hwnd, &desktop_pid);

    /* We only update the display devices from the desktop process. */
    if (GetCurrentProcessId() != desktop_pid) return;

    NtUserPostMessage(desktop_hwnd, WM_WAYLAND_INIT_DISPLAY_DEVICES, 0, 0);
}

static void wayland_output_mode_free_rb(struct rb_entry *entry, void *ctx)
{
    free(RB_ENTRY_VALUE(entry, struct wayland_output_mode, entry));
}

static void wayland_output_done(struct wayland_output *output)
{
    struct wayland_output_mode *mode;

    /* Update current state from pending state. */
    pthread_mutex_lock(&process_wayland.output_mutex);

    if (output->pending_flags & WAYLAND_OUTPUT_CHANGED_GEOMETRY)
    {
        free(output->current.make);
        free(output->current.model);
        output->current.transform = output->pending.transform;
        output->current.physical_w = output->pending.physical_w;
        output->current.physical_h = output->pending.physical_h;
        output->current.make = output->pending.make;
        output->current.model = output->pending.model;
    }

    if (output->pending_flags & WAYLAND_OUTPUT_CHANGED_MODES)
    {
        RB_FOR_EACH_ENTRY(mode, &output->pending.modes, struct wayland_output_mode, entry)
        {
            /* Need to flip w,h when the output is transformed by 90 or 270 degrees */
            if (output->current.transform & WL_OUTPUT_TRANSFORM_90)
            {
                const int32_t temp = mode->width;
                mode->width = mode->height;
                mode->height = temp;
            }
            wayland_output_state_add_mode(&output->current,
                                          mode->width, mode->height, mode->refresh,
                                          mode == output->pending.current_mode);
        }
        rb_destroy(&output->pending.modes, wayland_output_mode_free_rb, NULL);
        rb_init(&output->pending.modes, wayland_output_mode_cmp_rb);
        output->pending.modes_count = 0;
    }

    if (output->pending_flags & WAYLAND_OUTPUT_CHANGED_NAME)
    {
        free(output->current.name);
        output->current.name = output->pending.name;
        output->pending.name = NULL;
    }

    if (output->pending_flags & WAYLAND_OUTPUT_CHANGED_LOGICAL_XY)
    {
        output->current.logical_x = output->pending.logical_x;
        output->current.logical_y = output->pending.logical_y;
    }

    if (output->pending_flags & WAYLAND_OUTPUT_CHANGED_LOGICAL_WH)
    {
        output->current.logical_w = output->pending.logical_w;
        output->current.logical_h = output->pending.logical_h;
    }

    if (output->pending_flags & WAYLAND_OUTPUT_CHANGED_PRIMARIES)
    {
        output->current.primaries = output->pending.primaries;
    }

    if (output->pending_flags & WAYLAND_OUTPUT_CHANGED_FALL)
    {
        output->current.max_fall = output->pending.max_fall;
    }

    if (output->pending_flags & WAYLAND_OUTPUT_CHANGED_CLL)
    {
        output->current.max_cll = output->pending.max_cll;
    }

    if (output->pending_flags & WAYLAND_OUTPUT_CHANGED_MAX_TARGET_L)
    {
        output->current.max_target_lum = output->pending.max_target_lum;
    }

    if (output->pending_flags & WAYLAND_OUTPUT_CHANGED_REF_L)
    {
        output->current.ref_lum = output->pending.ref_lum;
    }

    output->current.supports_hdr = FALSE;

    if (process_wayland.supports_extended_volume &&
        process_wayland.supports_pq &&
        process_wayland.supports_scrgb)
    {
        if (!process_wayland.supports_win_pq)
            ERR("Compositor doesn't supports windows_bt2100, HDR10 ST2084 may look broken\n");
        output->current.supports_hdr = (output->current.max_target_lum > output->current.ref_lum);
    }

    output->pending_flags = 0;

    /* Ensure the logical dimensions have sane values. */
    if ((!output->current.logical_w || !output->current.logical_h) &&
        output->current.current_mode)
    {
        output->current.logical_w = output->current.current_mode->width;
        output->current.logical_h = output->current.current_mode->height;
    }

    pthread_mutex_unlock(&process_wayland.output_mutex);

    TRACE("name=%s logical=%d,%d+%dx%d hdr=%u\n",
          output->current.name, output->current.logical_x, output->current.logical_y,
          output->current.logical_w, output->current.logical_h,
          output->current.supports_hdr);

    RB_FOR_EACH_ENTRY(mode, &output->current.modes, struct wayland_output_mode, entry)
    {
        TRACE("mode %dx%d @ %d %s\n",
              mode->width, mode->height, mode->refresh,
              output->current.current_mode == mode ? "*" : "");
    }

    maybe_init_display_devices();
}

static void output_handle_geometry(void *data, struct wl_output *wl_output,
                                   int32_t x, int32_t y,
                                   int32_t physical_width, int32_t physical_height,
                                   int32_t subpixel,
                                   const char *make, const char *model,
                                   int32_t output_transform)
{
    struct wayland_output *output = data;

    output->pending.transform = output_transform;
    output->pending.physical_w = physical_width;
    output->pending.physical_h = physical_height;
    output->pending.model = strdup(model);
    output->pending.make = strdup(make);

    output->pending_flags |= WAYLAND_OUTPUT_CHANGED_GEOMETRY;
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
                               uint32_t flags, int32_t width, int32_t height,
                               int32_t refresh)
{
    struct wayland_output *output = data;

    /* Windows apps don't expect a zero refresh rate, so use a default value. */
    if (refresh == 0) refresh = default_refresh;

    wayland_output_state_add_mode(&output->pending, width, height, refresh,
                                  (flags & WL_OUTPUT_MODE_CURRENT));

    output->pending_flags |= WAYLAND_OUTPUT_CHANGED_MODES;
}

static void output_handle_done(void *data, struct wl_output *wl_output)
{
    struct wayland_output *output = data;

    if (!output->zxdg_output_v1 ||
        zxdg_output_v1_get_version(output->zxdg_output_v1) >= 3)
    {
        wayland_output_done(output);
    }
}

static void output_handle_scale(void *data, struct wl_output *wl_output,
                                int32_t scale)
{
}

static const struct wl_output_listener output_listener = {
    output_handle_geometry,
    output_handle_mode,
    output_handle_done,
    output_handle_scale
};

static void zxdg_output_v1_handle_logical_position(void *data,
                                                   struct zxdg_output_v1 *zxdg_output_v1,
                                                   int32_t x,
                                                   int32_t y)
{
    struct wayland_output *output = data;
    TRACE("logical_x=%d logical_y=%d\n", x, y);
    output->pending.logical_x = x;
    output->pending.logical_y = y;
    output->pending_flags |= WAYLAND_OUTPUT_CHANGED_LOGICAL_XY;
}

static void zxdg_output_v1_handle_logical_size(void *data,
                                               struct zxdg_output_v1 *zxdg_output_v1,
                                               int32_t width,
                                               int32_t height)
{
    struct wayland_output *output = data;
    TRACE("logical_w=%d logical_h=%d\n", width, height);
    output->pending.logical_w = width;
    output->pending.logical_h = height;
    output->pending_flags |= WAYLAND_OUTPUT_CHANGED_LOGICAL_WH;
}

static void zxdg_output_v1_handle_done(void *data,
                                       struct zxdg_output_v1 *zxdg_output_v1)
{
    if (zxdg_output_v1_get_version(zxdg_output_v1) < 3)
    {
        struct wayland_output *output = data;
        wayland_output_done(output);
    }
}

static void zxdg_output_v1_handle_name(void *data,
                                       struct zxdg_output_v1 *zxdg_output_v1,
                                       const char *name)
{
    struct wayland_output *output = data;

    free(output->pending.name);
    output->pending.name = strdup(name);
    output->pending_flags |= WAYLAND_OUTPUT_CHANGED_NAME;
}

static void zxdg_output_v1_handle_description(void *data,
                                              struct zxdg_output_v1 *zxdg_output_v1,
                                              const char *description)
{
}

static const struct zxdg_output_v1_listener zxdg_output_v1_listener = {
    zxdg_output_v1_handle_logical_position,
    zxdg_output_v1_handle_logical_size,
    zxdg_output_v1_handle_done,
    zxdg_output_v1_handle_name,
    zxdg_output_v1_handle_description,
};

static void wayland_image_description_info_v1_done(void *data,
                                              struct wp_image_description_info_v1 *info)
{
    struct wayland_output *output = data;
    wayland_output_done(output);
}

static void wayland_image_description_info_v1_icc_file(void *data,
                                                  struct wp_image_description_info_v1 *info,
                                                  int32_t icc, uint32_t icc_size)
{
}

static void wayland_image_description_info_v1_primaries_named(void *data,
				            struct wp_image_description_info_v1 *info,
				            uint32_t primaries)
{
}

static void wayland_image_description_info_v1_tfpower(void *data,
				            struct wp_image_description_info_v1 *info,
				            uint32_t power)
{
}

static void wayland_image_description_info_v1_tfnamed(void *data,
				            struct wp_image_description_info_v1 *info,
				            uint32_t named)
{
}

static void wayland_image_description_info_v1_luminance(void *data,
                            struct wp_image_description_info_v1 *info,
                            uint32_t min, uint32_t max, uint32_t ref)
{
    struct wayland_output *output = data;

    TRACE("reference luminance: %u\n", ref);

    output->pending.ref_lum = ref;
    output->pending_flags |= WAYLAND_OUTPUT_CHANGED_REF_L;
}

static void wayland_image_description_info_v1_primaries(void *data,
                                                   struct wp_image_description_info_v1 *info,
                                                   int32_t r_x, int32_t r_y, int32_t g_x,
                                                   int32_t g_y, int32_t b_x, int32_t b_y,
                                                   int32_t w_x, int32_t w_y)
{
    struct wayland_output *output = data;

    if (!(output->pending_flags & WAYLAND_OUTPUT_CHANGED_PRIMARIES))
    {
#define COPY(name) output->pending.primaries.name = round((name * 1e-6) * 1024)
        COPY(r_x);
        COPY(r_y);
        COPY(g_x);
        COPY(g_y);
        COPY(b_x);
        COPY(b_y);
        COPY(w_x);
        COPY(w_y);
#undef COPY

        TRACE("primaries: {%lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf}\n",
            r_x * 1e-6, r_y * 1e-6, g_x * 1e-6, g_y * 1e-6,
            b_x * 1e-6, b_y * 1e-6, w_x * 1e-6, w_y * 1e-6);

        output->pending_flags |= WAYLAND_OUTPUT_CHANGED_PRIMARIES;
    }
}

static void wayland_image_description_info_v1_target_primaries(void *data,
				 struct wp_image_description_info_v1 *info,
				 int32_t r_x, int32_t r_y,
				 int32_t g_x, int32_t g_y,
				 int32_t b_x, int32_t b_y,
				 int32_t w_x, int32_t w_y)
{
    struct wayland_output *output = data;

#define COPY(name) output->pending.primaries.name = round((name * 1e-6) * 1024)
    COPY(r_x);
    COPY(r_y);
    COPY(g_x);
    COPY(g_y);
    COPY(b_x);
    COPY(b_y);
    COPY(w_x);
    COPY(w_y);
#undef COPY

    TRACE("primaries: {%lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf}\n",
            r_x * 1e-6, r_y * 1e-6, g_x * 1e-6, g_y * 1e-6,
            b_x * 1e-6, b_y * 1e-6, w_x * 1e-6, w_y * 1e-6);

    output->pending_flags |= WAYLAND_OUTPUT_CHANGED_PRIMARIES;
}

static void wayland_image_description_info_v1_target_luminance(void *data,
                            struct wp_image_description_info_v1 *info,
                            uint32_t min, uint32_t max)
{
    struct wayland_output *output = data;

    TRACE("max target luminance: %u\n", max);

    output->pending.max_target_lum = max;
    output->pending_flags |= WAYLAND_OUTPUT_CHANGED_MAX_TARGET_L;
}

static void wayland_image_description_info_v1_target_max_cll(void *data,
				            struct wp_image_description_info_v1 *info,
				            uint32_t max)
{
    struct wayland_output *output = data;

    TRACE("Max CLL: %u\n", max);

    output->pending.max_cll = max;
    output->pending_flags |= WAYLAND_OUTPUT_CHANGED_CLL;
}

static void wayland_image_description_info_v1_target_max_fall(void *data,
				            struct wp_image_description_info_v1 *info,
				            uint32_t max)
{
    struct wayland_output *output = data;
    TRACE("Max FALL: %u\n", max);

    output->pending.max_fall = max;
    output->pending_flags |= WAYLAND_OUTPUT_CHANGED_FALL;
}

static const struct wp_image_description_info_v1_listener image_description_info_listener = {
    wayland_image_description_info_v1_done,
    wayland_image_description_info_v1_icc_file,
    wayland_image_description_info_v1_primaries,
    wayland_image_description_info_v1_primaries_named,
    wayland_image_description_info_v1_tfpower,
    wayland_image_description_info_v1_tfnamed,
    wayland_image_description_info_v1_luminance,
    wayland_image_description_info_v1_target_primaries,
    wayland_image_description_info_v1_target_luminance,
    wayland_image_description_info_v1_target_max_cll,
    wayland_image_description_info_v1_target_max_fall
};

static void wayland_image_description_v1_failed(void *user_data,
                    struct wp_image_description_v1 *wp_image_description_v1,
                    uint32_t cause, const char *msg)
{
    struct wayland_output *output = user_data;
    ERR("cause=%u msg=%s\n", cause, debugstr_a(msg));

    wp_image_description_v1_destroy(output->wp_image_description_v1);
    output->wp_image_description_v1 = NULL;
}

static void wayland_image_description_v1_ready2(void *user_data,
                    struct wp_image_description_v1 *wp_image_description_v1,
                    uint32_t identity_hi, uint32_t identity_lo)
{
    struct wayland_output *output = user_data;
    TRACE("id=%#x%x\n", identity_hi, identity_lo);

    output->wp_image_description_info_v1 =
        wp_image_description_v1_get_information(
            output->wp_image_description_v1);
    if (!output->wp_image_description_info_v1)
    {
        ERR("Failed to allocate image description info object!\n");
        return;
    }
    wp_image_description_info_v1_add_listener(
        output->wp_image_description_info_v1,
        &image_description_info_listener, output);
}

static void wayland_image_description_v1_ready(void *user_data,
                    struct wp_image_description_v1 *wp_image_description_v1,
                    uint32_t identity)
{
    wayland_image_description_v1_ready2(user_data, wp_image_description_v1, 0, identity);
}

static const struct wp_image_description_v1_listener image_description_listener = {
    wayland_image_description_v1_failed,
    wayland_image_description_v1_ready,
    wayland_image_description_v1_ready2
};

static void wayland_color_management_output_image_description_changed(void *user_data,
                            struct wp_color_management_output_v1 *wp_color_management_output_v1);

static const struct wp_color_management_output_v1_listener color_management_output_listener = {
    wayland_color_management_output_image_description_changed
};

static void wayland_output_use_image_description(struct wayland_output *output)
{
    if (!output->wp_color_management_output_v1)
    {
        output->wp_color_management_output_v1 =
            wp_color_manager_v1_get_output(
                        process_wayland.wp_color_manager_v1,
                                    output->wl_output);
        wp_color_management_output_v1_add_listener(
            output->wp_color_management_output_v1,
            &color_management_output_listener, output);
        if (!output->wp_color_management_output_v1)
        {
            ERR("Failed to allocate color management output object!\n");
            return;
        }
    }
    output->wp_image_description_v1 =
        wp_color_management_output_v1_get_image_description(
            output->wp_color_management_output_v1);
    if (!output->wp_image_description_v1)
    {
        ERR("Failed to allocate image description object!\n");
        return;
    }
    wp_image_description_v1_add_listener(
        output->wp_image_description_v1,
        &image_description_listener, output);
}

static void wayland_color_management_output_image_description_changed(void *user_data,
                struct wp_color_management_output_v1 *wp_color_management_output_v1)
{
    struct wayland_output *output = user_data;

    if (output->wp_image_description_v1)
    {
        wp_image_description_v1_destroy(output->wp_image_description_v1);
        output->wp_image_description_v1 = NULL;
    }

    if (output->wp_image_description_info_v1)
    {
        wp_image_description_info_v1_destroy(output->wp_image_description_info_v1);
        output->wp_image_description_info_v1 = NULL;
    }

    wayland_output_use_image_description(output);
}

/**********************************************************************
 *          wayland_output_create
 *
 *  Creates a wayland_output and adds it to the output list.
 */
BOOL wayland_output_create(uint32_t id, uint32_t version)
{
    struct wayland_output *output = calloc(1, sizeof(*output));
    int name_len;

    if (!output)
    {
        ERR("Failed to allocate space for wayland_output\n");
        goto err;
    }

    output->wl_output = wl_registry_bind(process_wayland.wl_registry, id,
                                         &wl_output_interface,
                                         version < 2 ? version : 2);
    output->global_id = id;
    wl_output_add_listener(output->wl_output, &output_listener, output);

    wl_list_init(&output->link);
    rb_init(&output->pending.modes, wayland_output_mode_cmp_rb);
    output->pending.modes_count = 0;
    rb_init(&output->current.modes, wayland_output_mode_cmp_rb);
    output->current.modes_count = 0;

    /* Have a fallback while we don't have compositor given name. */
    name_len = snprintf(NULL, 0, "WaylandOutput%d", next_output_id);
    output->current.name = malloc(name_len + 1);
    if (output->current.name)
    {
        snprintf(output->current.name, name_len + 1, "WaylandOutput%d", next_output_id++);
    }
    else
    {
        ERR("Couldn't allocate space for output name\n");
        goto err;
    }

    if (!(output->current.model = strdup("Monitor")))
    {
        ERR("Couldn't allocate space for output model\n");
        goto err;
    }

    if (!(output->current.make = strdup("Wine")))
    {
        ERR("Couldn't allocate space for output make\n");
        goto err;
    }

    if (process_wayland.zxdg_output_manager_v1)
        wayland_output_use_xdg_extension(output);

    if (process_wayland.wp_color_manager_v1)
        wayland_output_use_image_description(output);

    pthread_mutex_lock(&process_wayland.output_mutex);
    wl_list_insert(process_wayland.output_list.prev, &output->link);
    pthread_mutex_unlock(&process_wayland.output_mutex);

    return TRUE;

err:
    if (output) wayland_output_destroy(output);
    return FALSE;
}

static void wayland_output_state_deinit(struct wayland_output_state *state)
{
    rb_destroy(&state->modes, wayland_output_mode_free_rb, NULL);
    free(state->name);
}

/**********************************************************************
 *          wayland_output_destroy
 *
 *  Destroys a wayland_output.
 */
void wayland_output_destroy(struct wayland_output *output)
{
    pthread_mutex_lock(&process_wayland.output_mutex);
    wl_list_remove(&output->link);
    pthread_mutex_unlock(&process_wayland.output_mutex);

    wayland_output_state_deinit(&output->pending);
    wayland_output_state_deinit(&output->current);
    if (output->wp_color_management_output_v1)
        wp_color_management_output_v1_destroy(output->wp_color_management_output_v1);
    if (output->wp_image_description_info_v1)
        wp_image_description_info_v1_destroy(output->wp_image_description_info_v1);
    if (output->wp_image_description_v1)
        wp_image_description_v1_destroy(output->wp_image_description_v1);
    if (output->zxdg_output_v1)
        zxdg_output_v1_destroy(output->zxdg_output_v1);
    wl_output_destroy(output->wl_output);
    free(output);

    maybe_init_display_devices();
}

/**********************************************************************
 *          wayland_output_use_xdg_extension
 *
 *  Use the zxdg_output_v1 extension to get output information.
 */
void wayland_output_use_xdg_extension(struct wayland_output *output)
{
    output->zxdg_output_v1 =
        zxdg_output_manager_v1_get_xdg_output(process_wayland.zxdg_output_manager_v1,
                                              output->wl_output);
    zxdg_output_v1_add_listener(output->zxdg_output_v1, &zxdg_output_v1_listener,
                                output);
}

static void wayland_color_manager_handle_supported_intent(void *data,
    struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t intent)
{
}

static void wayland_color_manager_handle_supported_feature(void *data,
    struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t feature)
{
    pthread_mutex_lock(&process_wayland.output_mutex);

    TRACE("feature %u\n", feature);

    if (feature == WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB)
        process_wayland.supports_scrgb = TRUE;
    else if (feature == WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_BT2100)
        process_wayland.supports_win_pq = TRUE;
    else if (feature == WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME)
        process_wayland.supports_extended_volume = TRUE;

    pthread_mutex_unlock(&process_wayland.output_mutex);
}

static void wayland_color_manager_handle_supported_named_tf(void *data,
    struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t tf)
{
    pthread_mutex_lock(&process_wayland.output_mutex);

    TRACE("named tf %u\n", tf);

    if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ)
        process_wayland.supports_pq = TRUE;

    pthread_mutex_unlock(&process_wayland.output_mutex);
}

static void wayland_color_manager_handle_supported_primaries(void *data,
    struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t primaries)
{
}

static void wayland_color_manager_handle_done(void *data,
                        struct wp_color_manager_v1 *wp_color_manager_v1)
{
}

static const struct wp_color_manager_v1_listener wp_color_manager_listener = {
    wayland_color_manager_handle_supported_intent,
    wayland_color_manager_handle_supported_feature,
    wayland_color_manager_handle_supported_named_tf,
    wayland_color_manager_handle_supported_primaries,
    wayland_color_manager_handle_done
};

/**********************************************************************
 *          wayland_color_manager_init
 */
void wayland_color_manager_init(void)
{
    wp_color_manager_v1_add_listener(process_wayland.wp_color_manager_v1,
                                     &wp_color_manager_listener, NULL);
}
