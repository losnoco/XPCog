#include "App.hpp"

#include "MainWindow.hpp"
#include "WinRT.hpp"

#include "PlaybackController.hpp"
#include "Session.hpp"
#include "Translations.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/platform/CrashReporter.hpp"
#include "xpcog/platform/SettingsStore.hpp"

#include <chrono>
#include <memory>

namespace xpcog::winui {

namespace {

/// Everything the player is, below the window. The order of the members is the
/// order of construction and the reverse of destruction: the registry holds a
/// pointer to the settings, the settings a reference to the store, and the
/// session all three.
class Player {
public:
    void start() {
        store_    = platform::makeNativeSettingsStore();
        settings_ = std::make_unique<Settings>(*store_);
        settings_->applyMigrations();

        // Before the first string is built, so nothing comes up in English and
        // stays that way.
        app::installNeutralTranslations(settings_->Language());

        // Crash reporting only with consent already given; asking is the
        // window's, later, and not this step's.
        if (settings_->SentryConsented()) {
            platform::startCrashReporting();
        }

        registry_ = std::make_unique<PluginRegistry>();
        registry_->setSettings(settings_.get());
        registerAllCodecs(*registry_);

        // The session's way back onto this thread, from its decoder, scan and
        // network threads. DispatcherQueue is agile, so capturing it and calling
        // TryEnqueue from any thread is what it is for. After the window closes
        // the queue refuses, and a refused post is dropped -- by then nothing is
        // left to want it.
        queue_   = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        session_ = std::make_unique<app::Session>(
            *registry_, *settings_, [queue = queue_](std::function<void()> work) {
                queue.TryEnqueue([work = std::move(work)] { work(); });
            });

        window_         = std::make_unique<MainWindow>(*session_);
        window_->closed = [this] { onClosed(); };

        // The window handle is what the media transport controls and the
        // taskbar buttons attach to.
        session_->attachDesktop(window_->hwnd());
        session_->start();

        timer_ = queue_.CreateTimer();
        timer_.Interval(std::chrono::milliseconds(app::PlaybackController::kTickIntervalMs));
        timer_.Tick([this](auto&&, auto&&) { session_->tick(); });
        timer_.Start();

        window_->activate();
    }

    ~Player() {
        window_.reset();
        session_.reset();
        platform::stopCrashReporting();
        registry_.reset();
        settings_.reset();
        store_.reset();
    }

private:
    void onClosed() {
        if (timer_) {
            timer_.Stop();
        }
        session_->save();
    }

    std::unique_ptr<ISettingsStore>           store_;
    std::unique_ptr<Settings>                 settings_;
    std::unique_ptr<PluginRegistry>           registry_;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue      queue_{nullptr};
    std::unique_ptr<app::Session>             session_;
    std::unique_ptr<MainWindow>               window_;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};
};

std::unique_ptr<Player> player;

// The XAML Application. Without it there are no control templates
// (XamlControlsResources) and no type information for the XAML reader -- the
// metadata provider is what lets a template loaded from text name a control.
struct XamlApp : mux::ApplicationT<XamlApp, mux::Markup::IXamlMetadataProvider> {
    void OnLaunched(mux::LaunchActivatedEventArgs const&) {
        try {
            Resources().MergedDictionaries().Append(mux::Controls::XamlControlsResources());
            player = std::make_unique<Player>();
            player->start();
        } catch (const winrt::hresult_error& error) {
            // An exception that leaves here ends the process as a stowed
            // exception, 0xC000027B, with nothing on screen to say why. Most
            // of what can throw is XAML text the reader refused; its message
            // names the line and the property.
            const std::wstring text = L"XPCog could not start.\n\n" +
                                      std::wstring(error.message().c_str());
            ::MessageBoxW(nullptr, text.c_str(), L"XPCog", MB_OK | MB_ICONERROR);
            Exit();
        }
    }

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

}  // namespace

int runApplication() {
    mux::Application::Start([](auto&&) { winrt::make<XamlApp>(); });
    // The loop has ended with the last window, so the dispatcher is gone and the
    // session can go without anything still queued for it.
    player.reset();
    return 0;
}

}  // namespace xpcog::winui
