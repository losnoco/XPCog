// The suite that needs a screen, and the one fault that justifies having one.
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
// It runs under Xvfb where tests/CMakeLists.txt found it, and skips rather than
// fails where there is no display at all, which is the same bargain the
// corpus-gated codec tests make.

#include "PreferencesDialog.hpp"

#include "xpcog/core/Settings.hpp"

#include <catch2/catch_test_macros.hpp>

#include <wx/app.h>
#include <wx/arrstr.h>
#include <wx/bookctrl.h>
#include <wx/frame.h>
#include <wx/init.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>

#include <algorithm>
#include <memory>
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
