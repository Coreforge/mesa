/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <wayland-client.h>

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <sys/mman.h>

#include "drm-uapi/drm_fourcc.h"

#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include <util/compiler.h>
#include <util/hash_table.h>
#include <util/timespec.h>
#include <util/u_vector.h>
#include <util/u_dynarray.h>
#include <util/anon_file.h>

struct wsi_wayland;

struct wsi_wl_format {
   VkFormat vk_format;
   uint32_t flags;
   struct u_vector modifiers;
};

struct dmabuf_feedback_format_table {
   unsigned int size;
   struct {
      uint32_t format;
      uint32_t padding; /* unused */
      uint64_t modifier;
   } *data;
};

struct dmabuf_feedback_tranche {
   dev_t target_device;
   uint32_t flags;
   struct u_vector formats;
};

struct dmabuf_feedback {
   dev_t main_device;
   struct dmabuf_feedback_format_table format_table;
   struct util_dynarray tranches;
   struct dmabuf_feedback_tranche pending_tranche;
};

struct wsi_wl_display {
   /* The real wl_display */
   struct wl_display *wl_display;
   /* Actually a proxy wrapper around the event queue */
   struct wl_display *wl_display_wrapper;
   struct wl_event_queue *queue;

   struct wl_shm *wl_shm;
   struct zwp_linux_dmabuf_v1 *wl_dmabuf;
   struct zwp_linux_dmabuf_feedback_v1 *wl_dmabuf_feedback;

   struct dmabuf_feedback_format_table format_table;

   struct wsi_wayland *wsi_wl;

   /* Formats populated by zwp_linux_dmabuf_v1 or wl_shm interfaces */
   struct u_vector formats;

   bool sw;
};

struct wsi_wayland {
   struct wsi_interface base;

   struct wsi_device *wsi;

   const VkAllocationCallbacks *alloc;
   VkPhysicalDevice physical_device;
};

struct wsi_wl_image {
   struct wsi_image base;
   struct wl_buffer *buffer;
   bool busy;
   int shm_fd;
   void *shm_ptr;
   unsigned shm_size;
};

enum wsi_wl_buffer_type {
   WSI_WL_BUFFER_NATIVE,
   WSI_WL_BUFFER_GPU_SHM,
   WSI_WL_BUFFER_SHM_MEMCPY,
};

struct wsi_wl_surface {
   VkIcdSurfaceWayland base;

   struct wsi_wl_swapchain *chain;
   struct wl_surface *surface;
   struct wsi_wl_display *display;

   struct zwp_linux_dmabuf_feedback_v1 *wl_dmabuf_feedback;
   struct dmabuf_feedback dmabuf_feedback, pending_dmabuf_feedback;
};

struct wsi_wl_swapchain {
   struct wsi_swapchain base;

   struct wsi_wl_surface *wsi_wl_surface;

   struct wl_callback *frame;

   VkExtent2D extent;
   VkFormat vk_format;
   enum wsi_wl_buffer_type buffer_type;
   uint32_t drm_format;
   enum wl_shm_format shm_format;

   bool suboptimal;

   uint32_t num_drm_modifiers;
   const uint64_t *drm_modifiers;

   VkPresentModeKHR present_mode;
   bool fifo_ready;

   struct wsi_wl_image images[0];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wsi_wl_swapchain, base.base, VkSwapchainKHR,
                               VK_OBJECT_TYPE_SWAPCHAIN_KHR)

enum wsi_wl_fmt_flag {
   WSI_WL_FMT_ALPHA = 1 << 0,
   WSI_WL_FMT_OPAQUE = 1 << 1,
};

static struct wsi_wl_format *
find_format(struct u_vector *formats, VkFormat format)
{
   struct wsi_wl_format *f;

   u_vector_foreach(f, formats)
      if (f->vk_format == format)
         return f;

   return NULL;
}

static struct wsi_wl_format *
wsi_wl_display_add_vk_format(struct wsi_wl_display *display,
                             struct u_vector *formats,
                             VkFormat format, uint32_t flags)
{
   assert(flags & (WSI_WL_FMT_ALPHA | WSI_WL_FMT_OPAQUE));

   /* Don't add a format that's already in the list */
   struct wsi_wl_format *f = find_format(formats, format);
   if (f) {
      f->flags |= flags;
      return f;
   }

   /* Don't add formats that aren't renderable. */
   VkFormatProperties props;

   display->wsi_wl->wsi->GetPhysicalDeviceFormatProperties(display->wsi_wl->physical_device,
                                                           format, &props);
   if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
      return NULL;

   struct u_vector modifiers;
   if (!u_vector_init_pow2(&modifiers, 4, sizeof(uint64_t)))
      return NULL;

   f = u_vector_add(formats);
   if (!f) {
      u_vector_finish(&modifiers);
      return NULL;
   }

   f->vk_format = format;
   f->flags = flags;
   f->modifiers = modifiers;

   return f;
}

static void
wsi_wl_format_add_modifier(struct wsi_wl_format *format, uint64_t modifier)
{
   uint64_t *mod;

   if (modifier == DRM_FORMAT_MOD_INVALID)
      return;

   u_vector_foreach(mod, &format->modifiers)
      if (*mod == modifier)
         return;

   mod = u_vector_add(&format->modifiers);
   if (mod)
      *mod = modifier;
}

static void
wsi_wl_display_add_vk_format_modifier(struct wsi_wl_display *display,
                                      struct u_vector *formats,
                                      VkFormat vk_format, uint32_t flags,
                                      uint64_t modifier)
{
   struct wsi_wl_format *format;

   format = wsi_wl_display_add_vk_format(display, formats, vk_format, flags);
   if (format)
      wsi_wl_format_add_modifier(format, modifier);
}

static void
wsi_wl_display_add_drm_format_modifier(struct wsi_wl_display *display,
                                       struct u_vector *formats,
                                       uint32_t drm_format, uint64_t modifier)
{
   switch (drm_format) {
#if 0
   /* TODO: These are only available when VK_EXT_4444_formats is enabled, so
    * we probably need to make their use conditional on this extension. */
   case DRM_FORMAT_ARGB4444:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_A4R4G4B4_UNORM_PACK16,
                                            WSI_WL_FMT_ALPHA, modifier);
      break;
   case DRM_FORMAT_XRGB4444:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_A4R4G4B4_UNORM_PACK16,
                                            WSI_WL_FMT_OPAQUE, modifier);
      break;
   case DRM_FORMAT_ABGR4444:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_A4B4G4R4_UNORM_PACK16,
                                            WSI_WL_FMT_ALPHA, modifier);
      break;
   case DRM_FORMAT_XBGR4444:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_A4B4G4R4_UNORM_PACK16,
                                            WSI_WL_FMT_OPAQUE, modifier);
      break;
#endif

   /* Vulkan _PACKN formats have the same component order as DRM formats
    * on little endian systems, on big endian there exists no analog. */
#if MESA_LITTLE_ENDIAN
   case DRM_FORMAT_RGBA4444:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_R4G4B4A4_UNORM_PACK16,
                                            WSI_WL_FMT_ALPHA, modifier);
      break;
   case DRM_FORMAT_RGBX4444:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_R4G4B4A4_UNORM_PACK16,
                                            WSI_WL_FMT_OPAQUE, modifier);
      break;
   case DRM_FORMAT_BGRA4444:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_B4G4R4A4_UNORM_PACK16,
                                            WSI_WL_FMT_ALPHA, modifier);
      break;
   case DRM_FORMAT_BGRX4444:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_B4G4R4A4_UNORM_PACK16,
                                            WSI_WL_FMT_OPAQUE, modifier);
      break;
   case DRM_FORMAT_RGB565:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_R5G6B5_UNORM_PACK16,
                                            WSI_WL_FMT_ALPHA | WSI_WL_FMT_OPAQUE,
                                            modifier);
      break;
   case DRM_FORMAT_BGR565:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_B5G6R5_UNORM_PACK16,
                                            WSI_WL_FMT_ALPHA | WSI_WL_FMT_OPAQUE,
                                            modifier);
      break;
   case DRM_FORMAT_ARGB1555:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_A1R5G5B5_UNORM_PACK16,
                                            WSI_WL_FMT_ALPHA, modifier);
      break;
   case DRM_FORMAT_XRGB1555:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_A1R5G5B5_UNORM_PACK16,
                                            WSI_WL_FMT_OPAQUE, modifier);
      break;
   case DRM_FORMAT_RGBA5551:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_R5G5B5A1_UNORM_PACK16,
                                            WSI_WL_FMT_ALPHA, modifier);
      break;
   case DRM_FORMAT_RGBX5551:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_R5G5B5A1_UNORM_PACK16,
                                            WSI_WL_FMT_OPAQUE, modifier);
      break;
   case DRM_FORMAT_BGRA5551:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_B5G5R5A1_UNORM_PACK16,
                                            WSI_WL_FMT_ALPHA, modifier);
      break;
   case DRM_FORMAT_BGRX5551:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_B5G5R5A1_UNORM_PACK16,
                                            WSI_WL_FMT_OPAQUE, modifier);
      break;
   case DRM_FORMAT_ARGB2101010:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_A2R10G10B10_UNORM_PACK32,
                                            WSI_WL_FMT_ALPHA, modifier);
      break;
   case DRM_FORMAT_XRGB2101010:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_A2R10G10B10_UNORM_PACK32,
                                            WSI_WL_FMT_OPAQUE, modifier);
      break;
   case DRM_FORMAT_ABGR2101010:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                            WSI_WL_FMT_ALPHA, modifier);
      break;
   case DRM_FORMAT_XBGR2101010:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                            WSI_WL_FMT_OPAQUE, modifier);
      break;
#endif

   /* Non-packed 8-bit formats have an inverted channel order compared to the
    * little endian DRM formats, because the DRM channel ordering is high->low
    * but the vulkan channel ordering is in memory byte order
    *
    * For all UNORM formats which have a SRGB variant, we must support both if
    * we can. SRGB in this context means that rendering to it will result in a
    * linear -> nonlinear SRGB colorspace conversion before the data is stored.
    * The inverse function is applied when sampling from SRGB images.
    * From Wayland's perspective nothing changes, the difference is just how
    * Vulkan interprets the pixel data. */
   case DRM_FORMAT_XBGR8888:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_R8G8B8_SRGB,
                                            WSI_WL_FMT_ALPHA | WSI_WL_FMT_OPAQUE,
                                            modifier);
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_R8G8B8_UNORM,
                                            WSI_WL_FMT_ALPHA | WSI_WL_FMT_OPAQUE,
                                            modifier);
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_R8G8B8A8_SRGB,
                                            WSI_WL_FMT_OPAQUE, modifier);
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_R8G8B8A8_UNORM,
                                            WSI_WL_FMT_OPAQUE, modifier);
      break;
   case DRM_FORMAT_ABGR8888:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_R8G8B8A8_SRGB,
                                            WSI_WL_FMT_ALPHA, modifier);
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_R8G8B8A8_UNORM,
                                            WSI_WL_FMT_ALPHA, modifier);
      break;
   case DRM_FORMAT_XRGB8888:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_B8G8R8_SRGB,
                                            WSI_WL_FMT_ALPHA | WSI_WL_FMT_OPAQUE,
                                            modifier);
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_B8G8R8_UNORM,
                                            WSI_WL_FMT_ALPHA | WSI_WL_FMT_OPAQUE,
                                            modifier);
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_B8G8R8A8_SRGB,
                                            WSI_WL_FMT_OPAQUE, modifier);
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_B8G8R8A8_UNORM,
                                            WSI_WL_FMT_OPAQUE, modifier);
      break;
   case DRM_FORMAT_ARGB8888:
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_B8G8R8A8_SRGB,
                                            WSI_WL_FMT_ALPHA, modifier);
      wsi_wl_display_add_vk_format_modifier(display, formats,
                                            VK_FORMAT_B8G8R8A8_UNORM,
                                            WSI_WL_FMT_ALPHA, modifier);
      break;
   }
}

static uint32_t
drm_format_for_wl_shm_format(enum wl_shm_format shm_format)
{
   /* wl_shm formats are identical to DRM, except ARGB8888 and XRGB8888 */
   switch (shm_format) {
   case WL_SHM_FORMAT_ARGB8888:
      return DRM_FORMAT_ARGB8888;
   case WL_SHM_FORMAT_XRGB8888:
      return DRM_FORMAT_XRGB8888;
   default:
      return shm_format;
   }
}

static void
wsi_wl_display_add_wl_shm_format(struct wsi_wl_display *display,
                                 struct u_vector *formats,
                                 enum wl_shm_format shm_format)
{
   uint32_t drm_format = drm_format_for_wl_shm_format(shm_format);

   wsi_wl_display_add_drm_format_modifier(display, formats, drm_format,
                                          DRM_FORMAT_MOD_INVALID);
}

static uint32_t
wl_drm_format_for_vk_format(VkFormat vk_format, bool alpha)
{
   switch (vk_format) {
#if 0
   case VK_FORMAT_A4R4G4B4_UNORM_PACK16:
      return alpha ? DRM_FORMAT_ARGB4444 : DRM_FORMAT_XRGB4444;
   case VK_FORMAT_A4B4G4R4_UNORM_PACK16:
      return alpha ? DRM_FORMAT_ABGR4444 : DRM_FORMAT_XBGR4444;
#endif
#if MESA_LITTLE_ENDIAN
   case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
      return alpha ? DRM_FORMAT_RGBA4444 : DRM_FORMAT_RGBX4444;
   case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
      return alpha ? DRM_FORMAT_BGRA4444 : DRM_FORMAT_BGRX4444;
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
      return DRM_FORMAT_RGB565;
   case VK_FORMAT_B5G6R5_UNORM_PACK16:
      return DRM_FORMAT_BGR565;
   case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
      return alpha ? DRM_FORMAT_ARGB1555 : DRM_FORMAT_XRGB1555;
   case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
      return alpha ? DRM_FORMAT_RGBA5551 : DRM_FORMAT_RGBX5551;
   case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
      return alpha ? DRM_FORMAT_BGRA5551 : DRM_FORMAT_BGRX5551;
   case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
      return alpha ? DRM_FORMAT_ARGB2101010 : DRM_FORMAT_XRGB2101010;
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return alpha ? DRM_FORMAT_ABGR2101010 : DRM_FORMAT_XBGR2101010;
#endif
   case VK_FORMAT_R8G8B8_UNORM:
   case VK_FORMAT_R8G8B8_SRGB:
      return DRM_FORMAT_XBGR8888;
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
      return alpha ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_XBGR8888;
   case VK_FORMAT_B8G8R8_UNORM:
   case VK_FORMAT_B8G8R8_SRGB:
      return DRM_FORMAT_BGRX8888;
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
      return alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;

   default:
      assert(!"Unsupported Vulkan format");
      return DRM_FORMAT_INVALID;
   }
}

static enum wl_shm_format
wl_shm_format_for_vk_format(VkFormat vk_format, bool alpha)
{
   uint32_t drm_format = wl_drm_format_for_vk_format(vk_format, alpha);
   if (drm_format == DRM_FORMAT_INVALID) {
      return 0;
   }

   /* wl_shm formats are identical to DRM, except ARGB8888 and XRGB8888 */
   switch (drm_format) {
   case DRM_FORMAT_ARGB8888:
      return WL_SHM_FORMAT_ARGB8888;
   case DRM_FORMAT_XRGB8888:
      return WL_SHM_FORMAT_XRGB8888;
   default:
      return drm_format;
   }
}

static void
dmabuf_handle_format(void *data, struct zwp_linux_dmabuf_v1 *dmabuf,
                     uint32_t format)
{
   /* Formats are implicitly advertised by the modifier event, so we ignore
    * them here. */
}

static void
dmabuf_handle_modifier(void *data, struct zwp_linux_dmabuf_v1 *dmabuf,
                       uint32_t format, uint32_t modifier_hi,
                       uint32_t modifier_lo)
{
   struct wsi_wl_display *display = data;
   uint64_t modifier;

   /* Ignore this if the compositor advertised dma-buf feedback. From version 4
    * onwards (when dma-buf feedback was introduced), the compositor should not
    * advertise this event anymore, but let's keep this for safety. */
   if (display->wl_dmabuf_feedback)
      return;

   modifier = ((uint64_t) modifier_hi << 32) | modifier_lo;
   wsi_wl_display_add_drm_format_modifier(display, &display->formats,
                                          format, modifier);
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
   dmabuf_handle_format,
   dmabuf_handle_modifier,
};

static void
dmabuf_feedback_format_table_fini(struct dmabuf_feedback_format_table *format_table)
{
   if (format_table->data && format_table->data != MAP_FAILED)
      munmap(format_table->data, format_table->size);
}

static void
dmabuf_feedback_format_table_init(struct dmabuf_feedback_format_table *format_table)
{
   memset(format_table, 0, sizeof(*format_table));
}

static void
dmabuf_feedback_tranche_fini(struct dmabuf_feedback_tranche *tranche)
{
   struct wsi_wl_format *format;

   u_vector_foreach(format, &tranche->formats)
      u_vector_finish(&format->modifiers);

   u_vector_finish(&tranche->formats);
}

static int
dmabuf_feedback_tranche_init(struct dmabuf_feedback_tranche *tranche)
{
   memset(tranche, 0, sizeof(*tranche));

   if (!u_vector_init(&tranche->formats, 8, sizeof(struct wsi_wl_format)))
      return -1;

   return 0;
}

static void
dmabuf_feedback_fini(struct dmabuf_feedback *dmabuf_feedback)
{
   dmabuf_feedback_tranche_fini(&dmabuf_feedback->pending_tranche);

   util_dynarray_foreach(&dmabuf_feedback->tranches,
                         struct dmabuf_feedback_tranche, tranche)
      dmabuf_feedback_tranche_fini(tranche);
   util_dynarray_fini(&dmabuf_feedback->tranches);

   dmabuf_feedback_format_table_fini(&dmabuf_feedback->format_table);
}

static int
dmabuf_feedback_init(struct dmabuf_feedback *dmabuf_feedback)
{
   memset(dmabuf_feedback, 0, sizeof(*dmabuf_feedback));

   if (dmabuf_feedback_tranche_init(&dmabuf_feedback->pending_tranche) < 0)
      return -1;

   util_dynarray_init(&dmabuf_feedback->tranches, NULL);

   dmabuf_feedback_format_table_init(&dmabuf_feedback->format_table);

   return 0;
}

static void
default_dmabuf_feedback_format_table(void *data,
                                     struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
                                     int32_t fd, uint32_t size)
{
   struct wsi_wl_display *display = data;

   display->format_table.size = size;
   display->format_table.data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

   close(fd);
}

static void
default_dmabuf_feedback_main_device(void *data,
                                    struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                    struct wl_array *device)
{
   /* ignore this event */
}

static void
default_dmabuf_feedback_tranche_target_device(void *data,
                                              struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                              struct wl_array *device)
{
   /* ignore this event */
}

static void
default_dmabuf_feedback_tranche_flags(void *data,
                                      struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                      uint32_t flags)
{
   /* ignore this event */
}

static void
default_dmabuf_feedback_tranche_formats(void *data,
                                        struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                        struct wl_array *indices)
{
   struct wsi_wl_display *display = data;
   uint32_t format;
   uint64_t modifier;
   uint16_t *index;

   /* We couldn't map the format table or the compositor didn't advertise it,
    * so we have to ignore the feedback. */
   if (display->format_table.data == MAP_FAILED ||
       display->format_table.data == NULL)
      return;

   wl_array_for_each(index, indices) {
      format = display->format_table.data[*index].format;
      modifier = display->format_table.data[*index].modifier;
      wsi_wl_display_add_drm_format_modifier(display, &display->formats,
                                             format, modifier);
   }
}

static void
default_dmabuf_feedback_tranche_done(void *data,
                                     struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback)
{
   /* ignore this event */
}

static void
default_dmabuf_feedback_done(void *data,
                             struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback)
{
   /* ignore this event */
}

static const struct zwp_linux_dmabuf_feedback_v1_listener
dmabuf_feedback_listener = {
   .format_table = default_dmabuf_feedback_format_table,
   .main_device = default_dmabuf_feedback_main_device,
   .tranche_target_device = default_dmabuf_feedback_tranche_target_device,
   .tranche_flags = default_dmabuf_feedback_tranche_flags,
   .tranche_formats = default_dmabuf_feedback_tranche_formats,
   .tranche_done = default_dmabuf_feedback_tranche_done,
   .done = default_dmabuf_feedback_done,
};

static void
shm_handle_format(void *data, struct wl_shm *shm, uint32_t format)
{
   struct wsi_wl_display *display = data;

   wsi_wl_display_add_wl_shm_format(display, &display->formats, format);
}

static const struct wl_shm_listener shm_listener = {
   .format = shm_handle_format
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
                       uint32_t name, const char *interface, uint32_t version)
{
   struct wsi_wl_display *display = data;

   if (display->sw) {
      if (strcmp(interface, wl_shm_interface.name) == 0) {
         display->wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
         wl_shm_add_listener(display->wl_shm, &shm_listener, display);
      }
      return;
   }

   if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0 && version >= 3) {
      display->wl_dmabuf =
         wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface,
                          MIN2(version, ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION));
      zwp_linux_dmabuf_v1_add_listener(display->wl_dmabuf,
                                       &dmabuf_listener, display);
   }
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
                              uint32_t name)
{ /* No-op */ }

static const struct wl_registry_listener registry_listener = {
   registry_handle_global,
   registry_handle_global_remove
};

static void
wsi_wl_display_finish(struct wsi_wl_display *display)
{
   struct wsi_wl_format *f;
   u_vector_foreach(f, &display->formats)
      u_vector_finish(&f->modifiers);
   u_vector_finish(&display->formats);
   if (display->wl_shm)
      wl_shm_destroy(display->wl_shm);
   if (display->wl_dmabuf)
      zwp_linux_dmabuf_v1_destroy(display->wl_dmabuf);
   if (display->wl_display_wrapper)
      wl_proxy_wrapper_destroy(display->wl_display_wrapper);
   if (display->queue)
      wl_event_queue_destroy(display->queue);
}

static VkResult
wsi_wl_display_init(struct wsi_wayland *wsi_wl,
                    struct wsi_wl_display *display,
                    struct wl_display *wl_display,
                    bool get_format_list, bool sw)
{
   VkResult result = VK_SUCCESS;
   memset(display, 0, sizeof(*display));

   if (!u_vector_init(&display->formats, 8, sizeof(struct wsi_wl_format)))
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   display->wsi_wl = wsi_wl;
   display->wl_display = wl_display;
   display->sw = sw;

   display->queue = wl_display_create_queue(wl_display);
   if (!display->queue) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   display->wl_display_wrapper = wl_proxy_create_wrapper(wl_display);
   if (!display->wl_display_wrapper) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   wl_proxy_set_queue((struct wl_proxy *) display->wl_display_wrapper,
                      display->queue);

   struct wl_registry *registry =
      wl_display_get_registry(display->wl_display_wrapper);
   if (!registry) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   wl_registry_add_listener(registry, &registry_listener, display);

   /* Round-trip to get wl_shm and zwp_linux_dmabuf_v1 globals */
   wl_display_roundtrip_queue(display->wl_display, display->queue);
   if (!display->wl_dmabuf && !display->wl_shm) {
      result = VK_ERROR_SURFACE_LOST_KHR;
      goto fail_registry;
   }

   /* Caller doesn't expect us to query formats/modifiers, so return */
   if (!get_format_list)
      goto out;

   /* Get the default dma-buf feedback */
   if (display->wl_dmabuf && zwp_linux_dmabuf_v1_get_version(display->wl_dmabuf) >=
                             ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) {
         dmabuf_feedback_format_table_init(&display->format_table);
         display->wl_dmabuf_feedback =
            zwp_linux_dmabuf_v1_get_default_feedback(display->wl_dmabuf);
         zwp_linux_dmabuf_feedback_v1_add_listener(display->wl_dmabuf_feedback,
                                                   &dmabuf_feedback_listener, display);
   }

   /* Round-trip again to get formats and modifiers */
   wl_display_roundtrip_queue(display->wl_display, display->queue);

   if (wsi_wl->wsi->force_bgra8_unorm_first) {
      /* Find BGRA8_UNORM in the list and swap it to the first position if we
       * can find it.  Some apps get confused if SRGB is first in the list.
       */
      struct wsi_wl_format *first_fmt = u_vector_head(&display->formats);
      struct wsi_wl_format *f, tmp_fmt;
      f = find_format(&display->formats, VK_FORMAT_B8G8R8A8_UNORM);
      if (f) {
         tmp_fmt = *f;
         *f = *first_fmt;
         *first_fmt = tmp_fmt;
      }
   }

out:
   /* We don't need this anymore */
   wl_registry_destroy(registry);

   /* Destroy default dma-buf feedback object and format table */
   if (display->wl_dmabuf_feedback) {
      zwp_linux_dmabuf_feedback_v1_destroy(display->wl_dmabuf_feedback);
      display->wl_dmabuf_feedback = NULL;
      dmabuf_feedback_format_table_fini(&display->format_table);
   }

   return VK_SUCCESS;

fail_registry:
   if (registry)
      wl_registry_destroy(registry);

fail:
   wsi_wl_display_finish(display);
   return result;
}

static VkResult
wsi_wl_display_create(struct wsi_wayland *wsi, struct wl_display *wl_display,
                      bool sw,
                      struct wsi_wl_display **display_out)
{
   struct wsi_wl_display *display =
      vk_alloc(wsi->alloc, sizeof(*display), 8,
               VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!display)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkResult result = wsi_wl_display_init(wsi, display, wl_display, true,
                                         sw);
   if (result != VK_SUCCESS) {
      vk_free(wsi->alloc, display);
      return result;
   }

   *display_out = display;

   return result;
}

static void
wsi_wl_display_destroy(struct wsi_wl_display *display)
{
   struct wsi_wayland *wsi = display->wsi_wl;
   wsi_wl_display_finish(display);
   vk_free(wsi->alloc, display);
}

VKAPI_ATTR VkBool32 VKAPI_CALL
wsi_GetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                   uint32_t queueFamilyIndex,
                                                   struct wl_display *wl_display)
{
   VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);
   struct wsi_device *wsi_device = pdevice->wsi_device;
   struct wsi_wayland *wsi =
      (struct wsi_wayland *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_WAYLAND];

   struct wsi_wl_display display;
   VkResult ret = wsi_wl_display_init(wsi, &display, wl_display, false,
                                      wsi_device->sw);
   if (ret == VK_SUCCESS)
      wsi_wl_display_finish(&display);

   return ret == VK_SUCCESS;
}

static VkResult
wsi_wl_surface_get_support(VkIcdSurfaceBase *surface,
                           struct wsi_device *wsi_device,
                           uint32_t queueFamilyIndex,
                           VkBool32* pSupported)
{
   *pSupported = true;

   return VK_SUCCESS;
}

static const VkPresentModeKHR present_modes[] = {
   VK_PRESENT_MODE_MAILBOX_KHR,
   VK_PRESENT_MODE_FIFO_KHR,
};

static VkResult
wsi_wl_surface_get_capabilities(VkIcdSurfaceBase *surface,
                                struct wsi_device *wsi_device,
                                VkSurfaceCapabilitiesKHR* caps)
{
   /* For true mailbox mode, we need at least 4 images:
    *  1) One to scan out from
    *  2) One to have queued for scan-out
    *  3) One to be currently held by the Wayland compositor
    *  4) One to render to
    */
   caps->minImageCount = 4;
   /* There is no real maximum */
   caps->maxImageCount = 0;

   caps->currentExtent = (VkExtent2D) { UINT32_MAX, UINT32_MAX };
   caps->minImageExtent = (VkExtent2D) { 1, 1 };
   caps->maxImageExtent = (VkExtent2D) {
      wsi_device->maxImageDimension2D,
      wsi_device->maxImageDimension2D,
   };

   caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->maxImageArrayLayers = 1;

   caps->supportedCompositeAlpha =
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;

   caps->supportedUsageFlags =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

   return VK_SUCCESS;
}

static VkResult
wsi_wl_surface_get_capabilities2(VkIcdSurfaceBase *surface,
                                 struct wsi_device *wsi_device,
                                 const void *info_next,
                                 VkSurfaceCapabilities2KHR* caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);

   VkResult result =
      wsi_wl_surface_get_capabilities(surface, wsi_device,
                                      &caps->surfaceCapabilities);

   vk_foreach_struct(ext, caps->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *protected = (void *)ext;
         protected->supportsProtected = VK_FALSE;
         break;
      }

      default:
         /* Ignored */
         break;
      }
   }

   return result;
}

static VkResult
wsi_wl_surface_get_formats(VkIcdSurfaceBase *icd_surface,
			   struct wsi_device *wsi_device,
                           uint32_t* pSurfaceFormatCount,
                           VkSurfaceFormatKHR* pSurfaceFormats)
{
   VkIcdSurfaceWayland *surface = (VkIcdSurfaceWayland *)icd_surface;
   struct wsi_wayland *wsi =
      (struct wsi_wayland *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_WAYLAND];

   struct wsi_wl_display display;
   if (wsi_wl_display_init(wsi, &display, surface->display, true,
                           wsi_device->sw))
      return VK_ERROR_SURFACE_LOST_KHR;

   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out,
                          pSurfaceFormats, pSurfaceFormatCount);

   struct wsi_wl_format *disp_fmt;
   u_vector_foreach(disp_fmt, &display.formats) {
      /* Skip formats for which we can't support both alpha & opaque
       * formats.
       */
      if (!(disp_fmt->flags & WSI_WL_FMT_ALPHA) ||
          !(disp_fmt->flags & WSI_WL_FMT_OPAQUE))
         continue;

      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, out_fmt) {
         out_fmt->format = disp_fmt->vk_format;
         out_fmt->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   wsi_wl_display_finish(&display);

   return vk_outarray_status(&out);
}

static VkResult
wsi_wl_surface_get_formats2(VkIcdSurfaceBase *icd_surface,
			    struct wsi_device *wsi_device,
                            const void *info_next,
                            uint32_t* pSurfaceFormatCount,
                            VkSurfaceFormat2KHR* pSurfaceFormats)
{
   VkIcdSurfaceWayland *surface = (VkIcdSurfaceWayland *)icd_surface;
   struct wsi_wayland *wsi =
      (struct wsi_wayland *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_WAYLAND];

   struct wsi_wl_display display;
   if (wsi_wl_display_init(wsi, &display, surface->display, true,
                           wsi_device->sw))
      return VK_ERROR_SURFACE_LOST_KHR;

   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out,
                          pSurfaceFormats, pSurfaceFormatCount);

   struct wsi_wl_format *disp_fmt;
   u_vector_foreach(disp_fmt, &display.formats) {
      /* Skip formats for which we can't support both alpha & opaque
       * formats.
       */
      if (!(disp_fmt->flags & WSI_WL_FMT_ALPHA) ||
          !(disp_fmt->flags & WSI_WL_FMT_OPAQUE))
         continue;

      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, out_fmt) {
         out_fmt->surfaceFormat.format = disp_fmt->vk_format;
         out_fmt->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   wsi_wl_display_finish(&display);

   return vk_outarray_status(&out);
}

static VkResult
wsi_wl_surface_get_present_modes(VkIcdSurfaceBase *surface,
                                 uint32_t* pPresentModeCount,
                                 VkPresentModeKHR* pPresentModes)
{
   if (pPresentModes == NULL) {
      *pPresentModeCount = ARRAY_SIZE(present_modes);
      return VK_SUCCESS;
   }

   *pPresentModeCount = MIN2(*pPresentModeCount, ARRAY_SIZE(present_modes));
   typed_memcpy(pPresentModes, present_modes, *pPresentModeCount);

   if (*pPresentModeCount < ARRAY_SIZE(present_modes))
      return VK_INCOMPLETE;
   else
      return VK_SUCCESS;
}

static VkResult
wsi_wl_surface_get_present_rectangles(VkIcdSurfaceBase *surface,
                                      struct wsi_device *wsi_device,
                                      uint32_t* pRectCount,
                                      VkRect2D* pRects)
{
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);

   vk_outarray_append_typed(VkRect2D, &out, rect) {
      /* We don't know a size so just return the usual "I don't know." */
      *rect = (VkRect2D) {
         .offset = { 0, 0 },
         .extent = { UINT32_MAX, UINT32_MAX },
      };
   }

   return vk_outarray_status(&out);
}

void
wsi_wl_surface_destroy(VkIcdSurfaceBase *icd_surface, VkInstance _instance,
                       const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   struct wsi_wl_surface *wsi_wl_surface =
      wl_container_of((VkIcdSurfaceWayland *)icd_surface, wsi_wl_surface, base);

   if (wsi_wl_surface->surface)
      wl_proxy_wrapper_destroy(wsi_wl_surface->surface);

   if (wsi_wl_surface->display)
      wsi_wl_display_destroy(wsi_wl_surface->display);

   if (wsi_wl_surface->wl_dmabuf_feedback) {
      zwp_linux_dmabuf_feedback_v1_destroy(wsi_wl_surface->wl_dmabuf_feedback);
      dmabuf_feedback_fini(&wsi_wl_surface->dmabuf_feedback);
      dmabuf_feedback_fini(&wsi_wl_surface->pending_dmabuf_feedback);
   }

   vk_free2(&instance->alloc, pAllocator, wsi_wl_surface);
}

static struct wsi_wl_format *
pick_format_from_surface_dmabuf_feedback(struct wsi_wl_surface *wsi_wl_surface,
                                         VkFormat vk_format)
{
   struct wsi_wl_format *f = NULL;

   /* If the main_device was not advertised, we don't have valid feedback */
   if (wsi_wl_surface->dmabuf_feedback.main_device == 0)
      return NULL;

   util_dynarray_foreach(&wsi_wl_surface->dmabuf_feedback.tranches,
                         struct dmabuf_feedback_tranche, tranche) {
      f = find_format(&tranche->formats, vk_format);
      if (f)
         break;
   }

   return f;
}

static void
surface_dmabuf_feedback_format_table(void *data,
                                     struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
                                     int32_t fd, uint32_t size)
{
   struct wsi_wl_surface *wsi_wl_surface = data;
   struct dmabuf_feedback *feedback = &wsi_wl_surface->pending_dmabuf_feedback;

   feedback->format_table.size = size;
   feedback->format_table.data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

   close(fd);
}

static void
surface_dmabuf_feedback_main_device(void *data,
                                    struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                    struct wl_array *device)
{
   struct wsi_wl_surface *wsi_wl_surface = data;
   struct dmabuf_feedback *feedback = &wsi_wl_surface->pending_dmabuf_feedback;

   memcpy(&feedback->main_device, device->data, sizeof(feedback->main_device));
}

static void
surface_dmabuf_feedback_tranche_target_device(void *data,
                                              struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                              struct wl_array *device)
{
   struct wsi_wl_surface *wsi_wl_surface = data;
   struct dmabuf_feedback *feedback = &wsi_wl_surface->pending_dmabuf_feedback;

   memcpy(&feedback->pending_tranche.target_device, device->data,
          sizeof(feedback->pending_tranche.target_device));
}

static void
surface_dmabuf_feedback_tranche_flags(void *data,
                                      struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                      uint32_t flags)
{
   struct wsi_wl_surface *wsi_wl_surface = data;
   struct dmabuf_feedback *feedback = &wsi_wl_surface->pending_dmabuf_feedback;

   feedback->pending_tranche.flags = flags;
}

static void
surface_dmabuf_feedback_tranche_formats(void *data,
                                        struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                        struct wl_array *indices)
{
   struct wsi_wl_surface *wsi_wl_surface = data;
   struct dmabuf_feedback *feedback = &wsi_wl_surface->pending_dmabuf_feedback;
   uint32_t format;
   uint64_t modifier;
   uint16_t *index;

   /* Compositor may advertise or not a format table. If it does, we use it.
    * Otherwise, we steal the most recent advertised format table. If we don't have
    * a most recent advertised format table, compositor did something wrong. */
   if (feedback->format_table.data == NULL) {
      feedback->format_table = wsi_wl_surface->dmabuf_feedback.format_table;
      dmabuf_feedback_format_table_init(&wsi_wl_surface->dmabuf_feedback.format_table);
   }
   if (feedback->format_table.data == MAP_FAILED ||
       feedback->format_table.data == NULL)
      return;

   wl_array_for_each(index, indices) {
      format = feedback->format_table.data[*index].format;
      modifier = feedback->format_table.data[*index].modifier;

      wsi_wl_display_add_drm_format_modifier(wsi_wl_surface->display,
                        &wsi_wl_surface->pending_dmabuf_feedback.pending_tranche.formats,
                        format, modifier);
   }
}

static void
surface_dmabuf_feedback_tranche_done(void *data,
                                     struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback)
{
   struct wsi_wl_surface *wsi_wl_surface = data;
   struct dmabuf_feedback *feedback = &wsi_wl_surface->pending_dmabuf_feedback;

   /* Add tranche to array of tranches. */
   util_dynarray_append(&feedback->tranches, struct dmabuf_feedback_tranche,
                        feedback->pending_tranche);

   dmabuf_feedback_tranche_init(&feedback->pending_tranche);
}

static bool
sets_of_modifiers_are_the_same(uint32_t num_drm_modifiers_A, const uint64_t *modifiers_A,
                               uint32_t num_drm_modifiers_B, const uint64_t *modifiers_B)
{
   uint32_t i, j;
   bool mod_found;

   if (num_drm_modifiers_A != num_drm_modifiers_B)
      return false;

   for (i = 0; i < num_drm_modifiers_A; i++) {
      mod_found = false;
      for (j = 0; j < num_drm_modifiers_B; j++) {
         if (modifiers_A[i] == modifiers_B[j]) {
            mod_found = true;
            break;
         }
      }
      if (!mod_found)
         return false;
   }

   return true;
}

static void
surface_dmabuf_feedback_done(void *data,
                             struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback)
{
   struct wsi_wl_surface *wsi_wl_surface = data;
   struct wsi_wl_swapchain *chain = wsi_wl_surface->chain;
   struct wsi_wl_format *f;

   dmabuf_feedback_fini(&wsi_wl_surface->dmabuf_feedback);
   wsi_wl_surface->dmabuf_feedback = wsi_wl_surface->pending_dmabuf_feedback;
   dmabuf_feedback_init(&wsi_wl_surface->pending_dmabuf_feedback);

   /* It's not just because we received dma-buf feedback that re-allocation is a
    * good idea. In order to know if we should re-allocate or not, we must
    * compare the most recent parameters that we used to allocate with the ones
    * from the feedback we just received.
    *
    * The allocation parameters are: the format, its set of modifiers and the
    * tranche flags. On WSI we are not using the tranche flags for anything, so
    * we disconsider this. As we can't switch to another format (it is selected
    * by the client), we just need to compare the set of modifiers.
    *
    * So we just look for the vk_format in the tranches (respecting their
    * preferences), and compare its set of modifiers with the set of modifiers
    * we've used to allocate previously. If they differ, we are using suboptimal
    * parameters and should re-allocate.
    */
   f = pick_format_from_surface_dmabuf_feedback(wsi_wl_surface, chain->vk_format);
   if (f && !sets_of_modifiers_are_the_same(u_vector_length(&f->modifiers),
                                            u_vector_tail(&f->modifiers),
                                            chain->num_drm_modifiers,
                                            chain->drm_modifiers))
      wsi_wl_surface->chain->suboptimal = true;
}

static const struct zwp_linux_dmabuf_feedback_v1_listener
surface_dmabuf_feedback_listener = {
   .format_table = surface_dmabuf_feedback_format_table,
   .main_device = surface_dmabuf_feedback_main_device,
   .tranche_target_device = surface_dmabuf_feedback_tranche_target_device,
   .tranche_flags = surface_dmabuf_feedback_tranche_flags,
   .tranche_formats = surface_dmabuf_feedback_tranche_formats,
   .tranche_done = surface_dmabuf_feedback_tranche_done,
   .done = surface_dmabuf_feedback_done,
};

static VkResult wsi_wl_surface_bind_to_dmabuf_feedback(struct wsi_wl_surface *wsi_wl_surface)
{
   wsi_wl_surface->wl_dmabuf_feedback =
      zwp_linux_dmabuf_v1_get_surface_feedback(wsi_wl_surface->display->wl_dmabuf,
                                               wsi_wl_surface->surface);

   zwp_linux_dmabuf_feedback_v1_add_listener(wsi_wl_surface->wl_dmabuf_feedback,
                                             &surface_dmabuf_feedback_listener,
                                             wsi_wl_surface);

   if (dmabuf_feedback_init(&wsi_wl_surface->dmabuf_feedback) < 0)
      goto fail;
   if (dmabuf_feedback_init(&wsi_wl_surface->pending_dmabuf_feedback) < 0)
      goto fail_pending;

   return VK_SUCCESS;

fail_pending:
   dmabuf_feedback_fini(&wsi_wl_surface->dmabuf_feedback);
fail:
   zwp_linux_dmabuf_feedback_v1_destroy(wsi_wl_surface->wl_dmabuf_feedback);
   wsi_wl_surface->wl_dmabuf_feedback = NULL;
   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult wsi_wl_surface_init(struct wsi_wl_surface *wsi_wl_surface,
                                    struct wsi_device *wsi_device)
{
   struct wsi_wayland *wsi =
      (struct wsi_wayland *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_WAYLAND];
   VkResult result;

   /* wsi_wl_surface has already been initialized. */
   if (wsi_wl_surface->display)
      return VK_SUCCESS;

   result = wsi_wl_display_create(wsi, wsi_wl_surface->base.display,
                                  wsi_device->sw, &wsi_wl_surface->display);
   if (result != VK_SUCCESS)
      goto fail;

   wsi_wl_surface->surface = wl_proxy_create_wrapper(wsi_wl_surface->base.surface);
   if (!wsi_wl_surface->surface) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }
   wl_proxy_set_queue((struct wl_proxy *) wsi_wl_surface->surface,
                      wsi_wl_surface->display->queue);

   /* Bind wsi_wl_surface to dma-buf feedback. */
   if (wsi_wl_surface->display->wl_dmabuf &&
       zwp_linux_dmabuf_v1_get_version(wsi_wl_surface->display->wl_dmabuf) >=
       ZWP_LINUX_DMABUF_V1_GET_SURFACE_FEEDBACK_SINCE_VERSION) {
      result = wsi_wl_surface_bind_to_dmabuf_feedback(wsi_wl_surface);
      if (result != VK_SUCCESS)
         goto fail;

      wl_display_roundtrip_queue(wsi_wl_surface->display->wl_display,
                                 wsi_wl_surface->display->queue);
   }

   return VK_SUCCESS;

fail:
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateWaylandSurfaceKHR(VkInstance _instance,
                            const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   struct wsi_wl_surface *wsi_wl_surface;
   VkIcdSurfaceWayland *surface;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR);

   wsi_wl_surface = vk_zalloc2(&instance->alloc, pAllocator, sizeof *wsi_wl_surface,
                               8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (wsi_wl_surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface = &wsi_wl_surface->base;

   surface->base.platform = VK_ICD_WSI_PLATFORM_WAYLAND;
   surface->display = pCreateInfo->display;
   surface->surface = pCreateInfo->surface;

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base);

   return VK_SUCCESS;
}

static struct wsi_image *
wsi_wl_swapchain_get_wsi_image(struct wsi_swapchain *wsi_chain,
                               uint32_t image_index)
{
   struct wsi_wl_swapchain *chain = (struct wsi_wl_swapchain *)wsi_chain;
   return &chain->images[image_index].base;
}

static VkResult
wsi_wl_swapchain_acquire_next_image(struct wsi_swapchain *wsi_chain,
                                    const VkAcquireNextImageInfoKHR *info,
                                    uint32_t *image_index)
{
   struct wsi_wl_swapchain *chain = (struct wsi_wl_swapchain *)wsi_chain;
   struct wsi_wl_surface *wsi_wl_surface = chain->wsi_wl_surface;
   struct timespec start_time, end_time;
   struct timespec rel_timeout;
   int wl_fd = wl_display_get_fd(wsi_wl_surface->display->wl_display);

   timespec_from_nsec(&rel_timeout, info->timeout);

   clock_gettime(CLOCK_MONOTONIC, &start_time);
   timespec_add(&end_time, &rel_timeout, &start_time);

   while (1) {
      /* Try to dispatch potential events. */
      int ret = wl_display_dispatch_queue_pending(wsi_wl_surface->display->wl_display,
                                                  wsi_wl_surface->display->queue);
      if (ret < 0)
         return VK_ERROR_OUT_OF_DATE_KHR;

      /* Try to find a free image. */
      for (uint32_t i = 0; i < chain->base.image_count; i++) {
         if (!chain->images[i].busy) {
            /* We found a non-busy image */
            *image_index = i;
            chain->images[i].busy = true;
            return (chain->suboptimal ? VK_SUBOPTIMAL_KHR : VK_SUCCESS);
         }
      }

      /* Check for timeout. */
      struct timespec current_time;
      clock_gettime(CLOCK_MONOTONIC, &current_time);
      if (timespec_after(&current_time, &end_time))
         return VK_NOT_READY;

      /* Try to read events from the server. */
      ret = wl_display_prepare_read_queue(wsi_wl_surface->display->wl_display,
                                          wsi_wl_surface->display->queue);
      if (ret < 0) {
         /* Another thread might have read events for our queue already. Go
          * back to dispatch them.
          */
         if (errno == EAGAIN)
            continue;
         return VK_ERROR_OUT_OF_DATE_KHR;
      }

      struct pollfd pollfd = {
         .fd = wl_fd,
         .events = POLLIN
      };
      timespec_sub(&rel_timeout, &end_time, &current_time);
      ret = ppoll(&pollfd, 1, &rel_timeout, NULL);
      if (ret <= 0) {
         int lerrno = errno;
         wl_display_cancel_read(wsi_wl_surface->display->wl_display);
         if (ret < 0) {
            /* If ppoll() was interrupted, try again. */
            if (lerrno == EINTR || lerrno == EAGAIN)
               continue;
            return VK_ERROR_OUT_OF_DATE_KHR;
         }
         assert(ret == 0);
         continue;
      }

      ret = wl_display_read_events(wsi_wl_surface->display->wl_display);
      if (ret < 0)
         return VK_ERROR_OUT_OF_DATE_KHR;
   }
}

static void
frame_handle_done(void *data, struct wl_callback *callback, uint32_t serial)
{
   struct wsi_wl_swapchain *chain = data;

   chain->frame = NULL;
   chain->fifo_ready = true;

   wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {
   frame_handle_done,
};

static VkResult
wsi_wl_swapchain_queue_present(struct wsi_swapchain *wsi_chain,
                               uint32_t image_index,
                               const VkPresentRegionKHR *damage)
{
   struct wsi_wl_swapchain *chain = (struct wsi_wl_swapchain *)wsi_chain;
   struct wsi_wl_surface *wsi_wl_surface = chain->wsi_wl_surface;

   if (chain->buffer_type == WSI_WL_BUFFER_SHM_MEMCPY) {
      struct wsi_wl_image *image = &chain->images[image_index];
      memcpy(image->shm_ptr, image->base.cpu_map,
             image->base.row_pitches[0] * chain->extent.height);
   }
   if (chain->base.present_mode == VK_PRESENT_MODE_FIFO_KHR) {
      while (!chain->fifo_ready) {
         int ret = wl_display_dispatch_queue(wsi_wl_surface->display->wl_display,
                                             wsi_wl_surface->display->queue);
         if (ret < 0)
            return VK_ERROR_OUT_OF_DATE_KHR;
      }
   }

   assert(image_index < chain->base.image_count);
   wl_surface_attach(wsi_wl_surface->surface, chain->images[image_index].buffer, 0, 0);

   if (wl_surface_get_version(wsi_wl_surface->surface) >= 4 && damage &&
       damage->pRectangles && damage->rectangleCount > 0) {
      for (unsigned i = 0; i < damage->rectangleCount; i++) {
         const VkRectLayerKHR *rect = &damage->pRectangles[i];
         assert(rect->layer == 0);
         wl_surface_damage_buffer(wsi_wl_surface->surface,
                                  rect->offset.x, rect->offset.y,
                                  rect->extent.width, rect->extent.height);
      }
   } else {
      wl_surface_damage(wsi_wl_surface->surface, 0, 0, INT32_MAX, INT32_MAX);
   }

   if (chain->base.present_mode == VK_PRESENT_MODE_FIFO_KHR) {
      chain->frame = wl_surface_frame(wsi_wl_surface->surface);
      wl_callback_add_listener(chain->frame, &frame_listener, chain);
      chain->fifo_ready = false;
   }

   chain->images[image_index].busy = true;
   wl_surface_commit(wsi_wl_surface->surface);
   wl_display_flush(wsi_wl_surface->display->wl_display);

   return VK_SUCCESS;
}

static void
buffer_handle_release(void *data, struct wl_buffer *buffer)
{
   struct wsi_wl_image *image = data;

   assert(image->buffer == buffer);

   image->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
   buffer_handle_release,
};

static uint8_t *
wsi_wl_alloc_image_shm(struct wsi_image *imagew, unsigned size)
{
   struct wsi_wl_image *image = (struct wsi_wl_image *)imagew;

   /* Create a shareable buffer */
   int fd = os_create_anonymous_file(size, NULL);
   if (fd < 0)
      return NULL;

   void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (ptr == MAP_FAILED) {
      close(fd);
      return NULL;
   }

   image->shm_fd = fd;
   image->shm_ptr = ptr;
   image->shm_size = size;

   return ptr;
}

static VkResult
wsi_wl_image_init(struct wsi_wl_swapchain *chain,
                  struct wsi_wl_image *image,
                  const VkSwapchainCreateInfoKHR *pCreateInfo,
                  const VkAllocationCallbacks* pAllocator)
{
   struct wsi_wl_display *display = chain->wsi_wl_surface->display;
   VkResult result;

   result = wsi_create_image(&chain->base, &chain->base.image_info,
                             &image->base);
   if (result != VK_SUCCESS)
      return result;

   switch (chain->buffer_type) {
   case WSI_WL_BUFFER_GPU_SHM:
   case WSI_WL_BUFFER_SHM_MEMCPY: {
      if (chain->buffer_type == WSI_WL_BUFFER_SHM_MEMCPY) {
         wsi_wl_alloc_image_shm(&image->base, image->base.row_pitches[0] *
                                              chain->extent.height);
      }
      assert(image->shm_ptr != NULL);

      /* Share it in a wl_buffer */
      struct wl_shm_pool *pool = wl_shm_create_pool(display->wl_shm,
                                                    image->shm_fd,
                                                    image->shm_size);
      wl_proxy_set_queue((struct wl_proxy *)pool, display->queue);
      image->buffer = wl_shm_pool_create_buffer(pool, 0, chain->extent.width,
                                                chain->extent.height,
                                                image->base.row_pitches[0],
                                                chain->shm_format);
      wl_shm_pool_destroy(pool);
      break;
   }

   case WSI_WL_BUFFER_NATIVE: {
      assert(display->wl_dmabuf);

      struct zwp_linux_buffer_params_v1 *params =
         zwp_linux_dmabuf_v1_create_params(display->wl_dmabuf);
      if (!params)
         goto fail_image;

      for (int i = 0; i < image->base.num_planes; i++) {
         zwp_linux_buffer_params_v1_add(params,
                                        image->base.dma_buf_fd,
                                        i,
                                        image->base.offsets[i],
                                        image->base.row_pitches[i],
                                        image->base.drm_modifier >> 32,
                                        image->base.drm_modifier & 0xffffffff);
      }

      image->buffer =
         zwp_linux_buffer_params_v1_create_immed(params,
                                                 chain->extent.width,
                                                 chain->extent.height,
                                                 chain->drm_format,
                                                 0);
      zwp_linux_buffer_params_v1_destroy(params);
      break;
   }

   default:
      unreachable("Invalid buffer type");
   }

   if (!image->buffer)
      goto fail_image;

   wl_buffer_add_listener(image->buffer, &buffer_listener, image);

   return VK_SUCCESS;

fail_image:
   wsi_destroy_image(&chain->base, &image->base);

   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void
wsi_wl_swapchain_images_free(struct wsi_wl_swapchain *chain)
{
   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].buffer) {
         wl_buffer_destroy(chain->images[i].buffer);
         wsi_destroy_image(&chain->base, &chain->images[i].base);
         if (chain->images[i].shm_size) {
            close(chain->images[i].shm_fd);
            munmap(chain->images[i].shm_ptr, chain->images[i].shm_size);
         }
      }
   }
}

static void
wsi_wl_swapchain_chain_free(struct wsi_wl_swapchain *chain,
                            const VkAllocationCallbacks *pAllocator)
{
   if (chain->frame)
      wl_callback_destroy(chain->frame);
   if (chain->wsi_wl_surface)
      chain->wsi_wl_surface->chain = NULL;

   wsi_swapchain_finish(&chain->base);

   vk_free(pAllocator, chain);
}

static VkResult
wsi_wl_swapchain_destroy(struct wsi_swapchain *wsi_chain,
                         const VkAllocationCallbacks *pAllocator)
{
   struct wsi_wl_swapchain *chain = (struct wsi_wl_swapchain *)wsi_chain;

   wsi_wl_swapchain_images_free(chain);
   wsi_wl_swapchain_chain_free(chain, pAllocator);

   return VK_SUCCESS;
}

static VkResult
wsi_wl_surface_create_swapchain(VkIcdSurfaceBase *icd_surface,
                                VkDevice device,
                                struct wsi_device *wsi_device,
                                const VkSwapchainCreateInfoKHR* pCreateInfo,
                                const VkAllocationCallbacks* pAllocator,
                                struct wsi_swapchain **swapchain_out)
{
   struct wsi_wl_surface *wsi_wl_surface =
      wl_container_of((VkIcdSurfaceWayland *)icd_surface, wsi_wl_surface, base);
   struct wsi_wl_swapchain *chain;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   int num_images = pCreateInfo->minImageCount;

   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);
   chain = vk_zalloc(pAllocator, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   /* We are taking ownership of the wsi_wl_surface, so remove ownership from
    * oldSwapchain.
    *
    * If the surface is currently owned by a swapchain that is not
    * oldSwapchain we should return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR. There's
    * an open issue tracking that:
    *
    * https://gitlab.freedesktop.org/mesa/mesa/-/issues/7467
    */
   if (pCreateInfo->oldSwapchain) {
      VK_FROM_HANDLE(wsi_wl_swapchain, old_chain, pCreateInfo->oldSwapchain);
      old_chain->wsi_wl_surface = NULL;
   }

   /* Take ownership of the wsi_wl_surface */
   chain->wsi_wl_surface = wsi_wl_surface;
   wsi_wl_surface->chain = chain;

   result = wsi_wl_surface_init(wsi_wl_surface, wsi_device);
   if (result != VK_SUCCESS)
      goto fail;

   enum wsi_wl_buffer_type buffer_type;
   struct wsi_base_image_params *image_params = NULL;
   struct wsi_cpu_image_params cpu_image_params;
   struct wsi_drm_image_params drm_image_params;
   uint32_t num_drm_modifiers = 0;
   const uint64_t *drm_modifiers = NULL;
   if (wsi_device->sw) {
      cpu_image_params = (struct wsi_cpu_image_params) {
         .base.image_type = WSI_IMAGE_TYPE_CPU,
      };
      if (wsi_device->has_import_memory_host &&
          !(WSI_DEBUG & WSI_DEBUG_NOSHM)) {
         buffer_type = WSI_WL_BUFFER_GPU_SHM;
         cpu_image_params.alloc_shm = wsi_wl_alloc_image_shm;
      } else {
         buffer_type = WSI_WL_BUFFER_SHM_MEMCPY;
      }
      image_params = &cpu_image_params.base;
   } else {
      drm_image_params = (struct wsi_drm_image_params) {
         .base.image_type = WSI_IMAGE_TYPE_DRM,
         .same_gpu = true,
      };
      /* Use explicit DRM format modifiers when both the server and the driver
       * support them.
       */
      if (wsi_wl_surface->display->wl_dmabuf && wsi_device->supports_modifiers) {
         struct wsi_wl_format *f = NULL;
         /* Try to select modifiers for our vk_format from surface dma-buf
          * feedback. If that doesn't work, fallback to the list of supported
          * formats/modifiers by the display. */
         if (wsi_wl_surface->wl_dmabuf_feedback)
            f = pick_format_from_surface_dmabuf_feedback(wsi_wl_surface,
                                                         pCreateInfo->imageFormat);
         if (f == NULL)
            f = find_format(&chain->wsi_wl_surface->display->formats,
                            pCreateInfo->imageFormat);
         if (f != NULL) {
            num_drm_modifiers = u_vector_length(&f->modifiers);
            drm_modifiers = u_vector_tail(&f->modifiers);
            drm_image_params.num_modifier_lists = 1;
            drm_image_params.num_modifiers = &num_drm_modifiers;
            drm_image_params.modifiers = &drm_modifiers;
         }
      }
      buffer_type = WSI_WL_BUFFER_NATIVE;
      image_params = &drm_image_params.base;
   }

   result = wsi_swapchain_init(wsi_device, &chain->base, device,
                               pCreateInfo, image_params, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, chain);
      return result;
   }

   bool alpha = pCreateInfo->compositeAlpha ==
                      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;

   chain->base.destroy = wsi_wl_swapchain_destroy;
   chain->base.get_wsi_image = wsi_wl_swapchain_get_wsi_image;
   chain->base.acquire_next_image = wsi_wl_swapchain_acquire_next_image;
   chain->base.queue_present = wsi_wl_swapchain_queue_present;
   chain->base.present_mode = wsi_swapchain_get_present_mode(wsi_device, pCreateInfo);
   chain->base.image_count = num_images;
   chain->extent = pCreateInfo->imageExtent;
   chain->vk_format = pCreateInfo->imageFormat;
   chain->buffer_type = buffer_type;
   if (buffer_type == WSI_WL_BUFFER_NATIVE) {
      chain->drm_format = wl_drm_format_for_vk_format(chain->vk_format, alpha);
   } else {
      chain->shm_format = wl_shm_format_for_vk_format(chain->vk_format, alpha);
   }
   chain->num_drm_modifiers = num_drm_modifiers;
   chain->drm_modifiers = drm_modifiers;
   chain->fifo_ready = true;

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      result = wsi_wl_image_init(chain, &chain->images[i],
                                 pCreateInfo, pAllocator);
      if (result != VK_SUCCESS)
         goto fail_image_init;
      chain->images[i].busy = false;
   }

   *swapchain_out = &chain->base;

   return VK_SUCCESS;

fail_image_init:
   wsi_wl_swapchain_images_free(chain);

fail:
   wsi_wl_swapchain_chain_free(chain, pAllocator);

   return result;
}

VkResult
wsi_wl_init_wsi(struct wsi_device *wsi_device,
                const VkAllocationCallbacks *alloc,
                VkPhysicalDevice physical_device)
{
   struct wsi_wayland *wsi;
   VkResult result;

   wsi = vk_alloc(alloc, sizeof(*wsi), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   wsi->physical_device = physical_device;
   wsi->alloc = alloc;
   wsi->wsi = wsi_device;

   wsi->base.get_support = wsi_wl_surface_get_support;
   wsi->base.get_capabilities2 = wsi_wl_surface_get_capabilities2;
   wsi->base.get_formats = wsi_wl_surface_get_formats;
   wsi->base.get_formats2 = wsi_wl_surface_get_formats2;
   wsi->base.get_present_modes = wsi_wl_surface_get_present_modes;
   wsi->base.get_present_rectangles = wsi_wl_surface_get_present_rectangles;
   wsi->base.create_swapchain = wsi_wl_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_WAYLAND] = &wsi->base;

   return VK_SUCCESS;

fail:
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_WAYLAND] = NULL;

   return result;
}

void
wsi_wl_finish_wsi(struct wsi_device *wsi_device,
                  const VkAllocationCallbacks *alloc)
{
   struct wsi_wayland *wsi =
      (struct wsi_wayland *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_WAYLAND];
   if (!wsi)
      return;

   vk_free(alloc, wsi);
}
