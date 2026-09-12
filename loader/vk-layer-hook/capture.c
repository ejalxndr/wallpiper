/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ethan Alexander
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "wp_layer.h"

#include "config.h"
#include "logging.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ull
#endif

typedef struct {
  unsigned int sequence;
} wp_xcb_get_geometry_cookie_t;

typedef struct {
  uint8_t response_type;
  uint8_t depth;
  uint16_t sequence;
  uint32_t length;
  uint32_t root;
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
  uint16_t border_width;
  uint8_t pad0[2];
} wp_xcb_get_geometry_reply_t;

typedef void *(*wp_pfn_xcb_connect)(const char *, int *);
typedef int (*wp_pfn_xcb_connection_has_error)(void *);
typedef wp_xcb_get_geometry_cookie_t (*wp_pfn_xcb_get_geometry)(void *,
                                                                uint32_t);
typedef wp_xcb_get_geometry_reply_t *(*wp_pfn_xcb_get_geometry_reply)(
    void *, wp_xcb_get_geometry_cookie_t, void *);

static void *g_xcb_conn = NULL;
static pthread_once_t g_xcb_conn_once = PTHREAD_ONCE_INIT;

static void wp_xcb_connect_once(void) {
  wp_pfn_xcb_connect connect_fn =
      (wp_pfn_xcb_connect)dlsym(RTLD_DEFAULT, "xcb_connect");
  wp_pfn_xcb_connection_has_error has_error_fn =
      (wp_pfn_xcb_connection_has_error)dlsym(RTLD_DEFAULT,
                                             "xcb_connection_has_error");
  if (!connect_fn || !has_error_fn) {
    return;
  }
  void *conn = connect_fn(NULL, NULL);
  if (conn && has_error_fn(conn)) {
    conn = NULL;
  }
  g_xcb_conn = conn;
}

static bool wp_x11_window_geometry(unsigned long xid, int32_t *out_x,
                                   int32_t *out_y) {
  pthread_once(&g_xcb_conn_once, wp_xcb_connect_once);
  if (!g_xcb_conn) {
    return false;
  }

  wp_pfn_xcb_get_geometry get_geometry =
      (wp_pfn_xcb_get_geometry)dlsym(RTLD_DEFAULT, "xcb_get_geometry");
  wp_pfn_xcb_get_geometry_reply get_geometry_reply =
      (wp_pfn_xcb_get_geometry_reply)dlsym(RTLD_DEFAULT,
                                           "xcb_get_geometry_reply");
  if (!get_geometry || !get_geometry_reply) {
    return false;
  }

  wp_xcb_get_geometry_cookie_t cookie = get_geometry(g_xcb_conn, (uint32_t)xid);
  wp_xcb_get_geometry_reply_t *reply =
      get_geometry_reply(g_xcb_conn, cookie, NULL);
  if (!reply) {
    return false;
  }
  *out_x = reply->x;
  *out_y = reply->y;
  free(reply);
  return true;
}

static bool wp_force_linear_modifier(void) {
  const char *v = getenv("WALLPIPER_FORCE_LINEAR");
  return v && v[0] && v[0] != '0';
}

static uint32_t compute_candidate_modifiers(wp_device_data_t *dd,
                                            VkFormat format, uint64_t *out,
                                            uint32_t max_out) {
  VkFormatFeatureFlags required = VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                  VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

  VkDrmFormatModifierPropertiesListEXT modifier_list = {
      .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
  };
  VkFormatProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
                                .pNext = &modifier_list};
  if (!wp_global_instance_get_format_properties2(dd->physical_device, format,
                                                 &props2)) {
    return 0;
  }
  uint32_t count = modifier_list.drmFormatModifierCount;
  if (count == 0) {
    return 0;
  }

  VkDrmFormatModifierPropertiesEXT *modifier_props =
      calloc(count, sizeof(*modifier_props));
  if (!modifier_props) {
    return 0;
  }

  VkDrmFormatModifierPropertiesListEXT modifier_list2 = {
      .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
      .drmFormatModifierCount = count,
      .pDrmFormatModifierProperties = modifier_props,
  };
  VkFormatProperties2 props2b = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
                                 .pNext = &modifier_list2};
  wp_global_instance_get_format_properties2(dd->physical_device, format,
                                            &props2b);

  const bool force_linear = wp_force_linear_modifier();
  uint32_t n = 0;
  bool linear_usable = false;
  for (uint32_t i = 0; i < count && n < max_out; i++) {
    const VkDrmFormatModifierPropertiesEXT *m = &modifier_props[i];
    if (m->drmFormatModifierPlaneCount == 1 &&
        (m->drmFormatModifierTilingFeatures & required) == required) {
      out[n++] = m->drmFormatModifier;
      if (m->drmFormatModifier == DRM_FORMAT_MOD_LINEAR) {
        linear_usable = true;
      }
    }
  }
  free(modifier_props);

  if (force_linear && linear_usable) {
    out[0] = DRM_FORMAT_MOD_LINEAR;
    WP_LOG("WALLPIPER_FORCE_LINEAR set: restricting capture image to "
           "DRM_FORMAT_MOD_LINEAR");
    return 1;
  }
  if (force_linear) {
    WP_LOG("WALLPIPER_FORCE_LINEAR set but no usable linear modifier is "
           "advertised for this format; keeping %u tiled candidate(s)",
           n);
  }
  return n;
}

static bool create_capture_slot(wp_device_data_t *dd, VkFormat format,
                                VkExtent2D extent,
                                const uint64_t *candidate_modifiers,
                                uint32_t candidate_count,
                                uint32_t queue_family_index,
                                wp_capture_slot_t *out) {
  if (candidate_count == 0) {
    WP_LOG(
        "no usable single-plane DRM format modifier found for capture image");
    return false;
  }

  VkExternalMemoryImageCreateInfo export_image_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
  };
  VkImageDrmFormatModifierListCreateInfoEXT modifier_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
      .pNext = &export_image_info,
      .drmFormatModifierCount = candidate_count,
      .pDrmFormatModifiers = candidate_modifiers,
  };
  VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &modifier_create_info,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {extent.width, extent.height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  VkImage image;
  if (dd->create_image(dd->device, &image_info, NULL, &image) != VK_SUCCESS) {
    WP_LOG("create_image (capture target) failed");
    return false;
  }

  VkImageDrmFormatModifierPropertiesEXT chosen_modifier_props = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
  };
  if (dd->real_get_image_drm_format_modifier_properties_ext(
          dd->device, image, &chosen_modifier_props) != VK_SUCCESS) {
    WP_LOG("vkGetImageDrmFormatModifierPropertiesEXT failed");
    dd->destroy_image(dd->device, image, NULL);
    return false;
  }
  uint64_t modifier = chosen_modifier_props.drmFormatModifier;

  VkMemoryRequirements mem_req;
  dd->get_image_memory_requirements(dd->device, image, &mem_req);

  uint32_t mem_type_index;
  if (!wp_instance_find_memory_type(dd, mem_req.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                    &mem_type_index)) {
    WP_LOG("no suitable memory type found for capture image");
    dd->destroy_image(dd->device, image, NULL);
    return false;
  }

  VkExportMemoryAllocateInfo export_alloc_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
  };
  VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &export_alloc_info,
      .allocationSize = mem_req.size,
      .memoryTypeIndex = mem_type_index,
  };

  VkDeviceMemory memory;
  if (dd->allocate_memory(dd->device, &alloc_info, NULL, &memory) !=
      VK_SUCCESS) {
    WP_LOG("allocate_memory (export) failed");
    dd->destroy_image(dd->device, image, NULL);
    return false;
  }

  if (dd->bind_image_memory(dd->device, image, memory, 0) != VK_SUCCESS) {
    WP_LOG("bind_image_memory failed");
    dd->destroy_image(dd->device, image, NULL);
    dd->free_memory(dd->device, memory, NULL);
    return false;
  }

  VkImageSubresource subresource = {
      .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
      .mipLevel = 0,
      .arrayLayer = 0,
  };
  VkSubresourceLayout layout;
  dd->get_image_subresource_layout(dd->device, image, &subresource, &layout);
  uint32_t stride = (uint32_t)layout.rowPitch;

  VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = queue_family_index,
  };
  VkCommandPool pool;
  if (dd->create_command_pool(dd->device, &pool_info, NULL, &pool) !=
      VK_SUCCESS) {
    WP_LOG("create_command_pool failed");
    dd->destroy_image(dd->device, image, NULL);
    dd->free_memory(dd->device, memory, NULL);
    return false;
  }

  VkCommandBufferAllocateInfo cmd_alloc_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  VkCommandBuffer cmd;
  if (dd->allocate_command_buffers(dd->device, &cmd_alloc_info, &cmd) !=
      VK_SUCCESS) {
    WP_LOG("allocate_command_buffers failed");
    dd->destroy_command_pool(dd->device, pool, NULL);
    dd->destroy_image(dd->device, image, NULL);
    dd->free_memory(dd->device, memory, NULL);
    return false;
  }

  VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  if (dd->create_fence(dd->device, &fence_info, NULL, &fence) != VK_SUCCESS) {
    WP_LOG("create_fence failed");
    dd->destroy_command_pool(dd->device, pool, NULL);
    dd->destroy_image(dd->device, image, NULL);
    dd->free_memory(dd->device, memory, NULL);
    return false;
  }

  VkExportSemaphoreCreateInfo export_semaphore_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
  };
  VkSemaphoreCreateInfo semaphore_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &export_semaphore_info,
  };
  VkSemaphore ready_semaphore;
  if (dd->create_semaphore(dd->device, &semaphore_info, NULL,
                           &ready_semaphore) != VK_SUCCESS) {
    WP_LOG("create_semaphore (export) failed");
    dd->destroy_fence(dd->device, fence, NULL);
    dd->destroy_command_pool(dd->device, pool, NULL);
    dd->destroy_image(dd->device, image, NULL);
    dd->free_memory(dd->device, memory, NULL);
    return false;
  }

  WP_LOG("capture slot created: image=%llu memory=%llu %ux%u format=%d "
         "stride=%u modifier=%llu queue_family=%u",
         (unsigned long long)image, (unsigned long long)memory, extent.width,
         extent.height, (int)format, stride, (unsigned long long)modifier,
         queue_family_index);

  memset(out, 0, sizeof(*out));
  out->in_use = true;
  out->image = image;
  out->memory = memory;
  out->pool = pool;
  out->cmd = cmd;
  out->fence = fence;
  out->fence_pending = false;
  out->ready_semaphore = ready_semaphore;
  out->ownership = WP_SLOT_NEVER_WRITTEN;
  out->buf_sent = false;
  out->stride = stride;
  out->modifier = modifier;
  return true;
}

static void destroy_capture_slot(wp_device_data_t *dd,
                                 wp_capture_slot_t *slot) {
  if (slot->fence_pending) {
    dd->wait_for_fences(dd->device, 1, &slot->fence, VK_TRUE,
                        WP_SLOT_FENCE_TIMEOUT_NS);
  }
  dd->destroy_semaphore(dd->device, slot->ready_semaphore, NULL);
  dd->destroy_fence(dd->device, slot->fence, NULL);
  dd->destroy_command_pool(dd->device, slot->pool, NULL);
  dd->destroy_image(dd->device, slot->image, NULL);
  dd->free_memory(dd->device, slot->memory, NULL);
  slot->in_use = false;
}

static bool record_and_submit_copy(wp_device_data_t *dd, VkQueue queue,
                                   uint32_t queue_family_index,
                                   wp_capture_slot_t *slot, VkImage src_image,
                                   VkExtent2D extent) {
  VkCommandBuffer cmd = slot->cmd;
  VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  if (dd->begin_command_buffer(cmd, &begin_info) != VK_SUCCESS) {
    WP_LOG("begin_command_buffer failed");
    return false;
  }

  VkImageSubresourceRange subresource = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0,
      .levelCount = 1,
      .baseArrayLayer = 0,
      .layerCount = 1,
  };

  VkImageMemoryBarrier to_transfer_src = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = src_image,
      .subresourceRange = subresource,
      .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
  };

  VkImageMemoryBarrier to_transfer_dst;
  if (slot->ownership == WP_SLOT_NEVER_WRITTEN) {
    to_transfer_dst = (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = slot->image,
        .subresourceRange = subresource,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    };
  } else {
    to_transfer_dst = (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = queue_family_index,
        .image = slot->image,
        .subresourceRange = subresource,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    };
  }

  VkImageMemoryBarrier pre_barriers[2] = {to_transfer_src, to_transfer_dst};
  dd->cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                           2, pre_barriers);

  VkImageSubresourceLayers copy_subresource = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .mipLevel = 0,
      .baseArrayLayer = 0,
      .layerCount = 1,
  };
  VkImageCopy region = {
      .srcSubresource = copy_subresource,
      .srcOffset = {0, 0, 0},
      .dstSubresource = copy_subresource,
      .dstOffset = {0, 0, 0},
      .extent = {extent.width, extent.height, 1},
  };
  dd->cmd_copy_image(cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     slot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                     &region);

  VkImageMemoryBarrier back_to_present = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = src_image,
      .subresourceRange = subresource,
      .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
  };
  VkImageMemoryBarrier release_to_foreign = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = queue_family_index,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
      .image = slot->image,
      .subresourceRange = subresource,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = 0,
  };
  VkImageMemoryBarrier post_barriers[2] = {back_to_present, release_to_foreign};
  dd->cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                           NULL, 2, post_barriers);

  if (dd->end_command_buffer(cmd) != VK_SUCCESS) {
    WP_LOG("end_command_buffer failed");
    return false;
  }

  VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &slot->ready_semaphore,
  };
  if (dd->queue_submit(queue, 1, &submit_info, slot->fence) != VK_SUCCESS) {
    WP_LOG("queue_submit failed");
    return false;
  }

  return true;
}

static void get_swapchain_images(wp_device_data_t *dd, VkSwapchainKHR swapchain,
                                 VkImage *out, uint32_t max_out,
                                 uint32_t *out_count) {
  *out_count = 0;
  uint32_t count = 0;
  VkResult res =
      dd->real_get_swapchain_images_khr(dd->device, swapchain, &count, NULL);
  if (res != VK_SUCCESS || count == 0) {
    WP_LOG("get_swapchain_images (count) failed");
    return;
  }
  if (count > max_out) {
    WP_LOG("get_swapchain_images: %u images exceeds capture capacity of %u, "
           "truncating",
           count, max_out);
    count = max_out;
  }
  res = dd->real_get_swapchain_images_khr(dd->device, swapchain, &count, out);
  if (res != VK_SUCCESS) {
    WP_LOG("get_swapchain_images (fetch) failed");
    return;
  }
  *out_count = count;
}

static wp_swapchain_state_t *find_swapchain_locked(wp_device_data_t *dd,
                                                   VkSwapchainKHR swapchain) {
  for (size_t i = 0; i < WP_MAX_SWAPCHAINS; i++) {
    if (dd->swapchains[i].in_use && dd->swapchains[i].swapchain == swapchain) {
      return &dd->swapchains[i];
    }
  }
  return NULL;
}

static pthread_mutex_t g_channel_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_channel_in_use[WP_MAX_CAPTURE_CHANNELS];

static bool alloc_wire_channel(uint32_t *out) {
  pthread_mutex_lock(&g_channel_mutex);
  for (uint32_t i = 0; i < WP_MAX_CAPTURE_CHANNELS; i++) {
    if (!g_channel_in_use[i]) {
      g_channel_in_use[i] = true;
      *out = i;
      pthread_mutex_unlock(&g_channel_mutex);
      return true;
    }
  }
  pthread_mutex_unlock(&g_channel_mutex);
  return false;
}

static void free_wire_channel(uint32_t channel) {
  if (channel < WP_MAX_CAPTURE_CHANNELS) {
    pthread_mutex_lock(&g_channel_mutex);
    g_channel_in_use[channel] = false;
    pthread_mutex_unlock(&g_channel_mutex);
  }
}

static pthread_mutex_t g_channel_geom_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_channel_has_geometry[WP_MAX_CAPTURE_CHANNELS];
static int32_t g_channel_geom_x[WP_MAX_CAPTURE_CHANNELS];
static int32_t g_channel_geom_y[WP_MAX_CAPTURE_CHANNELS];

static void set_channel_geometry(uint32_t channel, bool has_geometry,
                                 int32_t geom_x, int32_t geom_y) {
  if (channel >= WP_MAX_CAPTURE_CHANNELS) {
    return;
  }
  pthread_mutex_lock(&g_channel_geom_mutex);
  g_channel_has_geometry[channel] = has_geometry;
  g_channel_geom_x[channel] = geom_x;
  g_channel_geom_y[channel] = geom_y;
  pthread_mutex_unlock(&g_channel_geom_mutex);
}

static bool get_channel_geometry(uint32_t channel, int32_t *out_x,
                                 int32_t *out_y) {
  if (channel >= WP_MAX_CAPTURE_CHANNELS) {
    return false;
  }
  pthread_mutex_lock(&g_channel_geom_mutex);
  bool has_geometry = g_channel_has_geometry[channel];
  *out_x = g_channel_geom_x[channel];
  *out_y = g_channel_geom_y[channel];
  pthread_mutex_unlock(&g_channel_geom_mutex);
  return has_geometry;
}

void wp_register_swapchain(wp_device_data_t *dd, VkSwapchainKHR swapchain,
                           const VkSwapchainCreateInfoKHR *create_info) {
  VkImage images[16];
  uint32_t image_count;
  get_swapchain_images(dd, swapchain, images, 16, &image_count);

  uint64_t candidate_modifiers[32];
  uint32_t candidate_count = compute_candidate_modifiers(
      dd, create_info->imageFormat, candidate_modifiers, 32);

  WP_LOG("create_swapchain_khr -> %llu, format=%d, extent=%ux%u, %u images, %u "
         "candidate modifiers",
         (unsigned long long)swapchain, (int)create_info->imageFormat,
         create_info->imageExtent.width, create_info->imageExtent.height,
         image_count, candidate_count);

  unsigned long xid;
  int32_t geom_x = 0, geom_y = 0;
  bool has_geometry = false;
  if (wp_surface_xid_lookup(create_info->surface, &xid)) {
    has_geometry = wp_x11_window_geometry(xid, &geom_x, &geom_y);
  }

  pthread_mutex_lock(&dd->swapchains_mutex);
  wp_swapchain_state_t *slot = NULL;
  for (size_t i = 0; i < WP_MAX_SWAPCHAINS; i++) {
    if (!dd->swapchains[i].in_use) {
      slot = &dd->swapchains[i];
      break;
    }
  }
  if (!slot) {
    pthread_mutex_unlock(&dd->swapchains_mutex);
    WP_LOG("register_swapchain: swapchain table full, capture disabled for "
           "this swapchain");
    return;
  }

  uint32_t wire_channel;
  if (!alloc_wire_channel(&wire_channel)) {
    pthread_mutex_unlock(&dd->swapchains_mutex);
    WP_LOG("register_swapchain: no free wire channel (max %d), capture "
           "disabled for this swapchain",
           WP_MAX_CAPTURE_CHANNELS);
    return;
  }

  memset(slot, 0, sizeof(*slot));
  slot->in_use = true;
  slot->swapchain = swapchain;
  slot->format = create_info->imageFormat;
  slot->extent = create_info->imageExtent;
  memcpy(slot->images, images, image_count * sizeof(VkImage));
  slot->image_count = image_count;
  memcpy(slot->candidate_modifiers, candidate_modifiers,
         candidate_count * sizeof(uint64_t));
  slot->candidate_modifier_count = candidate_count;
  slot->next_slot = 0;
  slot->present_count = 0;
  slot->has_last_present_at = false;
  slot->wire_channel = wire_channel;
  pthread_mutex_unlock(&dd->swapchains_mutex);

  set_channel_geometry(wire_channel, has_geometry, geom_x, geom_y);

  if (has_geometry) {
    WP_LOG("register_swapchain: swapchain=%llu assigned wire_channel=%u "
           "geometry=(%d,%d)",
           (unsigned long long)swapchain, wire_channel, geom_x, geom_y);
  } else {
    WP_LOG("register_swapchain: swapchain=%llu assigned wire_channel=%u "
           "(no window geometry available)",
           (unsigned long long)swapchain, wire_channel);
  }
}

void wp_teardown_swapchain(wp_device_data_t *dd, VkSwapchainKHR swapchain) {
  pthread_mutex_lock(&dd->swapchains_mutex);
  wp_swapchain_state_t *state = find_swapchain_locked(dd, swapchain);
  if (!state) {
    pthread_mutex_unlock(&dd->swapchains_mutex);
    return;
  }

  wp_capture_slot_t slots_copy[WP_CAPTURE_SLOT_COUNT];
  memcpy(slots_copy, state->slots, sizeof(slots_copy));
  state->in_use = false;
  free_wire_channel(state->wire_channel);
  pthread_mutex_unlock(&dd->swapchains_mutex);

  WP_LOG("destroy_swapchain_khr: tearing down capture slot(s) for %llu",
         (unsigned long long)swapchain);
  for (size_t i = 0; i < WP_CAPTURE_SLOT_COUNT; i++) {
    if (slots_copy[i].in_use) {
      destroy_capture_slot(dd, &slots_copy[i]);
    }
  }
}

void wp_capture_and_notify(wp_device_data_t *dd, VkQueue queue,
                           VkSwapchainKHR swapchain, uint32_t image_index) {
  uint32_t queue_family_index;
  if (!wp_queue_family_for(dd, queue, &queue_family_index)) {
    WP_LOG("capture: unknown queue family for presenting queue, skipping "
           "capture this frame");
    return;
  }

  pthread_mutex_lock(&dd->swapchains_mutex);
  wp_swapchain_state_t *state = find_swapchain_locked(dd, swapchain);
  if (!state || image_index >= state->image_count) {
    pthread_mutex_unlock(&dd->swapchains_mutex);
    return;
  }
  VkImage src_image = state->images[image_index];

  state->present_count++;
  uint64_t psn = state->present_count;
  bool verbose = wp_capture_should_sample(psn);

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  bool have_delta = state->has_last_present_at;
  double delta_ms = 0.0;
  if (have_delta) {
    delta_ms = (double)(now.tv_sec - state->last_present_at.tv_sec) * 1000.0 +
               (double)(now.tv_nsec - state->last_present_at.tv_nsec) / 1e6;
  }
  state->last_present_at = now;
  state->has_last_present_at = true;

  size_t slot_idx = state->next_slot;
  state->next_slot = (slot_idx + 1) % WP_CAPTURE_SLOT_COUNT;

  if (!state->slots[slot_idx].in_use) {
    VkFormat format = state->format;
    VkExtent2D extent = state->extent;
    uint64_t candidate_modifiers[32];
    uint32_t candidate_count = state->candidate_modifier_count;
    memcpy(candidate_modifiers, state->candidate_modifiers,
           candidate_count * sizeof(uint64_t));

    wp_capture_slot_t new_slot;
    if (!create_capture_slot(dd, format, extent, candidate_modifiers,
                             candidate_count, queue_family_index, &new_slot)) {
      pthread_mutex_unlock(&dd->swapchains_mutex);
      WP_LOG("capture: failed to create capture slot, skipping capture this "
             "frame");
      return;
    }
    state->slots[slot_idx] = new_slot;
  }

  wp_capture_slot_t *slot = &state->slots[slot_idx];

  if (slot->fence_pending) {
    VkResult status = dd->get_fence_status(dd->device, slot->fence);
    if (status == VK_SUCCESS) {
      slot->fence_pending = false;
    } else if (status == VK_NOT_READY) {
      VkResult wait_res = dd->wait_for_fences(
          dd->device, 1, &slot->fence, VK_TRUE, WP_SLOT_FENCE_TIMEOUT_NS);
      if (wait_res == VK_SUCCESS) {
        slot->fence_pending = false;
      } else {
        pthread_mutex_unlock(&dd->swapchains_mutex);
        WP_LOG("capture: slot %zu still busy after %lluns, skipping capture "
               "this frame",
               slot_idx, (unsigned long long)WP_SLOT_FENCE_TIMEOUT_NS);
        return;
      }
    } else {
      pthread_mutex_unlock(&dd->swapchains_mutex);
      WP_LOG("capture: get_fence_status failed, skipping capture this frame");
      return;
    }
  }

  if (dd->reset_fences(dd->device, 1, &slot->fence) != VK_SUCCESS) {
    pthread_mutex_unlock(&dd->swapchains_mutex);
    WP_LOG("reset_fences failed");
    return;
  }
  if (dd->reset_command_buffer(slot->cmd, 0) != VK_SUCCESS) {
    pthread_mutex_unlock(&dd->swapchains_mutex);
    WP_LOG("reset_command_buffer failed");
    return;
  }

  bool ok = record_and_submit_copy(dd, queue, queue_family_index, slot,
                                   src_image, state->extent);
  if (verbose) {
    if (have_delta) {
      WP_LOG("swapchain=%llu present #%llu: slot=%zu copy submitted -> %d "
             "since_last_present=%.2fms",
             (unsigned long long)swapchain, (unsigned long long)psn, slot_idx,
             ok, delta_ms);
    } else {
      WP_LOG("swapchain=%llu present #%llu: slot=%zu copy submitted -> %d "
             "since_last_present=n/a",
             (unsigned long long)swapchain, (unsigned long long)psn, slot_idx,
             ok);
    }
  }
  if (!ok) {
    pthread_mutex_unlock(&dd->swapchains_mutex);
    return;
  }

  slot->ownership = WP_SLOT_RELEASED_TO_FOREIGN;
  slot->fence_pending = true;
  bool need_buf_msg = !slot->buf_sent;
  uint32_t stride = slot->stride;
  uint64_t modifier = slot->modifier;
  VkDeviceMemory memory = slot->memory;
  VkSemaphore ready_semaphore = slot->ready_semaphore;
  VkFormat format = state->format;
  VkExtent2D extent = state->extent;
  uint32_t wire_channel = state->wire_channel;
  uint32_t wire_slot =
      wire_channel * WP_CAPTURE_SLOT_COUNT + (uint32_t)slot_idx;

  pthread_mutex_unlock(&dd->swapchains_mutex);

  int32_t geom_x = 0, geom_y = 0;
  bool has_geometry = get_channel_geometry(wire_channel, &geom_x, &geom_y);

  VkSemaphoreGetFdInfoKHR sync_get_fd_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
      .semaphore = ready_semaphore,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
  };
  int sync_fd = -1;
  VkResult sync_res =
      dd->real_get_semaphore_fd_khr(dd->device, &sync_get_fd_info, &sync_fd);
  if (sync_res != VK_SUCCESS) {
    WP_LOG("vkGetSemaphoreFdKHR failed, consumer will sample unsynchronized");
    sync_fd = -1;
  }

  if (need_buf_msg) {
    VkMemoryGetFdInfoKHR get_fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    int fd = -1;
    VkResult res = dd->real_get_memory_fd_khr(dd->device, &get_fd_info, &fd);
    if (res != VK_SUCCESS) {
      WP_LOG("vkGetMemoryFdKHR failed");
      if (sync_fd >= 0) {
        close(sync_fd);
      }
      return;
    }
    bool sent = wp_capture_link_send_buf(
        dd->capture_link, wire_slot, extent.width, extent.height,
        (uint32_t)format, stride, modifier, has_geometry, geom_x, geom_y, fd,
        sync_fd);
    if (sent) {
      pthread_mutex_lock(&dd->swapchains_mutex);
      wp_swapchain_state_t *state2 = find_swapchain_locked(dd, swapchain);
      if (state2 && state2->slots[slot_idx].in_use) {
        state2->slots[slot_idx].buf_sent = true;
      }
      pthread_mutex_unlock(&dd->swapchains_mutex);
    }
  } else {
    wp_capture_link_send_frame(dd->capture_link, wire_slot, sync_fd);
  }
}
