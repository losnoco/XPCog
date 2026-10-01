#include "WinUIIsland.hpp"

#include <wx/apptrait.h>
#include <wx/evtloop.h>
#include <wx/utils.h>

// <windows.h> has arrived through wx by now, and winbase.h defines
// GetCurrentTime as a macro, which breaks the Storyboard method of that name in
// the XAML projection. The documented workaround is exactly this.
#undef GetCurrentTime

#include <MddBootstrap.h>

#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

#include <cmath>

namespace xpcog::app {

namespace {

namespace mux = winrt::Microsoft::UI::Xaml;

// A XAML Application is required even though nothing here is launched as one:
// it is where the control templates live (XamlControlsResources) and what the
// XAML runtime asks for type information. Without the metadata provider every
// control from Microsoft.UI.Xaml.Controls that has a template fails to find it.
struct IslandApplication
    : mux::ApplicationT<IslandApplication, mux::Markup::IXamlMetadataProvider> {
    mux::Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type) {
        return provider_.GetXamlType(type);
    }
    mux::Markup::IXamlType GetXamlType(winrt::hstring const& fullName) {
        return provider_.GetXamlType(fullName);
    }
    winrt::com_array<mux::Markup::XmlnsDefinition> GetXmlnsDefinitions() {
        return provider_.GetXmlnsDefinitions();
    }

    mux::XamlTypeInfo::XamlControlsXamlMetaDataProvider provider_;
};

using PreTranslate = BOOL(__stdcall*)(const MSG*);

struct Runtime {
    bool                                                   bootstrapped = false;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueController dispatcher{nullptr};
    mux::Application                                       application{nullptr};
    mux::Hosting::WindowsXamlManager                       manager{nullptr};
    PreTranslate                                           preTranslate = nullptr;
    wxString                                               failure;
};

Runtime& runtime() {
    static Runtime instance;
    return instance;
}

wxString describe(const char* what, const winrt::hresult_error& error) {
    return wxString::Format("%s (0x%08lX): %s", what,
                            static_cast<unsigned long>(error.code().value),
                            wxString(error.message().c_str()));
}

class WinUIEventLoop : public wxGUIEventLoop {
public:
    bool PreProcessMessage(WXMSG* msg) override {
        // Messages for the island's own windows never reach wx's handling at
        // all -- there is no wxWindow for their HWND -- so this is the only
        // place WinUI hears about keyboard input before it is dispatched.
        if (const PreTranslate hook = runtime().preTranslate; hook != nullptr && hook(msg)) {
            return true;
        }
        return wxGUIEventLoop::PreProcessMessage(msg);
    }
};

class WinUIAppTraits : public wxGUIAppTraits {
public:
    wxEventLoopBase* CreateEventLoop() override { return new WinUIEventLoop; }
};

}  // namespace

bool startWinUI() {
    Runtime& rt = runtime();

    // Major version only: from 2.0 the minor is ignored, and any 2.x runtime at
    // or above the one these headers were generated from will do.
    PACKAGE_VERSION minimum{};
    minimum.Major    = XPCOG_WINAPPSDK_RUNTIME_MAJOR;
    minimum.Minor    = XPCOG_WINAPPSDK_RUNTIME_MINOR;
    minimum.Build    = XPCOG_WINAPPSDK_RUNTIME_PATCH;
    minimum.Revision = 0;
    const HRESULT hr = MddBootstrapInitialize2(
        (XPCOG_WINAPPSDK_RUNTIME_MAJOR << 16) | XPCOG_WINAPPSDK_RUNTIME_MINOR, L"", minimum,
        MddBootstrapInitializeOptions_None);
    if (FAILED(hr)) {
        rt.failure = wxString::Format("Windows App Runtime %d.%d.%d not available (0x%08lX)",
                                      XPCOG_WINAPPSDK_RUNTIME_MAJOR, XPCOG_WINAPPSDK_RUNTIME_MINOR,
                                      XPCOG_WINAPPSDK_RUNTIME_PATCH, static_cast<unsigned long>(hr));
        return false;
    }
    rt.bootstrapped = true;

    try {
        // The order is the Windows App SDK islands sample's: a dispatcher queue
        // on this thread, then the Application, then XAML for the thread, and
        // only then the resources, which need all three.
        rt.dispatcher =
            winrt::Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnCurrentThread();
        rt.application = winrt::make<IslandApplication>();
        rt.manager     = mux::Hosting::WindowsXamlManager::InitializeForCurrentThread();
        rt.application.Resources().MergedDictionaries().Append(
            mux::Controls::XamlControlsResources());

        // Looked up rather than linked: the DLL is in the framework package, so
        // an import would stop the executable loading at all on a machine
        // without the runtime, long before anything could fall back.
        if (HMODULE core = ::LoadLibraryW(L"Microsoft.UI.Windowing.Core.dll")) {
            rt.preTranslate = reinterpret_cast<PreTranslate>(
                ::GetProcAddress(core, "ContentPreTranslateMessage"));
        }
    } catch (const winrt::hresult_error& error) {
        stopWinUI();
        rt.failure = describe("WinUI did not start", error);
        return false;
    }
    return true;
}

void stopWinUI() {
    Runtime& rt  = runtime();
    rt.preTranslate = nullptr;
    if (rt.manager) {
        rt.manager.Close();
        rt.manager = nullptr;
    }
    rt.application = nullptr;
    if (rt.dispatcher) {
        rt.dispatcher.ShutdownQueue();
        rt.dispatcher = nullptr;
    }
    if (rt.bootstrapped) {
        MddBootstrapShutdown();
        rt.bootstrapped = false;
    }
}

bool winUIRunning() {
    return static_cast<bool>(runtime().manager);
}

wxString winUIFailure() {
    return runtime().failure;
}

wxAppTraits* makeWinUIAppTraits() {
    return new WinUIAppTraits;
}

// --- the island ---------------------------------------------------------------

struct WinUIIsland::Impl {
    mux::Hosting::DesktopWindowXamlSource source{nullptr};
    mux::Controls::Slider                 slider{nullptr};
    mux::Controls::Button                 playPause{nullptr};
    bool                                  quiet = false;
};

WinUIIsland::WinUIIsland(wxWindow* parent, double volume)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      impl_(std::make_unique<Impl>()) {
    SetMinSize(FromDIP(wxSize(-1, 44)));
    try {
        build(volume);
    } catch (const winrt::hresult_error& error) {
        runtime().failure = describe("The island did not build", error);
        if (impl_->source) {
            impl_->source.Close();
        }
        impl_ = std::make_unique<Impl>();
    }
}

bool WinUIIsland::ok() const {
    return static_cast<bool>(impl_->source);
}

void WinUIIsland::build(double volume) {
    Impl& impl = *impl_;

    impl.source = mux::Hosting::DesktopWindowXamlSource();
    impl.source.Initialize(winrt::Microsoft::UI::WindowId{
        reinterpret_cast<uint64_t>(GetHWND())});

    auto row = mux::Controls::StackPanel();
    row.Orientation(mux::Controls::Orientation::Horizontal);
    row.Spacing(12);
    row.Padding(mux::ThicknessHelper::FromLengths(8, 4, 8, 4));
    row.VerticalAlignment(mux::VerticalAlignment::Center);

    auto label = mux::Controls::TextBlock();
    label.Text(L"WinUI island");
    label.VerticalAlignment(mux::VerticalAlignment::Center);
    row.Children().Append(label);

    impl.playPause = mux::Controls::Button();
    impl.playPause.Content(winrt::box_value(L"Play"));
    impl.playPause.Click([this](auto&&, auto&&) {
        if (playPauseClicked) {
            playPauseClicked();
        }
    });
    row.Children().Append(impl.playPause);

    impl.slider = mux::Controls::Slider();
    impl.slider.Minimum(0);
    impl.slider.Maximum(100);
    impl.slider.Width(200);
    impl.slider.Value(volume * 100.0);
    impl.slider.VerticalAlignment(mux::VerticalAlignment::Center);
    impl.slider.ValueChanged([this](auto&&, auto const& args) {
        if (!impl_->quiet && volumeChanged) {
            volumeChanged(args.NewValue() / 100.0);
        }
    });
    row.Children().Append(impl.slider);

    // A Grid around the row so the island has a surface of its own colour;
    // without one it composites as black where nothing is drawn.
    auto root = mux::Controls::Grid();
    root.Background(mux::Media::SolidColorBrush(
        winrt::unbox_value<winrt::Windows::UI::Color>(mux::Application::Current().Resources().Lookup(
            winrt::box_value(L"SolidBackgroundFillColorBase")))));
    root.Children().Append(row);
    impl.source.Content(root);

    // The island is laid out by the bridge window, which wx does not know is
    // there: it has to be told the size every time this window is.
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        const wxSize size = GetClientSize();
        if (impl_->source) {
            impl_->source.SiteBridge().MoveAndResize({0, 0, size.x, size.y});
        }
    });

    // Focus in both directions: into the XAML content when wx tabs onto this
    // window, and back out to wx when Tab runs off either end of it.
    Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent&) {
        if (!impl_->source) {
            return;
        }
        const bool backwards = wxGetKeyState(WXK_SHIFT);
        impl_->source.NavigateFocus(mux::Hosting::XamlSourceFocusNavigationRequest(
            backwards ? mux::Hosting::XamlSourceFocusNavigationReason::Last
                      : mux::Hosting::XamlSourceFocusNavigationReason::First));
    });
    impl.source.TakeFocusRequested([this](auto&&, auto const& args) {
        Navigate(args.Request().Reason() == mux::Hosting::XamlSourceFocusNavigationReason::First
                     ? wxNavigationKeyEvent::IsForward
                     : wxNavigationKeyEvent::IsBackward);
    });
}

WinUIIsland::~WinUIIsland() {
    if (impl_->source) {
        impl_->source.Close();
    }
}

void WinUIIsland::setVolume(double gain) {
    if (!impl_->slider) {
        return;
    }
    impl_->quiet = true;
    impl_->slider.Value(std::round(gain * 100.0));
    impl_->quiet = false;
}

void WinUIIsland::setPlaying(bool playing) {
    if (!impl_->playPause) {
        return;
    }
    impl_->playPause.Content(winrt::box_value(playing ? L"Pause" : L"Play"));
}

}  // namespace xpcog::app
