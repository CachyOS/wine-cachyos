/*
 * WAYLAND display device functions
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "waylanddrv.h"

#include "wine/debug.h"

#include "ntuser.h"

#include <stdlib.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static int wayland_output_cmp_primary_x_y(const void *va, const void *vb)
{
    const struct wayland_output * const *output_a = va;
    const struct wayland_output * const *output_b = vb;
    const struct wayland_output_state *a = &(*output_a)->current;
    const struct wayland_output_state *b = &(*output_b)->current;
    BOOL a_is_primary = a->physical_x == 0 && a->physical_y == 0;
    BOOL b_is_primary = b->physical_x == 0 && b->physical_y == 0;

    if (a_is_primary && !b_is_primary) return -1;
    if (!a_is_primary && b_is_primary) return 1;
    if (a->physical_x < b->physical_x) return -1;
    if (a->physical_x > b->physical_x) return 1;
    if (a->physical_y < b->physical_y) return -1;
    if (a->physical_y > b->physical_y) return 1;
    return strcmp(a->name, b->name);
}

static inline BOOL wayland_output_overlap(struct wayland_output_state *a, struct wayland_output_state *b)
{
    return b->physical_x < a->physical_x + a->current_mode->width &&
           b->physical_x + b->current_mode->width > a->physical_x &&
           b->physical_y < a->physical_y + a->current_mode->height &&
           b->physical_y + b->current_mode->height > a->physical_y;
}

/* Map a point to one of the four quadrants of our 2d coordinate space:
 * 0: bottom right (x >= 0, y >= 0)
 * 1: top right (x >= 0, y < 0)
 * 2: bottom left (x < 0, y >= 0)
 * 3: top left (x < 0, y < 0) */
static inline int point_to_quadrant(int x, int y)
{
    return (x < 0) * 2 + (y < 0);
}

/* Decide which of two outputs to keep stationary in order
 * to resolve an overlap. */
static struct wayland_output_state *wayland_output_get_overlap_anchor(struct wayland_output_state *a,
                                                                      struct wayland_output_state *b)
{
    /* Preferences for the direction of growth in each quadrant, with a
     * lower value signifying a higher preference. */
    static const int quadrant_prefs[4][4] =
    {
        {0, 1, 2, 3}, /* quadrant 0 */
        {3, 0, 2, 1}, /* quadrant 1 */
        {2, 3, 0, 1}, /* quadrant 2 */
        {3, 2, 1, 0}, /* quadrant 3 */
    };
    int qa = point_to_quadrant(a->logical_x, a->logical_y);
    int qb = point_to_quadrant(b->logical_x, b->logical_y);
    /* Direction of growth if a is the anchor. */
    int qab = point_to_quadrant(b->logical_x - a->logical_x,
                                b->logical_y - a->logical_y);
    /* Direction of growth if b is the anchor. */
    int qba = point_to_quadrant(a->logical_x - b->logical_x,
                                a->logical_y - b->logical_y);

    /* If the two output origins are in different quadrants, use the output
     * in the lower valued quadrant as the anchor (so effectively outputs
     * grow/move away from quadrant 0). */
    if (qa != qb) return (qa < qb) ? a : b;

    /* If the outputs are in the same quadrant, use the preference for the
     * direction of growth in that quadrant to select the anchor. Again the
     * intended effect is to grow/move outputs away from the origin. */
    return (quadrant_prefs[qa][qab] < quadrant_prefs[qa][qba]) ? a : b;
}

static BOOL wayland_output_array_resolve_overlaps(struct wl_array *output_info_array)
{
    struct wayland_output **a, **b;
    BOOL found_overlap = FALSE;

    wl_array_for_each(a, output_info_array)
    {
        if (!(*a)->current.current_mode) continue;
        wl_array_for_each(b, output_info_array)
        {
            struct wayland_output_state *anchor, *move;
            BOOL x_use_end, y_use_end;
            double rel_x, rel_y;

            /* Break if we reach the same output in the inner loop, so that we
             * don't process output pairs twice (since order doesn't matter for
             * our algorithm.) */
            if (!(*b)->current.current_mode) continue;
            if (a == b) break;

            if (!wayland_output_overlap(&(*a)->current, &(*b)->current)) continue;
            found_overlap = TRUE;

            /* Decide which output to move to resolve the overlap. */
            anchor = wayland_output_get_overlap_anchor(&(*a)->current, &(*b)->current);
            move = anchor == &(*a)->current ? &(*b)->current : &(*a)->current;

            /* Move the selected output on the X axis to resolve the overlap,
             * while maintaining the same relative positioning of the outputs as
             * the one they have in logical space. Use either the start or end
             * of the moved output as the point to maintain the relative
             * position of, depending on whether the anchor is before or after
             * the moved output on the axis. */
            x_use_end = move->logical_x < anchor->logical_x;
            rel_x = (move->logical_x - anchor->logical_x +
                     (x_use_end ? move->logical_w : 0)) /
                    (double)anchor->logical_w;
            move->physical_x = anchor->physical_x + anchor->current_mode->width * rel_x -
                               (x_use_end ? move->current_mode->width : 0);

            /* Similarly for the Y axis. */
            y_use_end = move->logical_y < anchor->logical_y;
            rel_y = (move->logical_y - anchor->logical_y +
                     (y_use_end ? move->logical_h : 0)) /
                    (double)anchor->logical_h;
            move->physical_y = anchor->physical_y + anchor->current_mode->height * rel_y -
                               (y_use_end ? move->current_mode->height : 0);
        }
    }

    return found_overlap;
}

static void wayland_output_array_zero_primary(struct wl_array *output_info_array)
{
    const char *env = getenv("WAYLANDDRV_PRIMARY_MONITOR");
    int x_offset = 0, y_offset = 0;
    struct wayland_output **pos;
    UINT64 max_score = 0;
    int count = 0;

    if (env)
    {
        wl_array_for_each(pos, output_info_array)
        {
            if (!(*pos)->current.current_mode) continue;
            if (!strcmp((*pos)->current.name, env))
            {
                x_offset = (*pos)->current.physical_x;
                y_offset = (*pos)->current.physical_y;
                count++;
            }
        }

        if (count > 1)
        {
            x_offset = 0;
            y_offset = 0;
            ERR("More than one output with name %s\n", debugstr_a(env));
        }
        else if (count == 0)
        {
            ERR("Could not find output %s\n", debugstr_a(env));
        }
        else ERR("HACK: Using %s as primary output!\n", debugstr_a(env));
    }
    else
    {
        /* rank monitors by bandwidth */
        wl_array_for_each(pos, output_info_array)
        {
            struct wayland_output_mode *mode = (*pos)->current.current_mode;
            UINT64 score;

            if (!mode) continue;

            score = (UINT64)mode->height * (UINT64)mode->width *
                    ((UINT64)(mode->refresh + 500) / 1000) -
                    (INT64)((*pos)->current.logical_x / 100) -
                    (INT64)((*pos)->current.logical_y / 100) +
                    (UINT64)(*pos)->current.max_cll;

            if (score > max_score)
            {
                x_offset = (*pos)->current.physical_x;
                y_offset = (*pos)->current.physical_y;
                max_score = score;
            }
        }
    }

    wl_array_for_each(pos, output_info_array)
    {
        (*pos)->current.physical_x -= x_offset;
        (*pos)->current.physical_y -= y_offset;
    }
}

void wayland_output_array_arrange_physical_coords(void)
{
    struct wl_array *output_array = &process_wayland.output_array;
    struct wayland_output **output;
    size_t num_outputs = output_array->size / sizeof(struct wayland_output *);
    int steps = 0;

    /* Set the initial physical pixel coordinates. */
    wl_array_for_each(output, output_array)
    {
        (*output)->current.physical_x = (*output)->current.logical_x;
        (*output)->current.physical_y = (*output)->current.logical_y;
    }

    /* Try to iteratively resolve overlaps, but be defensive and set an upper
     * iteration bound to ensure we avoid infinite loops. */
    while (wayland_output_array_resolve_overlaps(output_array) &&
           ++steps < num_outputs)
        continue;

    /* places the primary output at 0,0 and offsets the other outputs accordingly */
    wayland_output_array_zero_primary(output_array);

    /* Now that we have our physical pixel coordinates, sort from physical left
     * to right, but ensure the primary output is first. */
    qsort(output_array->data, num_outputs, sizeof(struct wayland_output *),
          wayland_output_cmp_primary_x_y);
}

static void wayland_add_device_gpu(const struct gdi_device_manager *device_manager,
                                   void *param)
{
    struct pci_id pci_id = {0};

    TRACE("\n");

    device_manager->add_gpu(NULL, &pci_id, NULL, param);
}

static void wayland_add_device_source(const struct gdi_device_manager *device_manager,
                                       void *param, UINT state_flags, struct wayland_output_state *output)
{
    UINT dpi = NtUserGetSystemDpiForProcess( NULL );
    TRACE("name=%s state_flags=0x%x\n",
          output->name, state_flags);
    device_manager->add_source(output->name, state_flags, dpi, param);
}

/* borrowed from gamescope with permission */
static uint8_t encode_max_luminance(float nits)
{
    if (nits == 0.0f)
        return 0;

    return ceilf((logf(nits / 50.0f) / logf(2.0f)) * 32.0f);
}

static UINT get_edid(struct wayland_output_state *output, unsigned char **edid)
{
    const struct wayland_primaries *primaries = &output->primaries;
    struct wayland_output_mode *mode = output->current_mode;
    const char *model = output->model;
    unsigned int edid_size = 128, extensions = 0;
    unsigned char l[3] = {19, 1, 13}; /* SAM */
    unsigned int i, mwidth, mheight;
    unsigned char *data, *p, c;
    char temp_model[13] = {0};

    if (output->supports_hdr)
    {
        edid_size += 128;
        extensions++;
    }

    if (!(*edid = calloc(edid_size, sizeof(**edid))))
        return 0;

    data = *edid;

    mwidth = output->physical_w;
    mheight = output->physical_h;

    if (mwidth == 0 || mheight == 0)
    {
        /* assume ~150 dpi */
        mwidth = mode->width / 60;
        mheight = mode->width / 60;
    }

    *(uint64_t*)data = 0x00ffffffffffff00;

    /* we cannot get this information from wayland, so make something up */
    data[8] = ((l[0] & 0x1f) << 2) | ((l[1] & 0x18) >> 3);
    data[9] = ((l[1] & 0x7) << 5) | (l[2] & 0x1f);
    data[10] = 0xad;
    data[11] = 0xde;
    /* serial number is all zeros */
    data[16] = 0xFF;
    data[17] = 31; /* 2021 */
    data[18] = 1;
    data[19] = 4;
    data[20] = 0xa0; /* FIXME */
    data[21] = round(mwidth / 10.0); /* cm */
    data[22] = round(mheight / 10.0); /* cm */
    data[24] = 0x6;
    data[25] = ((primaries->r_x & 0x3) << 6) | ((primaries->r_y & 0x3) << 4) |
               ((primaries->g_x & 0x3) << 2) | (primaries->g_y & 0x3);
    data[26] = ((primaries->b_x & 0x3) << 6) | ((primaries->b_y & 0x3) << 4) |
               ((primaries->w_x & 0x3) << 2) | (primaries->w_y & 0x3);
    data[27] = (primaries->r_x & 0x3fc) >> 2;
    data[28] = (primaries->r_y & 0x3fc) >> 2;
    data[29] = (primaries->g_x & 0x3fc) >> 2;
    data[30] = (primaries->g_y & 0x3fc) >> 2;
    data[31] = (primaries->b_x & 0x3fc) >> 2;
    data[32] = (primaries->b_y & 0x3fc) >> 2;
    data[33] = (primaries->w_x & 0x3fc) >> 2;
    data[34] = (primaries->w_y & 0x3fc) >> 2;

    for (i = 0; i < 16; ++i) data[38 + i] = 1;

    p = data + 54;

    *(uint16_t*)p = 0x0; /* 0 = reserved */

    /* assume blanking time is 0 */
    p[2] = mode->width;
    p[4] = (((mode->width >> 8) & 0xf) << 4);
    p[5] = mode->height;
    p[7] = (((mode->height >> 8) & 0xf) << 4);
    p[12] = mwidth;
    p[13] = mheight;
    p[14] = (((mwidth >> 8) & 0xf) << 4) | ((mheight >> 8) & 0xf);

    p += 18;
    p[3] = 0xfc;

    if (model) lstrcpynA(temp_model, model, sizeof(temp_model));
    else strcpy(temp_model, "Default");

    for (i = 0; i < sizeof(temp_model); i++)
    {
        if (!temp_model[i])
        {
            temp_model[i++] = '\n';
            break;
        }
    }
    for (; i < sizeof(temp_model); i++)
    {
        if (!temp_model[i]) temp_model[i] = ' ';
    }

    TRACE("edid model %s\n", debugstr_an(temp_model, sizeof(temp_model)));
    memcpy((char *)p + 5, temp_model, sizeof(temp_model));

    p += 18;
    p[3] = 0x10;
    p += 18;
    p[3] = 0x10;

    c = 0;
    data[126] = extensions;
    for (i = 0; i < 127; ++i)
        c += data[i];
    data[127] = 256 - c;

    p = data;

    if (output->supports_hdr)
    {
        p += 128;

        p[0] = 2;
        p[1] = 3;
        p[2] = 0xb;

        p += 4;

        p[0] = (0x7 << 5) | 0x6; /* HDR static metadata size */
        p[1] = 6;

        /* HDR static metadata block */

        p[2] = 0x7; /* ST2084 | SDR | HDR */
        p[3] = 1;
        p[4] = encode_max_luminance(output->max_cll);
        p[5] = encode_max_luminance(output->max_fall);
        p[6] = 0; /* assume undefined, games often don't implement this properly */

        /* reset p to beginning of the CTA block */
        p = data + 128;
        c = 0;

        for (i = 0; i < 127; ++i)
            c += p[i];
        p[127] = 256 - c;
    }

    return edid_size;
}

static void wayland_add_device_monitor(const struct gdi_device_manager *device_manager,
                                       void *param, struct wayland_output_state *output,
                                       struct wayland_output_state *primary)
{
    const char *env;
    struct gdi_monitor monitor = {0};

    SetRect(&monitor.rc_monitor, output->physical_x, output->physical_y,
            output->physical_x + output->current_mode->width,
            output->physical_y + output->current_mode->height);
    OffsetRect(&monitor.rc_monitor, -primary->physical_x, -primary->physical_y);

    monitor.edid_len = get_edid(output, &monitor.edid);
    /* We don't have a direct way to get the work area in Wayland. */
    monitor.rc_work = monitor.rc_monitor;
    monitor.hdr_enabled = output->supports_hdr;

    if ((env = getenv("DXVK_HDR")) && *env == '1')
        monitor.hdr_enabled = TRUE;
    else if ((env = getenv("DXVK_NO_HDR")) && *env == '1')
        monitor.hdr_enabled = FALSE;

    TRACE("name=%s rc_monitor=rc_work=%s\n",
          output->name, wine_dbgstr_rect(&monitor.rc_monitor));

    device_manager->add_monitor(&monitor, param);
    free(monitor.edid);
}

static void populate_devmode(struct wayland_output_mode *output_mode, DEVMODEW *mode)
{
    mode->dmFields = DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                     DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
    mode->dmDisplayOrientation = DMDO_DEFAULT;
    mode->dmDisplayFlags = 0;
    mode->dmBitsPerPel = 32;
    mode->dmPelsWidth = output_mode->width;
    mode->dmPelsHeight = output_mode->height;
    /* Round the refresh rate to calculate the win32 display frequency. */
    mode->dmDisplayFrequency = (output_mode->refresh + 500) / 1000;
}

static void wayland_add_device_modes(const struct gdi_device_manager *device_manager,
                                     void *param, struct wayland_output_state *output,
                                     struct wayland_output_state *primary)
{
    DEVMODEW *modes, current = {.dmSize = sizeof(current)};
    struct wayland_output_mode *output_mode;
    int modes_count = 0;

    if (!(modes = malloc(output->modes_count * sizeof(*modes))))
        return;

    populate_devmode(output->current_mode, &current);
    current.dmFields |= DM_POSITION;
    current.dmPosition.x = output->physical_x - primary->physical_x;
    current.dmPosition.y = output->physical_y - primary->physical_y;

    RB_FOR_EACH_ENTRY(output_mode, &output->modes,
                      struct wayland_output_mode, entry)
    {
        DEVMODEW mode = {.dmSize = sizeof(mode)};
        populate_devmode(output_mode, &mode);
        modes[modes_count++] = mode;
    }

    device_manager->add_modes(&current, modes_count, modes, param);
    free(modes);
}

/***********************************************************************
 *      UpdateDisplayDevices (WAYLAND.@)
 */
UINT WAYLAND_UpdateDisplayDevices(const struct gdi_device_manager *device_manager, void *param)
{
    DWORD state_flags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    struct wayland_output *primary = NULL, **pos;

    TRACE("\n");

    pthread_mutex_lock(&process_wayland.output_mutex);

    /* Populate GDI devices. */
    wayland_add_device_gpu(device_manager, param);

    wl_array_for_each(pos, &process_wayland.output_array)
    {
        if (!(*pos)->current.current_mode) continue;
        if (!primary) primary = *pos;
        wayland_add_device_source(device_manager, param, state_flags, &(*pos)->current);
        wayland_add_device_monitor(device_manager, param, &(*pos)->current, &primary->current);
        wayland_add_device_modes(device_manager, param, &(*pos)->current, &primary->current);
        state_flags &= ~DISPLAY_DEVICE_PRIMARY_DEVICE;
    }

    pthread_mutex_unlock(&process_wayland.output_mutex);

    return STATUS_SUCCESS;
}
