/* WAYLANDDRV Vulkan implementation
 *
 * Copyright 2017 Roderick Colenbrander
 * Copyright 2021 Alexandros Frantzis
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

#include <dlfcn.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "waylanddrv.h"
#include "wine/debug.h"

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

static const struct vulkan_driver_funcs wayland_vulkan_driver_funcs;

static struct wayland_client_surface *stash_client_surface(HWND hwnd,
                                                           struct wayland_client_surface *surface)
{
    struct wayland_client_surface *ret = NULL;
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(hwnd))) return NULL;

    if (surface)
    {
        if ((ret = data->stashed_client) == surface)
        {
            wayland_win_data_release(data);
            return ret;
        }
        client_surface_add_ref(&surface->client);
        data->stashed_client = surface;
    }
    else if ((ret = data->stashed_client) && !ReadAcquire(&ret->client.busy_ref))
    {
        /* cannot decrease the ref count here as there is a
         * new VkSurface referencing this client surface */
        data->stashed_client = NULL;
        if (data->client_surface == ret)
        {
            wayland_client_surface_attach(ret, NULL);
            data->client_surface = NULL;
        }
    }
    else ret = NULL;

    wayland_win_data_release(data);

    if (ret && surface) client_surface_release(&ret->client);

    return ret;
}

static VkResult wayland_vulkan_surface_create(HWND hwnd, BOOL raw, const struct vulkan_instance *instance,
                                              VkSurfaceKHR *handle, struct client_surface **client)
{
    VkResult res;
    VkWaylandSurfaceCreateInfoKHR create_info_host;
    struct wayland_client_surface *surface;

    TRACE("%p %p %p %p\n", hwnd, instance, handle, client);

    if (!(surface = stash_client_surface(hwnd, NULL)) &&
        !(surface = wayland_client_surface_create(hwnd)))
    {
        ERR("Failed to create vulkan client surface\n");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    create_info_host.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    create_info_host.pNext = NULL;
    create_info_host.flags = 0; /* reserved */
    create_info_host.display = process_wayland.wl_display;
    create_info_host.surface = surface->wl_surface;

    res = instance->p_vkCreateWaylandSurfaceKHR(instance->host.instance, &create_info_host, NULL /* allocator */, handle);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to create vulkan wayland surface, res=%d\n", res);
        client_surface_release(&surface->client);
        return res;
    }

    set_client_surface(hwnd, surface);
    stash_client_surface(hwnd, surface);
    *client = &surface->client;

    TRACE("Created surface=0x%s, client=%p\n", wine_dbgstr_longlong(*handle), *client);
    return VK_SUCCESS;
}

static VkBool32 wayland_get_physical_device_presentation_support(struct vulkan_physical_device *physical_device,
                                                                 uint32_t index)
{
    struct vulkan_instance *instance = physical_device->instance;

    TRACE("%p %u\n", physical_device, index);

    return instance->p_vkGetPhysicalDeviceWaylandPresentationSupportKHR(physical_device->host.physical_device, index,
                                                                        process_wayland.wl_display);
}

static void wayland_map_instance_extensions(struct vulkan_instance_extensions *extensions)
{
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_KHR_wayland_surface = 1;
    if (extensions->has_VK_KHR_wayland_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void wayland_map_device_extensions(struct vulkan_device_extensions *extensions)
{
    if (extensions->has_VK_KHR_external_memory_win32) extensions->has_VK_KHR_external_memory_fd = 1;
    if (extensions->has_VK_KHR_external_memory_fd) extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_semaphore_win32) extensions->has_VK_KHR_external_semaphore_fd = 1;
    if (extensions->has_VK_KHR_external_semaphore_fd) extensions->has_VK_KHR_external_semaphore_win32 = 1;
    if (extensions->has_VK_KHR_external_fence_win32) extensions->has_VK_KHR_external_fence_fd = 1;
    if (extensions->has_VK_KHR_external_fence_fd) extensions->has_VK_KHR_external_fence_win32 = 1;
}

static VkResult wayland_vulkan_surface_configure(VkColorSpaceKHR *colorspace,
                                                 VkCompositeAlphaFlagBitsKHR alpha_bits,
                                                 struct client_surface *client)
{
    struct wp_image_description_v1 *wp_image_description_v1 = NULL;
    VkColorSpaceKHR old = *colorspace;

    if (process_wayland.supports_scrgb &&
        old == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
    {
        *colorspace = VK_COLOR_SPACE_PASS_THROUGH_EXT;
        wp_image_description_v1 =
            wp_color_manager_v1_create_windows_scrgb(process_wayland.wp_color_manager_v1);

        if (!wp_image_description_v1) goto err;
    }
    else if (process_wayland.supports_win_pq &&
             old == VK_COLOR_SPACE_HDR10_ST2084_EXT)
    {
        *colorspace = VK_COLOR_SPACE_PASS_THROUGH_EXT;
        wp_image_description_v1 =
            wp_color_manager_v1_create_windows_bt2100(process_wayland.wp_color_manager_v1);

        if (!wp_image_description_v1) goto err;
    }

    TRACE("mapping colorspace %u => %u\n", old, *colorspace);

    wayland_client_surface_attach_image_description(client, wp_image_description_v1);

    return VK_SUCCESS;
err:
    ERR("Failed to configure image description for client surface!\n");
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static const struct vulkan_driver_funcs wayland_vulkan_driver_funcs =
{
    .p_vulkan_surface_create = wayland_vulkan_surface_create,
    .p_vulkan_surface_configure = wayland_vulkan_surface_configure,
    .p_get_physical_device_presentation_support = wayland_get_physical_device_presentation_support,
    .p_map_instance_extensions = wayland_map_instance_extensions,
    .p_map_device_extensions = wayland_map_device_extensions,
};

/**********************************************************************
 *           WAYLAND_VulkanInit
 */
UINT WAYLAND_VulkanInit(UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs)
{
    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR("version mismatch, win32u wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION);
        return STATUS_INVALID_PARAMETER;
    }

    *driver_funcs = &wayland_vulkan_driver_funcs;
    return STATUS_SUCCESS;
}
