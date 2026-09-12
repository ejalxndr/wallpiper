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

#include "vulkan_dmabuf_importer.h"

#include <QDebug>
#include <QSGRendererInterface>
#include <QVulkanInstance>
#include <qsgtexture_platform.h>

#include <unistd.h>

#ifndef VK_EXT_physical_device_drm
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT                   \
  ((VkStructureType)1000353000)
typedef struct VkPhysicalDeviceDrmPropertiesEXT {
  VkStructureType sType;
  void *pNext;
  VkBool32 hasPrimary;
  VkBool32 hasRender;
  int64_t primaryMajor;
  int64_t primaryMinor;
  int64_t renderMajor;
  int64_t renderMinor;
} VkPhysicalDeviceDrmPropertiesEXT;
#endif

namespace WallpiperKde {

namespace {
constexpr uint64_t kFenceWaitTimeoutNs = 2'000'000'000ull;
} // namespace

VulkanDmabufImporter::~VulkanDmabufImporter() {
  if (m_bound) {
    destroyCommandObjects();
  }
}

bool VulkanDmabufImporter::isBound() const { return m_bound; }

bool VulkanDmabufImporter::ensureBound(QQuickWindow *window) {
  if (m_bound) {
    return true;
  }
  if (!window) {
    return false;
  }

  auto *rif = window->rendererInterface();
  if (!rif || rif->graphicsApi() != QSGRendererInterface::Vulkan) {
    if (!m_loggedUnsupported) {
      qWarning() << "[vulkan] Qt Quick scenegraph is not on the Vulkan RHI "
                    "backend (graphicsApi ="
                 << (rif ? rif->graphicsApi() : QSGRendererInterface::Unknown)
                 << ")";
      m_loggedUnsupported = true;
    }
    return false;
  }

  QVulkanInstance *qvkInst = window->vulkanInstance();
  if (!qvkInst || !qvkInst->isValid()) {
    qWarning() << "[vulkan] no valid QVulkanInstance on window";
    return false;
  }

  auto *pInstance = static_cast<VkInstance *>(
      rif->getResource(window, QSGRendererInterface::VulkanInstanceResource));
  auto *pPhysicalDevice = static_cast<VkPhysicalDevice *>(
      rif->getResource(window, QSGRendererInterface::PhysicalDeviceResource));
  auto *pDevice = static_cast<VkDevice *>(
      rif->getResource(window, QSGRendererInterface::DeviceResource));
  auto *pQueueFamilyIndex = static_cast<uint32_t *>(rif->getResource(
      window, QSGRendererInterface::GraphicsQueueFamilyIndexResource));
  auto *pQueue = static_cast<VkQueue *>(
      rif->getResource(window, QSGRendererInterface::CommandQueueResource));

  if (!pInstance || !pPhysicalDevice || !pDevice || !pQueue) {
    qWarning() << "[vulkan] Vulkan API but missing Qt RHI resources"
               << "(instance=" << static_cast<void *>(pInstance)
               << "physicalDevice=" << static_cast<void *>(pPhysicalDevice)
               << "device=" << static_cast<void *>(pDevice)
               << "queue=" << static_cast<void *>(pQueue) << ")";
    return false;
  }

  m_instance = *pInstance;
  m_physicalDevice = *pPhysicalDevice;
  m_device = *pDevice;
  m_queueFamilyIndex = pQueueFamilyIndex ? *pQueueFamilyIndex : 0;
  m_queue = *pQueue;

  m_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      qvkInst->getInstanceProcAddr("vkGetInstanceProcAddr"));
  if (!m_vkGetInstanceProcAddr) {
    qWarning() << "[vulkan] failed to resolve vkGetInstanceProcAddr";
    return false;
  }

  if (!resolveFunctions()) {
    return false;
  }
  if (!createCommandObjects()) {
    return false;
  }

  m_bound = true;
  qInfo() << "[vulkan] bound Vulkan dmabuf importer, device"
          << static_cast<void *>(m_device) << "queue family"
          << m_queueFamilyIndex;
  return true;
}

bool VulkanDmabufImporter::resolveFunctions() {
  PFN_vkVoidFunction gdpaRaw =
      m_vkGetInstanceProcAddr(m_instance, "vkGetDeviceProcAddr");
  if (!gdpaRaw) {
    qWarning() << "[vulkan] failed to resolve vkGetDeviceProcAddr";
    return false;
  }
  m_vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gdpaRaw);

#define WP_RESOLVE_DEV(SLOT, NAME)                                             \
  do {                                                                         \
    m_##SLOT = reinterpret_cast<decltype(m_##SLOT)>(                           \
        m_vkGetDeviceProcAddr(m_device, NAME));                                \
    if (!m_##SLOT) {                                                           \
      qWarning() << "[vulkan] failed to resolve" << NAME;                      \
      return false;                                                            \
    }                                                                          \
  } while (0)

  WP_RESOLVE_DEV(vkCreateImage, "vkCreateImage");
  WP_RESOLVE_DEV(vkDestroyImage, "vkDestroyImage");
  WP_RESOLVE_DEV(vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements");
  WP_RESOLVE_DEV(vkAllocateMemory, "vkAllocateMemory");
  WP_RESOLVE_DEV(vkFreeMemory, "vkFreeMemory");
  WP_RESOLVE_DEV(vkBindImageMemory, "vkBindImageMemory");
  WP_RESOLVE_DEV(vkCreateSemaphore, "vkCreateSemaphore");
  WP_RESOLVE_DEV(vkDestroySemaphore, "vkDestroySemaphore");
  WP_RESOLVE_DEV(vkCreateFence, "vkCreateFence");
  WP_RESOLVE_DEV(vkDestroyFence, "vkDestroyFence");
  WP_RESOLVE_DEV(vkWaitForFences, "vkWaitForFences");
  WP_RESOLVE_DEV(vkResetFences, "vkResetFences");
  WP_RESOLVE_DEV(vkCreateCommandPool, "vkCreateCommandPool");
  WP_RESOLVE_DEV(vkDestroyCommandPool, "vkDestroyCommandPool");
  WP_RESOLVE_DEV(vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
  WP_RESOLVE_DEV(vkResetCommandPool, "vkResetCommandPool");
  WP_RESOLVE_DEV(vkBeginCommandBuffer, "vkBeginCommandBuffer");
  WP_RESOLVE_DEV(vkEndCommandBuffer, "vkEndCommandBuffer");
  WP_RESOLVE_DEV(vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
  WP_RESOLVE_DEV(vkCmdCopyImage, "vkCmdCopyImage");
  WP_RESOLVE_DEV(vkQueueSubmit, "vkQueueSubmit");
  WP_RESOLVE_DEV(vkGetMemoryFdPropertiesKHR, "vkGetMemoryFdPropertiesKHR");
  WP_RESOLVE_DEV(vkImportSemaphoreFdKHR, "vkImportSemaphoreFdKHR");

#undef WP_RESOLVE_DEV

  m_vkGetPhysicalDeviceMemoryProperties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          m_vkGetInstanceProcAddr(m_instance,
                                  "vkGetPhysicalDeviceMemoryProperties"));
  if (!m_vkGetPhysicalDeviceMemoryProperties) {
    qWarning() << "[vulkan] failed to resolve "
                  "vkGetPhysicalDeviceMemoryProperties";
    return false;
  }

  m_vkGetPhysicalDeviceProperties2 =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
          m_vkGetInstanceProcAddr(m_instance,
                                  "vkGetPhysicalDeviceProperties2"));

  return true;
}

bool VulkanDmabufImporter::createCommandObjects() {
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.queueFamilyIndex = m_queueFamilyIndex;
  if (m_vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) !=
      VK_SUCCESS) {
    qWarning() << "[vulkan] vkCreateCommandPool failed";
    return false;
  }

  VkCommandBufferAllocateInfo cbInfo{};
  cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbInfo.commandPool = m_commandPool;
  cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbInfo.commandBufferCount = 1;
  if (m_vkAllocateCommandBuffers(m_device, &cbInfo, &m_commandBuffer) !=
      VK_SUCCESS) {
    qWarning() << "[vulkan] vkAllocateCommandBuffers failed";
    return false;
  }

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (m_vkCreateFence(m_device, &fenceInfo, nullptr, &m_fence) != VK_SUCCESS) {
    qWarning() << "[vulkan] vkCreateFence failed";
    return false;
  }

  VkSemaphoreCreateInfo semInfo{};
  semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  if (m_vkCreateSemaphore(m_device, &semInfo, nullptr, &m_acquireSemaphore) !=
      VK_SUCCESS) {
    qWarning() << "[vulkan] vkCreateSemaphore failed";
    return false;
  }

  return true;
}

void VulkanDmabufImporter::destroyCommandObjects() {
  if (m_fenceArmed) {
    m_vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
    m_fenceArmed = false;
  }
  if (m_acquireSemaphore != VK_NULL_HANDLE) {
    m_vkDestroySemaphore(m_device, m_acquireSemaphore, nullptr);
    m_acquireSemaphore = VK_NULL_HANDLE;
  }
  if (m_fence != VK_NULL_HANDLE) {
    m_vkDestroyFence(m_device, m_fence, nullptr);
    m_fence = VK_NULL_HANDLE;
  }
  if (m_commandPool != VK_NULL_HANDLE) {
    m_vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    m_commandPool = VK_NULL_HANDLE;
  }
  m_commandBuffer = VK_NULL_HANDLE;
}

bool VulkanDmabufImporter::createShadowImage(uint32_t width, uint32_t height,
                                             VkFormat format, VkImage *outImage,
                                             VkDeviceMemory *outMemory) const {
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = format;
  imageInfo.extent = {width, height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage image = VK_NULL_HANDLE;
  if (m_vkCreateImage(m_device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
    qWarning() << "[vulkan] vkCreateImage(shadow) failed";
    return false;
  }

  VkMemoryRequirements memReq{};
  m_vkGetImageMemoryRequirements(m_device, image, &memReq);

  VkPhysicalDeviceMemoryProperties memProps{};
  m_vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);

  uint32_t memoryTypeIndex = UINT32_MAX;
  for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    if (!(memReq.memoryTypeBits & (1u << i))) {
      continue;
    }
    if (memProps.memoryTypes[i].propertyFlags &
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
      memoryTypeIndex = i;
      break;
    }
  }
  if (memoryTypeIndex == UINT32_MAX) {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
      if (memReq.memoryTypeBits & (1u << i)) {
        memoryTypeIndex = i;
        break;
      }
    }
  }
  if (memoryTypeIndex == UINT32_MAX) {
    qWarning() << "[vulkan] no memory type available for the shadow image";
    m_vkDestroyImage(m_device, image, nullptr);
    return false;
  }

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memReq.size;
  allocInfo.memoryTypeIndex = memoryTypeIndex;

  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (m_vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) !=
      VK_SUCCESS) {
    qWarning() << "[vulkan] vkAllocateMemory(shadow) failed";
    m_vkDestroyImage(m_device, image, nullptr);
    return false;
  }

  if (m_vkBindImageMemory(m_device, image, memory, 0) != VK_SUCCESS) {
    qWarning() << "[vulkan] vkBindImageMemory(shadow) failed";
    m_vkFreeMemory(m_device, memory, nullptr);
    m_vkDestroyImage(m_device, image, nullptr);
    return false;
  }

  *outImage = image;
  *outMemory = memory;
  return true;
}

std::optional<VulkanDmabufImporter::Import>
VulkanDmabufImporter::importDmabuf(int width, int height, uint32_t stride,
                                   uint64_t modifier, VkFormat format, int fd) {
  if (!m_bound) {
    ::close(fd);
    return std::nullopt;
  }

  VkSubresourceLayout planeLayout{};
  planeLayout.offset = 0;
  planeLayout.rowPitch = stride;

  VkImageDrmFormatModifierExplicitCreateInfoEXT modInfo{};
  modInfo.sType =
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
  modInfo.drmFormatModifier = modifier;
  modInfo.drmFormatModifierPlaneCount = 1;
  modInfo.pPlaneLayouts = &planeLayout;

  VkExternalMemoryImageCreateInfo extInfo{};
  extInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  extInfo.pNext = &modInfo;
  extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.pNext = &extInfo;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = format;
  imageInfo.extent = {static_cast<uint32_t>(width),
                      static_cast<uint32_t>(height), 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage importedImage = VK_NULL_HANDLE;
  if (m_vkCreateImage(m_device, &imageInfo, nullptr, &importedImage) !=
      VK_SUCCESS) {
    qWarning() << "[vulkan] vkCreateImage(import) failed (format" << format
               << "modifier" << Qt::hex << modifier << Qt::dec << ")";
    ::close(fd);
    return std::nullopt;
  }

  VkMemoryRequirements memReq{};
  m_vkGetImageMemoryRequirements(m_device, importedImage, &memReq);

  VkMemoryFdPropertiesKHR fdProps{};
  fdProps.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
  if (m_vkGetMemoryFdPropertiesKHR(
          m_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd,
          &fdProps) != VK_SUCCESS) {
    qWarning() << "[vulkan] vkGetMemoryFdPropertiesKHR failed for fd" << fd;
    m_vkDestroyImage(m_device, importedImage, nullptr);
    ::close(fd);
    return std::nullopt;
  }

  const uint32_t candidateMask = memReq.memoryTypeBits & fdProps.memoryTypeBits;
  if (candidateMask == 0) {
    qWarning()
        << "[vulkan] no memory type accepts both the image and the dmabuf fd"
        << "(image=" << Qt::hex << memReq.memoryTypeBits
        << "fd=" << fdProps.memoryTypeBits << Qt::dec
        << "). likely cross-GPU/PRIME import: the capture producer's GPU "
           "differs from Plasma's Qt Quick scenegraph";
    m_vkDestroyImage(m_device, importedImage, nullptr);
    ::close(fd);
    return std::nullopt;
  }

  VkPhysicalDeviceMemoryProperties memProps{};
  m_vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);

  VkDeviceMemory importedMemory = VK_NULL_HANDLE;
  bool consumedFd = false;
  for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    if (!(candidateMask & (1u << i))) {
      continue;
    }

    int attemptFd = ::dup(fd);
    if (attemptFd < 0) {
      continue;
    }

    VkMemoryDedicatedAllocateInfo dedicatedInfo{};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.image = importedImage;

    VkImportMemoryFdInfoKHR importInfo{};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    importInfo.pNext = &dedicatedInfo;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importInfo.fd = attemptFd;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &importInfo;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = i;

    if (m_vkAllocateMemory(m_device, &allocInfo, nullptr, &importedMemory) ==
        VK_SUCCESS) {
      consumedFd = true;
      break;
    }
    ::close(attemptFd);
  }

  ::close(fd);

  if (!consumedFd) {
    qWarning() << "[vulkan] vkAllocateMemory exhausted every candidate "
                  "memory type for the imported dmabuf (mask"
               << Qt::hex << candidateMask << Qt::dec << ")";
    m_vkDestroyImage(m_device, importedImage, nullptr);
    return std::nullopt;
  }

  if (m_vkBindImageMemory(m_device, importedImage, importedMemory, 0) !=
      VK_SUCCESS) {
    qWarning() << "[vulkan] vkBindImageMemory(import) failed";
    m_vkFreeMemory(m_device, importedMemory, nullptr);
    m_vkDestroyImage(m_device, importedImage, nullptr);
    return std::nullopt;
  }

  VkImage shadowImage = VK_NULL_HANDLE;
  VkDeviceMemory shadowMemory = VK_NULL_HANDLE;
  if (!createShadowImage(static_cast<uint32_t>(width),
                         static_cast<uint32_t>(height), format, &shadowImage,
                         &shadowMemory)) {
    m_vkFreeMemory(m_device, importedMemory, nullptr);
    m_vkDestroyImage(m_device, importedImage, nullptr);
    return std::nullopt;
  }

  Import result;
  result.importedImage = importedImage;
  result.importedMemory = importedMemory;
  result.shadowImage = shadowImage;
  result.shadowMemory = shadowMemory;
  result.width = static_cast<uint32_t>(width);
  result.height = static_cast<uint32_t>(height);
  result.format = format;

  if (!blitFrame(result, -1)) {
    qWarning() << "[vulkan] initial unsynchronized blit failed for freshly "
                  "imported dmabuf";
  }
  return result;
}

bool VulkanDmabufImporter::blitFrame(Import &import, int syncFd) {
  if (!m_bound || import.importedImage == VK_NULL_HANDLE) {
    if (syncFd >= 0) {
      ::close(syncFd);
    }
    return false;
  }

  if (m_fenceArmed) {
    VkResult drainResult =
        m_vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, kFenceWaitTimeoutNs);
    if (drainResult != VK_SUCCESS) {
      qWarning() << "[vulkan] previous blit submission did not complete in "
                    "time, skipping this frame";
      if (syncFd >= 0) {
        ::close(syncFd);
      }
      return false;
    }
    m_vkResetFences(m_device, 1, &m_fence);
    m_fenceArmed = false;
  }

  VkSemaphore waitSem = VK_NULL_HANDLE;
  if (syncFd >= 0) {
    VkImportSemaphoreFdInfoKHR importInfo{};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    importInfo.semaphore = m_acquireSemaphore;
    importInfo.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
    importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    importInfo.fd = syncFd;
    if (m_vkImportSemaphoreFdKHR(m_device, &importInfo) != VK_SUCCESS) {
      qWarning() << "[vulkan] vkImportSemaphoreFdKHR failed, sampling "
                    "unsynchronized";
      ::close(syncFd);
    } else {
      waitSem = m_acquireSemaphore;
    }
  }

  m_vkResetCommandPool(m_device, m_commandPool, 0);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (m_vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS) {
    qWarning() << "[vulkan] vkBeginCommandBuffer failed";
    return false;
  }

  VkImageSubresourceRange range{};
  range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  range.levelCount = 1;
  range.layerCount = 1;

  VkImageMemoryBarrier acquireImported{};
  acquireImported.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  acquireImported.srcAccessMask = 0;
  acquireImported.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  acquireImported.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  acquireImported.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  acquireImported.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
  acquireImported.dstQueueFamilyIndex = m_queueFamilyIndex;
  acquireImported.image = import.importedImage;
  acquireImported.subresourceRange = range;

  VkImageMemoryBarrier acquireShadow{};
  acquireShadow.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  acquireShadow.srcAccessMask =
      import.shadowHasContent ? VK_ACCESS_SHADER_READ_BIT : 0;
  acquireShadow.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  acquireShadow.oldLayout = import.shadowHasContent
                                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_UNDEFINED;
  acquireShadow.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  acquireShadow.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  acquireShadow.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  acquireShadow.image = import.shadowImage;
  acquireShadow.subresourceRange = range;

  VkImageMemoryBarrier preBarriers[2] = {acquireImported, acquireShadow};
  m_vkCmdPipelineBarrier(m_commandBuffer,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, preBarriers);

  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {import.width, import.height, 1};
  m_vkCmdCopyImage(m_commandBuffer, import.importedImage,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, import.shadowImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier releaseImported{};
  releaseImported.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  releaseImported.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  releaseImported.dstAccessMask = 0;
  releaseImported.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  releaseImported.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  releaseImported.srcQueueFamilyIndex = m_queueFamilyIndex;
  releaseImported.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
  releaseImported.image = import.importedImage;
  releaseImported.subresourceRange = range;

  VkImageMemoryBarrier releaseShadow{};
  releaseShadow.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  releaseShadow.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  releaseShadow.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  releaseShadow.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  releaseShadow.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  releaseShadow.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  releaseShadow.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  releaseShadow.image = import.shadowImage;
  releaseShadow.subresourceRange = range;

  VkImageMemoryBarrier postBarriers[2] = {releaseImported, releaseShadow};
  m_vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, postBarriers);

  if (m_vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) {
    qWarning() << "[vulkan] vkEndCommandBuffer failed";
    return false;
  }

  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.waitSemaphoreCount = waitSem != VK_NULL_HANDLE ? 1u : 0u;
  submitInfo.pWaitSemaphores = waitSem != VK_NULL_HANDLE ? &waitSem : nullptr;
  submitInfo.pWaitDstStageMask =
      waitSem != VK_NULL_HANDLE ? &waitStage : nullptr;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &m_commandBuffer;

  if (m_vkQueueSubmit(m_queue, 1, &submitInfo, m_fence) != VK_SUCCESS) {
    qWarning() << "[vulkan] vkQueueSubmit failed";
    return false;
  }
  m_fenceArmed = true;

  VkResult waitResult =
      m_vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, kFenceWaitTimeoutNs);
  if (waitResult != VK_SUCCESS) {
    qWarning() << "[vulkan] vkWaitForFences timed out or failed, result"
               << waitResult
               << "- shadow may be stale until the GPU "
                  "recovers";
    return false;
  }
  m_vkResetFences(m_device, 1, &m_fence);
  m_fenceArmed = false;

  import.shadowHasContent = true;
  return true;
}

void VulkanDmabufImporter::destroyImport(Import &import) const {
  if (!m_bound) {
    return;
  }
  if (import.shadowImage != VK_NULL_HANDLE) {
    m_vkDestroyImage(m_device, import.shadowImage, nullptr);
  }
  if (import.shadowMemory != VK_NULL_HANDLE) {
    m_vkFreeMemory(m_device, import.shadowMemory, nullptr);
  }
  if (import.importedImage != VK_NULL_HANDLE) {
    m_vkDestroyImage(m_device, import.importedImage, nullptr);
  }
  if (import.importedMemory != VK_NULL_HANDLE) {
    m_vkFreeMemory(m_device, import.importedMemory, nullptr);
  }
  import = Import{};
}

bool VulkanDmabufImporter::queryRenderNode(uint32_t *major,
                                           uint32_t *minor) const {
  if (!m_bound || !m_vkGetPhysicalDeviceProperties2) {
    return false;
  }

  VkPhysicalDeviceDrmPropertiesEXT drm{};
  drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
  VkPhysicalDeviceProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &drm;

  m_vkGetPhysicalDeviceProperties2(m_physicalDevice, &props2);
  if (!drm.hasRender) {
    return false;
  }
  *major = static_cast<uint32_t>(drm.renderMajor);
  *minor = static_cast<uint32_t>(drm.renderMinor);
  return true;
}

QSGTexture *VulkanDmabufImporter::wrapTexture2D(QQuickWindow *window,
                                                const Import &import,
                                                const QSize &size) const {
  return QNativeInterface::QSGVulkanTexture::fromNative(
      import.shadowImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, window,
      size);
}

} // namespace WallpiperKde
