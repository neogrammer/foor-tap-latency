#include "DebugOutput.h"
#include "GraphicsAPI_Vulkan.h"
#include "OpenXRDebugUtils.h"

// include xr linear algebra for XrVector and XrMatrix classes.
#include "xr_linear_algebra.h"
// Declare some useful operators for vectors:
XrVector3f operator-(XrVector3f a, XrVector3f b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
XrVector3f operator*(XrVector3f a, float b) {
    return {a.x * b, a.y * b, a.z * b};
}

#include <memory>
#include <string>

#include "TapLogger.h"
#include "AudioEngine.h"
#include "XrClock.h"
#include "XRInput.h"
#include "HandTracking.h"
#include "TextRenderer.h"
#include "HandPointer.h"
#include "TapExperiment.h"

class VulkanXRay {
private:
    struct RenderLayerInfo;
public:


    void WriteSessionCSV() {
        if (m_wroteCSV) return;
        if (!androidApp || !androidApp->activity->externalDataPath) return;
        std::string out = std::string(androidApp->activity->externalDataPath)
                          + "/taps_" + m_participantId + ".csv";
        if (m_exp.WriteCSV(out)) {
            m_wroteCSV = true;
            XR_TUT_LOG("Wrote " << m_exp.EventCount() << " events to " << out);
        } else {
            XR_TUT_LOG_ERROR("Failed to write tap CSV to " << out);
        }
    }

    VulkanXRay(GraphicsAPI_Type apiType)
            : m_apiType(apiType)
    {
        if (!CheckGraphicsAPI_TypeIsValidForPlatform(m_apiType)) {
            std::cout << "ERROR: The provided Graphics API is not valid for this platform." << std::endl;
            DEBUG_BREAK;
        }
    }
    ~VulkanXRay() = default;
    void Run()
    {

        CreateInstance();
        CreateDebugMessenger();

        GetInstanceProperties();
        GetSystemID();
        GetViewConfigurationViews();
        GetEnvironmentBlendModes();
        CreateSession();
        CreateReferenceSpace();
        CreateSwapchains();
        CreateResources();

        if (!m_input.Init(m_xrInstance, m_session)) {
            XR_TUT_LOG_ERROR("Input init failed. No taps will be recorded.");
        }
        if (kEnableHands && m_hands.Init(m_xrInstance, m_session)) {
            m_hands.EnableSimultaneous();
            XR_TUT_LOG("Hands available. Simultaneous mode: "
                               << (m_hands.Simultaneous() ? "on" : "off"));
        }

        m_clock.Init(m_xrInstance);
        XR_TUT_LOG("Clock conversion: " << (m_clock.Exact() ? "exact (KHR)" : "measured offset"));
        if (kEnableAudio && m_audio.Init()) {
            m_exp.SetAudio(&m_audio, &m_clock);
            XR_TUT_LOG("Audio up at " << m_audio.SampleRate() << " Hz");
        } else {
            XR_TUT_LOG_ERROR("Audio unavailable. Falling back to a visual beat, "
                             "which is quantised to the display frame.");
        }

        m_exp.Init(m_participantId, m_participantNum);

        while (m_applicationRunning) {
            PollSystemEvents();
            PollEvents();
            if (m_sessionRunning) {
                RenderFrame();
            }
        }

        //if (androidApp && androidApp->activity->externalDataPath) {
         //   std::string out = std::string(androidApp->activity->externalDataPath) + "/head_motion.csv";
//            if (m_headLog.WriteCSV(out)) {
//                XR_TUT_LOG("Wrote " << m_headLog.Count() << " head samples to " << out);
//            } else {
//                XR_TUT_LOG_ERROR("Failed to write head motion CSV to " << out);
//            }
        //}
        WriteSessionCSV();
        m_audio.Destroy();
        m_hands.Destroy();
        m_input.Destroy();


        DestroyResources();
        DestroySwapchains();
        DestroyReferenceSpace();
        DestroySession();


        DestroyDebugMessenger();
        DestroyInstance();

    }

public:
    // Stored pointer to the android_app structure from android_main().
    static android_app *androidApp;

    // Custom data structure that is used by PollSystemEvents().
    // Modified from https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/d6b6d7a10bdcf8d4fe806b4f415fde3dd5726878/src/tests/hello_xr/main.cpp#L133C1-L189C2
    struct AndroidAppState {
        ANativeWindow *nativeWindow = nullptr;
        bool resumed = false;
    };
    static AndroidAppState androidAppState;

    // Processes the next command from the Android OS. It updates AndroidAppState.
    static void AndroidAppHandleCmd(struct android_app *app, int32_t cmd) {
        AndroidAppState *appState = (AndroidAppState *)app->userData;

        switch (cmd) {
            // There is no APP_CMD_CREATE. The ANativeActivity creates the application thread from onCreate().
            // The application thread then calls android_main().
            case APP_CMD_START: {
                break;
            }
            case APP_CMD_RESUME: {
                appState->resumed = true;
                break;
            }
            case APP_CMD_PAUSE: {
                appState->resumed = false;
                break;
            }
            case APP_CMD_STOP: {
                break;
            }
            case APP_CMD_DESTROY: {
                appState->nativeWindow = nullptr;
                break;
            }
            case APP_CMD_INIT_WINDOW: {
                appState->nativeWindow = app->window;
                break;
            }
            case APP_CMD_TERM_WINDOW: {
                appState->nativeWindow = nullptr;
                break;
            }
        }
    }

private:
    void CreateInstance() {
        XrApplicationInfo AI;
        strncpy(AI.applicationName, "VulkanXRay", XR_MAX_APPLICATION_NAME_SIZE);
        AI.applicationVersion = 1;
        strncpy(AI.engineName, "OpenXR Engine", XR_MAX_ENGINE_NAME_SIZE);
        AI.engineVersion = 1;
        AI.apiVersion = XR_CURRENT_API_VERSION;

        m_instanceExtensions.push_back(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);

#ifdef XR_EXT_HAND_TRACKING_EXTENSION_NAME
        m_instanceExtensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
#endif
#ifdef XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME
        m_instanceExtensions.push_back(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME);
#endif

        // Converts XrTime to and from CLOCK_MONOTONIC, which is how the audio
        // clock and the runtime clock are made to agree. Without it the code
        // falls back to a measured offset and says so in the CSV header.
#ifdef XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME
        m_instanceExtensions.push_back(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
#endif
        // The runtime's own pointing ray. Optional: without it the ray is
        // derived from joint positions, which dwell makes workable anyway.
#ifdef XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME
        m_instanceExtensions.push_back(XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME);
#endif

// Ensure m_apiType is already defined when we call this line.
        m_instanceExtensions.push_back(GetGraphicsAPIInstanceExtensionString(m_apiType));

        // Get all the API Layers from the OpenXR runtime.
        uint32_t apiLayerCount = 0;
        std::vector<XrApiLayerProperties> apiLayerProperties;
        OPENXR_CHECK(xrEnumerateApiLayerProperties(0, &apiLayerCount, nullptr), "Failed to enumerate ApiLayerProperties.");
        apiLayerProperties.resize(apiLayerCount, {XR_TYPE_API_LAYER_PROPERTIES});
        OPENXR_CHECK(xrEnumerateApiLayerProperties(apiLayerCount, &apiLayerCount, apiLayerProperties.data()), "Failed to enumerate ApiLayerProperties.");

// Check the requested API layers against the ones from the OpenXR. If found add it to the Active API Layers.
        for (auto &requestLayer : m_apiLayers) {
            for (auto &layerProperty : apiLayerProperties) {
                // strcmp returns 0 if the strings match.
                if (strcmp(requestLayer.c_str(), layerProperty.layerName) != 0) {
                    continue;
                } else {
                    m_activeAPILayers.push_back(requestLayer.c_str());
                    break;
                }
            }
        }

// Get all the Instance Extensions from the OpenXR instance.
        uint32_t extensionCount = 0;
        std::vector<XrExtensionProperties> extensionProperties;
        OPENXR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr), "Failed to enumerate InstanceExtensionProperties.");
        extensionProperties.resize(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
        OPENXR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensionProperties.data()), "Failed to enumerate InstanceExtensionProperties.");

// Check the requested Instance Extensions against the ones from the OpenXR runtime.
// If an extension is found add it to Active Instance Extensions.
// Log error if the Instance Extension is not found.
        for (auto &requestedInstanceExtension : m_instanceExtensions) {
            bool found = false;
            for (auto &extensionProperty : extensionProperties) {
                // strcmp returns 0 if the strings match.
                if (strcmp(requestedInstanceExtension.c_str(), extensionProperty.extensionName) != 0) {
                    continue;
                } else {
                    m_activeInstanceExtensions.push_back(requestedInstanceExtension.c_str());
                    found = true;
                    break;
                }
            }
            if (!found) {
                XR_TUT_LOG_ERROR("Failed to find OpenXR instance extension: " << requestedInstanceExtension);
            }
        }

        XrInstanceCreateInfo instanceCI{XR_TYPE_INSTANCE_CREATE_INFO};
        instanceCI.createFlags = 0;
        instanceCI.applicationInfo = AI;
        instanceCI.enabledApiLayerCount = static_cast<uint32_t>(m_activeAPILayers.size());
        instanceCI.enabledApiLayerNames = m_activeAPILayers.data();
        instanceCI.enabledExtensionCount = static_cast<uint32_t>(m_activeInstanceExtensions.size());
        instanceCI.enabledExtensionNames = m_activeInstanceExtensions.data();
        OPENXR_CHECK(xrCreateInstance(&instanceCI, &m_xrInstance), "Failed to create Instance.");

    }
    void DestroyInstance() {

        OPENXR_CHECK(xrDestroyInstance(m_xrInstance), "Failed to destroy Instance.");

    }
    void CreateDebugMessenger() {

        // Check that "XR_EXT_debug_utils" is in the active Instance Extensions before creating an XrDebugUtilsMessengerEXT.
        if (IsStringInVector(m_activeInstanceExtensions, XR_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            m_debugUtilsMessenger = CreateOpenXRDebugUtilsMessenger(m_xrInstance);  // From OpenXRDebugUtils.h.
        }

    }
    void DestroyDebugMessenger() {

        // Check that "XR_EXT_debug_utils" is in the active Instance Extensions before destroying the XrDebugUtilsMessengerEXT.
        if (m_debugUtilsMessenger != XR_NULL_HANDLE) {
            DestroyOpenXRDebugUtilsMessenger(m_xrInstance, m_debugUtilsMessenger);  // From OpenXRDebugUtils.h.
        }

    }
    void GetInstanceProperties() {

        XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
        OPENXR_CHECK(xrGetInstanceProperties(m_xrInstance, &instanceProperties), "Failed to get InstanceProperties.");

        XR_TUT_LOG("OpenXR Runtime: " << instanceProperties.runtimeName << " - "
                                      << XR_VERSION_MAJOR(instanceProperties.runtimeVersion) << "."
                                      << XR_VERSION_MINOR(instanceProperties.runtimeVersion) << "."
                                      << XR_VERSION_PATCH(instanceProperties.runtimeVersion));

    }
    void GetSystemID() {

        // Get the XrSystemId from the instance and the supplied XrFormFactor.
        XrSystemGetInfo systemGI{XR_TYPE_SYSTEM_GET_INFO};
        systemGI.formFactor = m_formFactor;
        OPENXR_CHECK(xrGetSystem(m_xrInstance, &systemGI, &m_systemID), "Failed to get SystemID.");

// Get the System's properties for some general information about the hardware and the vendor.
        OPENXR_CHECK(xrGetSystemProperties(m_xrInstance, m_systemID, &m_systemProperties), "Failed to get SystemProperties.");

    }

    void CreateSession()
    {
        XrSessionCreateInfo sessionCI{XR_TYPE_SESSION_CREATE_INFO};
        m_graphicsAPI = std::make_unique<GraphicsAPI_Vulkan>(m_xrInstance, m_systemID);
        sessionCI.next = m_graphicsAPI->GetGraphicsBinding();
        sessionCI.createFlags = 0;
        sessionCI.systemId = m_systemID;

        OPENXR_CHECK(xrCreateSession(m_xrInstance, &sessionCI, &m_session), "Failed to create Session.");
    }
    void DestroySession()
    {
        OPENXR_CHECK(xrDestroySession(m_session), "Failed to destroy Session.");
    }

    void GetViewConfigurationViews()
    {
        // Gets the View Configuration Types. The first call gets the count of the array that will be returned. The next call fills out the array.
        uint32_t viewConfigurationCount = 0;
        OPENXR_CHECK(xrEnumerateViewConfigurations(m_xrInstance, m_systemID, 0, &viewConfigurationCount, nullptr), "Failed to enumerate View Configurations.");
        m_viewConfigurations.resize(viewConfigurationCount);
        OPENXR_CHECK(xrEnumerateViewConfigurations(m_xrInstance, m_systemID, viewConfigurationCount, &viewConfigurationCount, m_viewConfigurations.data()), "Failed to enumerate View Configurations.");

// Pick the first application supported View Configuration Type con supported by the hardware.
        for (const XrViewConfigurationType &viewConfiguration : m_applicationViewConfigurations) {
            if (std::find(m_viewConfigurations.begin(), m_viewConfigurations.end(), viewConfiguration) != m_viewConfigurations.end()) {
                m_viewConfiguration = viewConfiguration;
                break;
            }
        }
        if (m_viewConfiguration == XR_VIEW_CONFIGURATION_TYPE_MAX_ENUM) {
            std::cerr << "Failed to find a view configuration type. Defaulting to XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO." << std::endl;
            m_viewConfiguration = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        }

// Gets the View Configuration Views. The first call gets the count of the array that will be returned. The next call fills out the array.
        uint32_t viewConfigurationViewCount = 0;
        OPENXR_CHECK(xrEnumerateViewConfigurationViews(m_xrInstance, m_systemID, m_viewConfiguration, 0, &viewConfigurationViewCount, nullptr), "Failed to enumerate ViewConfiguration Views.");
        m_viewConfigurationViews.resize(viewConfigurationViewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        OPENXR_CHECK(xrEnumerateViewConfigurationViews(m_xrInstance, m_systemID, m_viewConfiguration, viewConfigurationViewCount, &viewConfigurationViewCount, m_viewConfigurationViews.data()), "Failed to enumerate ViewConfiguration Views.");

    }
    void CreateSwapchains()
    {
        // Get the supported swapchain formats as an array of int64_t and ordered by runtime preference.
        uint32_t formatCount = 0;
        OPENXR_CHECK(xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr), "Failed to enumerate Swapchain Formats");
        std::vector<int64_t> formats(formatCount);
        OPENXR_CHECK(xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data()), "Failed to enumerate Swapchain Formats");
        if (m_graphicsAPI->SelectDepthSwapchainFormat(formats) == 0) {
            std::cerr << "Failed to find depth format for Swapchain." << std::endl;
            DEBUG_BREAK;
        }

        //Resize the SwapchainInfo to match the number of view in the View Configuration.
        m_colorSwapchainInfos.resize(m_viewConfigurationViews.size());
        m_depthSwapchainInfos.resize(m_viewConfigurationViews.size());

        for (size_t i = 0; i < m_viewConfigurationViews.size(); i++) {

            SwapchainInfo &colorSwapchainInfo = m_colorSwapchainInfos[i];
            SwapchainInfo &depthSwapchainInfo = m_depthSwapchainInfos[i];

// Fill out an XrSwapchainCreateInfo structure and create an XrSwapchain.
// Color.
            XrSwapchainCreateInfo swapchainCI{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            swapchainCI.createFlags = 0;
            swapchainCI.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            swapchainCI.format = m_graphicsAPI->SelectColorSwapchainFormat(formats);                // Use GraphicsAPI to select the first compatible format.
            swapchainCI.sampleCount = m_viewConfigurationViews[i].recommendedSwapchainSampleCount;  // Use the recommended values from the XrViewConfigurationView.
            swapchainCI.width = m_viewConfigurationViews[i].recommendedImageRectWidth;
            swapchainCI.height = m_viewConfigurationViews[i].recommendedImageRectHeight;
            swapchainCI.faceCount = 1;
            swapchainCI.arraySize = 1;
            swapchainCI.mipCount = 1;
            OPENXR_CHECK(xrCreateSwapchain(m_session, &swapchainCI, &colorSwapchainInfo.swapchain), "Failed to create Color Swapchain");
            colorSwapchainInfo.swapchainFormat = swapchainCI.format;  // Save the swapchain format for later use.

// Depth.
            swapchainCI.createFlags = 0;
            swapchainCI.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            swapchainCI.format = m_graphicsAPI->SelectDepthSwapchainFormat(formats);                // Use GraphicsAPI to select the first compatible format.
            swapchainCI.sampleCount = m_viewConfigurationViews[i].recommendedSwapchainSampleCount;  // Use the recommended values from the XrViewConfigurationView.
            swapchainCI.width = m_viewConfigurationViews[i].recommendedImageRectWidth;
            swapchainCI.height = m_viewConfigurationViews[i].recommendedImageRectHeight;
            swapchainCI.faceCount = 1;
            swapchainCI.arraySize = 1;
            swapchainCI.mipCount = 1;
            OPENXR_CHECK(xrCreateSwapchain(m_session, &swapchainCI, &depthSwapchainInfo.swapchain), "Failed to create Depth Swapchain");
            depthSwapchainInfo.swapchainFormat = swapchainCI.format;  // Save the swapchain format for later use.


            // Get the number of images in the color/depth swapchain and allocate Swapchain image data via GraphicsAPI to store the returned array.
            uint32_t colorSwapchainImageCount = 0;
            OPENXR_CHECK(xrEnumerateSwapchainImages(colorSwapchainInfo.swapchain, 0, &colorSwapchainImageCount, nullptr), "Failed to enumerate Color Swapchain Images.");
            XrSwapchainImageBaseHeader *colorSwapchainImages = m_graphicsAPI->AllocateSwapchainImageData(colorSwapchainInfo.swapchain, GraphicsAPI::SwapchainType::COLOR, colorSwapchainImageCount);
            OPENXR_CHECK(xrEnumerateSwapchainImages(colorSwapchainInfo.swapchain, colorSwapchainImageCount, &colorSwapchainImageCount, colorSwapchainImages), "Failed to enumerate Color Swapchain Images.");

            uint32_t depthSwapchainImageCount = 0;
            OPENXR_CHECK(xrEnumerateSwapchainImages(depthSwapchainInfo.swapchain, 0, &depthSwapchainImageCount, nullptr), "Failed to enumerate Depth Swapchain Images.");
            XrSwapchainImageBaseHeader *depthSwapchainImages = m_graphicsAPI->AllocateSwapchainImageData(depthSwapchainInfo.swapchain, GraphicsAPI::SwapchainType::DEPTH, depthSwapchainImageCount);
            OPENXR_CHECK(xrEnumerateSwapchainImages(depthSwapchainInfo.swapchain, depthSwapchainImageCount, &depthSwapchainImageCount, depthSwapchainImages), "Failed to enumerate Depth Swapchain Images.");

            // Per image in the swapchains, fill out a GraphicsAPI::ImageViewCreateInfo structure and create a color/depth image view.
            for (uint32_t j = 0; j < colorSwapchainImageCount; j++) {
                GraphicsAPI::ImageViewCreateInfo imageViewCI;
                imageViewCI.image = m_graphicsAPI->GetSwapchainImage(colorSwapchainInfo.swapchain, j);
                imageViewCI.type = GraphicsAPI::ImageViewCreateInfo::Type::RTV;
                imageViewCI.view = GraphicsAPI::ImageViewCreateInfo::View::TYPE_2D;
                imageViewCI.format = colorSwapchainInfo.swapchainFormat;
                imageViewCI.aspect = GraphicsAPI::ImageViewCreateInfo::Aspect::COLOR_BIT;
                imageViewCI.baseMipLevel = 0;
                imageViewCI.levelCount = 1;
                imageViewCI.baseArrayLayer = 0;
                imageViewCI.layerCount = 1;
                colorSwapchainInfo.imageViews.push_back(m_graphicsAPI->CreateImageView(imageViewCI));
            }
            for (uint32_t j = 0; j < depthSwapchainImageCount; j++) {
                GraphicsAPI::ImageViewCreateInfo imageViewCI;
                imageViewCI.image = m_graphicsAPI->GetSwapchainImage(depthSwapchainInfo.swapchain, j);
                imageViewCI.type = GraphicsAPI::ImageViewCreateInfo::Type::DSV;
                imageViewCI.view = GraphicsAPI::ImageViewCreateInfo::View::TYPE_2D;
                imageViewCI.format = depthSwapchainInfo.swapchainFormat;
                imageViewCI.aspect = GraphicsAPI::ImageViewCreateInfo::Aspect::DEPTH_BIT;
                imageViewCI.baseMipLevel = 0;
                imageViewCI.levelCount = 1;
                imageViewCI.baseArrayLayer = 0;
                imageViewCI.layerCount = 1;
                depthSwapchainInfo.imageViews.push_back(m_graphicsAPI->CreateImageView(imageViewCI));
            }

        }
    }
    void DestroySwapchains()
    {
        // Per view in the view configuration:
        for (size_t i = 0; i < m_viewConfigurationViews.size(); i++) {
            SwapchainInfo &colorSwapchainInfo = m_colorSwapchainInfos[i];
            SwapchainInfo &depthSwapchainInfo = m_depthSwapchainInfos[i];

            // Destroy the color and depth image views from GraphicsAPI.
            for (void *&imageView : colorSwapchainInfo.imageViews) {
                m_graphicsAPI->DestroyImageView(imageView);
            }
            for (void *&imageView : depthSwapchainInfo.imageViews) {
                m_graphicsAPI->DestroyImageView(imageView);
            }

            // Free the Swapchain Image Data.
            m_graphicsAPI->FreeSwapchainImageData(colorSwapchainInfo.swapchain);
            m_graphicsAPI->FreeSwapchainImageData(depthSwapchainInfo.swapchain);

            // Destroy the swapchains.
            OPENXR_CHECK(xrDestroySwapchain(colorSwapchainInfo.swapchain), "Failed to destroy Color Swapchain");
            OPENXR_CHECK(xrDestroySwapchain(depthSwapchainInfo.swapchain), "Failed to destroy Depth Swapchain");
        }

    }


    void GetEnvironmentBlendModes()
    {
        // Retrieves the available blend modes. The first call gets the count of the array that will be returned. The next call fills out the array.
        uint32_t environmentBlendModeCount = 0;
        OPENXR_CHECK(xrEnumerateEnvironmentBlendModes(m_xrInstance, m_systemID, m_viewConfiguration, 0, &environmentBlendModeCount, nullptr), "Failed to enumerate EnvironmentBlend Modes.");
        m_environmentBlendModes.resize(environmentBlendModeCount);
        OPENXR_CHECK(xrEnumerateEnvironmentBlendModes(m_xrInstance, m_systemID, m_viewConfiguration, environmentBlendModeCount, &environmentBlendModeCount, m_environmentBlendModes.data()), "Failed to enumerate EnvironmentBlend Modes.");

// Pick the first application supported blend mode supported by the hardware.
        for (const XrEnvironmentBlendMode &environmentBlendMode : m_applicationEnvironmentBlendModes) {
            if (std::find(m_environmentBlendModes.begin(), m_environmentBlendModes.end(), environmentBlendMode) != m_environmentBlendModes.end()) {
                m_environmentBlendMode = environmentBlendMode;
                break;
            }
        }
        if (m_environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_MAX_ENUM) {
            XR_TUT_LOG_ERROR("Failed to find a compatible blend mode. Defaulting to XR_ENVIRONMENT_BLEND_MODE_OPAQUE.");
            m_environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        }
    }
    void CreateReferenceSpace()
    {
        // Fill out an XrReferenceSpaceCreateInfo structure and create a reference XrSpace, specifying a Local space with an identity pose as the origin.
        XrReferenceSpaceCreateInfo referenceSpaceCI{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        referenceSpaceCI.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        referenceSpaceCI.poseInReferenceSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        OPENXR_CHECK(xrCreateReferenceSpace(m_session, &referenceSpaceCI, &m_localSpace), "Failed to create ReferenceSpace.");
        XrReferenceSpaceCreateInfo viewSpaceCI{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        viewSpaceCI.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        viewSpaceCI.poseInReferenceSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
      //  OPENXR_CHECK(xrCreateReferenceSpace(m_session, &viewSpaceCI, &m_viewSpace), "Failed to create VIEW ReferenceSpace.");
    }
    void DestroyReferenceSpace()
    {
        // Destroy the reference XrSpace.
        OPENXR_CHECK(xrDestroySpace(m_localSpace), "Failed to destroy Space.")
       // if (m_viewSpace != XR_NULL_HANDLE) {
       //     OPENXR_CHECK(xrDestroySpace(m_viewSpace), "Failed to destroy VIEW Space.")
       // }
    }
    void RenderFrame()
    {
        // Get the XrFrameState for timing and rendering info.
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo frameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
        OPENXR_CHECK(xrWaitFrame(m_session, &frameWaitInfo, &frameState), "Failed to wait for XR Frame.");
       // if (m_viewSpace != XR_NULL_HANDLE) {
       //     XrSpaceLocation headLoc{XR_TYPE_SPACE_LOCATION};
       //     if (xrLocateSpace(m_viewSpace, m_localSpace, frameState.predictedDisplayTime, &headLoc) == XR_SUCCESS) {
       //         m_headLog.Record(frameState.predictedDisplayTime, headLoc);
       //     }
      //  }

        if (m_sessionRunning) {
            m_clock.Calibrate(frameState.predictedDisplayTime);
            m_input.Sync(frameState.predictedDisplayTime);
            if (m_hands.Available()) {
                m_hands.Update(m_localSpace, frameState.predictedDisplayTime);
            }
            // Point and HOLD drives the dialogs. Dwell rather than pinch:
            // pinching curls the index finger and swings the aim ray away in the
            // exact moment of the click. The held controller's trigger stays
            // wired as a fallback.
            const float dt = (m_lastFrameTime > 0)
                ? (float)((frameState.predictedDisplayTime - m_lastFrameTime) / 1000000LL) / 1000.0f
                : 0.0f;
            m_lastFrameTime = frameState.predictedDisplayTime;

            m_pointer.Update(m_hands, kTextZ);

            const float panelCY    = -m_viewHeightM + 1.52f;
            const float ratingRowY = -m_viewHeightM + 1.53f;

            int target = PanelUI::kTargetNone;
            {
                const HandPointer::State &p = m_pointer.Get();
                if (p.active) {
                    if (PanelUI::InGo(p.x, p.y, panelCY)) {
                        target = PanelUI::kTargetGo;
                    } else {
                        const int d = PanelUI::DigitAt(p.x, p.y, ratingRowY);
                        if (d) target = PanelUI::TargetDigit(d);
                    }
                }
            }
            m_pointer.Dwell(target, dt);

            const HandPointer::State &ptr = m_pointer.Get();
            bool go   = m_input.Confirm().justPressed;
            int  pick = 0;
            if (ptr.fired) {
                if (ptr.target == PanelUI::kTargetGo) go = true;
                else {
                    const int d = PanelUI::DigitOfTarget(ptr.target);
                    if (d) pick = d;
                }
            }

            m_exp.Update(frameState.predictedDisplayTime,
                         m_input.Tap().justPressed, m_input.Tap().changeTime,
                         go, pick);

            // Flush as soon as the session ends so a crash later costs nothing.
            if (m_exp.Finished()) WriteSessionCSV();
        }

// Tell the OpenXR compositor that the application is beginning the frame.
        XrFrameBeginInfo frameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        OPENXR_CHECK(xrBeginFrame(m_session, &frameBeginInfo), "Failed to begin the XR Frame.");

// Variables for rendering and layer composition.
        bool rendered = false;
        RenderLayerInfo renderLayerInfo;
        renderLayerInfo.predictedDisplayTime = frameState.predictedDisplayTime;

// Check that the session is active and that we should render.
        bool sessionActive = (m_sessionState == XR_SESSION_STATE_SYNCHRONIZED || m_sessionState == XR_SESSION_STATE_VISIBLE || m_sessionState == XR_SESSION_STATE_FOCUSED);
        if (sessionActive && frameState.shouldRender) {
            // Render the stereo image and associate one of swapchain images with the XrCompositionLayerProjection structure.
            rendered = RenderLayer(renderLayerInfo);
            if (rendered) {
                renderLayerInfo.layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&renderLayerInfo.layerProjection));
            }
        }

// Tell OpenXR that we are finished with this frame; specifying its display time, environment blending and layers.
        XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};
        frameEndInfo.displayTime = frameState.predictedDisplayTime;
        frameEndInfo.environmentBlendMode = m_environmentBlendMode;
        frameEndInfo.layerCount = static_cast<uint32_t>(renderLayerInfo.layers.size());
        frameEndInfo.layers = renderLayerInfo.layers.data();
        OPENXR_CHECK(xrEndFrame(m_session, &frameEndInfo), "Failed to end the XR Frame.");
    }


    static XrVector3f Accent() { return {0.25f, 0.55f, 0.95f}; }
    static XrVector3f Green()  { return {0.20f, 0.78f, 0.45f}; }
    static XrVector3f Dim()    { return {0.26f, 0.27f, 0.31f}; }
    static XrVector3f Ink()    { return {0.72f, 0.78f, 0.88f}; }
    static XrVector3f Hot()    { return {0.45f, 0.75f, 1.00f}; }

    // Text and buttons sit just in front of the panel.
    static constexpr float kTextZ  = -1.26f;
    static constexpr float kPanelZ = -1.30f;

    void Say(const char *s, float y, float h, XrVector3f c) {
        StrokeText::Draw(s, 0.0f, y, kTextZ, h, c,
            [&](XrPosef p, XrVector3f sc, XrVector3f col) { RenderCuboid(p, sc, col); });
    }
    void SayAt(const char *s, float x, float y, float h, XrVector3f c) {
        StrokeText::Draw(s, x, y, kTextZ, h, c,
            [&](XrPosef p, XrVector3f sc, XrVector3f col) { RenderCuboid(p, sc, col); });
    }

    void Pip(float x, float y, XrVector3f c) {
        RenderCuboid({{0.0f, 0.0f, 0.0f, 1.0f}, {x, y, -1.27f}},
                     {0.075f, 0.075f, 0.02f}, c);
    }

    // A dwell bar under a target. Fills left to right as the participant holds
    // their aim. This is the only feedback that tells them the hold is working,
    // so it has to be obvious.
    void DwellBar(float cx, float cy, float w, float p) {
        if (p <= 0.001f) return;
        const float full = w;
        const float fill = full * (p > 1.0f ? 1.0f : p);
        RenderCuboid({{0.0f, 0.0f, 0.0f, 1.0f}, {cx, cy, -1.255f}},
                     {full, 0.010f, 0.010f}, {0.22f, 0.24f, 0.30f});
        RenderCuboid({{0.0f, 0.0f, 0.0f, 1.0f},
                      {cx - full * 0.5f + fill * 0.5f, cy, -1.252f}},
                     {fill, 0.014f, 0.012f}, Hot());
    }

    // The GO button. Present on every screen that waits for the participant, so
    // there is exactly one thing to learn and it is always in the same corner.
    void DrawGo(float panelCY, const HandPointer::State &ptr) {
        const float gx = PanelUI::GoX(), gy = PanelUI::GoY(panelCY);
        const bool  hot = (ptr.target == PanelUI::kTargetGo);
        RenderCuboid({{0.0f, 0.0f, 0.0f, 1.0f}, {gx, gy, -1.285f}},
                     {PanelUI::kGoW, PanelUI::kGoH, 0.018f},
                     hot ? Accent() : XrVector3f{0.18f, 0.22f, 0.30f});
        SayAt("GO", gx, gy, 0.050f, hot ? XrVector3f{1.0f, 1.0f, 1.0f} : Ink());
        if (hot) DwellBar(gx, gy - PanelUI::kGoH * 0.5f - 0.022f,
                          PanelUI::kGoW * 0.85f, ptr.progress);
    }

    // Where the hand is aiming, and a short stub along the finger so the ray is
    // legible without drawing a full laser.
    void DrawPointer(const HandPointer::State &ptr) {
        if (!ptr.active) return;
        const float cs = 0.022f + 0.014f * (ptr.progress > 1.0f ? 1.0f : ptr.progress);
        RenderCuboid({{0.0f, 0.0f, 0.0f, 1.0f}, {ptr.x, ptr.y, kTextZ + 0.012f}},
                     {cs, cs, 0.012f}, (ptr.progress > 0.02f) ? Hot() : Accent());

        const float dx = ptr.x - ptr.rayX, dy = ptr.y - ptr.rayY, dz = kTextZ - ptr.rayZ;
        const float len = sqrtf(dx*dx + dy*dy + dz*dz);
        if (len < 0.05f) return;
        const float stub = 0.10f;
        RenderCuboid({{0.0f, 0.0f, 0.0f, 1.0f},
                      {ptr.rayX + dx / len * stub * 0.5f,
                       ptr.rayY + dy / len * stub * 0.5f,
                       ptr.rayZ + dz / len * stub * 0.5f}},
                     {0.008f, 0.008f, stub}, Dim());
    }

    // Everything is a box, including the letters. One pipeline, no textures.
    void DrawScene(const TapExperiment::RenderState &rs, const HandPointer::State &ptr) {
        using Phase = TapExperiment::Phase;
        const float floorY  = -m_viewHeightM;
        const float panelCY = floorY + 1.52f;
        const XrQuaternionf noRot{0.0f, 0.0f, 0.0f, 1.0f};
        char buf[40];

        // Floor
        RenderCuboid({noRot, {0.0f, floorY, 0.0f}}, {3.0f, 0.08f, 3.0f},
                     {0.22f, 0.24f, 0.27f});

        // Target pad. Where the controller goes. Lit the whole session so the
        // participant always knows where the foot belongs.
        {
            XrVector3f c = {0.30f, 0.34f, 0.40f};
            if (rs.feedbackFlash > 0.02f) {
                c = {0.20f + 0.80f * rs.feedbackFlash,
                     0.34f + 0.30f * rs.feedbackFlash,
                     0.40f - 0.25f * rs.feedbackFlash};
            }
            RenderCuboid({noRot, {0.0f, floorY + 0.06f, -0.55f}},
                         {0.34f, 0.03f, 0.34f}, c);
        }

        // Beat indicator. Never delayed. This is the timing reference.
        {
            const float sz = 0.16f + 0.10f * rs.beatPulse;
            RenderCuboid({noRot, {0.0f, floorY + 1.05f, -1.15f}}, {sz, sz, sz},
                         {0.25f + 0.60f * rs.beatPulse,
                          0.55f + 0.35f * rs.beatPulse, 0.95f});
        }

        // Dialog panel
        RenderCuboid({noRot, {0.0f, panelCY, kPanelZ}},
                     {PanelUI::kPanelW, PanelUI::kPanelH, 0.03f},
                     {0.14f, 0.15f, 0.18f});

        const float L1 = floorY + 1.63f;   // upper line
        const float L2 = floorY + 1.53f;   // middle line, also the rating row
        const float L3 = floorY + 1.44f;   // lower line

        bool wantsGo = true;               // screens that wait for the participant

        switch (rs.phase) {

        case Phase::Diagnostic:
            // The one unknown in the design, answerable in under a minute.
            Say("STEP ON PEDAL", L1, 0.050f, Ink());
            Pip(0.10f, L2, rs.leftPressed  ? Green() : Dim());
            Pip(0.22f, L2, rs.rightPressed ? Green() : Dim());
            Pip(0.34f, L2, rs.handsActive  ? Green() : Dim());
            Pip(0.46f, L2, rs.audioReady   ? Green() : Dim());
            Say("POINT AT GO AND HOLD", L3, 0.032f, Dim());
            break;

        case Phase::Welcome:
            Say("TAP WITH THE BEAT", L1, 0.050f, Ink());
            Say("5 BLOCKS",          L3, 0.038f, Dim());
            break;

        case Phase::PracticeIntro:
            Say("PRACTICE",     L1, 0.050f, Ink());
            Say("NOT RECORDED", L3, 0.038f, Dim());
            break;

        case Phase::Practice:
            Say("PRACTICE", L1, 0.050f, Dim());
            wantsGo = false;
            break;

        case Phase::ReadyCheck:
            snprintf(buf, sizeof(buf), "BLOCK %d OF %d", rs.blockNumber + 1, rs.blockTotal);
            Say(buf,              L1, 0.050f, Ink());
            Say("TAP EVERY BEAT", L3, 0.038f, Dim());
            break;

        case Phase::Countdown:
            snprintf(buf, sizeof(buf), "%d", rs.countdown);
            Say(buf, L2, 0.150f, Accent());
            wantsGo = false;
            break;

        case Phase::Block: {
            snprintf(buf, sizeof(buf), "BLOCK %d OF %d", rs.blockNumber, rs.blockTotal);
            Say(buf, L1, 0.042f, Dim());
            const int filled = (rs.tapsTarget > 0)
                ? (rs.tapsThisBlock * 10) / rs.tapsTarget : 0;
            for (int i = 0; i < 10; i++)
                Pip(-0.315f + 0.07f * i, L3, (i < filled) ? Green() : Dim());
            wantsGo = false;
            break;
        }

        case Phase::BlockComplete:
            snprintf(buf, sizeof(buf), "BLOCK %d DONE", rs.blockNumber);
            Say(buf, L1, 0.050f, Green());
            break;

        case Phase::Rating: {
            // Pinch a digit to select, then pinch GO. Hovered digit lights up so
            // aiming is visible before committing.
            Say("HOW RESPONSIVE?", L1, 0.046f, Ink());
            const int hover = PanelUI::DigitOfTarget(ptr.target);
            for (int v = 1; v <= 7; v++) {
                snprintf(buf, sizeof(buf), "%d", v);
                XrVector3f c = Dim();
                if (v == rs.rating) c = Accent();
                else if (v == hover) c = Hot();
                SayAt(buf, PanelUI::DigitX(v), L2, 0.055f, c);
            }
            if (hover) DwellBar(PanelUI::DigitX(hover), L2 - 0.042f, 0.085f, ptr.progress);
            // Anchors. Without these a participant has no idea which end of the
            // scale is which, and the rating is noise.
            SayAt("LAGGY",   PanelUI::DigitX(1), L3, 0.030f, Dim());
            SayAt("INSTANT", PanelUI::DigitX(7), L3, 0.030f, Dim());
            break;
        }

        case Phase::Rest:
            Say("REST", L1, 0.050f, Ink());
            break;

        case Phase::Done:
            Say("SESSION COMPLETE", L1, 0.046f, Green());
            Say("THANK YOU",        L3, 0.038f, Ink());
            wantsGo = false;
            break;
        }

        if (wantsGo) DrawGo(panelCY, ptr);
        DrawPointer(ptr);

        // Skeleton hands. Now load-bearing: they are how the participant aims.
        // Still never used for timing.
        if (m_hands.Available()) {
            for (int h = 0; h < 2; h++) {
                const HandTracking::Hand &hand = m_hands.GetHand(h);
                if (!hand.active) continue;
                for (int j = 0; j < hand.count; j++) {
                    const HandTracking::Joint &jt = hand.joints[j];
                    if (!jt.valid) continue;
                    const float sz = (jt.radius > 0.0f) ? jt.radius * 2.0f : 0.012f;
                    const bool tipJoint = (j == HandPointer::kIndexTip ||
                                           j == HandPointer::kThumbTip);
                    RenderCuboid({noRot, {jt.x, jt.y, jt.z}},
                                 {sz, sz, sz},
                                 tipJoint ? XrVector3f{0.95f, 0.88f, 0.70f}
                                          : XrVector3f{0.85f, 0.80f, 0.72f});
                }
            }
        }
    }



    bool RenderLayer(RenderLayerInfo& renderLayerInfo)
    {
        // Locate the views from the view configuration within the (reference) space at the display time.
        std::vector<XrView> views(m_viewConfigurationViews.size(), {XR_TYPE_VIEW});

        XrViewState viewState{XR_TYPE_VIEW_STATE};  // Will contain information on whether the position and/or orientation is valid and/or tracked.
        XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        viewLocateInfo.viewConfigurationType = m_viewConfiguration;
        viewLocateInfo.displayTime = renderLayerInfo.predictedDisplayTime;
        viewLocateInfo.space = m_localSpace;
        uint32_t viewCount = 0;
        XrResult result = xrLocateViews(m_session, &viewLocateInfo, &viewState, static_cast<uint32_t>(views.size()), &viewCount, views.data());
        if (result != XR_SUCCESS) {
            XR_TUT_LOG("Failed to locate Views.");
            return false;
        }

        // Resize the layer projection views to match the view count. The layer projection views are used in the layer projection.
        renderLayerInfo.layerProjectionViews.resize(viewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        TapExperiment::RenderState rs = m_exp.Render();
        rs.leftPressed  = m_input.AnyLeftPressed();
        rs.rightPressed = m_input.AnyRightPressed();
        rs.handsActive  = m_hands.Available() &&
                          (m_hands.GetHand(0).active || m_hands.GetHand(1).active);
        rs.simultaneous = m_hands.Simultaneous();

        // Per view in the view configuration:
        for (uint32_t i = 0; i < viewCount; i++) {
            SwapchainInfo &colorSwapchainInfo = m_colorSwapchainInfos[i];
            SwapchainInfo &depthSwapchainInfo = m_depthSwapchainInfos[i];

            // Acquire and wait for an image from the swapchains.
            // Get the image index of an image in the swapchains.
            // The timeout is infinite.
            uint32_t colorImageIndex = 0;
            uint32_t depthImageIndex = 0;
            XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            OPENXR_CHECK(xrAcquireSwapchainImage(colorSwapchainInfo.swapchain, &acquireInfo, &colorImageIndex), "Failed to acquire Image from the Color Swapchian");
            OPENXR_CHECK(xrAcquireSwapchainImage(depthSwapchainInfo.swapchain, &acquireInfo, &depthImageIndex), "Failed to acquire Image from the Depth Swapchian");

            XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = XR_INFINITE_DURATION;
            OPENXR_CHECK(xrWaitSwapchainImage(colorSwapchainInfo.swapchain, &waitInfo), "Failed to wait for Image from the Color Swapchain");
            OPENXR_CHECK(xrWaitSwapchainImage(depthSwapchainInfo.swapchain, &waitInfo), "Failed to wait for Image from the Depth Swapchain");

            // Get the width and height and construct the viewport and scissors.
            const uint32_t &width = m_viewConfigurationViews[i].recommendedImageRectWidth;
            const uint32_t &height = m_viewConfigurationViews[i].recommendedImageRectHeight;
            GraphicsAPI::Viewport viewport = {0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
            GraphicsAPI::Rect2D scissor = {{(int32_t)0, (int32_t)0}, {width, height}};
            float nearZ = 0.05f;
            float farZ = 100.0f;

            // Fill out the XrCompositionLayerProjectionView structure specifying the pose and fov from the view.
            // This also associates the swapchain image with this layer projection view.
            renderLayerInfo.layerProjectionViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            renderLayerInfo.layerProjectionViews[i].pose = views[i].pose;
            renderLayerInfo.layerProjectionViews[i].fov = views[i].fov;
            renderLayerInfo.layerProjectionViews[i].subImage.swapchain = colorSwapchainInfo.swapchain;
            renderLayerInfo.layerProjectionViews[i].subImage.imageRect.offset.x = 0;
            renderLayerInfo.layerProjectionViews[i].subImage.imageRect.offset.y = 0;
            renderLayerInfo.layerProjectionViews[i].subImage.imageRect.extent.width = static_cast<int32_t>(width);
            renderLayerInfo.layerProjectionViews[i].subImage.imageRect.extent.height = static_cast<int32_t>(height);
            renderLayerInfo.layerProjectionViews[i].subImage.imageArrayIndex = 0;  // Useful for multiview rendering.

            // Rendering code to clear the color and depth image views.
            m_graphicsAPI->BeginRendering();

            if (m_environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
                // VR mode use a background color.
                m_graphicsAPI->ClearColor(colorSwapchainInfo.imageViews[colorImageIndex], 0.17f, 0.17f, 0.17f, 1.00f);
            } else {
                // In AR mode make the background color black.
                m_graphicsAPI->ClearColor(colorSwapchainInfo.imageViews[colorImageIndex], 0.00f, 0.00f, 0.00f, 1.00f);
            }
            m_graphicsAPI->ClearDepth(depthSwapchainInfo.imageViews[depthImageIndex], 1.0f);


            m_graphicsAPI->SetRenderAttachments(&colorSwapchainInfo.imageViews[colorImageIndex], 1, depthSwapchainInfo.imageViews[depthImageIndex], width, height, m_pipeline);
            m_graphicsAPI->SetViewports(&viewport, 1);
            m_graphicsAPI->SetScissors(&scissor, 1);

               // Compute the view-projection transform.
             // All matrices (including OpenXR's) are column-major, right-handed.
            XrMatrix4x4f proj;
            XrMatrix4x4f_CreateProjectionFov(&proj, m_apiType, views[i].fov, nearZ, farZ);
            XrMatrix4x4f toView;
            XrVector3f scale1m{1.0f, 1.0f, 1.0f};
            XrMatrix4x4f_CreateTranslationRotationScale(&toView, &views[i].pose.position, &views[i].pose.orientation, &scale1m);
            XrMatrix4x4f view;
            XrMatrix4x4f_InvertRigidBody(&view, &toView);
            XrMatrix4x4f_Multiply(&cameraConstants.viewProj, &proj, &view);


            renderCuboidIndex = 0;
            DrawScene(rs, m_pointer.Get());


            m_graphicsAPI->EndRendering();

            // Give the swapchain image back to OpenXR, allowing the compositor to use the image.
            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            OPENXR_CHECK(xrReleaseSwapchainImage(colorSwapchainInfo.swapchain, &releaseInfo), "Failed to release Image back to the Color Swapchain");
            OPENXR_CHECK(xrReleaseSwapchainImage(depthSwapchainInfo.swapchain, &releaseInfo), "Failed to release Image back to the Depth Swapchain");
        }

        // Fill out the XrCompositionLayerProjection structure for usage with xrEndFrame().
        renderLayerInfo.layerProjection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
        renderLayerInfo.layerProjection.space = m_localSpace;
        renderLayerInfo.layerProjection.viewCount = static_cast<uint32_t>(renderLayerInfo.layerProjectionViews.size());
        renderLayerInfo.layerProjection.views = renderLayerInfo.layerProjectionViews.data();

        return true;


    }
    size_t renderCuboidIndex = 0;
    void RenderCuboid(XrPosef pose, XrVector3f scale, XrVector3f color)
    {
        XrMatrix4x4f_CreateTranslationRotationScale(&cameraConstants.model, &pose.position, &pose.orientation, &scale);

        XrMatrix4x4f_Multiply(&cameraConstants.modelViewProj, &cameraConstants.viewProj, &cameraConstants.model);
        cameraConstants.color = {color.x, color.y, color.z, 1.0};
        size_t offsetCameraUB = sizeof(CameraConstants) * renderCuboidIndex;

        m_graphicsAPI->SetPipeline(m_pipeline);

        m_graphicsAPI->SetBufferData(m_uniformBuffer_Camera, offsetCameraUB, sizeof(CameraConstants), &cameraConstants);
        m_graphicsAPI->SetDescriptor({0, m_uniformBuffer_Camera, GraphicsAPI::DescriptorInfo::Type::BUFFER, GraphicsAPI::DescriptorInfo::Stage::VERTEX, false, offsetCameraUB, sizeof(CameraConstants)});
        m_graphicsAPI->SetDescriptor({1, m_uniformBuffer_Normals, GraphicsAPI::DescriptorInfo::Type::BUFFER, GraphicsAPI::DescriptorInfo::Stage::VERTEX, false, 0, sizeof(normals)});

        m_graphicsAPI->UpdateDescriptors();

        m_graphicsAPI->SetVertexBuffers(&m_vertexBuffer, 1);
        m_graphicsAPI->SetIndexBuffer(m_indexBuffer);
        m_graphicsAPI->DrawIndexed(36);

        renderCuboidIndex++;
    }

    struct CameraConstants {
        XrMatrix4x4f viewProj;
        XrMatrix4x4f modelViewProj;
        XrMatrix4x4f model;
        XrVector4f color;
        XrVector4f pad1;
        XrVector4f pad2;
        XrVector4f pad3;
    };
    CameraConstants cameraConstants;
    XrVector4f normals[6] = {
            {1.00f, 0.00f, 0.00f, 0},
            {-1.00f, 0.00f, 0.00f, 0},
            {0.00f, 1.00f, 0.00f, 0},
            {0.00f, -1.00f, 0.00f, 0},
            {0.00f, 0.00f, 1.00f, 0},
            {0.00f, 0.0f, -1.00f, 0}};

    void CreateResources()
    {

        // Vertices for a 1x1x1 meter cube. (Left/Right, Top/Bottom, Front/Back)
        constexpr XrVector4f vertexPositions[] = {
                {+0.5f, +0.5f, +0.5f, 1.0f},
                {+0.5f, +0.5f, -0.5f, 1.0f},
                {+0.5f, -0.5f, +0.5f, 1.0f},
                {+0.5f, -0.5f, -0.5f, 1.0f},
                {-0.5f, +0.5f, +0.5f, 1.0f},
                {-0.5f, +0.5f, -0.5f, 1.0f},
                {-0.5f, -0.5f, +0.5f, 1.0f},
                {-0.5f, -0.5f, -0.5f, 1.0f}};

#define CUBE_FACE(V1, V2, V3, V4, V5, V6) vertexPositions[V1], vertexPositions[V2], vertexPositions[V3], vertexPositions[V4], vertexPositions[V5], vertexPositions[V6],

        XrVector4f cubeVertices[] = {
                CUBE_FACE(2, 1, 0, 2, 3, 1)  // -X
                CUBE_FACE(6, 4, 5, 6, 5, 7)  // +X
                CUBE_FACE(0, 1, 5, 0, 5, 4)  // -Y
                CUBE_FACE(2, 6, 7, 2, 7, 3)  // +Y
                CUBE_FACE(0, 4, 6, 0, 6, 2)  // -Z
                CUBE_FACE(1, 3, 7, 1, 7, 5)  // +Z
        };

        uint32_t cubeIndices[36] = {
                0, 1, 2, 3, 4, 5,        // -X
                6, 7, 8, 9, 10, 11,      // +X
                12, 13, 14, 15, 16, 17,  // -Y
                18, 19, 20, 21, 22, 23,  // +Y
                24, 25, 26, 27, 28, 29,  // -Z
                30, 31, 32, 33, 34, 35,  // +Z
        };

        m_vertexBuffer = m_graphicsAPI->CreateBuffer({GraphicsAPI::BufferCreateInfo::Type::VERTEX, sizeof(float) * 4, sizeof(cubeVertices), &cubeVertices});

        m_indexBuffer = m_graphicsAPI->CreateBuffer({GraphicsAPI::BufferCreateInfo::Type::INDEX, sizeof(uint32_t), sizeof(cubeIndices), &cubeIndices});

        size_t numberOfCuboids = 256;
        m_uniformBuffer_Camera = m_graphicsAPI->CreateBuffer({GraphicsAPI::BufferCreateInfo::Type::UNIFORM, 0, sizeof(CameraConstants) * numberOfCuboids, nullptr});
        m_uniformBuffer_Normals = m_graphicsAPI->CreateBuffer({GraphicsAPI::BufferCreateInfo::Type::UNIFORM, 0, sizeof(normals), &normals});

        if (m_apiType == VULKAN) {
            std::vector<char> vertexSource = ReadBinaryFile("shaders/VertexShader.spv", androidApp->activity->assetManager);
            m_vertexShader = m_graphicsAPI->CreateShader({GraphicsAPI::ShaderCreateInfo::Type::VERTEX, vertexSource.data(), vertexSource.size()});
            std::vector<char> fragmentSource = ReadBinaryFile("shaders/PixelShader.spv", androidApp->activity->assetManager);
            m_fragmentShader = m_graphicsAPI->CreateShader({GraphicsAPI::ShaderCreateInfo::Type::FRAGMENT, fragmentSource.data(), fragmentSource.size()});
        }

        GraphicsAPI::PipelineCreateInfo pipelineCI;
        pipelineCI.shaders = {m_vertexShader, m_fragmentShader};
        pipelineCI.vertexInputState.attributes = {{0, 0, GraphicsAPI::VertexType::VEC4, 0, "TEXCOORD"}};
        pipelineCI.vertexInputState.bindings = {{0, 0, 4 * sizeof(float)}};
        pipelineCI.inputAssemblyState = {GraphicsAPI::PrimitiveTopology::TRIANGLE_LIST, false};
        pipelineCI.rasterisationState = {false, false, GraphicsAPI::PolygonMode::FILL, GraphicsAPI::CullMode::BACK, GraphicsAPI::FrontFace::COUNTER_CLOCKWISE, false, 0.0f, 0.0f, 0.0f, 1.0f};
        pipelineCI.multisampleState = {1, false, 1.0f, 0xFFFFFFFF, false, false};
        pipelineCI.depthStencilState = {true, true, GraphicsAPI::CompareOp::LESS_OR_EQUAL, false, false, {}, {}, 0.0f, 1.0f};
        pipelineCI.colorBlendState = {false, GraphicsAPI::LogicOp::NO_OP, {{true, GraphicsAPI::BlendFactor::SRC_ALPHA, GraphicsAPI::BlendFactor::ONE_MINUS_SRC_ALPHA, GraphicsAPI::BlendOp::ADD, GraphicsAPI::BlendFactor::ONE, GraphicsAPI::BlendFactor::ZERO, GraphicsAPI::BlendOp::ADD, (GraphicsAPI::ColorComponentBit)15}}, {0.0f, 0.0f, 0.0f, 0.0f}};
        pipelineCI.colorFormats = {m_colorSwapchainInfos[0].swapchainFormat};
        pipelineCI.depthFormat = m_depthSwapchainInfos[0].swapchainFormat;
        pipelineCI.layout = {{0, nullptr, GraphicsAPI::DescriptorInfo::Type::BUFFER, GraphicsAPI::DescriptorInfo::Stage::VERTEX},
                             {1, nullptr, GraphicsAPI::DescriptorInfo::Type::BUFFER, GraphicsAPI::DescriptorInfo::Stage::VERTEX},
                             {2, nullptr, GraphicsAPI::DescriptorInfo::Type::BUFFER, GraphicsAPI::DescriptorInfo::Stage::FRAGMENT}};
        m_pipeline = m_graphicsAPI->CreatePipeline(pipelineCI);
    }
    void DestroyResources()
    {
        m_graphicsAPI->DestroyPipeline(m_pipeline);
        m_graphicsAPI->DestroyShader(m_fragmentShader);
        m_graphicsAPI->DestroyShader(m_vertexShader);
        m_graphicsAPI->DestroyBuffer(m_uniformBuffer_Camera);
        m_graphicsAPI->DestroyBuffer(m_uniformBuffer_Normals);
        m_graphicsAPI->DestroyBuffer(m_indexBuffer);
        m_graphicsAPI->DestroyBuffer(m_vertexBuffer);
    }

    void PollEvents()
    {
        // Poll OpenXR for a new event.
        XrEventDataBuffer eventData{XR_TYPE_EVENT_DATA_BUFFER};
        auto XrPollEvents = [&]() -> bool {
            eventData = {XR_TYPE_EVENT_DATA_BUFFER};
            return xrPollEvent(m_xrInstance, &eventData) == XR_SUCCESS;
        };

        while (XrPollEvents()) {
            switch (eventData.type) {
                // Log the number of lost events from the runtime.
                case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
                    XrEventDataEventsLost *eventsLost = reinterpret_cast<XrEventDataEventsLost *>(&eventData);
                    XR_TUT_LOG("OPENXR: Events Lost: " << eventsLost->lostEventCount);
                    break;
                }
                    // Log that an instance loss is pending and shutdown the application.
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
                    XrEventDataInstanceLossPending *instanceLossPending = reinterpret_cast<XrEventDataInstanceLossPending *>(&eventData);
                    XR_TUT_LOG("OPENXR: Instance Loss Pending at: " << instanceLossPending->lossTime);
                    m_sessionRunning = false;
                    m_applicationRunning = false;
                    break;
                }
                    // Log that the interaction profile has changed.
                case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
                    XrEventDataInteractionProfileChanged *interactionProfileChanged = reinterpret_cast<XrEventDataInteractionProfileChanged *>(&eventData);
                    XR_TUT_LOG("OPENXR: Interaction Profile changed for Session: " << interactionProfileChanged->session);
                    if (interactionProfileChanged->session != m_session) {
                        XR_TUT_LOG("XrEventDataInteractionProfileChanged for unknown Session");
                        break;
                    }
                    break;
                }
                    // Log that there's a reference space change pending.
                case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                    XrEventDataReferenceSpaceChangePending *referenceSpaceChangePending = reinterpret_cast<XrEventDataReferenceSpaceChangePending *>(&eventData);
                    XR_TUT_LOG("OPENXR: Reference Space Change pending for Session: " << referenceSpaceChangePending->session);
                    if (referenceSpaceChangePending->session != m_session) {
                        XR_TUT_LOG("XrEventDataReferenceSpaceChangePending for unknown Session");
                        break;
                    }
                    break;
                }
                    // Session State changes:
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    XrEventDataSessionStateChanged *sessionStateChanged = reinterpret_cast<XrEventDataSessionStateChanged *>(&eventData);
                    if (sessionStateChanged->session != m_session) {
                        XR_TUT_LOG("XrEventDataSessionStateChanged for unknown Session");
                        break;
                    }

                    if (sessionStateChanged->state == XR_SESSION_STATE_READY) {
                        // SessionState is ready. Begin the XrSession using the XrViewConfigurationType.
                        XrSessionBeginInfo sessionBeginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                        sessionBeginInfo.primaryViewConfigurationType = m_viewConfiguration;
                        OPENXR_CHECK(xrBeginSession(m_session, &sessionBeginInfo), "Failed to begin Session.");
                        m_sessionRunning = true;
                    }
                    if (sessionStateChanged->state == XR_SESSION_STATE_STOPPING) {
                        // SessionState is stopping. End the XrSession.
                        OPENXR_CHECK(xrEndSession(m_session), "Failed to end Session.");
                        m_sessionRunning = false;
                    }
                    if (sessionStateChanged->state == XR_SESSION_STATE_EXITING) {
                        // SessionState is exiting. Exit the application.
                        m_sessionRunning = false;
                        m_applicationRunning = false;
                    }
                    if (sessionStateChanged->state == XR_SESSION_STATE_LOSS_PENDING) {
                        // SessionState is loss pending. Exit the application.
                        // It's possible to try a reestablish an XrInstance and XrSession, but we will simply exit here.
                        m_sessionRunning = false;
                        m_applicationRunning = false;
                    }
                    // Store state for reference across the application.
                    m_sessionState = sessionStateChanged->state;
                    break;
                }
                default: {
                    break;
                }
            }
        }
    }

    void PollSystemEvents()
    {
        // Checks whether Android has requested that application should by destroyed.
        if (androidApp->destroyRequested != 0) {
            m_applicationRunning = false;
            return;
        }
        while (true) {
            // Poll and process the Android OS system events.
            struct android_poll_source *source = nullptr;
            int events = 0;
            // The timeout depends on whether the application is active.
            const int timeoutMilliseconds = (!androidAppState.resumed && !m_sessionRunning && androidApp->destroyRequested == 0) ? -1 : 0;
            if (ALooper_pollOnce(timeoutMilliseconds, nullptr, &events, (void**)&source) >= 0) {
                if (source != nullptr) {
                    source->process(androidApp, source);
                }
            } else {
                break;
            }
        }
    }
private:
    XrInstance m_xrInstance = {};
    std::vector<const char *> m_activeAPILayers = {};
    std::vector<const char *> m_activeInstanceExtensions = {};
    std::vector<std::string> m_apiLayers = {};
    std::vector<std::string> m_instanceExtensions = {};

    XrDebugUtilsMessengerEXT m_debugUtilsMessenger = {};

    XrFormFactor m_formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId m_systemID = {};
    XrSystemProperties m_systemProperties = {XR_TYPE_SYSTEM_PROPERTIES};

    GraphicsAPI_Type m_apiType = UNKNOWN;

    std::unique_ptr<GraphicsAPI> m_graphicsAPI = nullptr;

    XrSession m_session = XR_NULL_HANDLE;
    XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;
    bool m_applicationRunning = true;
    bool m_sessionRunning = false;

    std::vector<XrViewConfigurationType> m_applicationViewConfigurations = {XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO};
    std::vector<XrViewConfigurationType> m_viewConfigurations;
    XrViewConfigurationType m_viewConfiguration = XR_VIEW_CONFIGURATION_TYPE_MAX_ENUM;
    std::vector<XrViewConfigurationView> m_viewConfigurationViews;

    struct SwapchainInfo {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        int64_t swapchainFormat = 0;
        std::vector<void *> imageViews;
    };
    std::vector<SwapchainInfo> m_colorSwapchainInfos = {};
    std::vector<SwapchainInfo> m_depthSwapchainInfos = {};

    std::vector<XrEnvironmentBlendMode> m_applicationEnvironmentBlendModes = {XR_ENVIRONMENT_BLEND_MODE_OPAQUE, XR_ENVIRONMENT_BLEND_MODE_ADDITIVE};
    std::vector<XrEnvironmentBlendMode> m_environmentBlendModes = {};
    XrEnvironmentBlendMode m_environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_MAX_ENUM;

    XrSpace m_localSpace = XR_NULL_HANDLE;
//    XrSpace          m_viewSpace = XR_NULL_HANDLE;
//    HeadMotionLogger m_headLog;

    struct RenderLayerInfo {
        XrTime predictedDisplayTime;
        std::vector<XrCompositionLayerBaseHeader *> layers;
        XrCompositionLayerProjection layerProjection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::vector<XrCompositionLayerProjectionView> layerProjectionViews;
    };

    float m_viewHeightM = 1.5f;

    static constexpr bool kEnableHands = true;   // kill switch
    static constexpr bool kEnableAudio = true;   // kill switch
    XRInput       m_input;
    HandTracking  m_hands;
    HandPointer m_pointer;
    AudioEngine   m_audio;
    XrClock       m_clock;
    TapExperiment m_exp;
    std::string   m_participantId  = "P01";
    int           m_participantNum = 1;          // drives the Latin square
    bool          m_wroteCSV = false;

    void *m_vertexBuffer = nullptr;
    void *m_indexBuffer = nullptr;
    void *m_uniformBuffer_Camera = nullptr;
    void *m_uniformBuffer_Normals = nullptr;
    void *m_vertexShader = nullptr, *m_fragmentShader = nullptr;
    void *m_pipeline = nullptr;

    XrTime        m_lastFrameTime = 0;

};

void VulkanXRay_Main(GraphicsAPI_Type apiType) {
    DebugOutput debugOutput;  // This redirects std::cerr and std::cout to the IDE's output or Android Studio's logcat.
    XR_TUT_LOG("Vulkan XRay");
    VulkanXRay app(apiType);
    app.Run();
}

android_app *VulkanXRay::androidApp = nullptr;
VulkanXRay::AndroidAppState VulkanXRay::androidAppState = {};

void android_main(struct android_app *app) {
    // Allow interaction with JNI and the JVM on this thread.
    // https://developer.android.com/training/articles/perf-jni#threads
    JNIEnv *env;
    app->activity->vm->AttachCurrentThread(&env, nullptr);

    // https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#XR_KHR_loader_init
    // Load xrInitializeLoaderKHR() function pointer. On Android, the loader must be initialized with variables from android_app *.
    // Without this, there's is no loader and thus our function calls to OpenXR would fail.
    XrInstance m_xrInstance = XR_NULL_HANDLE;  // Dummy XrInstance variable for OPENXR_CHECK macro.
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
    OPENXR_CHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&xrInitializeLoaderKHR), "Failed to get InstanceProcAddr for xrInitializeLoaderKHR.");
    if (!xrInitializeLoaderKHR) {
        return;
    }

    // Fill out an XrLoaderInitInfoAndroidKHR structure and initialize the loader for Android.
    XrLoaderInitInfoAndroidKHR loaderInitializeInfoAndroid{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInitializeInfoAndroid.applicationVM = app->activity->vm;
    loaderInitializeInfoAndroid.applicationContext = app->activity->clazz;
    OPENXR_CHECK(xrInitializeLoaderKHR((XrLoaderInitInfoBaseHeaderKHR *)&loaderInitializeInfoAndroid), "Failed to initialize Loader for Android.");

    // Set userData and Callback for PollSystemEvents().
    app->userData = &VulkanXRay::androidAppState;
    app->onAppCmd = VulkanXRay::AndroidAppHandleCmd;

    VulkanXRay::androidApp = app;

    VulkanXRay_Main(VULKAN);
}

