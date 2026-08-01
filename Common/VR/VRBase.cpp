#include "VRBase.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

static bool vr_platform[VR_PLATFORM_MAX];
static engine_t vr_engine;
int vr_initialized = 0;

void VR_Init( void* system, const char* name, int version ) {
	if (vr_initialized)
		return;

	if (!XRLoad()) {
		return;
	}

	ovrApp_Clear(&vr_engine.appState);

#ifdef ANDROID
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR;
	xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&xrInitializeLoaderKHR);
	if (xrInitializeLoaderKHR != NULL) {
		ovrJava* java = (ovrJava*)system;
		XrLoaderInitInfoAndroidKHR loaderInitializeInfo;
		memset(&loaderInitializeInfo, 0, sizeof(loaderInitializeInfo));
		loaderInitializeInfo.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
		loaderInitializeInfo.next = NULL;
		loaderInitializeInfo.applicationVM = java->Vm;
		loaderInitializeInfo.applicationContext = java->ActivityObject;
		xrInitializeLoaderKHR((XrLoaderInitInfoBaseHeaderKHR*)&loaderInitializeInfo);
	}
#endif

	std::vector<const char *> extensions;
	if (VR_GetPlatformFlag(VR_PLATFORM_RENDERER_VULKAN)) {
		extensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	} else {
#ifdef XR_USE_GRAPHICS_API_OPENGL_ES
		extensions.push_back(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
#endif
	}
	extensions.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
#ifdef ANDROID
	if (VR_GetPlatformFlag(VR_PLATFORM_EXTENSION_INSTANCE)) {
		extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
	}
	if (VR_GetPlatformFlag(VR_PLATFORM_EXTENSION_PASSTHROUGH)) {
		extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
	}
	if (VR_GetPlatformFlag(VR_PLATFORM_EXTENSION_PERFORMANCE)) {
		extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
		extensions.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
	}
#endif

	// Create the OpenXR instance.
	XrApplicationInfo appInfo;
	memset(&appInfo, 0, sizeof(appInfo));
	strcpy(appInfo.applicationName, name);
	strcpy(appInfo.engineName, name);
	appInfo.applicationVersion = version;
	appInfo.engineVersion = version;
	appInfo.apiVersion = XR_API_VERSION_1_0;

	XrInstanceCreateInfo instanceCreateInfo;
	memset(&instanceCreateInfo, 0, sizeof(instanceCreateInfo));
	instanceCreateInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
	instanceCreateInfo.next = NULL;
	instanceCreateInfo.createFlags = 0;
	instanceCreateInfo.applicationInfo = appInfo;
	instanceCreateInfo.enabledApiLayerCount = 0;
	instanceCreateInfo.enabledApiLayerNames = NULL;
	instanceCreateInfo.enabledExtensionCount = (uint32_t)extensions.size();
	instanceCreateInfo.enabledExtensionNames = extensions.data();

#ifdef ANDROID
	XrInstanceCreateInfoAndroidKHR instanceCreateInfoAndroid = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
	if (VR_GetPlatformFlag(VR_PLATFORM_EXTENSION_INSTANCE)) {
		ovrJava* java = (ovrJava*)system;
		instanceCreateInfoAndroid.applicationVM = java->Vm;
		instanceCreateInfoAndroid.applicationActivity = java->ActivityObject;
		instanceCreateInfo.next = (XrBaseInStructure*)&instanceCreateInfoAndroid;
	}
#endif

	XrResult initResult;
	OXR(initResult = xrCreateInstance(&instanceCreateInfo, &vr_engine.appState.Instance));
	if (initResult != XR_SUCCESS) {
		ALOGE("Failed to create XR instance: %d.", initResult);
		exit(1);
	}

	XRLoadInstanceFunctions(vr_engine.appState.Instance);

	XrInstanceProperties instanceInfo;
	instanceInfo.type = XR_TYPE_INSTANCE_PROPERTIES;
	instanceInfo.next = NULL;
	OXR(xrGetInstanceProperties(vr_engine.appState.Instance, &instanceInfo));
	ALOGV(
			"Runtime %s: Version : %u.%u.%u",
			instanceInfo.runtimeName,
			XR_VERSION_MAJOR(instanceInfo.runtimeVersion),
			XR_VERSION_MINOR(instanceInfo.runtimeVersion),
			XR_VERSION_PATCH(instanceInfo.runtimeVersion));

	XrSystemGetInfo systemGetInfo;
	memset(&systemGetInfo, 0, sizeof(systemGetInfo));
	systemGetInfo.type = XR_TYPE_SYSTEM_GET_INFO;
	systemGetInfo.next = NULL;
	systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	XrSystemId systemId;
	OXR(initResult = xrGetSystem(vr_engine.appState.Instance, &systemGetInfo, &systemId));
	if (initResult != XR_SUCCESS) {
		ALOGE("Failed to get system.");
		exit(1);
	}

	// Get the graphics requirements. The runtime is allowed to fail session creation if we skip this,
	// so it has to happen for whichever graphics API we're going to bind.
	if (VR_GetPlatformFlag(VR_PLATFORM_RENDERER_VULKAN)) {
		PFN_xrGetVulkanGraphicsRequirementsKHR pfnGetVulkanGraphicsRequirementsKHR = NULL;
		OXR(xrGetInstanceProcAddr(
				vr_engine.appState.Instance,
				"xrGetVulkanGraphicsRequirementsKHR",
				(PFN_xrVoidFunction*)(&pfnGetVulkanGraphicsRequirementsKHR)));

		XrGraphicsRequirementsVulkanKHR graphicsRequirements = {};
		graphicsRequirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
		if (pfnGetVulkanGraphicsRequirementsKHR) {
			OXR(pfnGetVulkanGraphicsRequirementsKHR(vr_engine.appState.Instance, systemId, &graphicsRequirements));
			ALOGV("OpenXR Vulkan API version range: %u.%u.%u - %u.%u.%u",
					XR_VERSION_MAJOR(graphicsRequirements.minApiVersionSupported),
					XR_VERSION_MINOR(graphicsRequirements.minApiVersionSupported),
					XR_VERSION_PATCH(graphicsRequirements.minApiVersionSupported),
					XR_VERSION_MAJOR(graphicsRequirements.maxApiVersionSupported),
					XR_VERSION_MINOR(graphicsRequirements.maxApiVersionSupported),
					XR_VERSION_PATCH(graphicsRequirements.maxApiVersionSupported));
		}
	} else {
#ifdef XR_USE_GRAPHICS_API_OPENGL_ES
		PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetOpenGLESGraphicsRequirementsKHR = NULL;
		OXR(xrGetInstanceProcAddr(
				vr_engine.appState.Instance,
				"xrGetOpenGLESGraphicsRequirementsKHR",
				(PFN_xrVoidFunction*)(&pfnGetOpenGLESGraphicsRequirementsKHR)));

		XrGraphicsRequirementsOpenGLESKHR graphicsRequirements = {};
		graphicsRequirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;
		OXR(pfnGetOpenGLESGraphicsRequirementsKHR(vr_engine.appState.Instance, systemId, &graphicsRequirements));
#endif
	}

#ifdef ANDROID
	vr_engine.appState.MainThreadTid = gettid();
#endif
	vr_engine.appState.SystemId = systemId;
	vr_initialized = 1;
}

void VR_Destroy( engine_t* engine ) {
	if (engine == &vr_engine) {
		xrDestroyInstance(engine->appState.Instance);
		ovrApp_Destroy(&engine->appState);
	}
}

void VR_Shutdown() {
	if (!vr_initialized) {
		return;
	}
	VR_LeaveVR(&vr_engine);
	VR_Destroy(&vr_engine);
	vr_initialized = 0;
}

void VR_EnterVR( engine_t* engine, XrGraphicsBindingVulkanKHR* graphicsBindingVulkan ) {

	if (engine->appState.Session) {
		ALOGE("VR_EnterVR called with existing session");
		return;
	}

	// Create the OpenXR Session.
	XrSessionCreateInfo sessionCreateInfo = {};
#ifdef ANDROID
	XrGraphicsBindingOpenGLESAndroidKHR graphicsBindingGL = {};
#elif XR_USE_GRAPHICS_API_OPENGL
	XrGraphicsBindingOpenGLWin32KHR graphicsBindingGL = {};
#endif
	memset(&sessionCreateInfo, 0, sizeof(sessionCreateInfo));
	if (VR_GetPlatformFlag(VR_PLATFORM_RENDERER_VULKAN)) {
		sessionCreateInfo.next = graphicsBindingVulkan;
	} else {
#ifdef ANDROID
		graphicsBindingGL.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
		graphicsBindingGL.next = NULL;
		graphicsBindingGL.display = eglGetCurrentDisplay();
		graphicsBindingGL.config = NULL;
		graphicsBindingGL.context = eglGetCurrentContext();
		sessionCreateInfo.next = &graphicsBindingGL;
#else
		//TODO:PCVR definition
#endif
	}
	sessionCreateInfo.type = XR_TYPE_SESSION_CREATE_INFO;
	sessionCreateInfo.createFlags = 0;
	sessionCreateInfo.systemId = engine->appState.SystemId;

	XrResult initResult;
	OXR(initResult = xrCreateSession(engine->appState.Instance, &sessionCreateInfo, &engine->appState.Session));
	if (initResult != XR_SUCCESS) {
		ALOGE("Failed to create XR session: %d.", initResult);
		exit(1);
	}

	// Create a space to the first path
	XrReferenceSpaceCreateInfo spaceCreateInfo = {};
	spaceCreateInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	spaceCreateInfo.poseInReferenceSpace.orientation.w = 1.0f;
	OXR(xrCreateReferenceSpace(engine->appState.Session, &spaceCreateInfo, &engine->appState.HeadSpace));
}

void VR_LeaveVR( engine_t* engine ) {
	if (engine->appState.Session) {
		OXR(xrDestroySpace(engine->appState.HeadSpace));
		// StageSpace is optional.
		if (engine->appState.StageSpace != XR_NULL_HANDLE) {
			OXR(xrDestroySpace(engine->appState.StageSpace));
		}
		OXR(xrDestroySpace(engine->appState.FakeStageSpace));
		engine->appState.CurrentSpace = XR_NULL_HANDLE;
		OXR(xrDestroySession(engine->appState.Session));
		engine->appState.Session = XR_NULL_HANDLE;
	}
}

/*
================================================================================

Vulkan interop (XR_KHR_vulkan_enable)

The runtime tells us which Vulkan instance/device extensions it needs and which physical device
it's going to composite from. Ignoring any of this is what made the previous attempt at a Vulkan
VR backend fail to produce an image, so all three are wired up here.

================================================================================
*/

// The XR_KHR_vulkan_enable "get extensions" calls return one space-separated string, not a list.
static void SplitExtensionString(const char* names, std::vector<std::string>* extensions) {
	const char* start = names;
	for (const char* p = names;; p++) {
		if (*p == ' ' || *p == '\0') {
			if (p > start) {
				extensions->push_back(std::string(start, p - start));
			}
			if (*p == '\0') {
				break;
			}
			start = p + 1;
		}
	}
}

static void VR_GetVulkanExtensions(bool device, std::vector<std::string>* extensions) {
	if (!vr_initialized || !VR_GetPlatformFlag(VR_PLATFORM_RENDERER_VULKAN)) {
		return;
	}

	// Both entry points have the same signature, so we can share the code.
	PFN_xrGetVulkanInstanceExtensionsKHR pfnGetExtensions = NULL;
	const char* name = device ? "xrGetVulkanDeviceExtensionsKHR" : "xrGetVulkanInstanceExtensionsKHR";
	OXR(xrGetInstanceProcAddr(vr_engine.appState.Instance, name, (PFN_xrVoidFunction*)(&pfnGetExtensions)));
	if (!pfnGetExtensions) {
		ALOGE("OpenXR: %s not available", name);
		return;
	}

	uint32_t size = 0;
	if (XR_FAILED(pfnGetExtensions(vr_engine.appState.Instance, vr_engine.appState.SystemId, 0, &size, NULL)) || !size) {
		return;
	}

	std::vector<char> names(size);
	if (XR_FAILED(pfnGetExtensions(vr_engine.appState.Instance, vr_engine.appState.SystemId, size, &size, names.data()))) {
		return;
	}
	names[size - 1] = '\0';  // Just in case - the runtime is supposed to null-terminate this.
	SplitExtensionString(names.data(), extensions);
}

void VR_GetVulkanInstanceExtensions(std::vector<std::string>* extensions) {
	VR_GetVulkanExtensions(false, extensions);
}

void VR_GetVulkanDeviceExtensions(std::vector<std::string>* extensions) {
	VR_GetVulkanExtensions(true, extensions);
}

VkPhysicalDevice VR_GetVulkanPhysicalDevice(VkInstance instance) {
	if (!vr_initialized || !VR_GetPlatformFlag(VR_PLATFORM_RENDERER_VULKAN)) {
		return VK_NULL_HANDLE;
	}

	PFN_xrGetVulkanGraphicsDeviceKHR pfnGetVulkanGraphicsDeviceKHR = NULL;
	OXR(xrGetInstanceProcAddr(
			vr_engine.appState.Instance,
			"xrGetVulkanGraphicsDeviceKHR",
			(PFN_xrVoidFunction*)(&pfnGetVulkanGraphicsDeviceKHR)));
	if (!pfnGetVulkanGraphicsDeviceKHR) {
		ALOGE("OpenXR: xrGetVulkanGraphicsDeviceKHR not available");
		return VK_NULL_HANDLE;
	}

	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	if (XR_FAILED(pfnGetVulkanGraphicsDeviceKHR(vr_engine.appState.Instance, vr_engine.appState.SystemId, instance, &physicalDevice))) {
		return VK_NULL_HANDLE;
	}
	return physicalDevice;
}

engine_t* VR_GetEngine( void ) {
	return &vr_engine;
}

bool VR_GetPlatformFlag(VRPlatformFlag flag) {
	return vr_platform[flag];
}

void VR_SetPlatformFLag(VRPlatformFlag flag, bool value) {
	vr_platform[flag] = value;
}
