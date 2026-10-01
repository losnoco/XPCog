// The command tables, now that they are read by more than one toolkit.
//
// Commands.cpp used to build wxMenus as well as hold the tables, so everything
// here was checked implicitly by a window existing. The tables are toolkit-free
// now and two things came out of them that are hand-written where wx was doing
// the work. Both are the silent kind.

#include "Commands.hpp"
#include "WxMenus.hpp"

#include "Text.hpp"

#include <catch2/catch_test_macros.hpp>

#include <wx/control.h>

#include <set>
#include <string>
#include <string_view>

using namespace xpcog::app;

TEST_CASE("mnemonics come out the way wx takes them out", "[wx][commands]") {
    // stripMnemonics is a reimplementation of wxControl::RemoveMnemonics, and a
    // reimplementation is only worth having if it agrees. Checked against wx
    // itself over every label the application actually ships, rather than
    // against a handful of cases someone thought of.
    std::size_t checked = 0;

    const auto agree = [&](std::string_view label) {
        INFO("label: " << label);
        CHECK(stripMnemonics(label) ==
              std::string(wxControl::RemoveMnemonics(toWx(label)).utf8_string()));
        ++checked;
    };

    for (const MenuItem& item : menuLayout()) {
        if (item.menu != nullptr) {
            agree(item.menu);
        }
        agree(item.label);
    }
    for (const MenuItem& item : playlistMenuLayout()) {
        agree(item.label);
    }

    CHECK(checked > 50);
}

TEST_CASE("the double ampersand survives", "[wx][commands]") {
    // The case a naive "drop every &" gets wrong, and the reason the loop above
    // is not just a smoke test: the View menu really carries "&Pitch && Tempo".
    CHECK(stripMnemonics("&Pitch && Tempo") == "Pitch & Tempo");
    CHECK(stripMnemonics("&File") == "File");
    CHECK(stripMnemonics("No mnemonic") == "No mnemonic");
    CHECK(stripMnemonics("") == "");

    // wx drops a trailing lone '&' as well; pinned so the two cannot diverge on
    // a label somebody mistypes.
    CHECK(stripMnemonics("Trailing&") ==
          std::string(wxControl::RemoveMnemonics("Trailing&").utf8_string()));
}

TEST_CASE("every id in a layout is unique to its table", "[commands]") {
    // The menu bar's rows. A duplicate id here would give two menu items one
    // Bind and one EVT_UPDATE_UI answer, so the second would silently take the
    // first one's enabled state.
    std::set<int> seen;
    for (const MenuItem& item : menuLayout()) {
        INFO("label: " << item.label);
        CHECK(seen.insert(static_cast<int>(item.id)).second);
    }
}

TEST_CASE("a label is looked up, not pasted", "[commands]") {
    // commandLabel reads the menu table, so a toolbar tool and a menu item
    // carrying one id cannot end up named differently. Asserted in English,
    // which is what an untranslated build answers.
    CHECK(commandLabel(PlaybackNext) == "Next");
    CHECK(commandLabel(ViewSpeed) == "Pitch & Tempo");

    // A command with no menu row invents nothing.
    CHECK(commandLabel(FirstWidgetId).empty());
    CHECK(commandAccelerator(FirstWidgetId).empty());
}

TEST_CASE("the accelerator is handed over as the table spells it", "[commands]") {
    // Not rendered here: wx turns Ctrl into Cmd and draws macOS's own symbols,
    // and a frontend painting its own menus writes the literal. Both need the
    // unrendered string, which is what this returns.
    CHECK(commandAccelerator(PlaybackNext) == "Ctrl+Right");
    CHECK(commandAccelerator(PlaybackPlayPause).empty());
}

TEST_CASE("the toolbar is the transport, in the transport's order", "[commands]") {
    // toolbarLayout builds itself from transportLayout rather than repeating
    // it, so that the mini player and the toolbar cannot drift apart.
    const auto& transport = transportLayout();
    const auto& toolbar   = toolbarLayout();

    REQUIRE(toolbar.size() == transport.size());
    for (std::size_t i = 0; i < toolbar.size(); ++i) {
        CHECK(toolbar[i].id == transport[i]);
    }
}
