#include "AboutDialog.hpp"

#include "Credits.hpp"
#include "Translations.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Version.hpp"

#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::winui {
namespace {

using app::tr;

constexpr const char* kMiddot = " \xC2\xB7 ";
/// The pages' height: long enough for the licences to be read in place, short
/// enough for the dialog to fit a laptop screen with room around it.
constexpr double kPageHeight = 360;
constexpr double kDialogWidth = 640;

std::string buildInfo() {
#if defined(__clang__)
    const std::string compiler =
        "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(_MSC_VER)
    const std::string compiler = "MSVC " + std::to_string(_MSC_VER);
#else
    const std::string compiler = "unknown compiler";
#endif
    return compiler + kMiddot + "WinUI 3";
}

/// A translated message with its markup taken out. Several here share their
/// msgids with the wx dialog, whose HTML page could show <b> and <i>; the
/// same msgid keeps the existing translations, and a TextBlock shows the
/// sentence without the tags.
std::string plain(std::string text) {
    std::string out;
    out.reserve(text.size());
    bool inTag = false;
    for (const char c : text) {
        if (c == '<') {
            inTag = true;
        } else if (c == '>') {
            inTag = false;
        } else if (!inTag) {
            out += c;
        }
    }
    return out;
}

mux::Controls::TextBlock paragraph(const std::string& text) {
    auto block = mux::Controls::TextBlock();
    block.Text(toH(text));
    block.TextWrapping(mux::TextWrapping::Wrap);
    block.IsTextSelectionEnabled(true);
    return block;
}

mux::Controls::TextBlock heading(const std::string& text) {
    auto block = paragraph(text);
    block.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
    block.Margin(mux::ThicknessHelper::FromLengths(0, 8, 0, 0));
    return block;
}

mux::Controls::HyperlinkButton link(const std::string& label, const wchar_t* url) {
    auto button = mux::Controls::HyperlinkButton();
    button.Content(winrt::box_value(toH(label)));
    button.NavigateUri(winrt::Windows::Foundation::Uri(url));
    button.Padding(mux::ThicknessHelper::FromUniformLength(0));
    return button;
}

/// A table: one row per entry, the first column bold, the rest wrapping.
class Table {
public:
    explicit Table(std::vector<mux::GridLength> widths) {
        grid_.ColumnSpacing(16);
        grid_.RowSpacing(6);
        for (const auto& width : widths) {
            auto column = mux::Controls::ColumnDefinition();
            column.Width(width);
            grid_.ColumnDefinitions().Append(column);
        }
    }

    void add(const std::vector<std::string>& cells, bool monospace = false) {
        auto row = mux::Controls::RowDefinition();
        row.Height(mux::GridLengthHelper::Auto());
        grid_.RowDefinitions().Append(row);
        const int index = static_cast<int>(grid_.RowDefinitions().Size()) - 1;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            auto cell = paragraph(cells[i]);
            if (i == 0) {
                cell.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
            } else {
                cell.Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
                if (monospace) {
                    cell.FontFamily(mux::Media::FontFamily(L"Cascadia Mono, Consolas"));
                }
            }
            mux::Controls::Grid::SetRow(cell, index);
            mux::Controls::Grid::SetColumn(cell, static_cast<int>(i));
            grid_.Children().Append(cell);
        }
    }

    [[nodiscard]] mux::Controls::Grid grid() const { return grid_; }

private:
    mux::Controls::Grid grid_;
};

mux::Controls::StackPanel page() {
    auto panel = mux::Controls::StackPanel();
    panel.Spacing(8);
    panel.Padding(mux::ThicknessHelper::FromLengths(0, 0, 16, 0));  // clear of the scroll bar
    return panel;
}

mux::UIElement aboutPage() {
    auto panel = page();
    panel.Children().Append(paragraph(tr("An audio player for Windows and Linux.")));
    panel.Children().Append(paragraph(tr("Copyright \xC2\xA9 2026 the XPCog authors.") + "\n" +
                                      tr("Copyright \xC2\xA9 2005\xE2\x80\x93"
                                         "2026 Vincent Spader, Christopher Snowhill and the Cog "
                                         "authors.")));
    panel.Children().Append(paragraph(
        plain(tr("XPCog is free software, licensed under the <b>GNU General Public "
                 "License, version 3 or later</b>. It comes with absolutely no warranty."))));
    auto links = mux::Controls::StackPanel();
    links.Spacing(4);
    links.Children().Append(link(tr("Cog:") + " cog.losno.co", L"https://cog.losno.co/"));
    links.Children().Append(
        link(tr("Source:") + " github.com/losnoco/XPCog", L"https://github.com/losnoco/XPCog"));
    panel.Children().Append(links);
    return panel;
}

/// What this build can play, grouped by the decoder that claims it, in the
/// registry's order -- descending priority, the order a file is offered to
/// them. See the wx dialog's formatsPage() for why it is grouped at all.
mux::UIElement formatsPage(const PluginRegistry& registry) {
    std::map<std::string_view, int> claims;
    for (const DecoderDescriptor& decoder : registry.decoders()) {
        for (const std::string_view extension : decoder.extensions) {
            ++claims[extension];
        }
    }

    auto panel = page();
    panel.Children().Append(paragraph(app::fmt(
        app::trn("%zu decoder is compiled in. A file goes to the first row "
                 "below that claims its extension:",
                 "%zu decoders are compiled in. A file goes to the first row "
                 "below that claims its extension:",
                 registry.decoderCount()),
        registry.decoderCount())));

    Table table({mux::GridLengthHelper::Auto(),
                 mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star)});
    bool anyShared = false;
    for (const DecoderDescriptor& decoder : registry.decoders()) {
        std::string extensions;
        for (const std::string_view extension : decoder.extensions) {
            if (!extensions.empty()) {
                extensions += kMiddot;
            }
            extensions += extension;
            if (claims[extension] > 1) {
                extensions += "*";
                anyShared = true;
            }
        }
        // silence:// and the HLS decoder are chosen by scheme and by MIME type;
        // saying so beats an empty cell that reads as a bug.
        const bool none = extensions.empty();
        if (none) {
            extensions = tr("chosen by scheme or MIME type");
        }
        // The descriptor's own name, untranslated: the identifier a bug report
        // should quote.
        table.add({std::string(decoder.name), extensions}, !none);
    }
    panel.Children().Append(table.grid());

    if (anyShared) {
        panel.Children().Append(paragraph(
            tr("An extension marked * is claimed by more than one decoder. The first row that "
               "claims it wins \xE2\x80\x94 which is what keeps FFmpeg, deliberately registered "
               "below the rest, from taking files a dedicated decoder handles better.")));
    }
    return panel;
}

void addComponents(const mux::Controls::StackPanel& panel, const std::string& title,
                   std::span<const app::Component> components) {
    panel.Children().Append(heading(title));
    Table table({mux::GridLengthHelper::Auto(), mux::GridLengthHelper::Auto(),
                 mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star)});
    for (const app::Component& component : components) {
        table.add({component.name, component.licence, tr(component.purpose)});
    }
    panel.Children().Append(table.grid());
}

mux::UIElement licencesPage() {
    auto panel = page();
    panel.Children().Append(paragraph(plain(tr(
        "XPCog is built from the following third-party components. Which decoder "
        "libraries a given build actually contains depends on how it was "
        "configured \xE2\x80\x94 the Formats tab lists what <i>this</i> build "
        "can play. Each library's own licence text ships with its sources."))));
    addComponents(panel, tr("The player"), app::playerComponents());
    addComponents(panel, tr("The interface"), app::winuiComponents());
    addComponents(panel, tr("Decoding and tags"), app::codecComponents());
    addComponents(panel, tr("Data"), app::dataComponents());
    return panel;
}

}  // namespace

void fillAboutDialog(const mux::Controls::ContentDialog& box, const PluginRegistry& registry) {
    // ContentDialog caps its width well short of a table with three columns.
    box.Resources().Insert(winrt::box_value(L"ContentDialogMaxWidth"), winrt::box_value(kDialogWidth + 48));

    auto body = mux::Controls::StackPanel();
    body.Width(kDialogWidth);
    body.Spacing(8);

    body.Children().Append(paragraph(
        app::trf("Version %s \xC2\xB7 %s", std::string(kVersionString), buildInfo())));

    // Built once, each kept while the dialog is up, and swapped into the one
    // scroller -- the wx dialog's notebook.
    auto pages = std::make_shared<std::vector<mux::UIElement>>(std::vector<mux::UIElement>{
        aboutPage(), formatsPage(registry), licencesPage()});

    auto scroller = mux::Controls::ScrollViewer();
    scroller.Height(kPageHeight);
    scroller.Content(pages->front());

    auto selector = mux::Controls::SelectorBar();
    for (const char* label :
         {XPCOG_TRANSLATE("About"), XPCOG_TRANSLATE("Formats"), XPCOG_TRANSLATE("Licences")}) {
        auto item = mux::Controls::SelectorBarItem();
        item.Text(toH(tr(label)));
        selector.Items().Append(item);
    }
    selector.SelectedItem(selector.Items().GetAt(0));
    selector.SelectionChanged([pages, scroller](mux::Controls::SelectorBar const& bar, auto&&) {
        uint32_t index = 0;
        if (bar.Items().IndexOf(bar.SelectedItem(), index) && index < pages->size()) {
            scroller.Content((*pages)[index]);
            scroller.ChangeView(nullptr, 0.0, nullptr, true);
        }
    });

    body.Children().Append(selector);
    body.Children().Append(scroller);
    box.Content(body);
}

}  // namespace xpcog::winui
