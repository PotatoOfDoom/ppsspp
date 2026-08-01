#pragma once

#include "VRBase.h"

void ovrApp_Clear(ovrApp* app);
void ovrApp_Destroy(ovrApp* app);
int ovrApp_HandleXrEvents(ovrApp* app);

void ovrFramebuffer_Acquire(ovrFramebuffer* frameBuffer);
void ovrFramebuffer_Release(ovrFramebuffer* frameBuffer);
void* ovrFramebuffer_SetCurrent(ovrFramebuffer* frameBuffer);

void ovrRenderer_Create(XrSession session, ovrRenderer* renderer, int width, int height);
void ovrRenderer_Destroy(ovrRenderer* renderer);
void ovrRenderer_MouseCursor(ovrRenderer* renderer, int x, int y, int sx, int sy);

// Picks the swapchain format we'll ask OpenXR for when using Vulkan. Cached after the first call,
// which needs a valid session. Returns VK_FORMAT_UNDEFINED if the format can't be determined yet.
VkFormat ovrFramebuffer_ChooseVulkanFormat(XrSession session);
// The VkImage of the given swapchain slot. VK_NULL_HANDLE if this isn't a Vulkan swapchain.
VkImage ovrFramebuffer_GetVulkanImage(ovrFramebuffer* frameBuffer, uint32_t index);
