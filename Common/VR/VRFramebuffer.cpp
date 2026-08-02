#include "VRFramebuffer.h"
#include "Common/VR/PPSSPPVRVulkan.h"

#if XR_USE_GRAPHICS_API_OPENGL || XR_USE_GRAPHICS_API_OPENGL_ES

#include "Common/GPU/OpenGL/GLCommon.h"

#endif

#if XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_GL_IMAGE XrSwapchainImageOpenGLESKHR
#define XR_GL_SWAPCHAIN XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR
#else
#define XR_GL_IMAGE XrSwapchainImageOpenGLKHR
#define XR_GL_SWAPCHAIN XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cassert>
#include <vector>

#if !defined(_WIN32)
#include <pthread.h>
#endif

double FromXrTime(const XrTime time) {
	return (time * 1e-9);
}

/*
================================================================================

ovrFramebuffer

================================================================================
*/


void ovrFramebuffer_Clear(ovrFramebuffer* frameBuffer) {
	frameBuffer->Width = 0;
	frameBuffer->Height = 0;
	frameBuffer->TextureSwapChainLength = 0;
	frameBuffer->TextureSwapChainIndex = 0;
	frameBuffer->ColorSwapChain.Handle = XR_NULL_HANDLE;
	frameBuffer->ColorSwapChain.Width = 0;
	frameBuffer->ColorSwapChain.Height = 0;
	frameBuffer->ColorSwapChainImage = NULL;
	frameBuffer->ColorFormat = 0;

	frameBuffer->GLDepthBuffers = NULL;
	frameBuffer->GLFrameBuffers = NULL;
	frameBuffer->Acquired = false;
}

#if XR_USE_GRAPHICS_API_OPENGL || XR_USE_GRAPHICS_API_OPENGL_ES

static const char* GlErrorString(GLenum error) {
	switch (error) {
	case GL_NO_ERROR:
		return "GL_NO_ERROR";
	case GL_INVALID_ENUM:
		return "GL_INVALID_ENUM";
	case GL_INVALID_VALUE:
		return "GL_INVALID_VALUE";
	case GL_INVALID_OPERATION:
		return "GL_INVALID_OPERATION";
	case GL_INVALID_FRAMEBUFFER_OPERATION:
		return "GL_INVALID_FRAMEBUFFER_OPERATION";
	case GL_OUT_OF_MEMORY:
		return "GL_OUT_OF_MEMORY";
	default:
		return "unknown";
	}
}

void GLCheckErrors(const char* file, int line) {
	for (int i = 0; i < 10; i++) {
		const GLenum error = glGetError();
		if (error == GL_NO_ERROR) {
			break;
		}
		ALOGE("GL error on line %s:%d %s", file, line, GlErrorString(error));
	}
}

#endif

#if XR_USE_GRAPHICS_API_OPENGL_ES || XR_USE_GRAPHICS_API_OPENGL

static bool ovrFramebuffer_CreateGL(XrSession session, ovrFramebuffer* frameBuffer, int width, int height) {
	frameBuffer->Width = width;
	frameBuffer->Height = height;

	XrSwapchainCreateInfo swapChainCreateInfo;
	memset(&swapChainCreateInfo, 0, sizeof(swapChainCreateInfo));
	swapChainCreateInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	swapChainCreateInfo.sampleCount = 1;
	swapChainCreateInfo.width = width;
	swapChainCreateInfo.height = height;
	swapChainCreateInfo.faceCount = 1;
	swapChainCreateInfo.mipCount = 1;
	swapChainCreateInfo.arraySize = 1;

	frameBuffer->ColorSwapChain.Width = swapChainCreateInfo.width;
	frameBuffer->ColorSwapChain.Height = swapChainCreateInfo.height;

	// Create the color swapchain.
	swapChainCreateInfo.format = GL_SRGB8_ALPHA8;
	frameBuffer->ColorFormat = swapChainCreateInfo.format;
	swapChainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	OXR(xrCreateSwapchain(session, &swapChainCreateInfo, &frameBuffer->ColorSwapChain.Handle));
	OXR(xrEnumerateSwapchainImages(frameBuffer->ColorSwapChain.Handle, 0, &frameBuffer->TextureSwapChainLength, NULL));
	frameBuffer->ColorSwapChainImage = malloc(frameBuffer->TextureSwapChainLength * sizeof(XR_GL_IMAGE));

	// Populate the swapchain image array.
	for (uint32_t i = 0; i < frameBuffer->TextureSwapChainLength; i++) {
		((XR_GL_IMAGE*)frameBuffer->ColorSwapChainImage)[i].type = XR_GL_SWAPCHAIN;
		((XR_GL_IMAGE*)frameBuffer->ColorSwapChainImage)[i].next = NULL;
	}
	OXR(xrEnumerateSwapchainImages(
			frameBuffer->ColorSwapChain.Handle,
			frameBuffer->TextureSwapChainLength,
			&frameBuffer->TextureSwapChainLength,
			(XrSwapchainImageBaseHeader*)frameBuffer->ColorSwapChainImage));

	frameBuffer->GLDepthBuffers = (GLuint*)malloc(frameBuffer->TextureSwapChainLength * sizeof(GLuint));
	frameBuffer->GLFrameBuffers = (GLuint*)malloc(frameBuffer->TextureSwapChainLength * sizeof(GLuint));
	for (uint32_t i = 0; i < frameBuffer->TextureSwapChainLength; i++) {
		const GLuint colorTexture = ((XR_GL_IMAGE*)frameBuffer->ColorSwapChainImage)[i].image;

		// Create the depth buffer.
		GL(glGenRenderbuffers(1, &frameBuffer->GLDepthBuffers[i]));
		GL(glBindRenderbuffer(GL_RENDERBUFFER, frameBuffer->GLDepthBuffers[i]));
		GL(glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height));
		GL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

		// Create the frame buffer.
		GL(glGenFramebuffers(1, &frameBuffer->GLFrameBuffers[i]));
		GL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, frameBuffer->GLFrameBuffers[i]));
		GL(glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, frameBuffer->GLDepthBuffers[i]));
		GL(glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, frameBuffer->GLDepthBuffers[i]));
		GL(glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture, 0));
		GL(GLenum renderFramebufferStatus = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER));
		GL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0));
		if (renderFramebufferStatus != GL_FRAMEBUFFER_COMPLETE) {
			ALOGE("Incomplete frame buffer object: %d", renderFramebufferStatus);
			return false;
		}
	}

	return true;
}

#endif

#if XR_USE_GRAPHICS_API_VULKAN

// In preference order. We deliberately prefer the UNORM formats: PPSSPP's presentation pass writes
// values that are already gamma-encoded, so letting the hardware apply the sRGB transfer function
// on top would wash the image out.
static const VkFormat vkPreferredFormats[] = {
	VK_FORMAT_R8G8B8A8_UNORM,
	VK_FORMAT_B8G8R8A8_UNORM,
	VK_FORMAT_R8G8B8A8_SRGB,
	VK_FORMAT_B8G8R8A8_SRGB,
};

static VkFormat vkChosenFormat = VK_FORMAT_UNDEFINED;

VkFormat ovrFramebuffer_ChooseVulkanFormat(XrSession session) {
	if (vkChosenFormat != VK_FORMAT_UNDEFINED) {
		return vkChosenFormat;
	}
	if (session == XR_NULL_HANDLE) {
		// No session yet, so we can't ask. The caller has to try again later.
		return VK_FORMAT_UNDEFINED;
	}

	uint32_t formatCount = 0;
	if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &formatCount, NULL)) || !formatCount) {
		return VK_FORMAT_UNDEFINED;
	}
	std::vector<int64_t> formats(formatCount);
	if (XR_FAILED(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()))) {
		return VK_FORMAT_UNDEFINED;
	}

	for (VkFormat preferred : vkPreferredFormats) {
		for (uint32_t i = 0; i < formatCount; i++) {
			if (formats[i] == (int64_t)preferred) {
				vkChosenFormat = preferred;
				ALOGV("OpenXR: Using Vulkan swapchain format %d", (int)vkChosenFormat);
				return vkChosenFormat;
			}
		}
	}

	// Nothing we recognize - take the runtime's first choice, which is its most preferred one.
	vkChosenFormat = (VkFormat)formats[0];
	ALOGE("OpenXR: No preferred Vulkan swapchain format supported, falling back to %d", (int)vkChosenFormat);
	return vkChosenFormat;
}

// Unlike the GL path, we don't build any Vulkan objects here - we only own the OpenXR swapchain
// and hand the images over. VulkanQueueRunner creates the views and framebuffers for them, using
// the same render pass it uses for the real backbuffer so that pipelines stay compatible.
static bool ovrFramebuffer_CreateVK(XrSession session, ovrFramebuffer* frameBuffer, int width, int height) {
	frameBuffer->Width = width;
	frameBuffer->Height = height;

	VkFormat format = ovrFramebuffer_ChooseVulkanFormat(session);
	if (format == VK_FORMAT_UNDEFINED) {
		ALOGE("OpenXR: No usable Vulkan swapchain format");
		return false;
	}

	XrSwapchainCreateInfo swapChainCreateInfo;
	memset(&swapChainCreateInfo, 0, sizeof(swapChainCreateInfo));
	swapChainCreateInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	swapChainCreateInfo.sampleCount = 1;
	swapChainCreateInfo.width = width;
	swapChainCreateInfo.height = height;
	swapChainCreateInfo.faceCount = 1;
	swapChainCreateInfo.mipCount = 1;
	// With single-pass stereo we render both eyes into the two array layers of one image, so the
	// runtime has to hand us an array swapchain. Only the first framebuffer is used in that case.
	swapChainCreateInfo.arraySize = IsVRVulkanStereo() ? 2 : 1;
	swapChainCreateInfo.format = format;
	swapChainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

	frameBuffer->ColorFormat = format;
	frameBuffer->ColorSwapChain.Width = swapChainCreateInfo.width;
	frameBuffer->ColorSwapChain.Height = swapChainCreateInfo.height;

	OXR(xrCreateSwapchain(session, &swapChainCreateInfo, &frameBuffer->ColorSwapChain.Handle));
	if (frameBuffer->ColorSwapChain.Handle == XR_NULL_HANDLE) {
		ALOGE("OpenXR: Failed to create the Vulkan swapchain");
		return false;
	}
	OXR(xrEnumerateSwapchainImages(frameBuffer->ColorSwapChain.Handle, 0, &frameBuffer->TextureSwapChainLength, NULL));
	frameBuffer->ColorSwapChainImage = malloc(frameBuffer->TextureSwapChainLength * sizeof(XrSwapchainImageVulkanKHR));

	// Populate the swapchain image array.
	for (uint32_t i = 0; i < frameBuffer->TextureSwapChainLength; i++) {
		((XrSwapchainImageVulkanKHR*)frameBuffer->ColorSwapChainImage)[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
		((XrSwapchainImageVulkanKHR*)frameBuffer->ColorSwapChainImage)[i].next = NULL;
	}
	OXR(xrEnumerateSwapchainImages(
			frameBuffer->ColorSwapChain.Handle,
			frameBuffer->TextureSwapChainLength,
			&frameBuffer->TextureSwapChainLength,
			(XrSwapchainImageBaseHeader*)frameBuffer->ColorSwapChainImage));

	return true;
}

VkImage ovrFramebuffer_GetVulkanImage(ovrFramebuffer* frameBuffer, uint32_t index) {
	if (!frameBuffer->ColorSwapChainImage || index >= frameBuffer->TextureSwapChainLength) {
		return VK_NULL_HANDLE;
	}
	return ((XrSwapchainImageVulkanKHR*)frameBuffer->ColorSwapChainImage)[index].image;
}

#else

VkFormat ovrFramebuffer_ChooseVulkanFormat(XrSession session) {
	return VK_FORMAT_UNDEFINED;
}

VkImage ovrFramebuffer_GetVulkanImage(ovrFramebuffer* frameBuffer, uint32_t index) {
	return VK_NULL_HANDLE;
}

#endif

void ovrFramebuffer_Destroy(ovrFramebuffer* frameBuffer) {
#if XR_USE_GRAPHICS_API_OPENGL_ES || XR_USE_GRAPHICS_API_OPENGL
	if (!VR_GetPlatformFlag(VR_PLATFORM_RENDERER_VULKAN)) {
		GL(glDeleteRenderbuffers(frameBuffer->TextureSwapChainLength, frameBuffer->GLDepthBuffers));
		GL(glDeleteFramebuffers(frameBuffer->TextureSwapChainLength, frameBuffer->GLFrameBuffers));
		free(frameBuffer->GLDepthBuffers);
		free(frameBuffer->GLFrameBuffers);
	}
#endif
	OXR(xrDestroySwapchain(frameBuffer->ColorSwapChain.Handle));
	free(frameBuffer->ColorSwapChainImage);

	ovrFramebuffer_Clear(frameBuffer);
}

void* ovrFramebuffer_SetCurrent(ovrFramebuffer* frameBuffer) {
	// On Vulkan there's nothing to bind here - the queue runner picks the framebuffer to render
	// into from the current eye/image index when it starts the backbuffer render pass.
#if XR_USE_GRAPHICS_API_OPENGL_ES || XR_USE_GRAPHICS_API_OPENGL
	if (!VR_GetPlatformFlag(VR_PLATFORM_RENDERER_VULKAN)) {
		GL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, frameBuffer->GLFrameBuffers[frameBuffer->TextureSwapChainIndex]));
	}
#endif
	return nullptr;
}

void ovrFramebuffer_Acquire(ovrFramebuffer* frameBuffer) {
	XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, NULL};
	OXR(xrAcquireSwapchainImage(frameBuffer->ColorSwapChain.Handle, &acquireInfo, &frameBuffer->TextureSwapChainIndex));

	XrSwapchainImageWaitInfo waitInfo;
	waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
	waitInfo.next = NULL;
	waitInfo.timeout = XR_INFINITE_DURATION;
	XrResult res = xrWaitSwapchainImage(frameBuffer->ColorSwapChain.Handle, &waitInfo);
	frameBuffer->Acquired = res == XR_SUCCESS;

	ovrFramebuffer_SetCurrent(frameBuffer);

	// On Vulkan the backbuffer render pass always clears, so there's nothing to do here.
#if XR_USE_GRAPHICS_API_OPENGL_ES || XR_USE_GRAPHICS_API_OPENGL
	if (!VR_GetPlatformFlag(VR_PLATFORM_RENDERER_VULKAN)) {
		GL(glEnable( GL_SCISSOR_TEST ));
		GL(glViewport( 0, 0, frameBuffer->Width, frameBuffer->Height ));
		GL(glClearColor( 0.0f, 0.0f, 0.0f, 1.0f ));
		GL(glScissor( 0, 0, frameBuffer->Width, frameBuffer->Height ));
		GL(glClear( GL_COLOR_BUFFER_BIT ));
		GL(glScissor( 0, 0, 0, 0 ));
		GL(glDisable( GL_SCISSOR_TEST ));
	}
#endif
}

void ovrFramebuffer_Release(ovrFramebuffer* frameBuffer) {
	if (frameBuffer->Acquired) {
		XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, NULL};
		OXR(xrReleaseSwapchainImage(frameBuffer->ColorSwapChain.Handle, &releaseInfo));
		frameBuffer->Acquired = false;

		// Clear the alpha channel, other way OpenXR would not transfer the framebuffer fully.
		// Vulkan has no equivalent of a channel-masked clear outside a render pass, so there we
		// instead tell the compositor that the layer is opaque (see VR_FinishFrame), which has
		// the same effect for free.
#if XR_USE_GRAPHICS_API_OPENGL_ES || XR_USE_GRAPHICS_API_OPENGL
		if (!VR_GetPlatformFlag(VR_PLATFORM_RENDERER_VULKAN)) {
			GL(glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE));
			GL(glClearColor(0.0f, 0.0f, 0.0f, 1.0f));
			GL(glClear(GL_COLOR_BUFFER_BIT));
			GL(glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
		}
#endif
	}
}

/*
================================================================================

ovrRenderer

================================================================================
*/

void ovrRenderer_Clear(ovrRenderer* renderer) {
	for (int i = 0; i < ovrMaxNumEyes; i++) {
		ovrFramebuffer_Clear(&renderer->FrameBuffer[i]);
	}
}

void ovrRenderer_Create(XrSession session, ovrRenderer* renderer, int width, int height) {
	for (int i = 0; i < ovrMaxNumEyes; i++) {
		if (VR_GetPlatformFlag(VR_PLATFORM_RENDERER_VULKAN)) {
#if XR_USE_GRAPHICS_API_VULKAN
			ovrFramebuffer_CreateVK(session, &renderer->FrameBuffer[i], width, height);
#endif
		} else {
#if XR_USE_GRAPHICS_API_OPENGL_ES || XR_USE_GRAPHICS_API_OPENGL
			ovrFramebuffer_CreateGL(session, &renderer->FrameBuffer[i], width, height);
#endif
		}
	}
}

void ovrRenderer_Destroy(ovrRenderer* renderer) {
	for (int i = 0; i < ovrMaxNumEyes; i++) {
		ovrFramebuffer_Destroy(&renderer->FrameBuffer[i]);
	}
}

void ovrRenderer_MouseCursor(ovrRenderer* renderer, int x, int y, int sx, int sy) {
	// The Vulkan path draws the cursor from inside the backbuffer render pass instead, see
	// GetVRVulkanCursorRect() - clears outside a render pass aren't a thing there.
#if XR_USE_GRAPHICS_API_OPENGL_ES || XR_USE_GRAPHICS_API_OPENGL
	if (!VR_GetPlatformFlag(VR_PLATFORM_RENDERER_VULKAN)) {
		GL(glEnable(GL_SCISSOR_TEST));
		GL(glScissor(x, y, sx, sy));
		GL(glViewport(x, y, sx, sy));
		GL(glClearColor(1.0f, 1.0f, 1.0f, 1.0f));
		GL(glClear(GL_COLOR_BUFFER_BIT));
		GL(glDisable(GL_SCISSOR_TEST));
	}
#endif
}

/*
================================================================================

ovrApp

================================================================================
*/

void ovrApp_Clear(ovrApp* app) {
	app->Focused = false;
	app->Instance = XR_NULL_HANDLE;
	app->Session = XR_NULL_HANDLE;
	memset(&app->ViewportConfig, 0, sizeof(XrViewConfigurationProperties));
	memset(&app->ViewConfigurationView, 0, ovrMaxNumEyes * sizeof(XrViewConfigurationView));
	app->SystemId = XR_NULL_SYSTEM_ID;
	app->HeadSpace = XR_NULL_HANDLE;
	app->StageSpace = XR_NULL_HANDLE;
	app->FakeStageSpace = XR_NULL_HANDLE;
	app->CurrentSpace = XR_NULL_HANDLE;
	app->SessionActive = false;
	app->SwapInterval = 1;
	memset(app->Layers, 0, sizeof(ovrCompositorLayer_Union) * ovrMaxLayerCount);
	app->LayerCount = 0;
	app->MainThreadTid = 0;
	app->RenderThreadTid = 0;

	ovrRenderer_Clear(&app->Renderer);
}

void ovrApp_Destroy(ovrApp* app) {
	ovrApp_Clear(app);
}


void ovrApp_HandleSessionStateChanges(ovrApp* app, XrSessionState state) {
	if (state == XR_SESSION_STATE_READY) {
		assert(app->SessionActive == false);

		XrSessionBeginInfo sessionBeginInfo;
		memset(&sessionBeginInfo, 0, sizeof(sessionBeginInfo));
		sessionBeginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
		sessionBeginInfo.next = NULL;
		sessionBeginInfo.primaryViewConfigurationType = app->ViewportConfig.viewConfigurationType;

		XrResult result;
		OXR(result = xrBeginSession(app->Session, &sessionBeginInfo));
		app->SessionActive = (result == XR_SUCCESS);

#ifdef ANDROID
		if (app->SessionActive && VR_GetPlatformFlag(VR_PLATFORM_EXTENSION_PERFORMANCE)) {
			XrPerfSettingsLevelEXT cpuPerfLevel = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
			XrPerfSettingsLevelEXT gpuPerfLevel = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;

			PFN_xrPerfSettingsSetPerformanceLevelEXT pfnPerfSettingsSetPerformanceLevelEXT = NULL;
			OXR(xrGetInstanceProcAddr(
					app->Instance,
					"xrPerfSettingsSetPerformanceLevelEXT",
					(PFN_xrVoidFunction*)(&pfnPerfSettingsSetPerformanceLevelEXT)));

			OXR(pfnPerfSettingsSetPerformanceLevelEXT(app->Session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, cpuPerfLevel));
			OXR(pfnPerfSettingsSetPerformanceLevelEXT(app->Session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, gpuPerfLevel));

			PFN_xrSetAndroidApplicationThreadKHR pfnSetAndroidApplicationThreadKHR = NULL;
			OXR(xrGetInstanceProcAddr(
					app->Instance,
					"xrSetAndroidApplicationThreadKHR",
					(PFN_xrVoidFunction*)(&pfnSetAndroidApplicationThreadKHR)));

			OXR(pfnSetAndroidApplicationThreadKHR(app->Session, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, app->MainThreadTid));
			OXR(pfnSetAndroidApplicationThreadKHR(app->Session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, app->RenderThreadTid));
		}
#endif
	} else if (state == XR_SESSION_STATE_STOPPING) {
		assert(app->SessionActive);

		OXR(xrEndSession(app->Session));
		app->SessionActive = false;
	}
}

int ovrApp_HandleXrEvents(ovrApp* app) {
	XrEventDataBuffer eventDataBuffer = {};
	int recenter = 0;

	// Poll for events
	for (;;) {
		XrEventDataBaseHeader* baseEventHeader = (XrEventDataBaseHeader*)(&eventDataBuffer);
		baseEventHeader->type = XR_TYPE_EVENT_DATA_BUFFER;
		baseEventHeader->next = NULL;
		XrResult r;
		OXR(r = xrPollEvent(app->Instance, &eventDataBuffer));
		if (r != XR_SUCCESS) {
			break;
		}

		switch (baseEventHeader->type) {
			case XR_TYPE_EVENT_DATA_EVENTS_LOST:
				ALOGV("xrPollEvent: received XR_TYPE_EVENT_DATA_EVENTS_LOST event");
				break;
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
				const XrEventDataInstanceLossPending* instance_loss_pending_event =
						(XrEventDataInstanceLossPending*)(baseEventHeader);
				ALOGV(
						"xrPollEvent: received XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING event: time %f",
						FromXrTime(instance_loss_pending_event->lossTime));
			} break;
			case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
				ALOGV("xrPollEvent: received XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED event");
				break;
			case XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT: {
				const XrEventDataPerfSettingsEXT* perf_settings_event =
						(XrEventDataPerfSettingsEXT*)(baseEventHeader);
				ALOGV(
						"xrPollEvent: received XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT event: type %d subdomain %d : level %d -> level %d",
						perf_settings_event->type,
						perf_settings_event->subDomain,
						perf_settings_event->fromLevel,
						perf_settings_event->toLevel);
			} break;
			case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
				XrEventDataReferenceSpaceChangePending* ref_space_change_event =
						(XrEventDataReferenceSpaceChangePending*)(baseEventHeader);
				ALOGV(
						"xrPollEvent: received XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING event: changed space: %d for session %p at time %f",
						ref_space_change_event->referenceSpaceType,
						(void*)ref_space_change_event->session,
						FromXrTime(ref_space_change_event->changeTime));
				recenter = 1;
			} break;
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
				const XrEventDataSessionStateChanged* session_state_changed_event =
						(XrEventDataSessionStateChanged*)(baseEventHeader);
				ALOGV(
						"xrPollEvent: received XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: %d for session %p at time %f",
						session_state_changed_event->state,
						(void*)session_state_changed_event->session,
						FromXrTime(session_state_changed_event->time));

				switch (session_state_changed_event->state) {
					case XR_SESSION_STATE_FOCUSED:
						app->Focused = true;
						break;
					case XR_SESSION_STATE_VISIBLE:
						app->Focused = false;
						break;
					case XR_SESSION_STATE_READY:
					case XR_SESSION_STATE_STOPPING:
						ovrApp_HandleSessionStateChanges(app, session_state_changed_event->state);
						break;
					default:
						break;
				}
				recenter = true;
			} break;
			default:
				ALOGV("xrPollEvent: Unknown event");
				break;
		}
	}
	return recenter;
}
