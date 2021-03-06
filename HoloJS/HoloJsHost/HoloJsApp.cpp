#include "pch.h"
#include "HoloJsApp.h"
#include <WindowsNumerics.h>
#include <angle_windowsstore.h>

using namespace HologramJS;

using namespace Windows::ApplicationModel::Core;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Foundation::Numerics;
using namespace Windows::Graphics::Display;
using namespace Windows::Graphics::Holographic;
using namespace Windows::Perception::Spatial;
using namespace Microsoft::WRL;
using namespace Platform;

#define GLEXT_ANGLE_HOLOGRAPHIC_MONO_VIEW_MATRIX 0x8FE6
#define GLEXT_ANGLE_HOLOGRAPHIC_MONO_PROJECTION_MATRIX 0x8FE9
#define GLEXT_ANGLE_HOLOGRAPHIC_LEFT_VIEW_MATRIX 0x8FEA
#define GLEXT_ANGLE_HOLOGRAPHIC_LEFT_PROJECTION_MATRIX 0x8FEB
#define GLEXT_ANGLE_HOLOGRAPHIC_RIGHT_VIEW_MATRIX 0x8FEC
#define GLEXT_ANGLE_HOLOGRAPHIC_RIGHT_PROJECTION_MATRIX 0x8FED

HoloJsAppView::HoloJsAppView(String ^ appUri)
    : m_WindowClosed(false),
      m_WindowVisible(true),
      m_EglDisplay(EGL_NO_DISPLAY),
      m_EglContext(EGL_NO_CONTEXT),
      m_EglSurface(EGL_NO_SURFACE),
      m_appUri(appUri)
{
    AutoStereoEnabled = true;
    ImageStabilizationEnabled = false;
    WorldOriginRelativePosition = float3(0, 0, -2);
    EnableWebArProtocolHandler = false;
    LaunchMode = HoloJsLaunchMode::AsActivated;
}

// The first method called when the IFrameworkView is being created.
void HoloJsAppView::Initialize(CoreApplicationView ^ applicationView)
{
    // Register event handlers for app lifecycle. This example includes Activated, so that we
    // can make the CoreWindow active and start rendering on the window.
    applicationView->Activated +=
        ref new TypedEventHandler<CoreApplicationView ^, IActivatedEventArgs ^>(this, &HoloJsAppView::OnActivated);

    // Logic for other event handlers could go here.
    // Information about the Suspending and Resuming event handlers can be found here:
    // http://msdn.microsoft.com/en-us/library/windows/apps/xaml/hh994930.aspx
}

void HoloJsAppView::ActivateWindowInCorrectContext(Windows::UI::Core::CoreWindow ^ window, bool isHolographicActivation)
{
    try {
        if ((isHolographicActivation && (LaunchMode == HoloJsLaunchMode::AsActivated)) ||
            (LaunchMode == HoloJsLaunchMode::Holographic)) {
			// Get the default SpatialLocator.
			SpatialLocator ^ mLocator = SpatialLocator::GetDefault();

			if (mLocator == nullptr) {
				InitializeEGL(window);
			}
			else {
				// Create a holographic space for the core window for the current view.
				mHolographicSpace = HolographicSpace::CreateForCoreWindow(window);

				// Create a stationary frame of reference.
				mStationaryReferenceFrame =
					mLocator->CreateStationaryFrameOfReferenceAtCurrentLocation(WorldOriginRelativePosition);

				// The HolographicSpace has been created, so EGL can be initialized in holographic mode.
				InitializeEGL(mHolographicSpace);
			}
        } else {
            // Initialize a flat view for the app
            InitializeEGL(window);
        }
    } catch (Platform::Exception ^ ex) {
        if (ex->HResult == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) {
            // Device does not support holographic spaces. Initialize EGL to use the CoreWindow instead.
            InitializeEGL(window);
        }
    }
}

// Called when the CoreWindow object is created (or re-created).
void HoloJsAppView::SetWindow(CoreWindow ^ window)
{
    window->VisibilityChanged += ref new TypedEventHandler<CoreWindow ^, VisibilityChangedEventArgs ^>(
        this, &HoloJsAppView::OnVisibilityChanged);

    window->Closed +=
        ref new TypedEventHandler<CoreWindow ^, CoreWindowEventArgs ^>(this, &HoloJsAppView::OnWindowClosed);

#ifndef HOLOJS_VS2015
    if (!Windows::Foundation::Metadata::ApiInformation::IsApiContractPresent(
            Platform::StringReference(L"Windows.Foundation.UniversalApiContract"), 4, 0)) {
        ActivateWindowInCorrectContext(window, true /* before RS3 all activations are considered holographic*/);
    }
#else
	ActivateWindowInCorrectContext(window, true /* before RS3 all activations are considered holographic*/);
#endif
}

void HoloJsAppView::LoadAndExecuteScript()
{
    // Create a host for the app now that we have a window
    if (m_holoScriptHost != nullptr) {
        m_holoScriptHost->Shutdown();
    }

    m_holoScriptHost = ref new HologramJS::HologramScriptHost();
    m_holoScriptHost->Initialize();

    m_holoScriptHost->EnableHolographicExperimental(mStationaryReferenceFrame, AutoStereoEnabled);

    eglQuerySurface(m_EglDisplay, m_EglSurface, EGL_WIDTH, &m_PanelWidth);
    eglQuerySurface(m_EglDisplay, m_EglSurface, EGL_HEIGHT, &m_PanelHeight);

    m_holoScriptHost->ResizeWindow(m_PanelWidth, m_PanelHeight);

    if (OnBeforeRun) {
        OnBeforeRun();
    }

    m_holoScriptHost->RunApp(m_appUri);
}

// Initializes scene resources
void HoloJsAppView::Load(Platform::String ^ entryPoint) { RecreateRenderer(); }

void HoloJsAppView::RecreateRenderer() {}

// This method is called after the window becomes active.
void HoloJsAppView::Run()
{
    Windows::Foundation::Numerics::float4x4 holographicViewMatrixMid;
    Windows::Foundation::Numerics::float4x4 holographicProjectionMatrixMid;
    Windows::Foundation::Numerics::float4x4 holographicViewMatrixLeft;
    Windows::Foundation::Numerics::float4x4 holographicProjectionMatrixLeft;
    Windows::Foundation::Numerics::float4x4 holographicViewMatrixRight;
    Windows::Foundation::Numerics::float4x4 holographicProjectionMatrixRight;

    while (!m_WindowClosed) {
        if (m_WindowVisible) {
            CoreWindow::GetForCurrentThread()->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);

            EGLint panelWidth = 0;
            EGLint panelHeight = 0;
            eglQuerySurface(m_EglDisplay, m_EglSurface, EGL_WIDTH, &panelWidth);
            eglQuerySurface(m_EglDisplay, m_EglSurface, EGL_HEIGHT, &panelHeight);

            if (m_PanelWidth != panelWidth || m_PanelHeight != panelHeight) {
                m_holoScriptHost->ResizeWindow(panelWidth, panelHeight);
                m_PanelHeight = panelHeight;
                m_PanelWidth = panelWidth;
            }

            glClear(GL_COLOR_BUFFER_BIT);
            glGetFloatv(GLEXT_ANGLE_HOLOGRAPHIC_MONO_VIEW_MATRIX, &holographicViewMatrixMid.m11);
            glGetFloatv(GLEXT_ANGLE_HOLOGRAPHIC_MONO_PROJECTION_MATRIX, &holographicProjectionMatrixMid.m11);
            glGetFloatv(GLEXT_ANGLE_HOLOGRAPHIC_LEFT_VIEW_MATRIX, &holographicViewMatrixLeft.m11);
            glGetFloatv(GLEXT_ANGLE_HOLOGRAPHIC_LEFT_PROJECTION_MATRIX, &holographicProjectionMatrixLeft.m11);
            glGetFloatv(GLEXT_ANGLE_HOLOGRAPHIC_RIGHT_VIEW_MATRIX, &holographicViewMatrixRight.m11);
            glGetFloatv(GLEXT_ANGLE_HOLOGRAPHIC_RIGHT_PROJECTION_MATRIX, &holographicProjectionMatrixRight.m11);

            m_holoScriptHost->VSync(holographicViewMatrixMid,
                                    holographicProjectionMatrixMid,
                                    holographicViewMatrixLeft,
                                    holographicProjectionMatrixLeft,
                                    holographicViewMatrixRight,
                                    holographicProjectionMatrixRight);

            // The call to eglSwapBuffers might not be successful (e.g. due to Device Lost)
            // If the call fails, then we must reinitialize EGL and the GL resources.
            if (eglSwapBuffers(m_EglDisplay, m_EglSurface) != GL_TRUE) {
                CleanupEGL();

                if (mHolographicSpace != nullptr) {
                    InitializeEGL(mHolographicSpace);
                } else {
                    InitializeEGL(CoreWindow::GetForCurrentThread());
                }

                RecreateRenderer();
            }
        } else {
            CoreWindow::GetForCurrentThread()->Dispatcher->ProcessEvents(
                CoreProcessEventsOption::ProcessOneAndAllPending);
        }
    }

    CleanupEGL();
}

// Terminate events do not cause Uninitialize to be called. It will be called if your IFrameworkView
// class is torn down while the app is in the foreground.
void HoloJsAppView::Uninitialize() {}

// Application lifecycle event handler.
void HoloJsAppView::OnActivated(CoreApplicationView ^ applicationView, IActivatedEventArgs ^ args)
{
#ifndef HOLOJS_VS2015
    if (Windows::Foundation::Metadata::ApiInformation::IsApiContractPresent(
            Platform::StringReference(L"Windows.Foundation.UniversalApiContract"), 4, 0)) {
        auto isHolographicActivation =
            Windows::ApplicationModel::Preview::Holographic::HolographicApplicationPreview::IsHolographicActivation(
                args);

        // Activate the window in the correct context, depending on where was the app activate from and launch settings
        // set by the app itself
        ActivateWindowInCorrectContext(applicationView->CoreWindow, isHolographicActivation);
    }
#endif

    // On protocol activation, use the URI from the activation as the app URI
    if (args->Kind == ActivationKind::Protocol && EnableWebArProtocolHandler) {
        ProtocolActivatedEventArgs ^ eventArgs = dynamic_cast<ProtocolActivatedEventArgs ^>(args);

        std::wstring appUri = eventArgs->Uri->RawUri->Data();
        if (appUri.find(L"web-ar:") == 0) {
            appUri.replace(0, wcslen(L"web-ar:"), L"");
        }

        m_appUri = ref new String(appUri.data());
    }

    LoadAndExecuteScript();
    // Run() won't start until the CoreWindow is activated.
    CoreWindow::GetForCurrentThread()->Activate();
}

// Window event handlers.
void HoloJsAppView::OnVisibilityChanged(CoreWindow ^ sender, VisibilityChangedEventArgs ^ args)
{
    m_WindowVisible = args->Visible;
}

void HoloJsAppView::OnWindowClosed(CoreWindow ^ sender, CoreWindowEventArgs ^ args) { m_WindowClosed = true; }

void HoloJsAppView::InitializeEGL(Windows::UI::Core::CoreWindow ^ window) { HoloJsAppView::InitializeEGLInner(window); }

void HoloJsAppView::InitializeEGL(Windows::Graphics::Holographic::HolographicSpace ^ holographicSpace)
{
    HoloJsAppView::InitializeEGLInner(holographicSpace);
}

void HoloJsAppView::InitializeEGLInner(Platform::Object ^ windowBasis)
{
    const EGLint configAttributes[] = {EGL_RED_SIZE,
                                       8,
                                       EGL_GREEN_SIZE,
                                       8,
                                       EGL_BLUE_SIZE,
                                       8,
                                       EGL_ALPHA_SIZE,
                                       8,
                                       EGL_DEPTH_SIZE,
                                       8,
                                       EGL_STENCIL_SIZE,
                                       8,
                                       EGL_NONE};

    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};

    const EGLint surfaceAttributes[] = {// EGL_ANGLE_SURFACE_RENDER_TO_BACK_BUFFER is part of the same optimization as
                                        // EGL_ANGLE_DISPLAY_ALLOW_RENDER_TO_BACK_BUFFER (see above). If you have
                                        // compilation issues with it then please update your Visual Studio templates.
                                        EGL_ANGLE_SURFACE_RENDER_TO_BACK_BUFFER,
                                        EGL_TRUE,
                                        EGL_NONE};

    const EGLint defaultDisplayAttributes[] = {
        // These are the default display attributes, used to request ANGLE's D3D11 renderer.
        // eglInitialize will only succeed with these attributes if the hardware supports D3D11 Feature Level 10_0+.
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,
        EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,

        // EGL_ANGLE_DISPLAY_ALLOW_RENDER_TO_BACK_BUFFER is an optimization that can have large performance benefits on
        // mobile devices. Its syntax is subject to change, though. Please update your Visual Studio templates if you
        // experience compilation issues with it.
        EGL_ANGLE_DISPLAY_ALLOW_RENDER_TO_BACK_BUFFER,
        EGL_TRUE,

        // EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE is an option that enables ANGLE to automatically call
        // the IDXGIDevice3::Trim method on behalf of the application when it gets suspended.
        // Calling IDXGIDevice3::Trim when an application is suspended is a Windows Store application certification
        // requirement.
        EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,
        EGL_TRUE,
        EGL_NONE,
    };

    const EGLint fl9_3DisplayAttributes[] = {
        // These can be used to request ANGLE's D3D11 renderer, with D3D11 Feature Level 9_3.
        // These attributes are used if the call to eglInitialize fails with the default display attributes.
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,
        EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
        EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE,
        9,
        EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE,
        3,
        EGL_ANGLE_DISPLAY_ALLOW_RENDER_TO_BACK_BUFFER,
        EGL_TRUE,
        EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,
        EGL_TRUE,
        EGL_NONE,
    };

    const EGLint warpDisplayAttributes[] = {
        // These attributes can be used to request D3D11 WARP.
        // They are used if eglInitialize fails with both the default display attributes and the 9_3 display attributes.
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,
        EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
        EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE,
        EGL_PLATFORM_ANGLE_DEVICE_TYPE_WARP_ANGLE,
        EGL_ANGLE_DISPLAY_ALLOW_RENDER_TO_BACK_BUFFER,
        EGL_TRUE,
        EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,
        EGL_TRUE,
        EGL_NONE,
    };

    EGLConfig config = NULL;

    // eglGetPlatformDisplayEXT is an alternative to eglGetDisplay. It allows us to pass in display attributes, used to
    // configure D3D11.
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!eglGetPlatformDisplayEXT) {
        throw Exception::CreateException(E_FAIL, L"Failed to get function eglGetPlatformDisplayEXT");
    }

    //
    // To initialize the display, we make three sets of calls to eglGetPlatformDisplayEXT and eglInitialize, with
    // varying parameters passed to eglGetPlatformDisplayEXT: 1) The first calls uses "defaultDisplayAttributes" as a
    // parameter. This corresponds to D3D11 Feature Level 10_0+. 2) If eglInitialize fails for step 1 (e.g. because
    // 10_0+ isn't supported by the default GPU), then we try again
    //    using "fl9_3DisplayAttributes". This corresponds to D3D11 Feature Level 9_3.
    // 3) If eglInitialize fails for step 2 (e.g. because 9_3+ isn't supported by the default GPU), then we try again
    //    using "warpDisplayAttributes".  This corresponds to D3D11 Feature Level 11_0 on WARP, a D3D11 software
    //    rasterizer.
    //

    // This tries to initialize EGL to D3D11 Feature Level 10_0+. See above comment for details.
    m_EglDisplay = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, defaultDisplayAttributes);
    if (m_EglDisplay == EGL_NO_DISPLAY) {
        throw Exception::CreateException(E_FAIL, L"Failed to get EGL display");
    }

    if (eglInitialize(m_EglDisplay, NULL, NULL) == EGL_FALSE) {
        // This tries to initialize EGL to D3D11 Feature Level 9_3, if 10_0+ is unavailable (e.g. on some mobile
        // devices).
        m_EglDisplay = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, fl9_3DisplayAttributes);
        if (m_EglDisplay == EGL_NO_DISPLAY) {
            throw Exception::CreateException(E_FAIL, L"Failed to get EGL display");
        }

        if (eglInitialize(m_EglDisplay, NULL, NULL) == EGL_FALSE) {
            // This initializes EGL to D3D11 Feature Level 11_0 on WARP, if 9_3+ is unavailable on the default GPU.
            m_EglDisplay =
                eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, warpDisplayAttributes);
            if (m_EglDisplay == EGL_NO_DISPLAY) {
                throw Exception::CreateException(E_FAIL, L"Failed to get EGL display");
            }

            if (eglInitialize(m_EglDisplay, NULL, NULL) == EGL_FALSE) {
                // If all of the calls to eglInitialize returned EGL_FALSE then an error has occurred.
                throw Exception::CreateException(E_FAIL, L"Failed to initialize EGL");
            }
        }
    }

    EGLint numConfigs = 0;
    if ((eglChooseConfig(m_EglDisplay, configAttributes, &config, 1, &numConfigs) == EGL_FALSE) || (numConfigs == 0)) {
        throw Exception::CreateException(E_FAIL, L"Failed to choose first EGLConfig");
    }

    // Create a PropertySet and initialize with the EGLNativeWindowType.
    PropertySet ^ surfaceCreationProperties = ref new PropertySet();
    surfaceCreationProperties->Insert(ref new String(EGLNativeWindowTypeProperty), windowBasis);
    if (mStationaryReferenceFrame != nullptr) {
        surfaceCreationProperties->Insert(ref new String(EGLBaseCoordinateSystemProperty), mStationaryReferenceFrame);
        surfaceCreationProperties->Insert(ref new String(EGLAutomaticStereoRenderingProperty),
                                          PropertyValue::CreateBoolean(AutoStereoEnabled));
        surfaceCreationProperties->Insert(ref new String(EGLAutomaticDepthBasedImageStabilizationProperty),
                                          PropertyValue::CreateBoolean(ImageStabilizationEnabled));
    }

    // You can configure the surface to render at a lower resolution and be scaled up to
    // the full window size. This scaling is often free on mobile hardware.
    //
    // One way to configure the SwapChainPanel is to specify precisely which resolution it should render at.
    // Size customRenderSurfaceSize = Size(800, 600);
    // surfaceCreationProperties->Insert(ref new String(EGLRenderSurfaceSizeProperty),
    // PropertyValue::CreateSize(customRenderSurfaceSize));
    //
    // Another way is to tell the SwapChainPanel to render at a certain scale factor compared to its size.
    // e.g. if the SwapChainPanel is 1920x1280 then setting a factor of 0.5f will make the app render at 960x640
    // float customResolutionScale = 0.5f;
    // surfaceCreationProperties->Insert(ref new String(EGLRenderResolutionScaleProperty),
    // PropertyValue::CreateSingle(customResolutionScale));

    m_EglSurface = eglCreateWindowSurface(
        m_EglDisplay, config, reinterpret_cast<IInspectable*>(surfaceCreationProperties), surfaceAttributes);
    if (m_EglSurface == EGL_NO_SURFACE) {
        throw Exception::CreateException(E_FAIL, L"Failed to create EGL fullscreen surface");
    }

    m_EglContext = eglCreateContext(m_EglDisplay, config, EGL_NO_CONTEXT, contextAttributes);
    if (m_EglContext == EGL_NO_CONTEXT) {
        throw Exception::CreateException(E_FAIL, L"Failed to create EGL context");
    }

    if (eglMakeCurrent(m_EglDisplay, m_EglSurface, m_EglSurface, m_EglContext) == EGL_FALSE) {
        throw Exception::CreateException(E_FAIL, L"Failed to make fullscreen EGLSurface current");
    }

    // By default, allow HolographicFrame::PresentUsingCurrentPrediction() to wait for the current frame to
    // finish before it returns.
    eglSurfaceAttrib(m_EglDisplay, m_EglSurface, EGLEXT_WAIT_FOR_VBLANK_ANGLE, false);
}

void HoloJsAppView::CleanupEGL()
{
    if (m_EglDisplay != EGL_NO_DISPLAY && m_EglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(m_EglDisplay, m_EglSurface);
        m_EglSurface = EGL_NO_SURFACE;
    }

    if (m_EglDisplay != EGL_NO_DISPLAY && m_EglContext != EGL_NO_CONTEXT) {
        eglDestroyContext(m_EglDisplay, m_EglContext);
        m_EglContext = EGL_NO_CONTEXT;
    }

    if (m_EglDisplay != EGL_NO_DISPLAY) {
        eglTerminate(m_EglDisplay);
        m_EglDisplay = EGL_NO_DISPLAY;
    }
}
