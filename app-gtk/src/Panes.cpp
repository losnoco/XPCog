#include "Panes.hpp"

#include "SpeedCurve.hpp"
#include "TrackText.hpp"
#include "Translations.hpp"

#include "xpcog/core/audio/Equalizer.hpp"
#include "xpcog/core/audio/EqualizerPresets.hpp"
#include "xpcog/core/library/Library.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string_view>
#include <utility>

namespace xpcog::gtk {

using app::tr;

namespace {

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

/// The same scale wx's sliders use: tenths of a decibel, over +/- 20 dB.
constexpr double kEqRangeDb = 20.0;
constexpr double kEqStepDb  = 0.1;

}  // namespace

// --- Info -------------------------------------------------------------------------

InfoPane::InfoPane(const Library* library) : library_(library) {
    root_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(root_), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);

    GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(column, 12);
    gtk_widget_set_margin_bottom(column, 12);
    gtk_widget_set_margin_start(column, 12);
    gtk_widget_set_margin_end(column, 12);

    // The cover, scaled into whatever width the sidebar has with its aspect
    // kept -- which is the whole of what wx's ArtworkView had to be written
    // for, and one property here. Never shorter than kCoverMinHeight: a
    // picture that can shrink is otherwise squeezed to a sliver when the
    // rows under it want the height.
    constexpr int kCoverMinHeight = 180;
    picture_ = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(picture_), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture_), TRUE);
    gtk_widget_set_size_request(picture_, -1, kCoverMinHeight);

    // A flat button around it, so a click, Enter or Space, and a screen
    // reader all find the larger view -- a picture with a click gesture
    // would give the pointer that and nobody else.
    coverButton_ = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(coverButton_), picture_);
    gtk_widget_add_css_class(coverButton_, "flat");
    gtk_widget_set_tooltip_text(coverButton_, tr("View Artwork").c_str());
    gtk_widget_set_cursor_from_name(coverButton_, "zoom-in");
    gtk_widget_set_visible(coverButton_, FALSE);
    connections_.push_back(Connection::to<void(GtkButton*)>(
        coverButton_, "clicked", [this](GtkButton*) { showArtwork(); }));
    gtk_box_append(GTK_BOX(column), coverButton_);

    grid_ = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid_), 12);
    gtk_box_append(GTK_BOX(column), grid_);

    empty_ = gtk_label_new(tr("Nothing selected or playing.").c_str());
    gtk_widget_add_css_class(empty_, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(empty_), TRUE);
    gtk_box_append(GTK_BOX(column), empty_);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(root_), column);

    // One row per field, built once; showEntry fills and hides them.
    const auto& labels = app::info::fieldLabels();
    for (std::size_t field = 0; field < labels.size(); ++field) {
        GtkWidget* label = gtk_label_new(tr(labels[field]).c_str());
        gtk_widget_add_css_class(label, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(label), 1.0F);
        gtk_widget_set_valign(label, GTK_ALIGN_START);

        // Selectable, so a path or a comment can be copied -- a large part of
        // why an info pane gets opened.
        GtkWidget* value = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(value), 0.0F);
        gtk_label_set_wrap(GTK_LABEL(value), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(value), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_selectable(GTK_LABEL(value), TRUE);
        gtk_widget_set_hexpand(value, TRUE);

        gtk_grid_attach(GTK_GRID(grid_), label, 0, static_cast<int>(field), 1, 1);
        gtk_grid_attach(GTK_GRID(grid_), value, 1, static_cast<int>(field), 1, 1);
        rows_.push_back(label);
        rows_.push_back(value);
    }
    showEntry(nullptr);
}

void InfoPane::showEntry(const PlaylistEntry* entry) {
    if (entry == nullptr) {
        gtk_widget_set_visible(grid_, FALSE);
        gtk_widget_set_visible(coverButton_, FALSE);
        cover_.reset();
        gtk_widget_set_visible(empty_, TRUE);
        return;
    }

    const std::array<std::string, app::info::FieldCount> values =
        app::info::describe(*entry, library_);
    for (std::size_t field = 0; field < values.size(); ++field) {
        GtkWidget* label = rows_[field * 2];
        GtkWidget* value = rows_[field * 2 + 1];
        const bool shown = !values[field].empty();
        gtk_widget_set_visible(label, shown);
        gtk_widget_set_visible(value, shown);
        if (shown) {
            gtk_label_set_text(GTK_LABEL(value), values[field].c_str());
        }
    }
    gtk_widget_set_visible(grid_, TRUE);
    gtk_widget_set_visible(empty_, FALSE);

    // The cover, as the bytes the file carried, decoded by GdkTexture -- which
    // is usually JPEG and sometimes PNG, and a decoder GTK already links.
    bool cover = false;
    cover_.reset();
    // Named for the album, because a cover is the album's: its artist -- the
    // track's where the file has no album artist, since on a compilation the
    // track's artist names one song on it -- and its title, or the track's
    // for a single file with no album.
    const SharedString& by   = entry->albumArtist.empty() ? entry->artist : entry->albumArtist;
    const std::string   what = entry->album.empty() ? entry->title() : entry->album.str();
    coverTitle_              = by.empty() ? what : by.str() + " \xE2\x80\x94 " + what;
    if (library_ != nullptr && !entry->artHash.empty()) {
        if (const auto bytes = library_->sharedArtwork(entry->artHash); bytes && !bytes->empty()) {
            const GBytesPtr data(g_bytes_new(bytes->data(), bytes->size()));
            GErrorPtr error;
            if (GdkTexture* texture = gdk_texture_new_from_bytes(data.get(), &error.value)) {
                cover_ = GObjectPtr<GdkTexture>::adopt(texture);
                gtk_picture_set_paintable(GTK_PICTURE(picture_), GDK_PAINTABLE(texture));
                cover = true;
            }
        }
    }
    gtk_widget_set_visible(coverButton_, cover);
}

void InfoPane::showArtwork() {
    if (!cover_) {
        return;
    }
    // As large as the window allows: its own size where that fits, scaled
    // down to most of the window where it does not, and up to twice its size
    // for a small cover, which is what someone who clicked to see it larger
    // came for. The header's height is allowed for.
    constexpr double kWindowShare = 0.85;
    constexpr double kMaxUpscale  = 2.0;
    constexpr int    kHeader      = 48;
    const double imageWidth  = gdk_texture_get_width(cover_.get());
    const double imageHeight = gdk_texture_get_height(cover_.get());
    GtkRoot*     root        = gtk_widget_get_root(root_);
    if (root == nullptr) {
        return;  // not in a window, so nowhere to show it over
    }
    const double roomWidth  = std::max(360, gtk_widget_get_width(GTK_WIDGET(root))) * kWindowShare;
    const double roomHeight =
        std::max(360, gtk_widget_get_height(GTK_WIDGET(root))) * kWindowShare - kHeader;
    const double scale =
        std::min({kMaxUpscale, roomWidth / imageWidth, std::max(1.0, roomHeight) / imageHeight});

    AdwDialog* dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, coverTitle_.c_str());
    // Never narrower than the header needs for its title and close button;
    // a small cover sits centred in the extra width.
    constexpr int kMinWidth = 360;
    adw_dialog_set_content_width(
        dialog, std::max(kMinWidth, static_cast<int>(std::lround(imageWidth * scale))));
    adw_dialog_set_content_height(dialog,
                                  static_cast<int>(std::lround(imageHeight * scale)) + kHeader);

    GtkWidget* picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(cover_.get()));
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
    gtk_widget_set_vexpand(picture, TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(picture), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   tr("Artwork").c_str(), -1);
    // A click on the picture puts it away again, as a lightbox does; Escape
    // and the close button are the dialog's own.
    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect_swapped(click, "released", G_CALLBACK(+[](AdwDialog* self) {
                                 adw_dialog_close(self);
                             }),
                             dialog);
    gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(click));
    gtk_widget_set_cursor_from_name(picture, "zoom-out");

    GtkWidget* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), picture);
    adw_dialog_set_child(dialog, view);
    adw_dialog_present(dialog, root_);
}

// --- Lyrics -------------------------------------------------------------------------

LyricsPane::LyricsPane(std::function<double()> position) : position_(std::move(position)) {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(root_, 8);
    gtk_widget_set_margin_bottom(root_, 8);
    gtk_widget_set_margin_start(root_, 8);
    gtk_widget_set_margin_end(root_, 8);

    // Which track these belong to: a pane that follows the selection changes
    // underneath you as you arrow down the playlist, and lyrics are the one
    // kind of text where recognising the song from its content is exactly
    // what you cannot rely on.
    heading_ = gtk_label_new("");
    gtk_widget_add_css_class(heading_, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(heading_), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(heading_), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(heading_, TRUE);

    // View -> Timed Lyrics where the words are, shown only for words that
    // carry timing. Bound to the window's action, so it and the menu row are
    // one switch and either shows the other's state.
    timed_ = gtk_toggle_button_new_with_label(tr("Timed").c_str());
    gtk_actionable_set_action_name(GTK_ACTIONABLE(timed_), "win.timed-lyrics");
    gtk_widget_set_tooltip_text(timed_, tr("Follow timed lyrics line by line").c_str());
    gtk_widget_add_css_class(timed_, "flat");
    gtk_widget_set_valign(timed_, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(timed_, FALSE);

    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(top), heading_);
    gtk_box_append(GTK_BOX(top), timed_);
    gtk_box_append(GTK_BOX(root_), top);

    // A text view rather than a label, for the info pane's reason: lyrics are
    // there to be read, and often to be copied. Not editable, so it cannot be
    // edited into disagreeing with the file -- nothing here writes tags.
    text_ = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text_), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(text_), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text_), 6);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), text_);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(root_), scroller);

    // Under the text rather than in the heading, and hidden rather than blank
    // when the words are the file's own: the heading names the track, and
    // "Artist -- Title -- LRCLIB" reads as a third name.
    source_ = gtk_label_new("");
    gtk_widget_add_css_class(source_, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(source_), 0.0F);
    gtk_widget_set_visible(source_, FALSE);
    gtk_box_append(GTK_BOX(root_), source_);

    // The sung line: bold, in the accent's standalone variant -- the one
    // libadwaita derives for text on the view background, so a yellow accent
    // on a light pane is darkened to read, which is what readableOn() does
    // for the wx pane by hand.
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_));
    sungTag_    = gtk_text_buffer_create_tag(buffer, "sung", "weight", PANGO_WEIGHT_BOLD, nullptr);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer, &start);
    scrollMark_ = gtk_text_buffer_create_mark(buffer, nullptr, &start, TRUE);
    applyAccent();
    AdwStyleManager* style = adw_style_manager_get_default();
    for (const char* property : {"notify::dark", "notify::accent-color"}) {
        connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
            style, property, [this](GObject*, GParamSpec*) { applyAccent(); }));
    }

    showEntry(nullptr, false);
}

void LyricsPane::applyAccent() {
    AdwStyleManager* style = adw_style_manager_get_default();
    GdkRGBA          colour;
    adw_accent_color_to_standalone_rgba(adw_style_manager_get_accent_color(style),
                                        adw_style_manager_get_dark(style), &colour);
    g_object_set(sungTag_, "foreground-rgba", &colour, nullptr);
}

void LyricsPane::setLookup(LyricsLookup* lookup) { presenter_.setLookup(lookup); }

void LyricsPane::showEntry(const PlaylistEntry* entry, bool playing) {
    playing_ = playing && entry != nullptr;
    const std::optional<app::LyricsText> text =
        presenter_.show(entry, [this](const app::LyricsText& answered) { present(answered); });
    if (text) {
        present(*text);
    } else {
        // The same words, but the track may have started or stopped playing,
        // which changes whether they are followed and nothing else.
        updateFollowing();
    }
}

void LyricsPane::present(const app::LyricsText& text) {
    gtk_label_set_text(GTK_LABEL(heading_), text.heading.c_str());
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_));
    gtk_text_buffer_set_text(buffer, text.body.c_str(), -1);
    // Back to the top: opening a song at its first line, not its last.
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_place_cursor(buffer, &start);
    gtk_text_buffer_move_mark(buffer, scrollMark_, &start);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(text_), scrollMark_, 0.0, FALSE, 0.0, 0.0);
    gtk_label_set_text(GTK_LABEL(source_), text.source.c_str());
    gtk_widget_set_visible(source_, !text.source.empty());
    gtk_widget_set_visible(timed_, text.timeable);

    // Setting the text dropped every tag, so nothing is marked as sung.
    synced_ = text.synced;
    sung_   = SyncedLyrics::npos;
    updateFollowing();
}

void LyricsPane::updateFollowing() {
    if (!playing_ || !synced_ || !position_) {
        timer_.stop();
        if (sung_ != SyncedLyrics::npos) {
            styleLine(sung_, false);
            sung_ = SyncedLyrics::npos;
        }
        return;
    }
    // A tenth of a second: late by less than anyone reading can notice, and
    // the work per tick is a binary search. The wx pane's interval.
    if (!timer_.running()) {
        timer_.start(100, [this] { tick(); });
    }
    tick();
}

void LyricsPane::tick() {
    // A pane in a hidden sidebar keeps its timer -- the window redraws it on
    // the way back in -- but has nothing to do meanwhile.
    if (!synced_ || !position_ || !gtk_widget_get_mapped(text_)) {
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
    if (line == SyncedLyrics::npos) {
        return;
    }
    styleLine(line, true);

    // Left alone while the reader has something selected: scrolling it away
    // mid-drag is the pane working against them.
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_));
    if (gtk_text_buffer_get_has_selection(buffer)) {
        return;
    }
    // Three lines ahead first, then the line itself: scrolling to a mark
    // moves as little as it can, so what comes next is in view before it is
    // needed without ever pushing the sung line out.
    constexpr std::size_t kLookAhead = 3;
    const std::size_t     ahead      = std::min(line + kLookAhead, synced_->lines.size() - 1);
    for (const std::size_t target : {ahead, line}) {
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_line(buffer, &at, static_cast<int>(target));
        gtk_text_buffer_move_mark(buffer, scrollMark_, &at);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(text_), scrollMark_, 0.0, FALSE, 0.0, 0.0);
    }
}

void LyricsPane::styleLine(std::size_t index, bool sung) {
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_));
    GtkTextIter    start;
    if (!gtk_text_buffer_get_iter_at_line(buffer, &start, static_cast<int>(index))) {
        return;
    }
    GtkTextIter end = start;
    if (!gtk_text_iter_ends_line(&end)) {
        gtk_text_iter_forward_to_line_end(&end);
    }
    if (sung) {
        gtk_text_buffer_apply_tag(buffer, sungTag_, &start, &end);
    } else {
        gtk_text_buffer_remove_tag(buffer, sungTag_, &start, &end);
    }
}

// --- Equalizer ----------------------------------------------------------------------

EqualizerPane::EqualizerPane(Settings& settings)
    : settings_(settings), presets_(shippedEqualizerPresets()) {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(root_, 4);
    gtk_widget_set_margin_bottom(root_, 4);
    gtk_widget_set_margin_start(root_, 8);
    gtk_widget_set_margin_end(root_, 8);

    // The preset row. No library, no row: the sliders work perfectly well
    // without it, and a selector whose only entry is "Custom" would be a
    // control that cannot be used for anything.
    if (presets_.size() > 0) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

        enabled_ = gtk_check_button_new_with_label(tr("Enable").c_str());
        gtk_widget_set_tooltip_text(
            enabled_, tr("Bypasses the equaliser without disturbing the curve, which is what "
                         "comparing one against the original needs. A flat equaliser is skipped "
                         "either way, so this costs nothing until a band is moved.")
                          .c_str());
        connections_.push_back(Connection::to<void(GtkCheckButton*)>(
            enabled_, "toggled", [this](GtkCheckButton* button) {
                if (syncing_) {
                    return;
                }
                settings_.setGraphicEqEnable(gtk_check_button_get_active(button));
                settingChanged.publish("GraphicEQenable");
            }));
        gtk_box_append(GTK_BOX(row), enabled_);

        gtk_box_append(GTK_BOX(row), gtk_label_new(tr("Preset").c_str()));
        auto names = GObjectPtr<GtkStringList>::adopt(gtk_string_list_new(nullptr));
        for (const EqualizerPreset& preset : presets_.presets()) {
            gtk_string_list_append(names.get(), preset.name.c_str());
        }
        gtk_string_list_append(names.get(), tr("Custom").c_str());
        presetDrop_ = gtk_drop_down_new(G_LIST_MODEL(g_object_ref(names.get())), nullptr);
        gtk_widget_set_tooltip_text(
            presetDrop_, tr("Presets store ten points; the 31 bands are interpolated from them. "
                            "Moving any slider afterwards leaves the curve alone and changes this "
                            "to Custom.")
                             .c_str());
        connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
            presetDrop_, "notify::selected", [this](GObject*, GParamSpec*) {
                if (syncing_) {
                    return;
                }
                const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(presetDrop_));
                if (selected != GTK_INVALID_LIST_POSITION) {
                    selectPreset(static_cast<int>(selected));
                }
            }));
        gtk_box_append(GTK_BOX(row), presetDrop_);

        trackGenre_ = gtk_check_button_new_with_label(tr("Follow the track's genre").c_str());
        gtk_widget_set_tooltip_text(
            trackGenre_,
            tr("Chooses the preset whose name matches each track's genre tag as it "
               "starts. A track with no genre, or one nothing matches, gets Flat -- so "
               "this rewrites the equaliser at every track boundary rather than only "
               "when it has something to say.")
                .c_str());
        connections_.push_back(Connection::to<void(GtkCheckButton*)>(
            trackGenre_, "toggled", [this](GtkCheckButton* button) {
                if (syncing_) {
                    return;
                }
                settings_.setGraphicEqTrackGenre(gtk_check_button_get_active(button));
                // Published so the session can apply the playing track's genre
                // at once.
                settingChanged.publish("GraphicEQtrackgenre");
            }));
        gtk_box_append(GTK_BOX(row), trackGenre_);

        gtk_box_append(GTK_BOX(root_), row);
    }

    // The bands, in a horizontal scroller: thirty-two columns have a natural
    // width a narrow strip does not have.
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_NEVER);
    GtkWidget* columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    addBand(columns, tr("Pre"), "eqPreamp");
    gtk_box_append(GTK_BOX(columns), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    const auto frequencies = Equalizer::bandFrequencies();
    const auto keys        = Equalizer::bandSettingsKeys();
    for (std::size_t band = 0; band < keys.size(); ++band) {
        addBand(columns, frequencyLabel(frequencies[band]), keys[band]);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), columns);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(root_), scroller);

    // The footer: Flat, and the note.
    GtkWidget* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* flat   = gtk_button_new_with_label(tr("Flat").c_str());
    gtk_widget_set_tooltip_text(
        flat, tr("Selects the Flat preset: every band and the preamp back to 0 dB, which "
                 "makes the equaliser bit-transparent again.")
                  .c_str());
    connections_.push_back(
        Connection::to<void(GtkButton*)>(flat, "clicked", [this](GtkButton*) { flatten(); }));
    gtk_box_append(GTK_BOX(footer), flat);
    GtkWidget* note = gtk_label_new(
        tr("31 bands, \xC2\xB1""20 dB. Changes apply to the track already playing. A boost can "
           "clip; the preamp is the headroom for it.")
            .c_str());
    gtk_widget_add_css_class(note, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(note), 0.0F);
    gtk_box_append(GTK_BOX(footer), note);
    gtk_box_append(GTK_BOX(root_), footer);

    refresh();
}

void EqualizerPane::addBand(GtkWidget* row, const std::string& caption, const std::string& key) {
    GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    GtkWidget* readout = gtk_label_new("");
    gtk_widget_add_css_class(readout, "numeric");
    gtk_widget_add_css_class(readout, "caption");
    gtk_widget_set_size_request(readout, 34, -1);
    gtk_box_append(GTK_BOX(column), readout);

    // Inverted, so a boost drags the handle up: a vertical scale's minimum is
    // at the top by default, which reads upside down for an equaliser.
    GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, -kEqRangeDb, kEqRangeDb,
                                                kEqStepDb);
    gtk_range_set_inverted(GTK_RANGE(scale), TRUE);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_scale_set_has_origin(GTK_SCALE(scale), FALSE);
    gtk_scale_add_mark(GTK_SCALE(scale), 0.0, GTK_POS_LEFT, nullptr);
    gtk_widget_set_size_request(scale, -1, 140);
    gtk_widget_set_vexpand(scale, TRUE);
    gtk_widget_set_tooltip_text(scale, caption.c_str());
    connections_.push_back(Connection::to<void(GtkRange*)>(
        scale, "value-changed", [this, key, readout](GtkRange* range) {
            if (syncing_) {
                return;
            }
            const double db = gtk_range_get_value(range);
            gtk_label_set_text(GTK_LABEL(readout), decibelLabel(db).c_str());
            settings_.setRawValue(key, std::to_string(db));
            enableIfSilent();
            markCustom();
            settingChanged.publish(key);
        }));
    gtk_box_append(GTK_BOX(column), scale);

    GtkWidget* label = gtk_label_new(caption.c_str());
    gtk_widget_add_css_class(label, "caption");
    gtk_box_append(GTK_BOX(column), label);

    gtk_box_append(GTK_BOX(row), column);
    scales_.push_back(scale);
    readouts_.push_back(readout);
    keys_.push_back(key);
}

int EqualizerPane::customIndex() const { return static_cast<int>(presets_.size()); }

void EqualizerPane::refresh() {
    syncing_ = true;
    for (std::size_t i = 0; i < scales_.size(); ++i) {
        const double db = toDouble(settings_.rawValue(keys_[i]));
        gtk_range_set_value(GTK_RANGE(scales_[i]), db);
        gtk_label_set_text(GTK_LABEL(readouts_[i]), decibelLabel(db).c_str());
    }
    if (presetDrop_ != nullptr) {
        const int stored = settings_.GraphicEqPreset();
        gtk_drop_down_set_selected(GTK_DROP_DOWN(presetDrop_),
                                   static_cast<guint>(presets_.at(stored) != nullptr ? stored
                                                                                     : customIndex()));
    }
    if (trackGenre_ != nullptr) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(trackGenre_), settings_.GraphicEqTrackGenre());
    }
    if (enabled_ != nullptr) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(enabled_), settings_.GraphicEqEnable());
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
        return;
    }
    applyEqualizerPreset(settings_, *preset);
    if (preset->name != "Flat") {
        enableIfSilent();
    }
    refresh();
    publishCurve();
}

void EqualizerPane::markCustom() {
    if (presetDrop_ == nullptr) {
        return;
    }
    const int custom = customIndex();
    if (settings_.GraphicEqPreset() == custom) {
        return;
    }
    settings_.setGraphicEqPreset(custom);
    syncing_ = true;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(presetDrop_), static_cast<guint>(custom));
    syncing_ = false;
}

void EqualizerPane::enableIfSilent() {
    if (settings_.GraphicEqEnable()) {
        return;
    }
    settings_.setGraphicEqEnable(true);
    if (enabled_ != nullptr) {
        syncing_ = true;
        gtk_check_button_set_active(GTK_CHECK_BUTTON(enabled_), TRUE);
        syncing_ = false;
    }
}

void EqualizerPane::flatten() {
    // Through the preset named "Flat" when the library has one, which is what
    // Cog's Flat button does: it leaves the selector reading "Flat" rather
    // than "Custom", so the state says which preset produced the curve.
    if (const int flat = presets_.indexOf("Flat"); flat >= 0) {
        selectPreset(flat);
        return;
    }
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        settings_.setRawValue(keys_[i], "0");
    }
    refresh();
    publishCurve();
}

// --- Speed -------------------------------------------------------------------------------

SpeedPane::SpeedPane(Settings& settings) : settings_(settings) {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(root_, 4);
    gtk_widget_set_margin_bottom(root_, 4);
    gtk_widget_set_margin_start(root_, 8);
    gtk_widget_set_margin_end(root_, 8);

    const auto makeRow = [this](const char* caption, GtkWidget*& scale, GtkWidget*& value,
                                const char* key) {
        GtkWidget* row   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* label = gtk_label_new(tr(caption).c_str());
        gtk_widget_set_size_request(label, 56, -1);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
        gtk_box_append(GTK_BOX(row), label);
        // The slider's travel is SpeedCurve's, so the two frontends and the
        // preferences pane agree about where 1.00x sits.
        scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, app::kSpeedSliderMax, 1);
        gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
        gtk_widget_set_hexpand(scale, TRUE);
        gtk_scale_add_mark(GTK_SCALE(scale), app::sliderFromSpeed(1.0), GTK_POS_BOTTOM, nullptr);
        gtk_box_append(GTK_BOX(row), scale);
        value = gtk_label_new("");
        gtk_widget_add_css_class(value, "numeric");
        gtk_widget_set_size_request(value, 56, -1);
        gtk_label_set_xalign(GTK_LABEL(value), 1.0F);
        gtk_box_append(GTK_BOX(row), value);
        connections_.push_back(Connection::to<void(GtkRange*)>(
            scale, "value-changed", [this, key](GtkRange* range) {
                if (syncing_) {
                    return;
                }
                const int position = static_cast<int>(std::lround(gtk_range_get_value(range)));
                write(key, app::snapSpeed(app::speedFromSlider(position)));
            }));
        return row;
    };

    pitchRow_ = makeRow(XPCOG_TRANSLATE("Pitch"), pitch_, pitchValue_, "pitch");
    gtk_box_append(GTK_BOX(root_), pitchRow_);
    gtk_box_append(GTK_BOX(root_), makeRow(XPCOG_TRANSLATE("Tempo"), tempo_, tempoValue_, "tempo"));

    GtkWidget* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    lock_ = gtk_check_button_new_with_label(tr("Lock pitch and tempo together").c_str());
    connections_.push_back(Connection::to<void(GtkCheckButton*)>(
        lock_, "toggled", [this](GtkCheckButton* button) {
            if (syncing_) {
                return;
            }
            settings_.setRawValue("speedLock", gtk_check_button_get_active(button) ? "true" : "false");
            settingChanged.publish("speedLock");
        }));
    gtk_box_append(GTK_BOX(footer), lock_);
    note_ = gtk_label_new(tr("No engine is chosen, so these do nothing yet.").c_str());
    gtk_widget_add_css_class(note_, "dim-label");
    gtk_widget_set_hexpand(note_, TRUE);
    gtk_label_set_xalign(GTK_LABEL(note_), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(note_), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(note_), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(note_), 2);
    gtk_box_append(GTK_BOX(footer), note_);
    GtkWidget* reset = gtk_button_new_with_label(tr("Reset to 1.00\xC3\x97").c_str());
    connections_.push_back(Connection::to<void(GtkButton*)>(reset, "clicked", [this](GtkButton*) {
        write("pitch", 1.0);
        write("tempo", 1.0);
    }));
    gtk_box_append(GTK_BOX(footer), reset);
    // "Preferences" rather than "Settings" because that is what this
    // application calls that window everywhere else, and the ellipsis is the
    // convention for a control that opens one.
    GtkWidget* settingsButton = gtk_button_new_with_label(tr("Preferences\xE2\x80\xA6").c_str());
    connections_.push_back(Connection::to<void(GtkButton*)>(
        settingsButton, "clicked", [this](GtkButton*) { settingsRequested.publish(); }));
    gtk_box_append(GTK_BOX(footer), settingsButton);
    gtk_box_append(GTK_BOX(root_), footer);

    refresh();
}

void SpeedPane::write(const char* key, double ratio) {
    settings_.setRawValue(key, std::to_string(ratio));
    settingChanged.publish(key);

    // Cog's speed lock is the UI writing both keys, not the engine linking
    // them (SpeedButton.m pressLock:) -- ported as-is. Under varispeed there
    // is nothing to lock: the engine moves pitch with tempo itself.
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
    gtk_range_set_value(GTK_RANGE(pitch_), app::sliderFromSpeed(pitch));
    gtk_range_set_value(GTK_RANGE(tempo_), app::sliderFromSpeed(tempo));
    gtk_label_set_text(GTK_LABEL(pitchValue_), ratioLabel(pitch).c_str());
    gtk_label_set_text(GTK_LABEL(tempoValue_), ratioLabel(tempo).c_str());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(lock_), settings_.SpeedLock());
    syncing_ = false;

    // Varispeed resamples, as a record player does: pitch *is* tempo under
    // it, so a second slider would be a duplicate and a lock would have
    // nothing to join.
    gtk_widget_set_visible(pitchRow_, !varispeed);
    gtk_widget_set_visible(lock_, !varispeed);
    gtk_widget_set_visible(note_, disabled);
}

// --- the strip ----------------------------------------------------------------------------

ToolsStrip::ToolsStrip() {
    // A scroller, so the strip's natural width never forces the window's:
    // three sections open side by side want more than a small window has,
    // and the answer is a scrollbar under them rather than a window that
    // grows past the screen.
    root_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(root_), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_NEVER);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(root_), TRUE);
    row_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(root_), row_);
}

void ToolsStrip::addSection(const std::string& name, const std::string& title, GtkWidget* content) {
    GtkWidget* section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(section, TRUE);

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(header, 8);
    GtkWidget* label = gtk_label_new(title.c_str());
    gtk_widget_add_css_class(label, "heading");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(header), label);
    GtkWidget* close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close, "flat");
    gtk_widget_add_css_class(close, "circular");
    connections_.push_back(Connection::to<void(GtkButton*)>(
        close, "clicked", [this, name](GtkButton*) { closeRequested.publish(name); }));
    gtk_box_append(GTK_BOX(header), close);
    gtk_box_append(GTK_BOX(section), header);

    gtk_widget_set_vexpand(content, TRUE);
    gtk_box_append(GTK_BOX(section), content);

    GtkWidget* revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_child(GTK_REVEALER(revealer), section);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
    gtk_widget_set_visible(revealer, FALSE);
    gtk_widget_set_hexpand(revealer, TRUE);
    gtk_box_append(GTK_BOX(row_), revealer);

    sections_[name] = Section{revealer};
}

void ToolsStrip::setShown(const std::string& name, bool shown) {
    const auto found = sections_.find(name);
    if (found == sections_.end()) {
        return;
    }
    // Visible as well as revealed, so a hidden section takes no width at all
    // rather than a collapsed revealer's few pixels.
    gtk_widget_set_visible(found->second.revealer, shown);
    gtk_revealer_set_reveal_child(GTK_REVEALER(found->second.revealer), shown);
}

bool ToolsStrip::shown(const std::string& name) const {
    const auto found = sections_.find(name);
    return found != sections_.end() &&
           gtk_revealer_get_reveal_child(GTK_REVEALER(found->second.revealer));
}

bool ToolsStrip::anyShown() const {
    for (const auto& [name, section] : sections_) {
        if (gtk_revealer_get_reveal_child(GTK_REVEALER(section.revealer))) {
            return true;
        }
    }
    return false;
}

}  // namespace xpcog::gtk
