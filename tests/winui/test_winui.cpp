// The WinUI player, opened: the main window, every pane the View menu shows,
// Preferences page by page, the mini player and the About box.
//
// What it is for is the class of failure that has already reached a listener
// twice in this frontend's short life -- XAML refused at run time, a property
// that does not exist, a template part that is not there -- which compiles,
// links and passes every other suite, and then ends the process with a stowed
// exception the first time the window is built. Here such a thing is a failed
// case, with the runtime's message.
//
// --- How it runs ------------------------------------------------------------
//
// The XAML application is real: the runtime bootstrapped, Application::Start,
// XamlControlsResources merged, exactly as the player does. Catch2 runs on a
// worker thread beside it, and every step is handed to the interface thread
// with ui() and waited for; settle() lets the loop turn a few times between
// steps, so layout, Loaded and the first frames happen as they would for a
// listener. An exception thrown on the interface thread comes back to the case
// that asked; Application::UnhandledException is counted and checked after
// each step.
//
// The session is a test one -- an in-memory settings store, a temporary data
// directory, offline output -- so nothing here reads or writes the settings
// or library of whoever runs it.
//
// No runtime installed means no case can run, and the binary exits 4, which
// ctest reports as a skip rather than a pass.

#include "MainWindow.hpp"
#include "PreferencesWindow.hpp"
#include "Runtime.hpp"
#include "Win2D.hpp"
#include "WinRT.hpp"

#include "Commands.hpp"
#include "Session.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/OfflineOutput.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace xpcog::winui {

/// The friend MainWindow and PreferencesWindow name: what a case needs to drive
/// them that the player itself never needed to make public.
struct TestAccess {
    static void run(MainWindow& window, app::CommandId id) { window.onCommand(id); }
    static bool miniShown(const MainWindow& window) { return window.miniShown(); }
    static mux::Window window(const MainWindow& window) { return window.window_; }
    static mux::Controls::TitleBar titleBar(const MainWindow& window) { return window.titleBar_; }
    static PreferencesWindow* preferences(const MainWindow& window) {
        return window.preferences_.get();
    }
    static mux::Controls::NavigationView navigation(const PreferencesWindow& preferences) {
        return preferences.navigation_;
    }
};

}  // namespace xpcog::winui

namespace {

using namespace xpcog;
using namespace xpcog::winui;
using app::CommandId;

winrt::Microsoft::UI::Dispatching::DispatcherQueue uiQueue{nullptr};

/// What the application reported through UnhandledException.
std::mutex               errorsMutex;
std::vector<std::string> errors;

std::vector<std::string> takeErrors() {
    std::lock_guard lock(errorsMutex);
    return std::exchange(errors, {});
}

/// Runs `work` on the interface thread and waits for it, handing back what it
/// returned and rethrowing here what it threw there -- a winrt error as its
/// message, so the case says why. Assertions stay on this side: Catch2's are
/// not to be made from another thread, so a step returns what the case checks.
template <typename Work>
auto ui(Work work) -> decltype(work()) {
    using Result = decltype(work());
    std::promise<Result> done;
    auto                 future = done.get_future();
    const bool queued = uiQueue.TryEnqueue([&] {
        try {
            if constexpr (std::is_void_v<Result>) {
                work();
                done.set_value();
            } else {
                done.set_value(work());
            }
        } catch (const winrt::hresult_error& error) {
            done.set_exception(std::make_exception_ptr(std::runtime_error(
                "XAML: " + winrt::to_string(error.message()))));
        } catch (...) {
            done.set_exception(std::current_exception());
        }
    });
    if (!queued) {
        throw std::runtime_error("the interface thread's queue is gone");
    }
    return future.get();
}

/// Lets the loop turn: layout, Loaded and a frame or two. Low priority, so
/// what the steps before queued at normal priority has run by the time this
/// returns.
void settle() {
    for (int turn = 0; turn < 3; ++turn) {
        std::promise<void> done;
        auto               future = done.get_future();
        uiQueue.TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
                           [&] { done.set_value(); });
        future.get();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

/// No unhandled exception since the last check.
void checkQuiet() {
    for (const std::string& error : takeErrors()) {
        FAIL_CHECK("unhandled: " << error);
    }
}

/// A player as the application builds one, around a session that touches
/// nothing of the person running the tests.
struct Player {
    std::filesystem::path           data;
    std::unique_ptr<ISettingsStore> store = makeMemorySettingsStore();
    Settings                        settings{*store};
    PluginRegistry                  registry;
    std::unique_ptr<app::Session>   session;
    std::unique_ptr<MainWindow>     window;

    Player() {
        data = std::filesystem::temp_directory_path() /
               ("xpcog-winui-tests-" + std::to_string(::GetCurrentProcessId()));
        std::filesystem::create_directories(data);

        // Asked, so the consent question does not open over the cases; and
        // the full window, whatever a default says.
        settings.setSentryAskedConsent(true);
        settings.setMiniMode(false);

        registry.setSettings(&settings);
        registerAllCodecs(registry);

        app::Session::Options options;
        options.dataDirectory  = data.string();
        options.cacheDirectory = (data / "cache").string();
        options.makeOutput = [](RingBuffer& ring) { return makeOfflineOutput(ring, 8.0); };
        session = std::make_unique<app::Session>(
            registry, settings,
            [queue = uiQueue](std::function<void()> work) {
                queue.TryEnqueue([work = std::move(work)] { work(); });
            },
            std::move(options));

        window = std::make_unique<MainWindow>(*session);
        session->start();
        window->activate();
    }

    ~Player() {
        window.reset();
        session.reset();
        std::error_code ignored;
        std::filesystem::remove_all(data, ignored);
    }

    void run(CommandId id) { TestAccess::run(*window, id); }
};

/// One player for the run, made on first use: closing the main window would
/// end the application, and the cases after it with it.
std::unique_ptr<Player> player;

Player& thePlayer() {
    if (!player) {
        ui([] { player = std::make_unique<Player>(); });
        settle();
    }
    return *player;
}

}  // namespace

TEST_CASE("The main window opens and lays out", "[winui]") {
    Player& p = thePlayer();
    const float width = ui([&] {
        const auto root = TestAccess::window(*p.window).Content().as<mux::UIElement>();
        root.UpdateLayout();
        return root.ActualSize().x;
    });
    CHECK(width > 0);
    checkQuiet();
}

TEST_CASE("The title bar reserves the caption buttons' width and no more", "[winui]") {
    // The template's last column is the caption buttons' space, sized from
    // AppWindowTitleBar::RightInset -- physical pixels -- and taken as DIPs
    // unless padTitleBarForCaptions() divides by the scale. In pixels here, so
    // a scaled display shows the difference and an unscaled one passes either way.
    Player& p = thePlayer();
    struct Widths {
        double column = -1;
        double inset  = 0;
    };
    const Widths widths = ui([&] {
        Widths     result;
        const auto bar   = TestAccess::titleBar(*p.window);
        const auto scale = bar.XamlRoot().RasterizationScale();
        result.inset     = TestAccess::window(*p.window).AppWindow().TitleBar().RightInset();
        if (const auto root = mux::Media::VisualTreeHelper::GetChild(bar, 0).try_as<mux::Controls::Grid>()) {
            const auto columns = root.ColumnDefinitions();
            result.column = columns.GetAt(columns.Size() - 1).ActualWidth() * scale;
        }
        return result;
    });
    INFO("inset " << widths.inset << " px, column " << widths.column << " px");
    CHECK(widths.inset > 0);
    CHECK(std::abs(widths.column - widths.inset) <= 1.0);
    checkQuiet();
}

TEST_CASE("Every pane the View menu shows opens and closes", "[winui]") {
    Player& p = thePlayer();
    const std::vector<CommandId> panes = {
        CommandId::ViewFileTree, CommandId::ViewInfo,       CommandId::ViewLyrics,
        CommandId::ViewSpectrum, CommandId::ViewOscilloscope, CommandId::ViewEqualizer,
        CommandId::ViewSpeed,    CommandId::ViewWaveform,
#ifdef XPCOG_HAVE_SC55_PANEL
        CommandId::ViewSc55Panel,
#endif
    };
    for (const CommandId id : panes) {
        INFO("command " << static_cast<int>(id));
        // Twice: open then close, or the reverse where the default has it
        // shown -- both directions are layouts someone will see.
        ui([&] { p.run(id); });
        settle();
        checkQuiet();
        ui([&] { p.run(id); });
        settle();
        checkQuiet();
    }
}

TEST_CASE("Preferences opens on every page", "[winui]") {
    Player& p = thePlayer();
    ui([&] { p.run(CommandId::FilePreferences); });
    settle();
    checkQuiet();

    const std::uint32_t pages = ui([&]() -> std::uint32_t {
        const PreferencesWindow* preferences = TestAccess::preferences(*p.window);
        return preferences != nullptr ? TestAccess::navigation(*preferences).MenuItems().Size() : 0;
    });
    REQUIRE(pages > 0);

    for (std::uint32_t page = 0; page < pages; ++page) {
        INFO("page " << page);
        ui([&] {
            auto navigation = TestAccess::navigation(*TestAccess::preferences(*p.window));
            navigation.SelectedItem(navigation.MenuItems().GetAt(page));
        });
        settle();
        checkQuiet();
    }

    ui([&] { TestAccess::preferences(*p.window)->close(); });
    settle();
    checkQuiet();
}

TEST_CASE("The mini player comes and goes", "[winui]") {
    Player& p = thePlayer();
    ui([&] { p.run(CommandId::ViewMiniPlayer); });
    settle();
    CHECK(ui([&] { return TestAccess::miniShown(*p.window); }));
    checkQuiet();

    ui([&] { p.run(CommandId::ViewMiniPlayer); });
    settle();
    CHECK_FALSE(ui([&] { return TestAccess::miniShown(*p.window); }));
    checkQuiet();
}

TEST_CASE("The About box opens", "[winui]") {
    Player& p = thePlayer();
    ui([&] { p.run(CommandId::HelpAbout); });
    settle();
    checkQuiet();

    // A ContentDialog draws in a popup of the window's XAML root; closing the
    // one open there is what the Close button does.
    const bool closed = ui([&] {
        const auto root  = TestAccess::window(*p.window).Content().XamlRoot();
        bool       found = false;
        for (const auto& popup : mux::Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(root)) {
            if (const auto box = popup.Child().try_as<mux::Controls::ContentDialog>()) {
                box.Hide();
                found = true;
            }
        }
        return found;
    });
    CHECK(closed);
    settle();
    checkQuiet();
}

// --- the application ------------------------------------------------------------

namespace {

int argCount = 0;
char** args  = nullptr;
std::atomic<int> result{1};
std::thread      runner;

struct TestApp : mux::ApplicationT<TestApp, mux::Markup::IXamlMetadataProvider> {
    void OnLaunched(mux::LaunchActivatedEventArgs const&) {
        Resources().MergedDictionaries().Append(mux::Controls::XamlControlsResources());
        uiQueue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        UnhandledException([](auto&&, mux::UnhandledExceptionEventArgs const& event) {
            std::lock_guard lock(errorsMutex);
            errors.push_back(winrt::to_string(event.Message()));
            // Recorded, and kept from ending the process, so the run can say
            // which case it was.
            event.Handled(true);
        });
        runner = std::thread([] {
            result = Catch::Session().run(argCount, args);
            // Ended the way the player ends: its window closed through Quit,
            // and the loop with it. The player itself goes after the loop, in
            // main(), as App.cpp's does -- not here, where what the session
            // has queued for the loop would still run, on freed memory, after
            // every case had passed.
            uiQueue.TryEnqueue([] {
                if (player) {
                    player->run(CommandId::FileQuit);
                }
                mux::Application::Current().Exit();
            });
        });
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

int main(int argc, char** argv) {
    argCount = argc;
    args     = argv;

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    installWin2DActivation();

    std::string reason;
    if (!bootstrapRuntime(reason)) {
        // A skip on a developer's machine without the runtime; a failure in
        // CI, which installs it first -- a skip there would be the suite going
        // quietly green with nothing run, the shape tests/CMakeLists.txt's
        // GTK block refuses for Xvfb.
        char        ci[8]{};
        const bool  inCi = ::GetEnvironmentVariableA("CI", ci, sizeof ci) > 0;
        std::fprintf(stderr, "%s: %s\n", inCi ? "Failed" : "Skipped", reason.c_str());
        return inCi ? 1 : 4;
    }

    mux::Application::Start([](auto&&) { winrt::make<TestApp>(); });
    if (runner.joinable()) {
        runner.join();
    }
    player.reset();
    // Released while the runtime is still there, rather than by the static
    // destructors after shutdownRuntime() has let go of what it belongs to.
    uiQueue = nullptr;
    shutdownRuntime();
    return result;
}
