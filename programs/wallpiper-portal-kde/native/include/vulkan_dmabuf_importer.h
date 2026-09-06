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

#pragma once

#include <QQuickWindow>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>

QT_FORWARD_DECLARE_CLASS(QSGTexture)

namespace WallpiperKde {

class VulkanDmabufImporter {
public:
  struct Import {
    VkImage importedImage = VK_NULL_HANDLE;
    VkDeviceMemory importedMemory = VK_NULL_HANDLE;
    VkImage shadowImage = VK_NULL_HANDLE;
    VkDeviceMemory shadowMemory = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool shadowHasContent = false;
  };

  ~VulkanDmabufImporter();

  bool ensureBound(QQuickWindow *window);
  bool isBound() const;

  std::optional<Import> importDmabuf(int width, int height, uint32_t stride,
                                     uint64_t modifier, VkFormat format,
                                     int fd);

  bool blitFrame(Import &import, int syncFd);

  void destroyImport(Import &import) const;

  QSGTexture *wrapTexture2D(QQuickWindow *window, const Import &import,
                            const QSize &size) const;

  bool queryRenderNode(uint32_t *major, uint32_t *minor) const;

private:
  bool resolveFunctions();
  bool createCommandObjects();
  void destroyCommandObjects();
  bool createShadowImage(uint32_t width, uint32_t height, VkFormat format,
                         VkImage *outImage, VkDeviceMemory *outMemory) const;

  VkInstance m_instance = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkDevice m_device = VK_NULL_HANDLE;
  VkQueue m_queue = VK_NULL_HANDLE;
  uint32_t m_queueFamilyIndex = 0;

  PFN_vkGetInstanceProcAddr m_vkGetInstanceProcAddr = nullptr;
  PFN_vkGetDeviceProcAddr m_vkGetDeviceProcAddr = nullptr;

  PFN_vkCreateImage m_vkCreateImage = nullptr;
  PFN_vkDestroyImage m_vkDestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements m_vkGetImageMemoryRequirements = nullptr;
  PFN_vkAllocateMemory m_vkAllocateMemory = nullptr;
  PFN_vkFreeMemory m_vkFreeMemory = nullptr;
  PFN_vkBindImageMemory m_vkBindImageMemory = nullptr;
  PFN_vkCreateSemaphore m_vkCreateSemaphore = nullptr;
  PFN_vkDestroySemaphore m_vkDestroySemaphore = nullptr;
  PFN_vkCreateFence m_vkCreateFence = nullptr;
  PFN_vkDestroyFence m_vkDestroyFence = nullptr;
  PFN_vkWaitForFences m_vkWaitForFences = nullptr;
  PFN_vkResetFences m_vkResetFences = nullptr;
  PFN_vkCreateCommandPool m_vkCreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool m_vkDestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers m_vkAllocateCommandBuffers = nullptr;
  PFN_vkResetCommandPool m_vkResetCommandPool = nullptr;
  PFN_vkBeginCommandBuffer m_vkBeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer m_vkEndCommandBuffer = nullptr;
  PFN_vkCmdPipelineBarrier m_vkCmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyImage m_vkCmdCopyImage = nullptr;
  PFN_vkQueueSubmit m_vkQueueSubmit = nullptr;
  PFN_vkGetMemoryFdPropertiesKHR m_vkGetMemoryFdPropertiesKHR = nullptr;
  PFN_vkImportSemaphoreFdKHR m_vkImportSemaphoreFdKHR = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties
      m_vkGetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkGetPhysicalDeviceProperties2 m_vkGetPhysicalDeviceProperties2 = nullptr;

  VkCommandPool m_commandPool = VK_NULL_HANDLE;
  VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
  VkFence m_fence = VK_NULL_HANDLE;
  VkSemaphore m_acquireSemaphore = VK_NULL_HANDLE;
  bool m_fenceArmed = false;

  bool m_bound = false;
  bool m_loggedUnsupported = false;
};

} // namespace WallpiperKde
