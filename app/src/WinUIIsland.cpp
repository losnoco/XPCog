#include "WinUIHost.hpp"

#include <wx/apptrait.h>
#include <wx/evtloop.h>
#include <wx/utils.h>

#include <MddBootstrap.h>

#include <cmath>

namespace xpcog::app {

namespace {

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

wxString describeWinRTError(const char* what, const winrt::hresult_error& error) {
    return wxString::Format("%s (0x%08lX): %s", what,
                            static_cast<unsigned long>(error.code().value),
                            wxString(error.message().c_str()));
}

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
        rt.failure = describeWinRTError("WinUI did not start", error);
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

// --- the host -----------------------------------------------------------------

WinUIIsland::WinUIIsland(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      host_(std::make_unique<Host>()) {
    try {
        host_->source = mux::Hosting::DesktopWindowXamlSource();
        host_->source.Initialize(
            winrt::Microsoft::UI::WindowId{reinterpret_cast<uint64_t>(GetHWND())});

        // Focus back out to wx when Tab runs off either end of the content.
        host_->source.TakeFocusRequested([this](auto&&, auto const& args) {
            Navigate(args.Request().Reason() ==
                             mux::Hosting::XamlSourceFocusNavigationReason::First
                         ? wxNavigationKeyEvent::IsForward
                         : wxNavigationKeyEvent::IsBackward);
        });
    } catch (const winrt::hresult_error& error) {
        fail(describeWinRTError("The island did not start", error));
        return;
    }

    // The island is laid out by the bridge window, which wx does not know is
    // there: it has to be told the size every time this window is.
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        const wxSize size = GetClientSize();
        if (host_->source) {
            host_->source.SiteBridge().MoveAndResize({0, 0, size.x, size.y});
        }
    });

    // And focus into the content when wx tabs onto this window.
    Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent&) {
        if (!host_->source) {
            return;
        }
        const bool backwards = wxGetKeyState(WXK_SHIFT);
        host_->source.NavigateFocus(mux::Hosting::XamlSourceFocusNavigationRequest(
            backwards ? mux::Hosting::XamlSourceFocusNavigationReason::Last
                      : mux::Hosting::XamlSourceFocusNavigationReason::First));
    });
}

WinUIIsland::~WinUIIsland() {
    if (host_->source) {
        host_->source.Close();
    }
}

bool WinUIIsland::ok() const {
    return static_cast<bool>(host_->source);
}

void WinUIIsland::fail(const wxString& reason) {
    runtime().failure = reason;
    if (host_->source) {
        host_->source.Close();
        host_->source = nullptr;
    }
}

// --- the transport strip --------------------------------------------------------

struct WinUITransport::Impl {
    mux::Controls::Slider slider{nullptr};
    mux::Controls::Button playPause{nullptr};
    bool                  quiet = false;
};

WinUITransport::WinUITransport(wxWindow* parent, double volume)
    : WinUIIsland(parent), impl_(std::make_unique<Impl>()) {
    SetMinSize(FromDIP(wxSize(-1, 64)));
    if (!ok()) {
        return;
    }

    try {
        Impl& impl = *impl_;

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

        auto toggle = mux::Controls::ToggleSwitch();
        toggle.Header(winrt::box_value(L"WinUI playlist"));
        toggle.VerticalAlignment(mux::VerticalAlignment::Center);
        toggle.Toggled([this](auto const& sender, auto&&) {
            if (winUIPlaylistToggled) {
                winUIPlaylistToggled(sender.template as<mux::Controls::ToggleSwitch>().IsOn());
            }
        });
        row.Children().Append(toggle);

        // A Grid around the row so the island has a surface of its own colour;
        // without one it composites as black where nothing is drawn.
        auto root = mux::Controls::Grid();
        root.Background(mux::Media::SolidColorBrush(winrt::unbox_value<winrt::Windows::UI::Color>(
            mux::Application::Current().Resources().Lookup(
                winrt::box_value(L"SolidBackgroundFillColorBase")))));
        root.Children().Append(row);
        host_->source.Content(root);
    } catch (const winrt::hresult_error& error) {
        fail(describeWinRTError("The transport island did not build", error));
    }
}

WinUITransport::~WinUITransport() = default;

void WinUITransport::setVolume(double gain) {
    if (!impl_->slider) {
        return;
    }
    impl_->quiet = true;
    impl_->slider.Value(std::round(gain * 100.0));
    impl_->quiet = false;
}

void WinUITransport::setPlaying(bool playing) {
    if (!impl_->playPause) {
        return;
    }
    impl_->playPause.Content(winrt::box_value(playing ? L"Pause" : L"Play"));
}

}  // namespace xpcog::app
