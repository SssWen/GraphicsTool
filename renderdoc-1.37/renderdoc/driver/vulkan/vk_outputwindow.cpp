/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2024 Baldur Karlsson
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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "core/settings.h"
#include "vk_core.h"
#include "vk_replay.h"

RDOC_EXTERN_CONFIG(bool, Vulkan_Debug_SingleSubmitFlushing);

VulkanReplay::OutputWindow::OutputWindow()
    : m_WindowSystem(WindowingSystem::Unknown), width(0), height(0)
{
  surface = VK_NULL_HANDLE;
  swap = VK_NULL_HANDLE;

  WINDOW_HANDLE_INIT;

  hasDepth = false;

  failures = recreatePause = 0;

  bb = VK_NULL_HANDLE;
  bbmem = VK_NULL_HANDLE;
  bbview = VK_NULL_HANDLE;

  resolveimg = VK_NULL_HANDLE;
  resolvemem = VK_NULL_HANDLE;

  dsimg = VK_NULL_HANDLE;
  dsmem = VK_NULL_HANDLE;
  dsview = VK_NULL_HANDLE;

  fb = VK_NULL_HANDLE;
  fbdepth = VK_NULL_HANDLE;
  rp = VK_NULL_HANDLE;
  rpdepth = VK_NULL_HANDLE;

  curidx = 0;

  m_ResourceManager = NULL;

  VkImageMemoryBarrier t = {
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      NULL,
      0,
      0,
      VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_UNDEFINED,
      VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED,
      VK_NULL_HANDLE,
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  bbBarrier = t;

  t.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  depthBarrier = t;
  depthBarrier.srcAccessMask = depthBarrier.dstAccessMask =
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
}

void VulkanReplay::OutputWindow::Destroy(WrappedVulkan *driver, VkDevice device)
{
  driver->SubmitCmds();
  driver->FlushQ();

  const VkDevDispatchTable *vt = ObjDisp(device);

  vt->DeviceWaitIdle(Unwrap(device));

  if(bb != VK_NULL_HANDLE)
  {
    vt->DestroyRenderPass(Unwrap(device), Unwrap(rp), NULL);
    GetResourceManager()->ReleaseWrappedResource(rp);
    rp = VK_NULL_HANDLE;

    vt->DestroyImage(Unwrap(device), Unwrap(bb), NULL);
    GetResourceManager()->ReleaseWrappedResource(bb);

    vt->DestroyImageView(Unwrap(device), Unwrap(bbview), NULL);
    GetResourceManager()->ReleaseWrappedResource(bbview);
    vt->FreeMemory(Unwrap(device), Unwrap(bbmem), NULL);
    GetResourceManager()->ReleaseWrappedResource(bbmem);
    vt->DestroyFramebuffer(Unwrap(device), Unwrap(fb), NULL);
    GetResourceManager()->ReleaseWrappedResource(fb);

    bb = VK_NULL_HANDLE;
    bbview = VK_NULL_HANDLE;
    bbmem = VK_NULL_HANDLE;
    fb = VK_NULL_HANDLE;
  }

  // not owned - freed with the swapchain
  for(size_t i = 0; i < colimg.size(); i++)
  {
    GetResourceManager()->ReleaseWrappedResource(colimg[i]);
  }
  colimg.clear();
  colBarrier.clear();

  if(dsimg != VK_NULL_HANDLE)
  {
    vt->DestroyRenderPass(Unwrap(device), Unwrap(rpdepth), NULL);
    GetResourceManager()->ReleaseWrappedResource(rpdepth);
    rpdepth = VK_NULL_HANDLE;

    vt->DestroyImage(Unwrap(device), Unwrap(dsimg), NULL);
    GetResourceManager()->ReleaseWrappedResource(dsimg);

    vt->DestroyImageView(Unwrap(device), Unwrap(dsview), NULL);
    GetResourceManager()->ReleaseWrappedResource(dsview);
    vt->FreeMemory(Unwrap(device), Unwrap(dsmem), NULL);
    GetResourceManager()->ReleaseWrappedResource(dsmem);
    vt->DestroyFramebuffer(Unwrap(device), Unwrap(fbdepth), NULL);
    GetResourceManager()->ReleaseWrappedResource(fbdepth);

    vt->DestroyImage(Unwrap(device), Unwrap(resolveimg), NULL);
    GetResourceManager()->ReleaseWrappedResource(resolveimg);
    vt->FreeMemory(Unwrap(device), Unwrap(resolvemem), NULL);
    GetResourceManager()->ReleaseWrappedResource(resolvemem);

    resolveimg = VK_NULL_HANDLE;
    resolvemem = VK_NULL_HANDLE;
    dsview = VK_NULL_HANDLE;
    dsimg = VK_NULL_HANDLE;
    dsmem = VK_NULL_HANDLE;
    fbdepth = VK_NULL_HANDLE;
    rpdepth = VK_NULL_HANDLE;
  }

  if(swap != VK_NULL_HANDLE)
  {
    vt->DestroySwapchainKHR(Unwrap(device), Unwrap(swap), NULL);
    GetResourceManager()->ReleaseWrappedResource(swap);
  }

  if(surface != VK_NULL_HANDLE)
  {
    ObjDisp(driver->GetInstance())
        ->DestroySurfaceKHR(Unwrap(driver->GetInstance()), Unwrap(surface), NULL);
    GetResourceManager()->ReleaseWrappedResource(surface);
    surface = VK_NULL_HANDLE;
  }
}

void VulkanReplay::OutputWindow::Create(WrappedVulkan *driver, VkDevice device, bool depth)
{
  const VkDevDispatchTable *vt = ObjDisp(device);
  VkInstance inst = driver->GetInstance();
  VkPhysicalDevice phys = driver->GetPhysDev();

  hasDepth = depth;

  // save the old swapchain so it isn't destroyed
  VkSwapchainKHR old = swap;
  swap = VK_NULL_HANDLE;

  // we can't destroy the surface until all swapchains are destroyed, so
  // we also save the surface here and restore it back after destroy
  VkSurfaceKHR oldsurf = surface;
  surface = VK_NULL_HANDLE;

  Destroy(driver, device);

  surface = oldsurf;

  fresh = true;

  if(surface == VK_NULL_HANDLE && m_WindowSystem != WindowingSystem::Headless)
  {
    CreateSurface(driver, inst);

    GetResourceManager()->WrapResource(Unwrap(inst), surface);
  }

  // sensible defaults
  VkFormat imformat = VK_FORMAT_B8G8R8A8_SRGB;
  VkPresentModeKHR presentmode = VK_PRESENT_MODE_FIFO_KHR;
  VkColorSpaceKHR imcolspace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

  VkResult vkr = VK_SUCCESS;

  uint32_t numImages = 2;

  if(m_WindowSystem != WindowingSystem::Headless)
  {
    VkSurfaceCapabilitiesKHR capabilities = {};

    ObjDisp(inst)->GetPhysicalDeviceSurfaceCapabilitiesKHR(Unwrap(phys), Unwrap(surface),
                                                           &capabilities);

    numImages = RDCMAX(numImages, capabilities.minImageCount);

    if(capabilities.supportedUsageFlags == 0)
    {
      if(old != VK_NULL_HANDLE)
      {
        vt->DestroySwapchainKHR(Unwrap(device), Unwrap(old), NULL);
        GetResourceManager()->ReleaseWrappedResource(old);
      }

      RDCERR("Surface reported unsuccessful. %d consecutive failures!", failures);
      failures++;

      return;
    }

    RDCASSERT(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    // AMD didn't report this capability for a while. If the assert fires for you, update
    // your drivers!
    RDCASSERT(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // find a supported alpha compositing mode
    if((capabilities.supportedCompositeAlpha & compositeAlpha) == 0)
    {
      VkCompositeAlphaFlagBitsKHR compositingBits[] = {
          VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
          VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
      for(VkCompositeAlphaFlagBitsKHR &compositingBit : compositingBits)
      {
        if(capabilities.supportedCompositeAlpha & compositingBit)
        {
          compositeAlpha = compositingBit;
          break;
        }
      }
    }

    // check format and present mode from driver
    {
      uint32_t numFormats = 0;

      vkr = ObjDisp(inst)->GetPhysicalDeviceSurfaceFormatsKHR(Unwrap(phys), Unwrap(surface),
                                                              &numFormats, NULL);
      CHECK_VKR(driver, vkr);

      if(numFormats > 0)
      {
        VkSurfaceFormatKHR *formats = new VkSurfaceFormatKHR[numFormats];

        vkr = ObjDisp(inst)->GetPhysicalDeviceSurfaceFormatsKHR(Unwrap(phys), Unwrap(surface),
                                                                &numFormats, formats);
        CHECK_VKR(driver, vkr);

        if(numFormats == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
        {
          // 1 entry with undefined means no preference, just use our default
          imformat = VK_FORMAT_B8G8R8A8_SRGB;
          imcolspace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        }
        else
        {
          // try and find a format with SRGB correction
          imformat = VK_FORMAT_UNDEFINED;
          imcolspace = formats[0].colorSpace;

          for(uint32_t i = 0; i < numFormats; i++)
          {
            if(IsSRGBFormat(formats[i].format))
            {
              imformat = formats[i].format;
              imcolspace = formats[i].colorSpace;
              RDCASSERT(imcolspace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
              break;
            }
          }

          if(imformat == VK_FORMAT_UNDEFINED)
          {
            RDCWARN("Couldn't find SRGB correcting output swapchain format");
            imformat = formats[0].format;
          }
        }

        SAFE_DELETE_ARRAY(formats);
      }

      uint32_t numModes = 0;

      vkr = ObjDisp(inst)->GetPhysicalDeviceSurfacePresentModesKHR(Unwrap(phys), Unwrap(surface),
                                                                   &numModes, NULL);
      CHECK_VKR(driver, vkr);

      if(numModes > 0)
      {
        VkPresentModeKHR *modes = new VkPresentModeKHR[numModes];

        vkr = ObjDisp(inst)->GetPhysicalDeviceSurfacePresentModesKHR(Unwrap(phys), Unwrap(surface),
                                                                     &numModes, modes);
        CHECK_VKR(driver, vkr);

        // If mailbox mode is available, use it, as is the lowest-latency non-
        // tearing mode.  If not, try IMMEDIATE which will usually be available,
        // and is fastest (though it tears).  If not, fall back to FIFO which is
        // always available.
        for(size_t i = 0; i < numModes; i++)
        {
          if(modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
          {
            presentmode = VK_PRESENT_MODE_MAILBOX_KHR;
            break;
          }

          if(modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
            presentmode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }

        SAFE_DELETE_ARRAY(modes);
      }
    }

    VkBool32 supported = false;
    ObjDisp(inst)->GetPhysicalDeviceSurfaceSupportKHR(Unwrap(phys), driver->GetQFamilyIdx(),
                                                      Unwrap(surface), &supported);

    // can't really recover from this anyway
    RDCASSERT(supported);

    VkSwapchainCreateInfoKHR swapInfo = {
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        NULL,
        0,
        Unwrap(surface),
        numImages,
        imformat,
        imcolspace,
        {width, height},
        1,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE,
        0,
        NULL,
        VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        compositeAlpha,
        presentmode,
        true,
        Unwrap(old),
    };

    vkr = vt->CreateSwapchainKHR(Unwrap(device), &swapInfo, NULL, &swap);
    CHECK_VKR(driver, vkr);

    if(old != VK_NULL_HANDLE)
    {
      vt->DestroySwapchainKHR(Unwrap(device), Unwrap(old), NULL);
      GetResourceManager()->ReleaseWrappedResource(old);
    }

    if(swap == VK_NULL_HANDLE)
    {
      RDCERR("Failed to create swapchain. %d consecutive failures!", failures);
      failures++;

      // do some sort of backoff.

      // the first time, try to recreate again next frame
      if(failures == 1)
        recreatePause = 0;
      // the next few times, wait 200 'frames' between attempts
      else if(failures < 10)
        recreatePause = 100;
      // otherwise, only reattempt very infrequently. A resize will
      // always retrigger a recreate, so ew probably don't want to
      // try again
      else
        recreatePause = 1000;

      return;
    }

    failures = 0;

    GetResourceManager()->WrapResource(Unwrap(device), swap);

    uint32_t numImgs = 0;
    vkr = vt->GetSwapchainImagesKHR(Unwrap(device), Unwrap(swap), &numImgs, NULL);
    CHECK_VKR(driver, vkr);

    colimg.resize(numImgs);
    colBarrier.resize(numImgs);

    vkr = vt->GetSwapchainImagesKHR(Unwrap(device), Unwrap(swap), &numImgs, colimg.data());
    CHECK_VKR(driver, vkr);

    for(size_t i = 0; i < numImgs; i++)
    {
      GetResourceManager()->WrapResource(Unwrap(device), colimg[i]);

      colBarrier[i] = {
          VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          NULL,
          0,
          0,
          VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_UNDEFINED,
          VK_QUEUE_FAMILY_IGNORED,
          VK_QUEUE_FAMILY_IGNORED,
          Unwrap(colimg[i]),
          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
    }
  }

  curidx = 0;

  // for our 'fake' backbuffer, create in RGBA8
  imformat = VK_FORMAT_R8G8B8A8_SRGB;

  if(depth)
  {
    VkImageCreateInfo imInfo = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        NULL,
        0,
        VK_IMAGE_TYPE_2D,
        VK_FORMAT_D32_SFLOAT,
        {width, height, 1},
        1,
        1,
        VULKAN_MESH_VIEW_SAMPLES,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_SHARING_MODE_EXCLUSIVE,
        0,
        NULL,
        VK_IMAGE_LAYOUT_UNDEFINED,
    };

    vkr = vt->CreateImage(Unwrap(device), &imInfo, NULL, &dsimg);
    CHECK_VKR(driver, vkr);

    GetResourceManager()->WrapResource(Unwrap(device), dsimg);

    NameVulkanObject(dsimg, "outputwindow dsimg");

    VkMemoryRequirements mrq = {0};

    vt->GetImageMemoryRequirements(Unwrap(device), Unwrap(dsimg), &mrq);

    VkMemoryAllocateInfo allocInfo = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        NULL,
        mrq.size,
        driver->GetGPULocalMemoryIndex(mrq.memoryTypeBits),
    };

    vkr = vt->AllocateMemory(Unwrap(device), &allocInfo, NULL, &dsmem);
    CHECK_VKR(driver, vkr);

    if(vkr != VK_SUCCESS)
      return;

    GetResourceManager()->WrapResource(Unwrap(device), dsmem);

    vkr = vt->BindImageMemory(Unwrap(device), Unwrap(dsimg), Unwrap(dsmem), 0);
    CHECK_VKR(driver, vkr);

    depthBarrier.image = Unwrap(dsimg);
    depthBarrier.oldLayout = depthBarrier.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageViewCreateInfo info = {
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        NULL,
        0,
        Unwrap(dsimg),
        VK_IMAGE_VIEW_TYPE_2D,
        VK_FORMAT_D32_SFLOAT,
        {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
         VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };

    vkr = vt->CreateImageView(Unwrap(device), &info, NULL, &dsview);
    CHECK_VKR(driver, vkr);
    NameUnwrappedVulkanObject(dsview, "output window dsview");

    GetResourceManager()->WrapResource(Unwrap(device), dsview);

    // create resolve target, since it must precisely match the pre-resolve format, it doesn't allow
    // any format conversion.
    imInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imInfo.format = imformat;
    imInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    vkr = vt->CreateImage(Unwrap(device), &imInfo, NULL, &resolveimg);
    CHECK_VKR(driver, vkr);

    GetResourceManager()->WrapResource(Unwrap(device), resolveimg);

    NameVulkanObject(resolveimg, "outputwindow resolveimg");

    vt->GetImageMemoryRequirements(Unwrap(device), Unwrap(resolveimg), &mrq);

    allocInfo.allocationSize = mrq.size;
    allocInfo.memoryTypeIndex = driver->GetGPULocalMemoryIndex(mrq.memoryTypeBits);

    vkr = vt->AllocateMemory(Unwrap(device), &allocInfo, NULL, &resolvemem);
    CHECK_VKR(driver, vkr);

    if(vkr != VK_SUCCESS)
      return;

    GetResourceManager()->WrapResource(Unwrap(device), resolvemem);

    vkr = vt->BindImageMemory(Unwrap(device), Unwrap(resolveimg), Unwrap(resolvemem), 0);
    CHECK_VKR(driver, vkr);
  }

  {
    VkAttachmentDescription attDesc[] = {
        {0, imformat, depth ? VULKAN_MESH_VIEW_SAMPLES : VK_SAMPLE_COUNT_1_BIT,
         VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {0, VK_FORMAT_D32_SFLOAT, depth ? VULKAN_MESH_VIEW_SAMPLES : VK_SAMPLE_COUNT_1_BIT,
         VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};

    VkAttachmentReference attRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkAttachmentReference dsRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription sub = {
        0,    VK_PIPELINE_BIND_POINT_GRAPHICS,
        0,    NULL,       // inputs
        1,    &attRef,    // color
        NULL,             // resolve
        NULL,             // depth-stencil
        0,    NULL,       // preserve
    };

    VkRenderPassCreateInfo rpinfo = {
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        NULL,
        0,
        1,
        attDesc,
        1,
        &sub,
        0,
        NULL,    // dependencies
    };

    vkr = vt->CreateRenderPass(Unwrap(device), &rpinfo, NULL, &rp);
    CHECK_VKR(driver, vkr);

    GetResourceManager()->WrapResource(Unwrap(device), rp);

    if(dsimg != VK_NULL_HANDLE)
    {
      sub.pDepthStencilAttachment = &dsRef;

      rpinfo.attachmentCount = 2;

      vkr = vt->CreateRenderPass(Unwrap(device), &rpinfo, NULL, &rpdepth);
      CHECK_VKR(driver, vkr);

      GetResourceManager()->WrapResource(Unwrap(device), rpdepth);
    }
  }

  {
    VkImageCreateInfo imInfo = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        NULL,
        0,
        VK_IMAGE_TYPE_2D,
        imformat,
        {width, height, 1},
        1,
        1,
        depth ? VULKAN_MESH_VIEW_SAMPLES : VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_SHARING_MODE_EXCLUSIVE,
        0,
        NULL,
        VK_IMAGE_LAYOUT_UNDEFINED,
    };

    vkr = vt->CreateImage(Unwrap(device), &imInfo, NULL, &bb);
    CHECK_VKR(driver, vkr);

    GetResourceManager()->WrapResource(Unwrap(device), bb);

    NameVulkanObject(bb, "outputwindow bb");

    VkMemoryRequirements mrq = {0};

    vt->GetImageMemoryRequirements(Unwrap(device), Unwrap(bb), &mrq);

    VkMemoryAllocateInfo allocInfo = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        NULL,
        mrq.size,
        driver->GetGPULocalMemoryIndex(mrq.memoryTypeBits),
    };

    vkr = vt->AllocateMemory(Unwrap(device), &allocInfo, NULL, &bbmem);
    CHECK_VKR(driver, vkr);

    if(vkr != VK_SUCCESS)
      return;

    GetResourceManager()->WrapResource(Unwrap(device), bbmem);

    vkr = vt->BindImageMemory(Unwrap(device), Unwrap(bb), Unwrap(bbmem), 0);
    CHECK_VKR(driver, vkr);

    bbBarrier.image = Unwrap(bb);
    bbBarrier.oldLayout = bbBarrier.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  }

  {
    VkImageViewCreateInfo info = {
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        NULL,
        0,
        Unwrap(bb),
        VK_IMAGE_VIEW_TYPE_2D,
        imformat,
        {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
         VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };

    vkr = vt->CreateImageView(Unwrap(device), &info, NULL, &bbview);
    CHECK_VKR(driver, vkr);
    NameUnwrappedVulkanObject(bbview, "output window bbview");

    GetResourceManager()->WrapResource(Unwrap(device), bbview);

    {
      VkFramebufferCreateInfo fbinfo = {
          VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          NULL,
          0,
          Unwrap(rp),
          1,
          UnwrapPtr(bbview),
          (uint32_t)width,
          (uint32_t)height,
          1,
      };

      vkr = vt->CreateFramebuffer(Unwrap(device), &fbinfo, NULL, &fb);
      CHECK_VKR(driver, vkr);

      GetResourceManager()->WrapResource(Unwrap(device), fb);
    }

    if(dsimg != VK_NULL_HANDLE)
    {
      VkImageView views[] = {Unwrap(bbview), Unwrap(dsview)};
      VkFramebufferCreateInfo fbinfo = {
          VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
          NULL,
          0,
          Unwrap(rpdepth),
          2,
          views,
          (uint32_t)width,
          (uint32_t)height,
          1,
      };

      vkr = vt->CreateFramebuffer(Unwrap(device), &fbinfo, NULL, &fbdepth);
      CHECK_VKR(driver, vkr);

      GetResourceManager()->WrapResource(Unwrap(device), fbdepth);
    }
  }
}

void VulkanReplay::GetOutputWindowData(uint64_t id, bytebuf &retData)
{
  if(id == 0 || m_OutputWindows.find(id) == m_OutputWindows.end())
    return;

  OutputWindow &outw = m_OutputWindows[id];

  VkDevice device = m_pDriver->GetDev();
  VkCommandBuffer cmd = m_pDriver->GetNextCmd();

  if(cmd == VK_NULL_HANDLE)
    return;

  const VkDevDispatchTable *vt = ObjDisp(device);

  vt->DeviceWaitIdle(Unwrap(device));

  VkBuffer readbackBuf = VK_NULL_HANDLE;

  VkResult vkr = VK_SUCCESS;

  // create readback buffer
  VkBufferCreateInfo bufInfo = {
      VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      NULL,
      0,
      GetByteSize(outw.width, outw.height, 1, VK_FORMAT_R8G8B8A8_UNORM, 0),
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
  };
  vt->CreateBuffer(Unwrap(device), &bufInfo, NULL, &readbackBuf);
  CHECK_VKR(m_pDriver, vkr);

  VkMemoryRequirements mrq = {0};

  vt->GetBufferMemoryRequirements(Unwrap(device), readbackBuf, &mrq);

  VkMemoryAllocateInfo allocInfo = {
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      NULL,
      mrq.size,
      m_pDriver->GetReadbackMemoryIndex(mrq.memoryTypeBits),
  };

  VkDeviceMemory readbackMem = VK_NULL_HANDLE;
  vkr = vt->AllocateMemory(Unwrap(device), &allocInfo, NULL, &readbackMem);
  CHECK_VKR(m_pDriver, vkr);

  if(vkr != VK_SUCCESS)
    return;

  vkr = vt->BindBufferMemory(Unwrap(device), readbackBuf, readbackMem, 0);
  CHECK_VKR(m_pDriver, vkr);

  VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

  // do image copy
  vkr = vt->BeginCommandBuffer(Unwrap(cmd), &beginInfo);
  CHECK_VKR(m_pDriver, vkr);

  VkBufferImageCopy cpy = {
      0,
      0,
      0,
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      {
          0,
          0,
          0,
      },
      {outw.width, outw.height, 1},
  };

  outw.bbBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  outw.bbBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

  DoPipelineBarrier(cmd, 1, &outw.bbBarrier);

  VkImage copySource = outw.bb;

  if(outw.resolveimg != VK_NULL_HANDLE)
  {
    VkImageResolve resolve = {
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
        {outw.width, outw.height, 1},
    };

    VkImageMemoryBarrier resolveBarrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        NULL,
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        Unwrap(outw.resolveimg),
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };

    // discard previous contents of resolve buffer and finish any work with it.
    DoPipelineBarrier(cmd, 1, &resolveBarrier);

    // resolve from the backbuffer to resolve buffer (identical format)
    vt->CmdResolveImage(Unwrap(cmd), Unwrap(outw.bb), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        Unwrap(outw.resolveimg), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve);

    // wait for resolve to finish before we blit
    copySource = outw.resolveimg;

    resolveBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    resolveBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    DoPipelineBarrier(cmd, 1, &resolveBarrier);
  }

  vt->CmdCopyImageToBuffer(Unwrap(cmd), Unwrap(copySource), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readbackBuf, 1, &cpy);

  outw.bbBarrier.oldLayout = outw.bbBarrier.newLayout;
  outw.bbBarrier.srcAccessMask = outw.bbBarrier.dstAccessMask;

  vkr = vt->EndCommandBuffer(Unwrap(cmd));
  CHECK_VKR(m_pDriver, vkr);

  m_pDriver->SubmitCmds();
  m_pDriver->FlushQ();    // need to wait so we can readback

  // map memory and readback
  byte *pData = NULL;
  vkr = vt->MapMemory(Unwrap(device), readbackMem, 0, bufInfo.size, 0, (void **)&pData);
  CHECK_VKR(m_pDriver, vkr);
  if(vkr != VK_SUCCESS)
    return;
  if(!pData)
  {
    RDCERR("Manually reporting failed memory map");
    CHECK_VKR(m_pDriver, VK_ERROR_MEMORY_MAP_FAILED);
    return;
  }

  VkMappedMemoryRange range = {
      VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, readbackMem, 0, VK_WHOLE_SIZE,
  };

  vkr = vt->InvalidateMappedMemoryRanges(Unwrap(device), 1, &range);
  CHECK_VKR(m_pDriver, vkr);

  {
    retData.resize(outw.width * outw.height * 3);

    byte *src = (byte *)pData;
    byte *dst = retData.data();

    for(uint32_t row = 0; row < outw.height; row++)
    {
      for(uint32_t x = 0; x < outw.width; x++)
      {
        dst[x * 3 + 0] = src[x * 4 + 0];
        dst[x * 3 + 1] = src[x * 4 + 1];
        dst[x * 3 + 2] = src[x * 4 + 2];
      }

      src += outw.width * 4;
      dst += outw.width * 3;
    }
  }

  vt->UnmapMemory(Unwrap(device), readbackMem);

  // delete all
  vt->DestroyBuffer(Unwrap(device), readbackBuf, NULL);
  vt->FreeMemory(Unwrap(device), readbackMem, NULL);
}

bool VulkanReplay::CheckResizeOutputWindow(uint64_t id)
{
  if(id == 0 || m_OutputWindows.find(id) == m_OutputWindows.end())
    return false;

  OutputWindow &outw = m_OutputWindows[id];

  if(outw.m_WindowSystem == WindowingSystem::Unknown ||
     outw.m_WindowSystem == WindowingSystem::Headless)
    return false;

  int32_t w, h;
  GetOutputWindowDimensions(id, w, h);

  if((uint32_t)w != outw.width || (uint32_t)h != outw.height)
  {
    outw.width = w;
    outw.height = h;

    if(outw.width > 0 && outw.height > 0)
      outw.Create(m_pDriver, m_pDriver->GetDev(), outw.hasDepth);

    return true;
  }

  if(outw.swap == VK_NULL_HANDLE && outw.width > 0 && outw.height > 0)
  {
    if(outw.recreatePause <= 0)
      outw.Create(m_pDriver, m_pDriver->GetDev(), outw.hasDepth);
    else
      outw.recreatePause--;

    return true;
  }

  if(outw.outofdate)
  {
    outw.outofdate = false;
    return true;
  }

  return false;
}

void VulkanReplay::BindOutputWindow(uint64_t id, bool depth)
{
  m_ActiveWinID = id;
  m_BindDepth = depth;

  auto it = m_OutputWindows.find(id);
  if(id == 0 || it == m_OutputWindows.end())
    return;

  OutputWindow &outw = it->second;

  m_DebugWidth = outw.width;
  m_DebugHeight = outw.height;

  // if the swapchain failed to create, do nothing. We will try to recreate it
  // again in CheckResizeOutputWindow (once per render 'frame')
  if(outw.m_WindowSystem != WindowingSystem::Headless && outw.swap == VK_NULL_HANDLE)
    return;

  VkDevice dev = m_pDriver->GetDev();
  const VkDevDispatchTable *vt = ObjDisp(dev);
  VkResult vkr = VK_SUCCESS;

  // if we have a swapchain, acquire the next image.
  if(outw.swap != VK_NULL_HANDLE)
  {
    // semaphore is short lived, so not wrapped, if it's cached (ideally)
    // then it should be wrapped
    VkSemaphore sem;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};

    vkr = vt->CreateSemaphore(Unwrap(dev), &semInfo, NULL, &sem);
    CHECK_VKR(m_pDriver, vkr);

    vkr = vt->AcquireNextImageKHR(Unwrap(dev), Unwrap(outw.swap), 2000000000ULL, sem,
                                  VK_NULL_HANDLE, &outw.curidx);

    if(vkr == VK_ERROR_OUT_OF_DATE_KHR)
    {
      // force a swapchain recreate.
      outw.width = 0;
      outw.height = 0;

      CheckResizeOutputWindow(id);

      m_DebugWidth = outw.width;
      m_DebugHeight = outw.height;

      // then try again to acquire.
      vkr = vt->AcquireNextImageKHR(Unwrap(dev), Unwrap(outw.swap), 2000000000ULL, sem,
                                    VK_NULL_HANDLE, &outw.curidx);

      if(vkr == VK_ERROR_OUT_OF_DATE_KHR)
      {
        RDCWARN("Swapchain still reported as out of date even after recreation");
        outw.outofdate = true;
      }
    }

    if(vkr == VK_SUBOPTIMAL_KHR)
      vkr = VK_SUCCESS;

    CHECK_VKR(m_pDriver, vkr);

    if(vkr == VK_SUCCESS)
    {
      VkSubmitInfo submitInfo = {
          VK_STRUCTURE_TYPE_SUBMIT_INFO,
          NULL,
          1,
          &sem,
          &stage,
          0,
          NULL,    // cmd buffers
          0,
          NULL,    // signal semaphores
      };

      vkr = vt->QueueSubmit(Unwrap(m_pDriver->GetQ()), 1, &submitInfo, VK_NULL_HANDLE);
      CHECK_VKR(m_pDriver, vkr);

      vt->QueueWaitIdle(Unwrap(m_pDriver->GetQ()));
    }

    vt->DestroySemaphore(Unwrap(dev), sem, NULL);
  }

  VkCommandBuffer cmd = m_pDriver->GetNextCmd();
  if(cmd == VK_NULL_HANDLE)
    return;

  VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

  vkr = vt->BeginCommandBuffer(Unwrap(cmd), &beginInfo);
  CHECK_VKR(m_pDriver, vkr);

  outw.depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  // first time rendering to the backbuffer, clear it, since our typical render pass
  // is set to LOAD_OP_LOAD
  if(outw.fresh)
  {
    outw.bbBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    outw.bbBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    DoPipelineBarrier(cmd, 1, &outw.bbBarrier);
    float black[] = {0.0f, 0.0f, 0.0f, 0.0f};
    vt->CmdClearColorImage(Unwrap(cmd), Unwrap(outw.bb), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (VkClearColorValue *)black, 1, &outw.bbBarrier.subresourceRange);

    outw.bbBarrier.oldLayout = outw.bbBarrier.newLayout;
    outw.bbBarrier.srcAccessMask = outw.bbBarrier.dstAccessMask;

    outw.fresh = false;
  }

  outw.bbBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  outw.bbBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  DoPipelineBarrier(cmd, 1, &outw.bbBarrier);
  if(outw.colBarrier.size() > 0)
  {
    outw.colBarrier[outw.curidx].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    outw.colBarrier[outw.curidx].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    DoPipelineBarrier(cmd, 1, &outw.colBarrier[outw.curidx]);
    outw.colBarrier[outw.curidx].oldLayout = outw.colBarrier[outw.curidx].newLayout;
    outw.colBarrier[outw.curidx].srcAccessMask = outw.colBarrier[outw.curidx].dstAccessMask;
  }
  if(outw.dsimg != VK_NULL_HANDLE)
    DoPipelineBarrier(cmd, 1, &outw.depthBarrier);

  outw.depthBarrier.oldLayout = outw.depthBarrier.newLayout;
  outw.bbBarrier.oldLayout = outw.bbBarrier.newLayout;
  outw.bbBarrier.srcAccessMask = outw.bbBarrier.dstAccessMask;

  vt->EndCommandBuffer(Unwrap(cmd));

  if(Vulkan_Debug_SingleSubmitFlushing())
    m_pDriver->SubmitCmds();
}

void VulkanReplay::ClearOutputWindowColor(uint64_t id, FloatVector col)
{
  auto it = m_OutputWindows.find(id);
  if(id == 0 || it == m_OutputWindows.end())
    return;

  OutputWindow &outw = it->second;

  // if the swapchain failed to create, do nothing. We will try to recreate it
  // again in CheckResizeOutputWindow (once per render 'frame')
  if(outw.m_WindowSystem != WindowingSystem::Headless && outw.swap == VK_NULL_HANDLE)
    return;

  VkDevice dev = m_pDriver->GetDev();
  VkCommandBuffer cmd = m_pDriver->GetNextCmd();
  const VkDevDispatchTable *vt = ObjDisp(dev);

  if(cmd == VK_NULL_HANDLE)
    return;

  VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

  VkResult vkr = vt->BeginCommandBuffer(Unwrap(cmd), &beginInfo);
  CHECK_VKR(m_pDriver, vkr);

  VkMarkerRegion::Begin("ClearOutputWindowColor", cmd);

  outw.bbBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  outw.bbBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  outw.bbBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  outw.bbBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

  DoPipelineBarrier(cmd, 1, &outw.bbBarrier);

  vt->CmdClearColorImage(Unwrap(cmd), Unwrap(outw.bb), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         (VkClearColorValue *)&col.x, 1, &outw.bbBarrier.subresourceRange);

  outw.bbBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  outw.bbBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  outw.bbBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  outw.bbBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  DoPipelineBarrier(cmd, 1, &outw.bbBarrier);

  outw.bbBarrier.srcAccessMask = outw.bbBarrier.dstAccessMask;
  outw.bbBarrier.oldLayout = outw.bbBarrier.newLayout;

  VkMarkerRegion::End(cmd);

  vt->EndCommandBuffer(Unwrap(cmd));

  if(Vulkan_Debug_SingleSubmitFlushing())
    m_pDriver->SubmitCmds();
}

void VulkanReplay::ClearOutputWindowDepth(uint64_t id, float depth, uint8_t stencil)
{
  auto it = m_OutputWindows.find(id);
  if(id == 0 || it == m_OutputWindows.end())
    return;

  OutputWindow &outw = it->second;

  // if the swapchain failed to create, do nothing. We will try to recreate it
  // again in CheckResizeOutputWindow (once per render 'frame')
  if(outw.m_WindowSystem != WindowingSystem::Headless && outw.swap == VK_NULL_HANDLE)
    return;

  VkDevice dev = m_pDriver->GetDev();
  VkCommandBuffer cmd = m_pDriver->GetNextCmd();
  const VkDevDispatchTable *vt = ObjDisp(dev);

  if(cmd == VK_NULL_HANDLE)
    return;

  VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

  VkResult vkr = vt->BeginCommandBuffer(Unwrap(cmd), &beginInfo);
  CHECK_VKR(m_pDriver, vkr);

  VkClearDepthStencilValue ds = {depth, stencil};

  outw.depthBarrier.srcAccessMask =
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
  outw.depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  outw.depthBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  outw.depthBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

  DoPipelineBarrier(cmd, 1, &outw.depthBarrier);

  vt->CmdClearDepthStencilImage(Unwrap(cmd), Unwrap(outw.dsimg), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &ds, 1, &outw.depthBarrier.subresourceRange);

  outw.depthBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  outw.depthBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  outw.depthBarrier.dstAccessMask =
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
  outw.depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  DoPipelineBarrier(cmd, 1, &outw.depthBarrier);

  outw.depthBarrier.oldLayout = outw.depthBarrier.newLayout;

  vt->EndCommandBuffer(Unwrap(cmd));

  if(Vulkan_Debug_SingleSubmitFlushing())
    m_pDriver->SubmitCmds();
}

void VulkanReplay::FlipOutputWindow(uint64_t id)
{
  auto it = m_OutputWindows.find(id);
  if(id == 0 || it == m_OutputWindows.end())
    return;

  OutputWindow &outw = it->second;

  // if the swapchain failed to create, do nothing. We will try to recreate it
  // again in CheckResizeOutputWindow (once per render 'frame')
  if(outw.swap == VK_NULL_HANDLE)
    return;

  VkDevice dev = m_pDriver->GetDev();
  VkCommandBuffer cmd = m_pDriver->GetNextCmd();
  const VkDevDispatchTable *vt = ObjDisp(dev);

  if(cmd == VK_NULL_HANDLE)
    return;

  VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

  VkResult vkr = vt->BeginCommandBuffer(Unwrap(cmd), &beginInfo);
  CHECK_VKR(m_pDriver, vkr);

  VkMarkerRegion::Begin("FlipOutputWindow", cmd);

  // ensure rendering has completed before copying
  outw.bbBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  outw.bbBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  outw.bbBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  DoPipelineBarrier(cmd, 1, &outw.bbBarrier);
  DoPipelineBarrier(cmd, 1, &outw.colBarrier[outw.curidx]);
  outw.bbBarrier.oldLayout = outw.bbBarrier.newLayout;
  outw.bbBarrier.srcAccessMask = 0;
  outw.bbBarrier.dstAccessMask = 0;

  VkImageBlit blit = {
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      {
          {0, 0, 0},
          {(int32_t)outw.width, (int32_t)outw.height, 1},
      },
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      {
          {0, 0, 0},
          {(int32_t)outw.width, (int32_t)outw.height, 1},
      },
  };

  VkImage blitSource = outw.bb;

  if(outw.dsimg != VK_NULL_HANDLE)
  {
    VkImageResolve resolve = {
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
        {outw.width, outw.height, 1},
    };

    VkImageMemoryBarrier resolveBarrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        NULL,
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        Unwrap(outw.resolveimg),
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};

    // discard previous contents of resolve buffer and finish any work with it.
    DoPipelineBarrier(cmd, 1, &resolveBarrier);

    // resolve from the backbuffer to resolve buffer (identical format)
    vt->CmdResolveImage(Unwrap(cmd), Unwrap(outw.bb), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        Unwrap(outw.resolveimg), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve);

    // wait for resolve to finish before we blit
    blitSource = outw.resolveimg;

    resolveBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    resolveBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    DoPipelineBarrier(cmd, 1, &resolveBarrier);
  }

  vt->CmdBlitImage(Unwrap(cmd), Unwrap(blitSource), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   Unwrap(outw.colimg[outw.curidx]), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_NEAREST);

  outw.bbBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  outw.bbBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  outw.bbBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  outw.colBarrier[outw.curidx].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  // make sure copy has completed before present
  outw.colBarrier[outw.curidx].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  outw.colBarrier[outw.curidx].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

  DoPipelineBarrier(cmd, 1, &outw.bbBarrier);
  DoPipelineBarrier(cmd, 1, &outw.colBarrier[outw.curidx]);

  outw.bbBarrier.oldLayout = outw.bbBarrier.newLayout;
  outw.bbBarrier.srcAccessMask = outw.bbBarrier.dstAccessMask;
  outw.colBarrier[outw.curidx].oldLayout = outw.colBarrier[outw.curidx].newLayout;

  outw.colBarrier[outw.curidx].srcAccessMask = 0;
  outw.colBarrier[outw.curidx].dstAccessMask = 0;

  VkMarkerRegion::End(cmd);

  vt->EndCommandBuffer(Unwrap(cmd));

  // submit all the cmds we recorded
  m_pDriver->SubmitCmds();

  VkPresentInfoKHR presentInfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                  NULL,
                                  0,
                                  NULL,    // wait semaphores
                                  1,
                                  UnwrapPtr(outw.swap),
                                  &outw.curidx,
                                  &vkr};

  VkResult retvkr;

  // if we were not able to acquire an image successfully in Bind even after resizing due to an
  // OUT_OF_DATE, then don't present here as we never got a valid image.
  // This will also force another recreate below
  if(outw.outofdate)
    retvkr = VK_ERROR_OUT_OF_DATE_KHR;
  else
    retvkr = vt->QueuePresentKHR(Unwrap(m_pDriver->GetQ()), &presentInfo);

  if(retvkr != VK_ERROR_OUT_OF_DATE_KHR && retvkr != VK_SUBOPTIMAL_KHR &&
     retvkr != VK_ERROR_SURFACE_LOST_KHR)
    CHECK_VKR(m_pDriver, retvkr);

  m_pDriver->FlushQ();

  if(retvkr == VK_ERROR_OUT_OF_DATE_KHR)
  {
    // this will check the current extent and use that if possible
    outw.Create(m_pDriver, m_pDriver->GetDev(), outw.hasDepth);

    outw.outofdate = true;
  }
}

void VulkanReplay::DestroyOutputWindow(uint64_t id)
{
  auto it = m_OutputWindows.find(id);
  if(id == 0 || it == m_OutputWindows.end())
    return;

  OutputWindow &outw = it->second;

  outw.Destroy(m_pDriver, m_pDriver->GetDev());

  m_OutputWindows.erase(it);
}

rdcarray<WindowingSystem> VulkanReplay::GetSupportedWindowSystems()
{
  return m_pDriver->m_SupportedWindowSystems;
}

uint64_t VulkanReplay::MakeOutputWindow(WindowingData window, bool depth)
{
  uint64_t id = m_OutputWinID;
  m_OutputWinID++;

  m_OutputWindows[id].m_WindowSystem = window.system;
  m_OutputWindows[id].m_ResourceManager = GetResourceManager();

  if(window.system != WindowingSystem::Unknown && window.system != WindowingSystem::Headless)
    m_OutputWindows[id].SetWindowHandle(window);

  if(window.system != WindowingSystem::Unknown)
  {
    int32_t w, h;

    if(window.system == WindowingSystem::Headless)
    {
      w = window.headless.width;
      h = window.headless.height;
    }
    else
    {
      GetOutputWindowDimensions(id, w, h);
    }

    m_OutputWindows[id].width = w;
    m_OutputWindows[id].height = h;

    m_OutputWindows[id].Create(m_pDriver, m_pDriver->GetDev(), depth);
  }

  return id;
}
