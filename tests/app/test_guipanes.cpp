// The suite that needs a screen, and the faults that justify having one.
//
// Two of them so far, and they have the same shape: a control that is the wrong
// size only once a real toolkit has given it a real one. Neither is visible to a
// test that calls functions.
//
// A note in the preferences form re-wraps itself from its own size event, and
// everything a wxStaticText does about its label -- wrapping it, and laying the
// pane out afterwards -- raises another size event before it returns. Get the
// bookkeeping wrong and the handler answers the events it caused itself, at two
// widths that never agree, until the stack runs out. That is not a state any
// free function can be asked about: it only exists once real windows are being
// given real sizes by a real toolkit, and switching to the Output pane was
// enough to hit it.
//
// So this walks the dialog the way a listener does. It fails by *crashing*,
// which is unusual enough to say out loud: a stack overflow cannot be caught
// and reported, so the evidence is ctest recording a subprocess that died
// rather than an assertion that failed. The checks below are the part that can
// be asserted -- that a note is still wrapped to somewhere it can be read.
//
// The third is the playlist's columns, whose widths are only decided once the
// toolkit has laid the control out: which column absorbs a wider window, and
// whether the duration column keeps the width it was given. Measured, because
// the toolkit's own rule -- the last column takes the slack -- is the bug.
//
// It runs under Xvfb where tests/CMakeLists.txt found it, and skips rather than
// fails where there is no display at all, which is the same bargain the
// corpus-gated codec tests make.

#include "EqualizerPanel.hpp"
#include "PlaylistColumns.hpp"
#include "PlaylistDataModel.hpp"
#include "PreferencesDialog.hpp"

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/Equalizer.hpp"
#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/library/PlaylistView.hpp"

#include <catch2/catch_test_macros.hpp>

#include <wx/app.h>
#include <wx/arrstr.h>
#include <wx/bookctrl.h>
#include <wx/dataview.h>
#include <wx/frame.h>
#include <wx/init.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

/// wxEntryStart() creates the application object through the initializer this
/// macro registers, and does not start a main loop -- which is what is wanted
/// here, since the events this test cares about are the synchronous ones raised
/// by layout rather than anything a loop would deliver.
class PanesApp : public wxApp {
public:
    bool OnInit() override { return true; }
};

}  // namespace

wxIMPLEMENT_APP_NO_MAIN(PanesApp);

namespace {

/// Starts and stops the toolkit around one test.
class Toolkit {
public:
    Toolkit() {
        static char  name[] = "xpcog-gui-tests";
        static char* argv[] = {name, nullptr};
        int          argc   = 1;
        started_            = wxEntryStart(argc, argv);
    }

    ~Toolkit() {
        if (started_) {
            wxEntryCleanup();
        }
    }

    Toolkit(const Toolkit&)            = delete;
    Toolkit& operator=(const Toolkit&) = delete;

    /// False when there is no display to open, which is a skip and not a
    /// failure. wx reports it the same way GTK does, by refusing to initialise.
    [[nodiscard]] bool started() const { return started_; }

private:
    bool started_ = false;
};

/// The dialog's page stack, which it keeps to itself. Found rather than exposed:
/// a test is not a reason to widen a header, and the shape it looks for -- one
/// book control in the dialog -- is the shape the dialog would have to lose for
/// this to stop compiling anyway.
[[nodiscard]] wxBookCtrlBase* findBook(wxWindow* window) {
    if (auto* book = dynamic_cast<wxBookCtrlBase*>(window)) {
        return book;
    }
    for (wxWindow* child : window->GetChildren()) {
        if (auto* found = findBook(child)) {
            return found;
        }
    }
    return nullptr;
}

/// How wide the widest line of `text` is drawn, measured in its own font.
[[nodiscard]] int widestLine(const wxStaticText& text) {
    int widest = 0;
    for (const wxString& line : wxSplit(text.GetLabel(), '\n')) {
        widest = std::max(widest, text.GetTextExtent(line).GetWidth());
    }
    return widest;
}

/// Every wrapped paragraph on `pane`. A caption is one line and is not what is
/// being checked; a note that has been through Wrap() carries the breaks in its
/// label, which is what tells the two apart from outside the dialog.
[[nodiscard]] std::vector<wxStaticText*> notesOn(wxWindow* pane) {
    std::vector<wxStaticText*> notes;
    for (wxWindow* child : pane->GetChildren()) {
        auto* text = dynamic_cast<wxStaticText*>(child);
        if (text != nullptr && text->GetLabel().Contains("\n")) {
            notes.push_back(text);
        }
    }
    return notes;
}

/// Lets the toolkit finish laying out `list`. wxYield() drains what is
/// pending, but GTK allocates from its frame clock, which schedules itself a
/// frame later rather than being pending now -- and under Xvfb with no window
/// manager a top-level window can take a few frames to settle at the size it
/// was asked for. So this waits for the control's size to stop changing.
void settle(const wxWindow& list) {
    wxSize last  = list.GetClientSize();
    int    still = 0;
    for (int i = 0; i < 100 && still < 5; ++i) {
        wxMilliSleep(20);
        wxYield();
        const wxSize now = list.GetClientSize();
        still            = now == last ? still + 1 : 0;
        last             = now;
    }
}

/// What the columns add up to.
[[nodiscard]] int columnsWidth(const wxDataViewCtrl& list) {
    int total = 0;
    for (unsigned int i = 0; i < list.GetColumnCount(); ++i) {
        total += list.GetColumn(i)->GetWidth();
    }
    return total;
}

/// The column showing `column`, wherever it has been dragged to.
[[nodiscard]] wxDataViewColumn* columnFor(wxDataViewCtrl& list,
                                          xpcog::PlaylistView::Column column) {
    for (unsigned int i = 0; i < list.GetColumnCount(); ++i) {
        if (list.GetColumn(i)->GetModelColumn() == static_cast<unsigned int>(column)) {
            return list.GetColumn(i);
        }
    }
    return nullptr;
}

void checkNotesAreReadable(wxWindow* page) {
    auto* pane = dynamic_cast<wxScrolled<wxPanel>*>(page);
    REQUIRE(pane != nullptr);

    const int client = pane->GetClientSize().GetWidth();
    // A pane whose form is wider than itself scrolls sideways, and then the
    // controls are legitimately off the right of the screen. The notes are the
    // half that must not be.
    const bool scrollsSideways = pane->GetVirtualSize().GetWidth() > client;

    for (wxStaticText* note : notesOn(pane)) {
        const int widest = widestLine(*note);

        // Not clipped: a wxStaticText neither scrolls nor ellipsizes, so a line
        // longer than the window it is drawn in simply loses its end. That was
        // the clipped help text this dialog once shipped with.
        CHECK(widest <= note->GetSize().GetWidth());

        // And not off the side either, where the pane is not scrolling to make
        // it reachable.
        if (!scrollsSideways) {
            CHECK(note->GetPosition().x + widest <= client);
        }
    }
}

}  // namespace

TEST_CASE("every preference pane can be opened and resized", "[gui][preferences]") {
    Toolkit toolkit;
    if (!toolkit.started()) {
        SKIP("no display: wx could not initialise the toolkit");
    }

    auto            store = xpcog::makeMemorySettingsStore();
    xpcog::Settings settings(*store);

    // A parent to own the dialog, and shown because an unmapped window is never
    // given a size -- and a size is the whole subject here.
    auto* frame = new wxFrame(nullptr, wxID_ANY, "xpcog-gui-tests");
    frame->Show();

    // On the heap and owned by the frame, because that is how a wxDialog is
    // taken down: Destroy() defers the delete to the next idle, which a stack
    // object cannot survive.
    auto* dialog = new xpcog::app::PreferencesDialog(frame, settings);
    dialog->Show();
    wxYield();

    wxBookCtrlBase* book = findBook(dialog);
    REQUIRE(book != nullptr);
    REQUIRE(book->GetPageCount() > 1);

    for (std::size_t page = 0; page < book->GetPageCount(); ++page) {
        INFO("pane: " << book->GetPageText(page).utf8_string());
        book->ChangeSelection(page);
        dialog->Layout();
        wxYield();
        checkNotesAreReadable(book->GetPage(page));

        // At the size the dialog opens at, the Output pane fits without
        // scrolling. It is the pane that did not, at 700x480: its device and
        // sample-rate rows wanted 672x383 of a 495x337 client, so the pane a
        // listener most often needs opened with a scrollbar and half a form.
        // The dialog's default size is chosen against this pane, and this is
        // what keeps the two from drifting apart. Advanced is the one pane
        // meant to scroll, and is not checked.
        if (book->GetPageText(page) == "Output") {
            auto* pane = dynamic_cast<wxScrolled<wxPanel>*>(book->GetPage(page));
            REQUIRE(pane != nullptr);
            CHECK(pane->GetVirtualSize().GetWidth() <= pane->GetClientSize().GetWidth());
            CHECK(pane->GetVirtualSize().GetHeight() <= pane->GetClientSize().GetHeight());
        }

        // Twice more at another width, because a note that wraps once on the way
        // in is a note that has answered one size event. The crash needed the
        // second.
        const wxSize opened = dialog->GetSize();
        dialog->SetSize(opened.GetWidth() - dialog->FromDIP(120), opened.GetHeight());
        wxYield();
        checkNotesAreReadable(book->GetPage(page));

        dialog->SetSize(opened);
        wxYield();
        checkNotesAreReadable(book->GetPage(page));
    }

    // The frame takes the dialog with it, and the yield is what actually runs
    // both deletes -- Destroy() only queues them.
    frame->Destroy();
    wxYield();
}

TEST_CASE("the equaliser's sliders are given room to be drawn", "[gui][equalizer]") {
    Toolkit toolkit;
    if (!toolkit.started()) {
        SKIP("no display: wx could not initialise the toolkit");
    }

    auto            store = xpcog::makeMemorySettingsStore();
    xpcog::Settings settings(*store);

    auto* frame = new wxFrame(nullptr, wxID_ANY, "xpcog-gui-tests", wxDefaultPosition,
                              wxSize(1000, 320));
    auto* panel = new xpcog::app::EqualizerPanel(frame, settings);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(panel, 1, wxEXPAND);
    frame->SetSizer(sizer);
    frame->Show();
    frame->Layout();
    wxYield();

    std::vector<wxSlider*> sliders;
    for (wxWindow* child : panel->GetChildren()) {
        if (auto* slider = dynamic_cast<wxSlider*>(child)) {
            sliders.push_back(slider);
        }
    }

    // The preamp and every band.
    REQUIRE(sliders.size() == xpcog::Equalizer::bandSettingsKeys().size() + 1);

    for (const wxSlider* slider : sliders) {
        // A control narrower than the toolkit will draw it in is not a narrow
        // control, it is an invisible one: GTK measures a scale's trough against
        // the width it was given, and at 24 pixels against a 34-pixel minimum it
        // warns once per slider and draws no scale at all. The panel therefore
        // asks for a height and leaves the width to the control.
        CHECK(slider->GetSize().GetWidth() >= slider->GetBestSize().GetWidth());
        CHECK(slider->GetSize().GetHeight() > 0);
    }

    // And the size the pane opens at is the whole curve's, not a scrollbar's:
    // GetBestSize() answers thirteen here, because a wxScrolled that scrolls
    // sideways reports its minimum width plus a scrollbar, and that is the whole
    // reason contentSize() exists.
    const wxSize content = panel->contentSize();
    CHECK(content.GetWidth() >=
          static_cast<int>(sliders.size()) * sliders.front()->GetSize().GetWidth());
    CHECK(content.GetHeight() >= panel->GetBestSize().GetHeight());

    frame->Destroy();
    wxYield();
}

TEST_CASE("the title column takes the slack, and the widths come back",
          "[gui][playlist]") {
    using Column = xpcog::PlaylistView::Column;

    Toolkit toolkit;
    if (!toolkit.started()) {
        SKIP("no display: wx could not initialise the toolkit");
    }

    auto            store = xpcog::makeMemorySettingsStore();
    xpcog::Settings settings(*store);

    xpcog::Playlist     playlist;
    xpcog::PlaylistView view{playlist};

    auto* frame = new wxFrame(nullptr, wxID_ANY, "xpcog-gui-tests", wxDefaultPosition,
                              wxSize(1000, 400));
    auto* list  = new wxDataViewCtrl(frame, wxID_ANY);
    auto* model = new xpcog::app::PlaylistDataModel(view);
    list->AssociateModel(model);
    model->DecRef();
    auto  columns = std::make_unique<xpcog::app::PlaylistColumns>(*list, settings);
    auto* sizer   = new wxBoxSizer(wxVERTICAL);
    sizer->Add(list, 1, wxEXPAND);
    frame->SetSizer(sizer);
    columns->restore();
    frame->Show();
    frame->Layout();
    settle(*list);

    wxDataViewColumn* title  = columnFor(*list, Column::Title);
    wxDataViewColumn* album  = columnFor(*list, Column::Album);
    wxDataViewColumn* length = columnFor(*list, Column::Length);
    REQUIRE(title != nullptr);
    REQUIRE(album != nullptr);
    REQUIRE(length != nullptr);

    // The control's client size on GTK is the scrolled window's, which is the
    // tree view's plus a frame of a pixel or two; the columns fill the tree.
    constexpr int kBorder = 4;

    // Length is the last column, and the one every toolkit would hand the
    // leftover to. It must be exactly as wide as it was declared -- 64 dialog
    // units -- with the window well wider than the columns' defaults add up to.
    const int declaredLength = list->FromDIP(64);
    CHECK(length->GetWidth() == declaredLength);

    // The slack is Title's: the columns add up to the control, not short of it.
    const int titleAtDefault = title->GetWidth();
    const int clientAtDefault = list->GetClientSize().GetWidth();
    CHECK(titleAtDefault > list->FromDIP(280));
    CHECK(std::abs(columnsWidth(*list) - clientAtDefault) <= kBorder);

    // Widen the window: Title grows by the difference, Length does not move.
    frame->SetSize(wxSize(1300, 400));
    settle(*list);
    const int grownBy = list->GetClientSize().GetWidth() - clientAtDefault;
    REQUIRE(grownBy > 0);
    CHECK(length->GetWidth() == declaredLength);
    CHECK(title->GetWidth() == titleAtDefault + grownBy);
    CHECK(std::abs(columnsWidth(*list) - list->GetClientSize().GetWidth()) <= kBorder);

    // Narrow it below what the columns need: Title stops at its floor rather
    // than vanishing, and the control scrolls sideways instead.
    frame->SetSize(wxSize(400, 400));
    settle(*list);
    CHECK(length->GetWidth() == declaredLength);
    CHECK(title->GetWidth() == list->FromDIP(80));

    // A width the listener chose survives a restart. Album is set as a drag
    // would set it, remembered, and read back into a fresh control -- one
    // that is shown, since a column that was never laid out has no width to
    // read.
    frame->SetSize(wxSize(1000, 400));
    settle(*list);
    album->SetWidth(list->FromDIP(230));
    settle(*list);
    CHECK(title->GetWidth() == titleAtDefault - list->FromDIP(50));
    columns->persist();
    const std::string saved = settings.rawValue("xpcog.playlist.columns");
    CHECK(saved.find("album=230") != std::string::npos);
    CHECK(saved.find("length=64") != std::string::npos);
    // Title's is derived, so it is not written: a saved width would only be
    // overridden by the fit anyway, and could go stale.
    CHECK(saved.find("title=") == std::string::npos);

    auto* again      = new wxDataViewCtrl(frame, wxID_ANY);
    auto* modelAgain = new xpcog::app::PlaylistDataModel(view);
    again->AssociateModel(modelAgain);
    modelAgain->DecRef();
    auto columnsAgain = std::make_unique<xpcog::app::PlaylistColumns>(*again, settings);
    columnsAgain->restore();
    sizer->Add(again, 1, wxEXPAND);
    frame->Layout();
    settle(*again);
    CHECK(columnFor(*again, Column::Album)->GetWidth() == list->FromDIP(230));
    CHECK(columnFor(*again, Column::Length)->GetWidth() == declaredLength);
    CHECK(columnFor(*again, Column::Title)->GetWidth() == title->GetWidth());

    columnsAgain.reset();
    columns.reset();
    frame->Destroy();
    wxYield();
}
