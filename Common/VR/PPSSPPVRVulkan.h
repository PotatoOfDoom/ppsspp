#pragma once

// The Vulkan-typed part of the VR integration. Kept out of PPSSPPVR.h because that one is
// included all over the place (config, UI, core) and shouldn't drag in the Vulkan headers.
//
// Rendering-wise, the Vulkan VR path works just like the OpenGL one: instead of drawing the
// final image to the backbuffer, we draw it into an OpenXR swapchain image and let the runtime
// composite it. The difference is that with Vulkan we don't hand OpenXR a framebuffer object -
// we only get VkImages from it, and VulkanQueueRunner builds the views/framebuffers itself,
// exactly like it does for the real swapchain.

#include <string>
#include <vector>

#include "Common/GPU/Vulkan/VulkanLoader.h"

// True when VR is enabled and the Vulkan renderer is the one bound to the OpenXR session.
// Everything else in this header only does something meaningful when this returns true.
bool IsVRVulkanRenderer();

// True when the VR path renders both eyes in one pass, into the two layers of a single OpenXR
// swapchain image (multiview). The swapchain array size, the render pass type, the composition
// layers and the GPU's single-pass-stereo feature flag all key off this, so they cannot disagree
// with each other - a mismatch there means a rejected frame and a black headset. Decided as soon
// as the session exists, because the swapchains are created on the first VR frame, well before
// any game (and hence any GPU backend object) does.
bool IsVRVulkanStereo();

// XR_KHR_vulkan_enable requires the runtime to have a say in how the Vulkan instance and device
// are created, so these have to be consulted before creating them. Safe (no-ops) when not in VR.
void GetVRVulkanInstanceExtensions(std::vector<std::string> *extensions);
void GetVRVulkanDeviceExtensions(std::vector<std::string> *extensions);
// Returns VK_NULL_HANDLE if the runtime doesn't care or we're not in VR, in which case the
// normal device selection applies.
VkPhysicalDevice GetVRVulkanPhysicalDevice(VkInstance instance);

// Describes the OpenXR swapchains we render into. There's one swapchain per eye, all with the
// same size and format. Returns false until the VR renderer has been initialized.
struct VRVulkanSwapchainDesc {
	int width;
	int height;
	uint32_t imageCount;  // Per eye.
	VkFormat format;
};
bool GetVRVulkanSwapchainDesc(VRVulkanSwapchainDesc *desc);
VkImage GetVRVulkanSwapchainImage(int eye, uint32_t index);

// The format the OpenXR swapchains will be (or were) created with. Known as soon as the session
// exists, which is before the Vulkan swapchain is set up - the backbuffer render pass needs it.
VkFormat GetVRVulkanSwapchainFormat();

// The eye and image index currently being rendered to, i.e. which framebuffer a backbuffer
// render pass should target.
int GetVRVulkanCurrentEye();
uint32_t GetVRVulkanCurrentImageIndex();

// True between PreVRFrameRender and PostVRFrameRender, i.e. while we actually hold a swapchain
// image we're allowed to render into.
bool IsVRVulkanImageAcquired();

// While in VR, the "backbuffer" is an OpenXR swapchain image with its own per-eye size, which has
// nothing to do with the size of the window surface. Returns false when not rendering in VR, in
// which case the real backbuffer size applies.
bool GetVRVulkanBackbufferSize(int *width, int *height);

// The mouse cursor that the flat-screen VR modes draw as a white rectangle. On OpenGL this is a
// scissored clear; here we let the queue runner do it with vkCmdClearAttachments.
// Returns false if no cursor should be drawn this frame.
bool GetVRVulkanCursorRect(int *x, int *y, int *w, int *h);
