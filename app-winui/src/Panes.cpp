#include "Panes.hpp"

#include "SpeedCurve.hpp"
#include "TrackText.hpp"
#include "Translations.hpp"

#include "xpcog/core/audio/Equalizer.hpp"
#include "xpcog/core/audio/EqualizerPresets.hpp"
#include "xpcog/core/library/Library.hpp"

#include <winrt/Windows.Storage.Streams.h>

#include <shcore.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string_view>
#include <utility>

namespace xpcog::winui {

using app::tr;

namespace {

// The GTK panes' formatting helpers, verbatim: the readouts should not differ
// between the two players by so much as a decimal place.
[[nodiscard]] std::string frequencyLabel(double hertz) {
    char buffer[16] = {};
    if (hertz < 1000.0) {
        std::snprintf(buffer, sizeof(buffer), "%g", hertz);
        return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "%gk", hertz / 1000.0);
    return buffer;
}

[[nodiscard]] std::string decibelLabel(double db) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%.1f", db);
    return buffer;
}

[[nodiscard]] double toDouble(const std::string& text) {
    try {
        return text.empty() ? 0.0 : std::stod(text);
    } catch (const std::exception&) {
        return 0.0;
    }
}

[[nodiscard]] std::string ratioLabel(double ratio) {
    char buffer[24] = {};
    std::snprintf(buffer, sizeof(buffer), "%.2f\xC3\x97", ratio);
    return buffer;
}

constexpr double kEqRangeDb = 20.0;
constexpr double kEqStepDb  = 0.1;

/// Encoded image bytes -- whatever the library cached: JPEG, PNG, WebP -- as
/// a XAML image source. Synchronously: SetSource on a memory stream decodes
/// on the spot, which for one cover is less than the async round trip costs.
mux::Media::Imaging::BitmapImage imageFrom(const std::vector<std::byte>& bytes) {
    winrt::com_ptr<IStream> memory;
    memory.attach(::SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()),
                                      static_cast<UINT>(bytes.size())));
    if (!memory) {
        return nullptr;
    }
    winrt::Windows::Storage::Streams::IRandomAccessStream stream;
    winrt::check_hresult(::CreateRandomAccessStreamOverStream(
        memory.get(), BSOS_DEFAULT,
        winrt::guid_of<winrt::Windows::Storage::Streams::IRandomAccessStream>(),
        winrt::put_abi(stream)));
    auto image = mux::Media::Imaging::BitmapImage();
    image.SetSource(stream);
    return image;
}

mux::Controls::Button subtleButton(const wchar_t* glyph, const std::string& name) {
    auto icon = mux::Controls::FontIcon();
    icon.Glyph(glyph);
    icon.FontSize(12);
    auto button = mux::Controls::Button();
    button.Content(icon);
    button.Width(28);
    button.Height(28);
    button.Padding(mux::ThicknessHelper::FromUniformLength(0));
    button.Background(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
    button.BorderBrush(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
    mux::Controls::ToolTipService::SetToolTip(button, winrt::box_value(toH(name)));
    mux::Automation::AutomationProperties::SetName(button, toH(name));
    return button;
}

}  // namespace

// --- Info ---------------------------------------------------------------------------

InfoPane::InfoPane(const Library* library, std::function<mux::XamlRoot()> xamlRoot)
    : library_(library), xamlRoot_(std::move(xamlRoot)) {
    root_ = mux::Controls::ScrollViewer();
    root_.HorizontalScrollBarVisibility(mux::Controls::ScrollBarVisibility::Disabled);

    auto column = mux::Controls::StackPanel();
    column.Spacing(12);
    column.Padding(mux::ThicknessHelper::FromUniformLength(16));

    // The cover is a button that opens it larger, as in the GTK pane.
    cover_ = mux::Controls::Image();
    cover_.Stretch(mux::Media::Stretch::Uniform);
    cover_.MaxHeight(260);
    coverButton_ = mux::Controls::Button();
    coverButton_.Content(cover_);
    coverButton_.Padding(mux::ThicknessHelper::FromUniformLength(0));
    coverButton_.HorizontalAlignment(mux::HorizontalAlignment::Center);
    coverButton_.Background(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
    coverButton_.BorderThickness(mux::ThicknessHelper::FromUniformLength(0));
    coverButton_.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(4));
    mux::Controls::ToolTipService::SetToolTip(coverButton_, winrt::box_value(toH(tr("View Artwork"))));
    mux::Automation::AutomationProperties::SetName(coverButton_, toH(tr("View Artwork")));
    coverButton_.Visibility(mux::Visibility::Collapsed);
    coverButton_.Click([this](auto&&, auto&&) { showArtwork(); });
    column.Children().Append(coverButton_);

    grid_ = mux::Controls::Grid();
    grid_.ColumnSpacing(12);
    grid_.RowSpacing(4);
    {
        auto labels = mux::Controls::ColumnDefinition();
        labels.Width(mux::GridLengthHelper::Auto());
        auto values = mux::Controls::ColumnDefinition();
        values.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        grid_.ColumnDefinitions().Append(labels);
        grid_.ColumnDefinitions().Append(values);
    }
    const auto& fields = app::info::fieldLabels();
    for (std::size_t field = 0; field < fields.size(); ++field) {
        auto row = mux::Controls::RowDefinition();
        row.Height(mux::GridLengthHelper::Auto());
        grid_.RowDefinitions().Append(row);

        auto label = secondaryText(L"TextAlignment='Right'");
        label.Text(toH(tr(fields[field])));
        mux::Controls::Grid::SetRow(label, static_cast<int32_t>(field));

        auto value = mux::Controls::TextBlock();
        value.TextWrapping(mux::TextWrapping::Wrap);
        value.IsTextSelectionEnabled(true);
        mux::Controls::Grid::SetRow(value, static_cast<int32_t>(field));
        mux::Controls::Grid::SetColumn(value, 1);

        grid_.Children().Append(label);
        grid_.Children().Append(value);
        labels_.push_back(label);
        values_.push_back(value);
    }
    column.Children().Append(grid_);

    empty_ = secondaryText(L"TextWrapping='Wrap'");
    empty_.Text(toH(tr("Nothing selected or playing.")));
    column.Children().Append(empty_);

    root_.Content(column);
    showEntry(nullptr);
}

void InfoPane::showEntry(const PlaylistEntry* entry) {
    if (entry == nullptr) {
        grid_.Visibility(mux::Visibility::Collapsed);
        coverButton_.Visibility(mux::Visibility::Collapsed);
        coverImage_ = nullptr;
        empty_.Visibility(mux::Visibility::Visible);
        return;
    }

    const std::array<std::string, app::info::FieldCount> values = app::info::describe(*entry, library_);
    for (std::size_t field = 0; field < values.size(); ++field) {
        // An empty field takes no row, as in the other two players.
        const auto shown = values[field].empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible;
        labels_[field].Visibility(shown);
        values_[field].Visibility(shown);
        values_[field].Text(toH(values[field]));
    }
    grid_.Visibility(mux::Visibility::Visible);
    empty_.Visibility(mux::Visibility::Collapsed);

    const SharedString& by   = entry->albumArtist.empty() ? entry->artist : entry->albumArtist;
    const std::string   what = entry->album.empty() ? entry->title() : entry->album.str();
    coverTitle_              = by.empty() ? what : by.str() + " \xE2\x80\x94 " + what;

    coverImage_ = nullptr;
    if (library_ != nullptr && !entry->artHash.empty()) {
        if (const auto bytes = library_->sharedArtwork(entry->artHash); bytes && !bytes->empty()) {
            try {
                coverImage_ = imageFrom(*bytes);
            } catch (const winrt::hresult_error&) {
                // Bytes the decoder will not take: no cover, rather than a pane
                // that failed to show the rest of what it knows.
                coverImage_ = nullptr;
            }
        }
    }
    cover_.Source(coverImage_);
    coverButton_.Visibility(coverImage_ ? mux::Visibility::Visible : mux::Visibility::Collapsed);
}

winrt::fire_and_forget InfoPane::showArtwork() {
    if (!coverImage_ || dialogOpen_ || !xamlRoot_) {
        co_return;
    }
    dialogOpen_ = true;
    auto box = mux::Controls::ContentDialog();
    box.XamlRoot(xamlRoot_());
    box.Style(mux::Application::Current()
                  .Resources()
                  .Lookup(winrt::box_value(L"DefaultContentDialogStyle"))
                  .as<mux::Style>());
    // Wider than a ContentDialog allows by default, which is sized for a
    // question rather than a picture.
    box.Resources().Insert(winrt::box_value(L"ContentDialogMaxWidth"), winrt::box_value(960.0));
    box.Resources().Insert(winrt::box_value(L"ContentDialogMaxHeight"), winrt::box_value(960.0));
    box.Title(winrt::box_value(toH(coverTitle_)));
    auto image = mux::Controls::Image();
    image.Source(coverImage_);
    image.Stretch(mux::Media::Stretch::Uniform);
    image.MaxHeight(760);
    mux::Automation::AutomationProperties::SetName(image, toH(tr("Artwork")));
    box.Content(image);
    box.CloseButtonText(L"OK");
    box.DefaultButton(mux::Controls::ContentDialogButton::Close);
    co_await box.ShowAsync();
    dialogOpen_ = false;
}

// --- Lyrics -------------------------------------------------------------------------

LyricsPane::LyricsPane(std::function<double()> position) : position_(std::move(position)) {
    root_ = mux::Controls::Grid();
    root_.Padding(mux::ThicknessHelper::FromLengths(16, 12, 16, 12));
    root_.RowSpacing(8);
    for (const auto& height : {mux::GridLengthHelper::Auto(),
                               mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star),
                               mux::GridLengthHelper::Auto()}) {
        auto row = mux::Controls::RowDefinition();
        row.Height(height);
        root_.RowDefinitions().Append(row);
    }

    auto top = mux::Controls::Grid();
    {
        auto grow = mux::Controls::ColumnDefinition();
        grow.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        auto fit = mux::Controls::ColumnDefinition();
        fit.Width(mux::GridLengthHelper::Auto());
        top.ColumnDefinitions().Append(grow);
        top.ColumnDefinitions().Append(fit);
    }
    heading_ = secondaryText(L"TextTrimming='CharacterEllipsis' VerticalAlignment='Center'");
    top.Children().Append(heading_);
    timed_ = mux::Controls::Primitives::ToggleButton();
    timed_.Content(winrt::box_value(toH(tr("Timed"))));
    mux::Controls::ToolTipService::SetToolTip(timed_, winrt::box_value(toH(tr("Follow timed lyrics line by line"))));
    timed_.Visibility(mux::Visibility::Collapsed);
    timed_.Click([this](auto&&, auto&&) {
        if (!settingTimed_ && timedToggled) {
            timedToggled();
        }
    });
    mux::Controls::Grid::SetColumn(timed_, 1);
    top.Children().Append(timed_);
    root_.Children().Append(top);

    lines_ = mux::Controls::StackPanel();
    lines_.Spacing(2);
    scroller_ = mux::Controls::ScrollViewer();
    scroller_.Content(lines_);
    mux::Controls::Grid::SetRow(scroller_, 1);
    root_.Children().Append(scroller_);

    source_ = secondaryText(L"Style='{StaticResource CaptionTextBlockStyle}'");
    source_.Visibility(mux::Visibility::Collapsed);
    mux::Controls::Grid::SetRow(source_, 2);
    root_.Children().Append(source_);

    timer_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
    timer_.Interval(std::chrono::milliseconds(100));
    timer_.Tick([this](auto&&, auto&&) { tick(); });

    showEntry(nullptr, false);
}

void LyricsPane::setTimed(bool timed) {
    presenter_.setTimed(timed);
    settingTimed_ = true;
    timed_.IsChecked(timed);
    settingTimed_ = false;
}

void LyricsPane::showEntry(const PlaylistEntry* entry, bool playing) {
    playing_ = playing && entry != nullptr;
    const std::optional<app::LyricsText> text =
        presenter_.show(entry, [this](const app::LyricsText& answered) { present(answered); });
    if (text) {
        present(*text);
    } else {
        updateFollowing();
    }
}

void LyricsPane::present(const app::LyricsText& text) {
    heading_.Text(toH(text.heading));
    source_.Text(toH(text.source));
    source_.Visibility(text.source.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
    timed_.Visibility(text.timeable ? mux::Visibility::Visible : mux::Visibility::Collapsed);

    // One TextBlock a line, so a sung line can be styled and scrolled to on
    // its own. Untimed text stays one block, so it can be selected whole.
    lines_.Children().Clear();
    synced_ = text.synced;
    sung_   = SyncedLyrics::npos;
    const auto addLine = [this](std::string_view line) {
        auto block = mux::Controls::TextBlock();
        block.Text(toH(line));
        block.TextWrapping(mux::TextWrapping::Wrap);
        block.IsTextSelectionEnabled(true);
        lines_.Children().Append(block);
    };
    if (synced_) {
        std::string_view rest = text.body;
        while (true) {
            const std::size_t newline = rest.find('\n');
            addLine(rest.substr(0, newline));
            if (newline == std::string_view::npos) {
                break;
            }
            rest = rest.substr(newline + 1);
        }
    } else {
        addLine(text.body);
    }
    scroller_.ChangeView(nullptr, 0.0, nullptr, true);
    updateFollowing();
}

void LyricsPane::updateFollowing() {
    if (!playing_ || !synced_ || !position_) {
        timer_.Stop();
        if (sung_ != SyncedLyrics::npos) {
            styleLine(sung_, false);
            sung_ = SyncedLyrics::npos;
        }
        return;
    }
    if (!timer_.IsRunning()) {
        timer_.Start();
    }
    tick();
}

void LyricsPane::tick() {
    if (!synced_ || !position_) {
        return;
    }
    const std::size_t line = synced_->lineAt(position_());
    if (line == sung_) {
        return;
    }
    if (sung_ != SyncedLyrics::npos) {
        styleLine(sung_, false);
    }
    sung_ = line;
    if (line == SyncedLyrics::npos || line >= lines_.Children().Size()) {
        return;
    }
    styleLine(line, true);

    // A third of the way down rather than at the top, so the lines coming are
    // in view as well as the one being sung.
    auto options = mux::BringIntoViewOptions();
    options.VerticalAlignmentRatio(0.33);
    options.AnimationDesired(true);
    lines_.Children().GetAt(static_cast<uint32_t>(line)).StartBringIntoView(options);
}

void LyricsPane::styleLine(std::size_t index, bool sung) {
    if (index >= lines_.Children().Size()) {
        return;
    }
    auto block = lines_.Children().GetAt(static_cast<uint32_t>(index)).as<mux::Controls::TextBlock>();
    block.FontWeight(sung ? winrt::Microsoft::UI::Text::FontWeights::SemiBold()
                          : winrt::Microsoft::UI::Text::FontWeights::Normal());
    if (sung) {
        // The accent, as the GTK pane uses libadwaita's: the system's accent
        // colour in the variant made for text, so it reads on either theme.
        block.Foreground(themeBrush(L"AccentTextFillColorPrimaryBrush"));
    } else {
        block.ClearValue(mux::Controls::TextBlock::ForegroundProperty());
    }
}

// --- the equaliser --------------------------------------------------------------------

EqualizerPane::EqualizerPane(Settings& settings)
    : settings_(settings), presets_(shippedEqualizerPresets()) {
    root_ = mux::Controls::StackPanel();
    root_.Spacing(8);
    root_.Padding(mux::ThicknessHelper::FromLengths(12, 4, 12, 12));

    if (presets_.size() > 0) {
        auto row = mux::Controls::StackPanel();
        row.Orientation(mux::Controls::Orientation::Horizontal);
        row.Spacing(16);

        enabled_ = mux::Controls::CheckBox();
        enabled_.Content(winrt::box_value(toH(tr("Enable"))));
        mux::Controls::ToolTipService::SetToolTip(
            enabled_, winrt::box_value(toH(tr(
                          "Bypasses the equaliser without disturbing the curve, which is what "
                          "comparing one against the original needs. A flat equaliser is skipped "
                          "either way, so this costs nothing until a band is moved."))));
        enabled_.Click([this](auto&&, auto&&) {
            if (syncing_) {
                return;
            }
            settings_.setGraphicEqEnable(enabled_.IsChecked().Value());
            settingChanged.publish("GraphicEQenable");
        });
        row.Children().Append(enabled_);

        preset_ = mux::Controls::ComboBox();
        preset_.Header(winrt::box_value(toH(tr("Preset"))));
        preset_.MinWidth(180);
        for (const EqualizerPreset& preset : presets_.presets()) {
            preset_.Items().Append(winrt::box_value(toH(preset.name)));
        }
        preset_.Items().Append(winrt::box_value(toH(tr("Custom"))));
        mux::Controls::ToolTipService::SetToolTip(
            preset_, winrt::box_value(toH(tr(
                         "Presets store ten points; the 31 bands are interpolated from them. "
                         "Moving any slider afterwards leaves the curve alone and changes this "
                         "to Custom."))));
        preset_.SelectionChanged([this](auto&&, auto&&) {
            if (!syncing_ && preset_.SelectedIndex() >= 0) {
                selectPreset(preset_.SelectedIndex());
            }
        });
        row.Children().Append(preset_);

        trackGenre_ = mux::Controls::CheckBox();
        trackGenre_.Content(winrt::box_value(toH(tr("Follow the track's genre"))));
        mux::Controls::ToolTipService::SetToolTip(
            trackGenre_,
            winrt::box_value(toH(tr("Chooses the preset whose name matches each track's genre tag as it "
                                    "starts. A track with no genre, or one nothing matches, gets Flat -- so "
                                    "this rewrites the equaliser at every track boundary rather than only "
                                    "when it has something to say."))));
        trackGenre_.Click([this](auto&&, auto&&) {
            if (syncing_) {
                return;
            }
            settings_.setGraphicEqTrackGenre(trackGenre_.IsChecked().Value());
            settingChanged.publish("GraphicEQtrackgenre");
        });
        row.Children().Append(trackGenre_);
        // The checkboxes sit level with the combo box, not with its header.
        enabled_.VerticalAlignment(mux::VerticalAlignment::Bottom);
        trackGenre_.VerticalAlignment(mux::VerticalAlignment::Bottom);

        root_.Children().Append(row);
    }

    auto columns = mux::Controls::StackPanel();
    columns.Orientation(mux::Controls::Orientation::Horizontal);
    columns.Spacing(2);
    addBand(columns, tr("Pre"), "eqPreamp");
    {
        auto rule = loadXaml<mux::Controls::Border>(
            L"<Border xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'"
            L" Width='1' Margin='4,8,4,8' Background='{ThemeResource DividerStrokeColorDefaultBrush}'/>");
        columns.Children().Append(rule);
    }
    const auto frequencies = Equalizer::bandFrequencies();
    const auto keys        = Equalizer::bandSettingsKeys();
    for (std::size_t band = 0; band < keys.size(); ++band) {
        addBand(columns, frequencyLabel(frequencies[band]), keys[band]);
    }
    // No scroller of its own: the strip's section scrolls the whole pane both
    // ways (ToolsStrip::Scroll::Both), header row and footer included, which a
    // scroller around the bands alone did not -- and the one that was here
    // scrolled only sideways, which a mouse wheel does not reach.
    root_.Children().Append(columns);

    auto footer = mux::Controls::Grid();
    footer.ColumnSpacing(12);
    {
        auto fit = mux::Controls::ColumnDefinition();
        fit.Width(mux::GridLengthHelper::Auto());
        auto grow = mux::Controls::ColumnDefinition();
        grow.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        footer.ColumnDefinitions().Append(fit);
        footer.ColumnDefinitions().Append(grow);
    }
    auto flat = mux::Controls::Button();
    flat.Content(winrt::box_value(toH(tr("Flat"))));
    mux::Controls::ToolTipService::SetToolTip(
        flat, winrt::box_value(toH(tr("Selects the Flat preset: every band and the preamp back to 0 dB, which "
                                      "makes the equaliser bit-transparent again."))));
    flat.Click([this](auto&&, auto&&) { flatten(); });
    footer.Children().Append(flat);
    // A width to wrap at: inside a pane that scrolls sideways there is no
    // width otherwise, and the note would run out in one line.
    auto note = secondaryText(L"TextWrapping='Wrap' VerticalAlignment='Center' MaxWidth='560' Style='{StaticResource CaptionTextBlockStyle}'");
    note.Text(toH(tr("31 bands, \xC2\xB1""20 dB. Changes apply to the track already playing. A boost can "
                     "clip; the preamp is the headroom for it.")));
    mux::Controls::Grid::SetColumn(note, 1);
    footer.Children().Append(note);
    root_.Children().Append(footer);

    refresh();
}

void EqualizerPane::addBand(const mux::Controls::StackPanel& row, const std::string& caption,
                            const std::string& key) {
    auto column = mux::Controls::StackPanel();
    column.Spacing(2);
    column.Width(36);

    auto readout = secondaryText(L"HorizontalAlignment='Center' Style='{StaticResource CaptionTextBlockStyle}'");
    column.Children().Append(readout);

    auto scale = mux::Controls::Slider();
    scale.Orientation(mux::Controls::Orientation::Vertical);
    scale.Minimum(-kEqRangeDb);
    scale.Maximum(kEqRangeDb);
    scale.StepFrequency(kEqStepDb);
    scale.Height(140);
    scale.HorizontalAlignment(mux::HorizontalAlignment::Center);
    // A tick at 0 dB, the one place on the scale worth finding by eye.
    scale.TickFrequency(kEqRangeDb);
    scale.TickPlacement(mux::Controls::Primitives::TickPlacement::Inline);
    mux::Controls::ToolTipService::SetToolTip(scale, winrt::box_value(toH(caption)));
    mux::Automation::AutomationProperties::SetName(scale, toH(caption));
    scale.ValueChanged([this, key, readout](auto&&, mux::Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
        if (syncing_) {
            return;
        }
        const double db = args.NewValue();
        readout.Text(toH(decibelLabel(db)));
        settings_.setRawValue(key, std::to_string(db));
        enableIfSilent();
        markCustom();
        settingChanged.publish(key);
    });
    column.Children().Append(scale);

    auto label = secondaryText(L"HorizontalAlignment='Center' Style='{StaticResource CaptionTextBlockStyle}'");
    label.Text(toH(caption));
    column.Children().Append(label);

    row.Children().Append(column);
    scales_.push_back(scale);
    readouts_.push_back(readout);
    keys_.push_back(key);
}

int EqualizerPane::customIndex() const {
    return static_cast<int>(presets_.size());
}

void EqualizerPane::refresh() {
    syncing_ = true;
    for (std::size_t i = 0; i < scales_.size(); ++i) {
        const double db = toDouble(settings_.rawValue(keys_[i]));
        scales_[i].Value(db);
        readouts_[i].Text(toH(decibelLabel(db)));
    }
    if (preset_) {
        const int stored = settings_.GraphicEqPreset();
        preset_.SelectedIndex(presets_.at(stored) != nullptr ? stored : customIndex());
    }
    if (trackGenre_) {
        trackGenre_.IsChecked(settings_.GraphicEqTrackGenre());
    }
    if (enabled_) {
        enabled_.IsChecked(settings_.GraphicEqEnable());
    }
    syncing_ = false;
}

void EqualizerPane::publishCurve() {
    for (const std::string& key : keys_) {
        settingChanged.publish(key);
    }
}

void EqualizerPane::selectPreset(int index) {
    settings_.setGraphicEqPreset(index);
    const EqualizerPreset* preset = presets_.at(index);
    if (preset == nullptr) {
        return;  // Custom: the curve stays as it is
    }
    applyEqualizerPreset(settings_, *preset);
    if (preset->name != "Flat") {
        enableIfSilent();
    }
    refresh();
    publishCurve();
}

void EqualizerPane::markCustom() {
    if (!preset_) {
        return;
    }
    const int custom = customIndex();
    if (settings_.GraphicEqPreset() == custom) {
        return;
    }
    settings_.setGraphicEqPreset(custom);
    syncing_ = true;
    preset_.SelectedIndex(custom);
    syncing_ = false;
}

void EqualizerPane::enableIfSilent() {
    // Moving a band of a bypassed equaliser and hearing nothing reads as the
    // equaliser being broken; the GTK pane switches it on, and so does this.
    if (settings_.GraphicEqEnable()) {
        return;
    }
    settings_.setGraphicEqEnable(true);
    settingChanged.publish("GraphicEQenable");
    if (enabled_) {
        syncing_ = true;
        enabled_.IsChecked(true);
        syncing_ = false;
    }
}

void EqualizerPane::flatten() {
    if (const int flat = presets_.indexOf("Flat"); flat >= 0) {
        selectPreset(flat);
        return;
    }
    for (const std::string& key : keys_) {
        settings_.setRawValue(key, "0");
    }
    refresh();
    publishCurve();
}

// --- speed ------------------------------------------------------------------------------

SpeedPane::SpeedPane(Settings& settings) : settings_(settings) {
    root_ = mux::Controls::StackPanel();
    root_.Spacing(8);
    root_.Padding(mux::ThicknessHelper::FromLengths(12, 4, 12, 12));
    root_.MinWidth(360);

    const auto makeRow = [this](const char* caption, mux::Controls::Slider& scale,
                                mux::Controls::TextBlock& value, const char* key) {
        auto row = mux::Controls::Grid();
        row.ColumnSpacing(12);
        for (const auto& width : {mux::GridLengthHelper::FromPixels(56),
                                  mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star),
                                  mux::GridLengthHelper::FromPixels(56)}) {
            auto column = mux::Controls::ColumnDefinition();
            column.Width(width);
            row.ColumnDefinitions().Append(column);
        }
        auto label = mux::Controls::TextBlock();
        label.Text(toH(tr(caption)));
        label.VerticalAlignment(mux::VerticalAlignment::Center);
        row.Children().Append(label);

        scale = mux::Controls::Slider();
        scale.Minimum(0);
        scale.Maximum(app::kSpeedSliderMax);
        scale.StepFrequency(1);
        // The ratio as the thumb's tooltip, not the slider's raw position.
        scale.IsThumbToolTipEnabled(false);
        scale.VerticalAlignment(mux::VerticalAlignment::Center);
        mux::Automation::AutomationProperties::SetName(scale, toH(tr(caption)));
        scale.ValueChanged([this, key](auto&&, mux::Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
            if (syncing_) {
                return;
            }
            write(key, app::snapSpeed(app::speedFromSlider(static_cast<int>(std::lround(args.NewValue())))));
        });
        mux::Controls::Grid::SetColumn(scale, 1);
        row.Children().Append(scale);

        value = secondaryText(L"TextAlignment='Right' VerticalAlignment='Center'");
        mux::Controls::Grid::SetColumn(value, 2);
        row.Children().Append(value);
        return row;
    };

    pitchRow_ = makeRow(XPCOG_TRANSLATE("Pitch"), pitch_, pitchValue_, "pitch");
    root_.Children().Append(pitchRow_);
    root_.Children().Append(makeRow(XPCOG_TRANSLATE("Tempo"), tempo_, tempoValue_, "tempo"));

    lock_ = mux::Controls::CheckBox();
    lock_.Content(winrt::box_value(toH(tr("Lock pitch and tempo together"))));
    lock_.Click([this](auto&&, auto&&) {
        if (syncing_) {
            return;
        }
        settings_.setRawValue("speedLock", lock_.IsChecked().Value() ? "true" : "false");
        settingChanged.publish("speedLock");
    });
    root_.Children().Append(lock_);

    auto footer = mux::Controls::StackPanel();
    footer.Orientation(mux::Controls::Orientation::Horizontal);
    footer.Spacing(12);
    auto reset = mux::Controls::Button();
    reset.Content(winrt::box_value(toH(tr("Reset to 1.00\xC3\x97"))));
    reset.Click([this](auto&&, auto&&) {
        write("pitch", 1.0);
        write("tempo", 1.0);
    });
    footer.Children().Append(reset);
    // The engine and its options are on the Pitch & Tempo page, as GTK's
    // pane points there too.
    auto preferences = mux::Controls::Button();
    preferences.Content(winrt::box_value(toH(tr("Preferences\xE2\x80\xA6"))));
    preferences.Click([this](auto&&, auto&&) { settingsRequested.publish(); });
    footer.Children().Append(preferences);
    note_ = secondaryText(L"TextWrapping='Wrap' VerticalAlignment='Center' MaxWidth='320'");
    note_.Text(toH(tr("No engine is chosen, so these do nothing yet.")));
    footer.Children().Append(note_);
    root_.Children().Append(footer);

    refresh();
}

void SpeedPane::write(const char* key, double ratio) {
    settings_.setRawValue(key, std::to_string(ratio));
    settingChanged.publish(key);

    // Locked, the other follows -- except under varispeed, where pitch and
    // tempo are one control and there is no other to move.
    const bool varispeed = settings_.RubberbandEngine() == "varispeed";
    if (settings_.SpeedLock() && !varispeed) {
        const char* other = (std::string_view{key} == "pitch") ? "tempo" : "pitch";
        settings_.setRawValue(other, std::to_string(ratio));
        settingChanged.publish(other);
    }
    refresh();
}

void SpeedPane::refresh() {
    const std::string engine    = settings_.RubberbandEngine();
    const bool        disabled  = engine == "disabled";
    const bool        varispeed = engine == "varispeed";

    syncing_ = true;
    const double pitch = settings_.Pitch();
    const double tempo = settings_.Tempo();
    pitch_.Value(app::sliderFromSpeed(pitch));
    tempo_.Value(app::sliderFromSpeed(tempo));
    pitchValue_.Text(toH(ratioLabel(pitch)));
    tempoValue_.Text(toH(ratioLabel(tempo)));
    lock_.IsChecked(settings_.SpeedLock());
    syncing_ = false;

    pitchRow_.Visibility(varispeed ? mux::Visibility::Collapsed : mux::Visibility::Visible);
    lock_.Visibility(varispeed ? mux::Visibility::Collapsed : mux::Visibility::Visible);
    note_.Visibility(disabled ? mux::Visibility::Visible : mux::Visibility::Collapsed);
}

// --- the tools strip ------------------------------------------------------------------

ToolsStrip::ToolsStrip() {
    root_ = mux::Controls::Grid();
    root_.ColumnSpacing(8);
}

void ToolsStrip::addSection(const std::string& name, const std::string& title,
                            const mux::UIElement& content, Scroll scroll) {
    auto section = card();
    auto body    = mux::Controls::Grid();
    {
        auto top = mux::Controls::RowDefinition();
        top.Height(mux::GridLengthHelper::Auto());
        auto rest = mux::Controls::RowDefinition();
        rest.Height(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        body.RowDefinitions().Append(top);
        body.RowDefinitions().Append(rest);
    }

    auto header = mux::Controls::Grid();
    header.Padding(mux::ThicknessHelper::FromLengths(12, 6, 6, 0));
    {
        auto grow = mux::Controls::ColumnDefinition();
        grow.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        auto fit = mux::Controls::ColumnDefinition();
        fit.Width(mux::GridLengthHelper::Auto());
        header.ColumnDefinitions().Append(grow);
        header.ColumnDefinitions().Append(fit);
    }
    auto label = loadXaml<mux::Controls::TextBlock>(
        L"<TextBlock xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'"
        L" Style='{StaticResource BodyStrongTextBlockStyle}' VerticalAlignment='Center'/>");
    label.Text(toH(title));
    header.Children().Append(label);
    auto close = subtleButton(L"\xE711", tr("Close"));  // Segoe Fluent "Cancel"
    close.Click([this, name](auto&&, auto&&) {
        if (closeRequested) {
            closeRequested(name);
        }
    });
    mux::Controls::Grid::SetColumn(close, 1);
    header.Children().Append(close);
    body.Children().Append(header);

    if (scroll == Scroll::None) {
        mux::Controls::Grid::SetRow(content.as<mux::FrameworkElement>(), 1);
        body.Children().Append(content);
    } else {
        // Scroll bars that show when the content does not fit and the pointer
        // is over it, as WinUI's do; the wheel scrolls down, and Shift with it
        // across.
        auto scroller = mux::Controls::ScrollViewer();
        scroller.VerticalScrollMode(mux::Controls::ScrollMode::Auto);
        scroller.VerticalScrollBarVisibility(mux::Controls::ScrollBarVisibility::Auto);
        const bool across = scroll == Scroll::Both;
        scroller.HorizontalScrollMode(across ? mux::Controls::ScrollMode::Auto
                                             : mux::Controls::ScrollMode::Disabled);
        scroller.HorizontalScrollBarVisibility(across ? mux::Controls::ScrollBarVisibility::Auto
                                                      : mux::Controls::ScrollBarVisibility::Disabled);
        scroller.Content(content);
        mux::Controls::Grid::SetRow(scroller, 1);
        body.Children().Append(scroller);
    }
    section.Child(body);
    section.Visibility(mux::Visibility::Collapsed);

    sections_.insert_or_assign(name, section);
    order_.push_back(name);
    root_.Children().Append(section);
    relayout();
}

void ToolsStrip::relayout() {
    // Shown sections share the width equally, in the order they were added;
    // hidden ones take no column at all, so nothing leaves a gap.
    root_.ColumnDefinitions().Clear();
    int column = 0;
    for (const std::string& name : order_) {
        const auto& section = sections_.at(name);
        if (section.Visibility() != mux::Visibility::Visible) {
            continue;
        }
        auto definition = mux::Controls::ColumnDefinition();
        definition.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        root_.ColumnDefinitions().Append(definition);
        mux::Controls::Grid::SetColumn(section, column++);
    }
}

void ToolsStrip::setShown(const std::string& name, bool shown) {
    const auto found = sections_.find(name);
    if (found == sections_.end()) {
        return;
    }
    found->second.Visibility(shown ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    relayout();
}

bool ToolsStrip::shown(const std::string& name) const {
    const auto found = sections_.find(name);
    return found != sections_.end() && found->second.Visibility() == mux::Visibility::Visible;
}

bool ToolsStrip::anyShown() const {
    for (const auto& [name, section] : sections_) {
        if (section.Visibility() == mux::Visibility::Visible) {
            return true;
        }
    }
    return false;
}

}  // namespace xpcog::winui
