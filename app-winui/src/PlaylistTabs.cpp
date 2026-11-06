#include "PlaylistTabs.hpp"

#include "Translations.hpp"

#include <cstdint>
#include <utility>

namespace xpcog::winui {

namespace {

[[nodiscard]] PlaylistSet::Id idOf(const winrt::Windows::Foundation::IInspectable& item) {
    return winrt::unbox_value<std::int64_t>(item.as<mux::Controls::TabViewItem>().Tag());
}

}  // namespace

PlaylistTabs::PlaylistTabs(PlaylistSet& playlists, Hooks hooks)
    : playlists_(playlists), hooks_(std::move(hooks)) {
    view_ = mux::Controls::TabView();
    view_.VerticalAlignment(mux::VerticalAlignment::Top);
    view_.CanDragTabs(false);
    view_.CanReorderTabs(true);
    view_.AllowDropTabs(false);
    view_.IsAddTabButtonVisible(true);

    view_.AddTabButtonClick(
        [this](auto&&, auto&&) { hooks_.run(app::CommandId::FileNewPlaylist); });

    view_.TabCloseRequested(
        [this](auto&&, const mux::Controls::TabViewTabCloseRequestedEventArgs& args) {
            hooks_.closeRequested(idOf(args.Tab()));
        });

    view_.SelectionChanged([this](auto&&, auto&&) {
        if (syncing_) {
            return;
        }
        const auto selected = view_.SelectedItem();
        if (selected != nullptr) {
            playlists_.show(idOf(selected));
        }
    });

    view_.TabItemsChanged([this](auto&&, auto&&) { onItemsChanged(); });

    refresh();
}

mux::Controls::MenuFlyout PlaylistTabs::makeMenu(PlaylistSet::Id id) {
    auto menu = mux::Controls::MenuFlyout();

    // The commands act on what is shown, so the tab comes first.
    menu.Opening([this, id](auto&&, auto&&) { playlists_.show(id); });

    const auto add = [this, &menu](const std::string& label, app::CommandId command) {
        auto item = mux::Controls::MenuFlyoutItem();
        item.Text(toH(label));
        item.Click([this, command](auto&&, auto&&) { hooks_.run(command); });
        menu.Items().Append(item);
    };
    add(app::tr("Rename Playlist..."), app::CommandId::FileRenamePlaylist);
    add(app::tr("Delete Playlist..."), app::CommandId::FileDeletePlaylist);
    return menu;
}

mux::Controls::TabViewItem PlaylistTabs::makeTab(PlaylistSet::Id id) {
    auto item = mux::Controls::TabViewItem();
    item.Tag(winrt::box_value(static_cast<std::int64_t>(id)));
    item.ContextFlyout(makeMenu(id));
    return item;
}

void PlaylistTabs::onItemsChanged() {
    if (syncing_) {
        return;
    }

    // A drag arrives as a removal and then an insertion; only the second has
    // every tab in it.
    const auto items = view_.TabItems();
    if (items.Size() != playlists_.size()) {
        return;
    }
    for (std::uint32_t i = 0; i < items.Size(); ++i) {
        static_cast<void>(playlists_.move(idOf(items.GetAt(i)), i));
    }
}

void PlaylistTabs::setPlaybackActive(bool active) {
    if (active != active_) {
        active_ = active;
        refresh();
    }
}

void PlaylistTabs::refresh() {
    syncing_ = true;

    const auto items = view_.TabItems();

    // Whatever the set no longer has.
    for (std::uint32_t i = items.Size(); i-- > 0;) {
        if (!playlists_.indexOf(idOf(items.GetAt(i)))) {
            items.RemoveAt(i);
        }
    }

    // Then position by position, moving a tab that is elsewhere rather than
    // making it again.
    for (std::size_t i = 0; i < playlists_.size(); ++i) {
        const PlaylistSet::Id id    = playlists_.at(i).id;
        const auto            index = static_cast<std::uint32_t>(i);

        mux::Controls::TabViewItem tab{nullptr};
        for (std::uint32_t j = index; j < items.Size(); ++j) {
            if (idOf(items.GetAt(j)) == id) {
                tab = items.GetAt(j).as<mux::Controls::TabViewItem>();
                if (j != index) {
                    items.RemoveAt(j);
                    items.InsertAt(index, tab);
                }
                break;
            }
        }
        if (tab == nullptr) {
            tab = makeTab(id);
            items.InsertAt(index, tab);
        }

        tab.Header(winrt::box_value(toH(hooks_.title(id))));
        tab.IsClosable(playlists_.size() > 1);

        if (id == playlists_.playingId() && active_) {
            auto icon = mux::Controls::SymbolIconSource();
            icon.Symbol(mux::Controls::Symbol::Play);
            tab.IconSource(icon);
        } else {
            tab.IconSource(nullptr);
        }
    }

    if (const auto shown = playlists_.indexOf(playlists_.shownId())) {
        view_.SelectedIndex(static_cast<std::int32_t>(*shown));
    }

    view_.Visibility(playlists_.size() > 1 ? mux::Visibility::Visible
                                           : mux::Visibility::Collapsed);

    syncing_ = false;
}

}  // namespace xpcog::winui
