// Preferences, the Services section and Advanced: Last.fm, ListenBrainz, the
// remote control, and every setting raw. GTK's pages of the same names
// (app-gtk/src/PreferencesDialog.cpp), row for row; see PreferencesRows.hpp
// for the vocabulary.

#include "PreferencesRows.hpp"

#include "LastFmAccount.hpp"
#include "ListenBrainzAccount.hpp"
#include "RemoteToken.hpp"
#include "Session.hpp"

#include "xpcog/core/net/HttpClient.hpp"
#include "xpcog/core/remote/RemoteServer.hpp"

#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Globalization.NumberFormatting.h>

#include <array>
#include <cmath>
#include <exception>
#include <memory>
#include <string>

namespace xpcog::winui {

using app::Choice;
using app::tr;

namespace {

[[nodiscard]] bool isTrue(const std::string& text) {
    return text == "1" || text == "true" || text == "YES";
}

[[nodiscard]] double toDouble(const std::string& text) {
    try {
        return text.empty() ? 0.0 : std::stod(text);
    } catch (const std::exception&) {
        return 0.0;
    }
}

[[nodiscard]] std::string trimmed(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    const auto end   = text.find_last_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    return text.substr(begin, end - begin + 1);
}

/// The text a note row shows, to set later.
void setNote(const Row& row, const std::string& text) {
    row.element.as<mux::Controls::TextBlock>().Text(toH(text));
}

/// A card's description line -- present when the row was made with a hint --
/// for rows whose description is a value that changes, like the token.
mux::Controls::TextBlock descriptionOf(const Row& row) {
    const auto grid  = row.element.as<mux::Controls::Border>().Child().as<mux::Controls::Grid>();
    const auto words = grid.Children().GetAt(0).as<mux::Controls::StackPanel>();
    return words.Children().Size() > 1 ? words.Children().GetAt(1).as<mux::Controls::TextBlock>()
                                       : nullptr;
}

mux::Controls::Button button(const std::string& label) {
    auto made = mux::Controls::Button();
    made.Content(winrt::box_value(toH(label)));
    return made;
}

void showIf(const mux::UIElement& element, bool shown) {
    element.Visibility(shown ? mux::Visibility::Visible : mux::Visibility::Collapsed);
}

}  // namespace

// --- Last.fm -----------------------------------------------------------------------------

void PreferencesWindow::buildLastFmPage() {
    XPCOG_ROWS("lastfm", "Last.fm", L"\xE7F6");  // a proper noun; Segoe "Headphone"
    app::LastFmAccount* account   = session_.lastFm();
    Scrobbler*          scrobbler = session_.scrobbler();
    Settings&           settings  = settings_;

    const Row enable  = row->toggle(tr("Scrobble to Last.fm"), "enableAudioScrobbler");
    const Row status  = row->note("", false);
    auto      connect = button(tr("Connect..."));
    auto      cancel  = button(tr("Cancel"));
    auto      forget  = button(tr("Disconnect"));
    row->buttons({connect, cancel, forget});
    row->note(tr("Plays are sent once you have heard half a track, or four minutes of it, "
                 "whichever comes first. Tracks under 30 seconds are never scrobbled."));
    row->link(tr("Your Last.fm applications"), "https://www.last.fm/settings/applications");
    row->note(tr("Revoking access there stops scrobbling immediately, whatever this pane says."));

    row->heading(tr("API account"));
    const Row keyStatus = row->note("");
    row->link(tr("Create a Last.fm API account"), "https://www.last.fm/api/account/create");
    const app::LastFmAccount::ApiCredentials own = app::LastFmAccount::loadApiCredentials();
    auto keyEdit = mux::Controls::TextBox();
    keyEdit.Width(280);
    keyEdit.Text(toH(own.key));
    row->add(tr("API key"), keyEdit);
    auto secretEdit = mux::Controls::PasswordBox();
    secretEdit.Width(280);
    secretEdit.Password(toH(own.secret));
    row->add(tr("Shared secret"), secretEdit);
    auto use    = button(tr("Use this key"));
    auto remove = button(tr("Remove"));
    row->buttons({use, remove});
    row->note(tr("A connection belongs to the key that opened it, so changing the key "
                 "disconnects you and you connect again under the new one."));

    const std::weak_ptr<int> alive = alive_;
    // Answers to the connection arrive later, on the interface thread through
    // the session's dispatcher; by then the window may be gone, and every
    // handler looks before it touches a control.
    const auto refresh = [=] {
        if (alive.expired()) {
            return;
        }
        const bool  built = account->usable();
        std::string problem;
        const bool  store   = app::LastFmAccount::storeAvailable(&problem);
        const bool  working = account->connecting();
        const auto  session = scrobbler->session();
        enable.enable(built && store);
        showIf(connect, !session.connected() && !working);
        showIf(cancel, working);
        showIf(forget, session.connected() && !working);
        connect.IsEnabled(built && store);

        const bool ownKey  = account->usingOwnCredentials();
        const bool canEdit = store && httpClientAvailable() && !working;
        keyEdit.IsEnabled(canEdit);
        secretEdit.IsEnabled(canEdit);
        use.IsEnabled(canEdit);
        showIf(remove, ownKey);
        remove.IsEnabled(canEdit);
        if (ownKey) {
            setNote(keyStatus, tr("Using your own API key."));
        } else if (app::LastFmAccount::hasBuiltInCredentials()) {
            setNote(keyStatus, tr("Using the key built into XPCog. Enter your own to scrobble as "
                                  "an application of your own."));
        } else {
            setNote(keyStatus, tr("This build carries no API key, so scrobbling needs one of "
                                  "yours."));
        }

        std::string text;
        if (!built) {
            text = account->unavailableReason();
        } else if (!store) {
            text = problem.empty() ? tr("The system password store is not available, so a Last.fm session "
                                        "cannot be kept.")
                                   : problem;
        } else if (working) {
            text = tr("Waiting for you to allow access in your browser...");
        } else if (session.connected()) {
            text = app::trf("Connected as %s.", session.username);
        } else {
            text = tr("Not connected. Connecting opens Last.fm in your browser; XPCog never sees "
                      "your password.");
        }
        if (const std::size_t waiting = scrobbler->pending(); waiting > 0) {
            text += "\n";
            text += app::fmt(app::trn("%zu play waiting to be sent.", "%zu plays waiting to be sent.",
                                      waiting),
                             waiting);
        }
        setNote(status, text);
    };

    connect.Click([=, &settings](auto&&, auto&&) {
        app::LastFmAccount::ConnectHandlers handlers;
        handlers.awaitingAuthorization = [refresh](const std::string&) { refresh(); };
        // The session and its settings outlive this window; the scrobbler
        // gets its session whether or not the window is still open.
        handlers.connected = [refresh, scrobbler, &settings](const Scrobbler::Session& session) {
            scrobbler->setSession(session);
            settings.setEnableScrobbling(true);
            refresh();
        };
        handlers.failed = [refresh, alive, status](const std::string& message) {
            refresh();
            if (!alive.expired()) {
                setNote(status, message);
            }
        };
        account->connect(session_.dispatcher(), std::move(handlers));
        refresh();
    });
    cancel.Click([=](auto&&, auto&&) {
        account->cancelConnect();
        refresh();
    });
    forget.Click([=](auto&&, auto&&) {
        account->forget();
        scrobbler->setSession({});
        refresh();
    });

    const auto apply = [=](app::LastFmAccount::ApiCredentials credentials, bool removing) {
        const auto result = (!removing && !credentials.complete())
                                ? app::LastFmAccount::ApplyResult::Incomplete
                                : account->setApiCredentials(std::move(credentials));
        std::string keyMessage;
        std::string connectionMessage;
        switch (result) {
            case app::LastFmAccount::ApplyResult::Applied:
                break;
            case app::LastFmAccount::ApplyResult::AppliedAndDisconnected:
                scrobbler->setSession({});
                connectionMessage = tr("The key changed, so you have been disconnected. Connect "
                                       "again to scrobble under it.");
                break;
            case app::LastFmAccount::ApplyResult::Incomplete:
                keyMessage = tr("Both the API key and the shared secret are needed.");
                break;
            case app::LastFmAccount::ApplyResult::StoreRefused:
                keyMessage = tr("The system password store would not keep the key.");
                break;
        }
        refresh();
        if (alive.expired()) {
            return;
        }
        if (!keyMessage.empty()) {
            setNote(keyStatus, keyMessage);
        }
        if (!connectionMessage.empty()) {
            setNote(status, connectionMessage);
        }
        const auto stored = app::LastFmAccount::loadApiCredentials();
        keyEdit.Text(toH(stored.key));
        secretEdit.Password(toH(stored.secret));
    };
    use.Click([=](auto&&, auto&&) {
        app::LastFmAccount::ApiCredentials credentials;
        credentials.key    = trimmed(toUtf8(keyEdit.Text()));
        credentials.secret = trimmed(toUtf8(secretEdit.Password()));
        apply(std::move(credentials), /*removing=*/false);
    });
    remove.Click([=](auto&&, auto&&) { apply(app::LastFmAccount::ApiCredentials{}, /*removing=*/true); });
    refresh();
}

// --- ListenBrainz ------------------------------------------------------------------------

void PreferencesWindow::buildListenBrainzPage() {
    XPCOG_ROWS("listenbrainz", "ListenBrainz", L"\xE7F6");  // likewise
    app::ListenBrainzAccount* account   = session_.listenBrainz();
    Scrobbler*                scrobbler = session_.listenBrainzScrobbler();
    Settings&                 settings  = settings_;

    const Row enable    = row->toggle(tr("Scrobble to ListenBrainz"), "enableListenBrainz");
    const Row status    = row->note("", false);
    auto      tokenEdit = mux::Controls::PasswordBox();
    tokenEdit.Width(280);
    const Row tokenRow  = row->add(tr("User token"), tokenEdit);
    const Row tokenLink = row->link(tr("Your ListenBrainz user token"), "https://listenbrainz.org/settings/");
    auto      connect   = button(tr("Connect"));
    auto      forget    = button(tr("Disconnect"));
    row->buttons({connect, forget});
    row->note(tr("Plays are sent once you have heard half a track, or four minutes of it, "
                 "whichever comes first. Tracks under 30 seconds are never scrobbled."));
    row->heading(tr("Server"));
    row->text(tr("API address"), "listenBrainzUrl");
    row->note(tr("The public service by default. A ListenBrainz you run yourself, or Maloja, "
                 "takes the same requests at its own address."));

    const std::weak_ptr<int> alive = alive_;
    const auto refresh = [=] {
        if (alive.expired()) {
            return;
        }
        const bool  built = account->usable();
        std::string problem;
        const bool  store   = app::LastFmAccount::storeAvailable(&problem);
        const bool  working = account->connecting();
        const auto  session = scrobbler->session();
        const bool  ready   = built && store;
        enable.enable(ready);
        tokenRow.show(!session.connected());
        tokenLink.show(!session.connected());
        tokenEdit.IsEnabled(ready && !working);
        showIf(connect, !session.connected());
        connect.IsEnabled(ready && !working);
        showIf(forget, session.connected());
        forget.IsEnabled(!working);

        std::string text;
        if (!built) {
            text = account->unavailableReason();
        } else if (!store) {
            text = problem.empty() ? tr("The system password store is not available, so a ListenBrainz token "
                                        "cannot be kept.")
                                   : problem;
        } else if (working) {
            text = tr("Checking the token with ListenBrainz...");
        } else if (session.connected()) {
            text = app::trf("Connected as %s.", session.username);
        } else {
            text = tr("Not connected. Paste the user token from your ListenBrainz settings page "
                      "and press Connect.");
        }
        if (const std::size_t waiting = scrobbler->pending(); waiting > 0) {
            text += "\n";
            text += app::fmt(app::trn("%zu play waiting to be sent.", "%zu plays waiting to be sent.",
                                      waiting),
                             waiting);
        }
        setNote(status, text);
    };
    const auto startConnect = [=, &settings] {
        const std::string typed = trimmed(toUtf8(tokenEdit.Password()));
        app::ListenBrainzAccount::ConnectHandlers handlers;
        handlers.connected = [refresh, scrobbler, alive, tokenEdit, &settings](const Scrobbler::Session& session) {
            scrobbler->setSession(session);
            settings.setEnableListenBrainz(true);
            if (!alive.expired()) {
                tokenEdit.Password(L"");
            }
            refresh();
        };
        handlers.failed = [refresh, alive, status](const std::string& message) {
            refresh();
            if (!alive.expired()) {
                setNote(status, message);
            }
        };
        account->connect(typed, session_.dispatcher(), std::move(handlers));
        refresh();
    };
    connect.Click([=](auto&&, auto&&) { startConnect(); });
    tokenEdit.KeyDown([=](auto&&, mux::Input::KeyRoutedEventArgs const& args) {
        if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
            startConnect();
        }
    });
    forget.Click([=](auto&&, auto&&) {
        account->forget();
        scrobbler->setSession({});
        refresh();
    });
    refresh();
}

// --- the remote control --------------------------------------------------------------------

void PreferencesWindow::buildRemotePage() {
    XPCOG_ROWS("remote", tr("Remote"), L"\xE703");  // Segoe "Connect"

    const Row enable = row->toggle(tr("Allow remote control over HTTP"), "remoteEnable");
    static constexpr std::array kAddresses = std::to_array<Choice>({
        {"127.0.0.1", XPCOG_TRANSLATE("This computer only")},
        {"0.0.0.0", XPCOG_TRANSLATE("Any computer on the network")},
    });
    row->choice(tr("Listen on"), "remoteAddress", kAddresses);
    row->number(tr("Port"), "remotePort", 1024, 65535);
    row->toggle(tr("Allow changes, not just reading"), "remoteAllowWrite");
    const Row local = row->toggle(tr("Let programs on this computer connect without the token"),
                                  "remoteLoopbackNoToken");
    row->note(tr("For a script or a command line driving your own player. Any program on this "
                 "computer can then control it, including a web page that was told to try. "
                 "Other computers still need the token."));

    auto copy       = button(tr("Copy"));
    auto regenerate = button(tr("Regenerate"));
    auto actions    = mux::Controls::StackPanel();
    actions.Orientation(mux::Controls::Orientation::Horizontal);
    actions.Spacing(8);
    actions.Children().Append(copy);
    actions.Children().Append(regenerate);
    // A one-space hint so the card has a description line; the token is
    // written into it below, selectable, as GTK's row shows it in its subtitle.
    const Row token = row->add(tr("Access token"), actions, " ");
    const auto shown = descriptionOf(token);
    if (shown) {
        shown.IsTextSelectionEnabled(true);
        shown.FontFamily(mux::Media::FontFamily(L"Cascadia Mono, Consolas"));
    }
    const Row status = row->note("", false);

    const std::weak_ptr<int> alive = alive_;
    const auto refresh = [=, this] {
        if (alive.expired()) {
            return;
        }
        const bool  built = remote::remoteServerAvailable();
        std::string problem;
        const bool  store = app::RemoteToken::storeAvailable(&problem);
        for (const Row& part : {enable, local, token}) {
            part.enable(built && store);
        }
        copy.IsEnabled(built && store);
        regenerate.IsEnabled(built && store);
        std::string text;
        if (!built) {
            text = tr("This build has no remote-control server.");
        } else if (!store) {
            text = problem.empty() ? tr("The system password store is not available, so an access "
                                        "token cannot be kept safely.")
                                   : problem;
        } else {
            if (shown) {
                shown.Text(toH(app::RemoteToken::load()));
            }
            if (!isTrue(settings_.rawValue("remoteEnable"))) {
                text = tr("Off. Nothing is listening.");
            } else {
                const std::string address = settings_.rawValue("remoteAddress");
                const std::string where =
                    "http://" + (address == "0.0.0.0" ? std::string("<this computer>") : address) + ":" +
                    settings_.rawValue("remotePort");
                text = app::trf("Listening on %s -- open %s/docs in a browser to try it.", where, where);
                if (isTrue(settings_.rawValue("remoteLoopbackNoToken"))) {
                    text += "\n\n";
                    text += tr("Programs on this computer are connecting without the token.");
                }
                if (address == "0.0.0.0") {
                    text += "\n\n";
                    text += tr("Reachable from other machines. The connection is not encrypted and "
                               "the access token is sent with every request, so use this on a "
                               "network you trust.");
                }
            }
        }
        setNote(status, text);
    };
    // After the builder's own handler, which writes the setting this reads.
    for (const Row& part : {enable, local}) {
        part.control.as<mux::Controls::ToggleSwitch>().Toggled([refresh](auto&&, auto&&) { refresh(); });
    }
    copy.Click([shown](auto&&, auto&&) {
        if (!shown) {
            return;
        }
        winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
        package.SetText(shown.Text());
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
    });
    regenerate.Click([=, this](auto const& sender, auto&&) -> winrt::fire_and_forget {
        // A confirmation, as GTK's alert: every device holding the old token
        // stops working, and that is not something to do by a stray click.
        auto ask = mux::Controls::ContentDialog();
        ask.XamlRoot(sender.template as<mux::UIElement>().XamlRoot());
        ask.Style(mux::Application::Current()
                      .Resources()
                      .Lookup(winrt::box_value(L"DefaultContentDialogStyle"))
                      .as<mux::Style>());
        ask.Title(winrt::box_value(toH(tr("Regenerate Access Token"))));
        auto body = mux::Controls::TextBlock();
        body.TextWrapping(mux::TextWrapping::Wrap);
        body.Text(toH(tr("Every device using the current token will stop working until it is given the "
                         "new one.\n\nGenerate a new token?")));
        ask.Content(body);
        ask.PrimaryButtonText(toH(tr("Regenerate")));
        ask.CloseButtonText(toH(tr("Cancel")));
        ask.DefaultButton(mux::Controls::ContentDialogButton::Close);
        const auto answer = co_await ask.ShowAsync();
        if (answer != mux::Controls::ContentDialogResult::Primary || alive.expired()) {
            co_return;
        }
        static_cast<void>(app::RemoteToken::regenerate());
        settingChanged.publish("remoteEnable");
        refresh();
    });
    refresh();
}

// --- Advanced ----------------------------------------------------------------------------

void PreferencesWindow::buildAdvancedPage() {
    XPCOG_ROWS("advanced", tr("Advanced"), L"\xE9F5");  // Segoe "Processing"

    // Every setting no other page has a row for, by its identifier and raw,
    // as GTK's page lists them: the switch for a bool, a number box for a
    // number, a text box for the rest.
    for (const Settings::Desc& descriptor : Settings::all()) {
        if (app::hasCuratedRow(descriptor.key)) {
            continue;
        }
        const std::string key(descriptor.key);
        const std::string label(descriptor.ident);
        const std::string value = settings_.rawValue(key);
        Row made;
        if (descriptor.type == "bool") {
            auto toggle = mux::Controls::ToggleSwitch();
            toggle.MinWidth(0);
            toggle.IsOn(isTrue(value));
            toggle.Toggled([this, key](auto const& sender, auto&&) {
                settings_.setRawValue(key, sender.template as<mux::Controls::ToggleSwitch>().IsOn() ? "true"
                                                                                                    : "false");
                settingChanged.publish(key);
            });
            made = row->add(label, toggle);
        } else if (descriptor.type == "int" || descriptor.type == "double") {
            const bool whole = descriptor.type == "int";
            auto box = mux::Controls::NumberBox();
            box.Minimum(-1000000.0);
            box.Maximum(1000000.0);
            box.SmallChange(whole ? 1.0 : 0.001);
            box.SpinButtonPlacementMode(mux::Controls::NumberBoxSpinButtonPlacementMode::Compact);
            box.ValidationMode(mux::Controls::NumberBoxValidationMode::InvalidInputOverwritten);
            box.MinWidth(160);
            auto format = winrt::Windows::Globalization::NumberFormatting::DecimalFormatter();
            format.FractionDigits(whole ? 0 : 3);
            format.IsGrouped(false);
            box.NumberFormatter(format);
            box.Value(toDouble(value));
            box.ValueChanged([this, key, whole](auto&&, mux::Controls::NumberBoxValueChangedEventArgs const& args) {
                const double v = args.NewValue();
                if (std::isnan(v)) {
                    return;
                }
                settings_.setRawValue(key, whole ? std::to_string(static_cast<long long>(std::lround(v)))
                                                 : std::to_string(v));
                settingChanged.publish(key);
            });
            made = row->add(label, box);
        } else {
            auto box = mux::Controls::TextBox();
            box.Width(280);
            box.Text(toH(value));
            // Written when the box is left or Enter is pressed, as GTK's
            // apply button has it, not per keystroke.
            const auto commit = [this, key, box] {
                const std::string typed = toUtf8(box.Text());
                if (typed != settings_.rawValue(key)) {
                    settings_.setRawValue(key, typed);
                    settingChanged.publish(key);
                }
            };
            box.LostFocus([commit](auto&&, auto&&) { commit(); });
            box.KeyDown([commit](auto&&, mux::Input::KeyRoutedEventArgs const& args) {
                if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
                    commit();
                }
            });
            made = row->add(label, box);
        }
        if (app::isInternalKey(descriptor.key)) {
            made.enable(false);
            mux::Controls::ToolTipService::SetToolTip(
                made.element, winrt::box_value(toH(tr("Maintained automatically, and not meant to be edited."))));
        }
    }
    row->note(tr("The greyed rows are what XPCog remembers about the last session, not settings."));
}

}  // namespace xpcog::winui
