#include "FileTreePane.hpp"

#include "Commands.hpp"  // uicore's stripMnemonics
#include "Translations.hpp"

#include <winrt/Microsoft.Windows.Storage.Pickers.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace xpcog::winui {

namespace {

namespace fs = std::filesystem;

// Segoe Fluent Icons: Folder and MusicNote, the glyphs Explorer and Media
// Player draw for the same things.
constexpr const wchar_t* kGlyphFolder = L"\xE8B7";
constexpr const wchar_t* kGlyphMusic  = L"\xE8D6";

// One row: an icon and a name. Two templates, chosen per node by
// KindSelector below, so the glyph is fixed in each and only the name is
// bound -- to the node's own Content, a TreeViewNode dependency property,
// which {Binding} reaches through WinUI's own type information and needs no
// property provider of ours.
//
// Not filled in code the way the playlist's rows are: the TreeView's inner
// list does not raise ContainerContentChanging for its rows, and an approach
// built on it left every row an empty chevron.
std::wstring rowTemplate(const wchar_t* glyph) {
    return std::wstring(
               L"<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'>"
               L"<StackPanel Orientation='Horizontal' Spacing='8'>"
               L"<FontIcon FontSize='16' VerticalAlignment='Center' Glyph='") +
           glyph +
           L"'/>"
           L"<TextBlock VerticalAlignment='Center' TextTrimming='CharacterEllipsis'"
           L" Text='{Binding Content}'/>"
           L"</StackPanel></DataTemplate>";
}

struct Entry {
    fs::path     path;
    std::wstring name;
    bool         folder = false;
};

/// The leaf name, or the whole thing when there is no leaf -- a drive root,
/// where filename() is empty and the path itself is the name.
std::wstring leafName(const fs::path& path) {
    const std::wstring leaf = path.filename().wstring();
    return leaf.empty() ? path.wstring() : leaf;
}

/// Hidden the way Explorer means it: the attribute, or the system one, which
/// is what keeps desktop.ini and $RECYCLE.BIN out of a music folder. A leading
/// dot too, for folders that came from somewhere else.
bool hidden(const fs::path& path) {
    const std::wstring name = path.filename().wstring();
    if (!name.empty() && name.front() == L'.') {
        return true;
    }
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
}

/// Folders first, then names the way Explorer orders them: case-insensitive,
/// and "Track 2" before "Track 10".
bool before(const Entry& a, const Entry& b) {
    if (a.folder != b.folder) {
        return a.folder;
    }
    return ::CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                             a.name.c_str(), static_cast<int>(a.name.size()), b.name.c_str(),
                             static_cast<int>(b.name.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
}

/// The nearest TreeViewItem at or above a hit element.
mux::Controls::TreeViewItem itemAt(winrt::Windows::Foundation::IInspectable const& source) {
    auto element = source.try_as<mux::DependencyObject>();
    while (element) {
        if (auto item = element.try_as<mux::Controls::TreeViewItem>()) {
            return item;
        }
        element = mux::Media::VisualTreeHelper::GetParent(element);
    }
    return nullptr;
}

/// The folder row or the file row, by what the node stands for. The pane
/// knows that, not the node, so it is asked through `isFolder`.
struct KindSelector : mux::Controls::DataTemplateSelectorT<KindSelector> {
    KindSelector(std::function<bool(const mux::Controls::TreeViewNode&)> isFolder)
        : isFolder_(std::move(isFolder)),
          folder_(mux::Markup::XamlReader::Load(rowTemplate(kGlyphFolder)).as<mux::DataTemplate>()),
          file_(mux::Markup::XamlReader::Load(rowTemplate(kGlyphMusic)).as<mux::DataTemplate>()) {}

    mux::DataTemplate SelectTemplateCore(winrt::Windows::Foundation::IInspectable const& item) {
        const auto node = item.try_as<mux::Controls::TreeViewNode>();
        return node && isFolder_(node) ? folder_ : file_;
    }
    mux::DataTemplate SelectTemplateCore(winrt::Windows::Foundation::IInspectable const& item,
                                         mux::DependencyObject const&) {
        return SelectTemplateCore(item);
    }

private:
    std::function<bool(const mux::Controls::TreeViewNode&)> isFolder_;
    mux::DataTemplate folder_;
    mux::DataTemplate file_;
};

}  // namespace

struct FileTreePane::Impl {
    Impl(FileTreePane& o, const PluginRegistry& r, std::function<winrt::Microsoft::UI::WindowId()> id)
        : owner(o), registry(r), windowId(std::move(id)) {}

    FileTreePane&                                   owner;
    const PluginRegistry&                           registry;
    std::function<winrt::Microsoft::UI::WindowId()> windowId;

    mux::Controls::Grid     root{nullptr};
    mux::Controls::TextBlock rootLabel{nullptr};
    mux::Controls::TreeView tree{nullptr};
    std::string             rootPath;

    /// What each node stands for. Keyed by the node's identity: the node's own
    /// Content holds only the name, for accessibility, and the path and kind
    /// live here. Cleared with the tree on every new root.
    std::unordered_map<void*, Entry> entries;

    /// The chooser is asynchronous, and the pane can go while it is open.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    [[nodiscard]] const Entry* entryOf(const mux::Controls::TreeViewNode& node) const {
        const auto found = entries.find(winrt::get_abi(node));
        return found != entries.end() ? &found->second : nullptr;
    }

    [[nodiscard]] bool shows(const fs::directory_entry& item) const {
        if (hidden(item.path())) {
            return false;
        }
        std::error_code error;
        if (item.is_directory(error)) {
            return true;
        }
        // The registry's extensions, lower-cased, against the name's.
        std::string extension = item.path().extension().string();
        if (extension.size() < 2) {
            return false;
        }
        extension.erase(0, 1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const std::string& known : registry.allExtensions()) {
            if (known == extension) {
                return true;
            }
        }
        return false;
    }

    /// A folder's children, filtered and sorted, as nodes. Synchronous: a
    /// folder is listed once, when it is first opened, and that is the moment
    /// the listener is waiting for it anyway.
    void list(const fs::path& folder,
              const winrt::Windows::Foundation::Collections::IVector<mux::Controls::TreeViewNode>& into) {
        std::vector<Entry> found;
        std::error_code    error;
        for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            if (!shows(*it)) {
                continue;
            }
            std::error_code kind;
            found.push_back({it->path(), leafName(it->path()), it->is_directory(kind)});
        }
        std::sort(found.begin(), found.end(), before);

        for (Entry& entry : found) {
            auto node = mux::Controls::TreeViewNode();
            node.Content(winrt::box_value(winrt::hstring(entry.name)));
            node.HasUnrealizedChildren(entry.folder);
            entries.emplace(winrt::get_abi(node), std::move(entry));
            into.Append(node);
        }
    }

    void rebuild() {
        tree.RootNodes().Clear();
        entries.clear();
        if (rootPath.empty()) {
            rootLabel.Text(toH(app::tr("Choose the folder to browse")));
            return;
        }
        const fs::path folder = fs::path(winrt::to_hstring(rootPath).c_str());
        rootLabel.Text(winrt::hstring(leafName(folder)));
        list(folder, tree.RootNodes());
    }


    [[nodiscard]] std::vector<Url> urlsOf(const mux::Controls::TreeViewNode& node) const {
        std::vector<Url> urls;
        if (const Entry* entry = node ? entryOf(node) : nullptr) {
            urls.push_back(Url::fromLocalPath(entry->path));
        }
        return urls;
    }

    winrt::fire_and_forget choose() {
        auto weak = std::weak_ptr<bool>(alive);
        try {
            winrt::Microsoft::Windows::Storage::Pickers::FolderPicker picker(windowId());
            const auto chosen = co_await picker.PickSingleFolderAsync();
            if (weak.expired() || !chosen) {
                co_return;
            }
            const std::string path = toUtf8(chosen.Path());
            owner.setRootPath(path);
            if (rootPath == path) {
                owner.rootChosen.publish();
            }
        } catch (const winrt::hresult_error&) {
            // A picker that could not open has already told the listener
            // nothing; there is no state here to undo.
        }
    }

    void showMenu(const mux::UIElement& target, std::optional<winrt::Windows::Foundation::Point> at,
                  const mux::Controls::TreeViewNode& node) {
        auto menu = mux::Controls::MenuFlyout();

        auto add = mux::Controls::MenuFlyoutItem();
        add.Text(toH(app::stripMnemonics(app::tr("&Add to Playlist"))));
        add.Icon(mux::Controls::SymbolIcon(mux::Controls::Symbol::Add));
        add.IsEnabled(static_cast<bool>(node));
        add.Click([this, node](auto&&, auto&&) {
            if (std::vector<Url> urls = urlsOf(node); !urls.empty()) {
                owner.addRequested.publish(urls);
            }
        });
        menu.Items().Append(add);
        menu.Items().Append(mux::Controls::MenuFlyoutSeparator());

        auto root = mux::Controls::MenuFlyoutItem();
        root.Text(toH(app::stripMnemonics(app::tr("Choose &Root Folder..."))));
        root.Click([this](auto&&, auto&&) { choose(); });
        menu.Items().Append(root);

        auto options = mux::Controls::Primitives::FlyoutShowOptions();
        if (at) {
            options.Position(*at);
        }
        menu.ShowAt(target, options);
    }
};

FileTreePane::FileTreePane(const PluginRegistry& registry,
                           std::function<winrt::Microsoft::UI::WindowId()> windowId)
    : impl_(std::make_unique<Impl>(*this, registry, std::move(windowId))) {
    Impl& impl = *impl_;

    // --- the root button ----------------------------------------------------
    //
    // A subtle button the width of the pane, labelled with the folder's name:
    // the header GTK's sidebar has, in Windows' terms.
    auto icon = mux::Controls::FontIcon();
    icon.Glyph(kGlyphFolder);
    icon.FontSize(16);
    impl.rootLabel = mux::Controls::TextBlock();
    impl.rootLabel.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    impl.rootLabel.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
    auto label = mux::Controls::StackPanel();
    label.Orientation(mux::Controls::Orientation::Horizontal);
    label.Spacing(8);
    label.Children().Append(icon);
    label.Children().Append(impl.rootLabel);

    auto button = mux::Controls::Button();
    button.Content(label);
    button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    button.HorizontalContentAlignment(mux::HorizontalAlignment::Left);
    button.Margin(mux::ThicknessHelper::FromLengths(4, 4, 4, 4));
    // Transparent rather than null, so the template's own hover and pressed
    // fills still appear.
    button.Background(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
    button.BorderBrush(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
    const winrt::hstring prompt = toH(app::tr("Choose the folder to browse"));
    mux::Controls::ToolTipService::SetToolTip(button, winrt::box_value(prompt));
    mux::Automation::AutomationProperties::SetName(button, prompt);
    button.Click([this](auto&&, auto&&) { impl_->choose(); });

    // --- the tree -----------------------------------------------------------
    impl.tree = mux::Controls::TreeView();
    impl.tree.SelectionMode(mux::Controls::TreeViewSelectionMode::Single);
    impl.tree.ItemTemplateSelector(winrt::make<KindSelector>([this](const mux::Controls::TreeViewNode& node) {
        const Entry* entry = impl_->entryOf(node);
        return entry != nullptr && entry->folder;
    }));

    // Children are listed the first time a folder opens, and never again:
    // HasUnrealizedChildren is what gives a folder its chevron before then.
    impl.tree.Expanding([this](auto&&, mux::Controls::TreeViewExpandingEventArgs const& args) {
        const auto node = args.Node();
        if (!node.HasUnrealizedChildren()) {
            return;
        }
        node.HasUnrealizedChildren(false);
        if (const Entry* entry = impl_->entryOf(node)) {
            const fs::path folder = entry->path;
            impl_->list(folder, node.Children());
        }
    });

    // Double-click and Enter add, as in the other two trees.
    impl.tree.DoubleTapped([this](auto&&, mux::Input::DoubleTappedRoutedEventArgs const& args) {
        if (const auto item = itemAt(args.OriginalSource())) {
            if (std::vector<Url> urls = impl_->urlsOf(impl_->tree.NodeFromContainer(item));
                !urls.empty()) {
                activated.publish(urls);
            }
        }
    });
    impl.tree.KeyDown([this](auto&&, mux::Input::KeyRoutedEventArgs const& args) {
        if (args.Key() != winrt::Windows::System::VirtualKey::Enter) {
            return;
        }
        if (std::vector<Url> urls = impl_->urlsOf(impl_->tree.SelectedNode()); !urls.empty()) {
            activated.publish(urls);
            args.Handled(true);
        }
    });

    // ContextRequested: the right-click, and Shift+F10 and the Menu key. The
    // row under the pointer becomes the selection first, so the menu acts on
    // what was clicked.
    impl.tree.ContextRequested([this](auto&&, mux::Input::ContextRequestedEventArgs const& args) {
        mux::Controls::TreeViewNode node{nullptr};
        if (const auto item = itemAt(args.OriginalSource())) {
            node = impl_->tree.NodeFromContainer(item);
            impl_->tree.SelectedNode(node);
        } else {
            node = impl_->tree.SelectedNode();
        }
        std::optional<winrt::Windows::Foundation::Point> at;
        winrt::Windows::Foundation::Point point{};
        if (args.TryGetPosition(impl_->tree, point)) {
            at = point;
        }
        impl_->showMenu(impl_->tree, at, node);
        args.Handled(true);
    });

    // --- the pane -------------------------------------------------------------
    //
    // No background: the window gives it its surface.
    impl.root = mux::Controls::Grid();
    impl.root.MinWidth(200);
    auto top = mux::Controls::RowDefinition();
    top.Height(mux::GridLengthHelper::Auto());
    auto rest = mux::Controls::RowDefinition();
    rest.Height(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
    impl.root.RowDefinitions().Append(top);
    impl.root.RowDefinitions().Append(rest);
    mux::Controls::Grid::SetRow(impl.tree, 1);
    impl.root.Children().Append(button);
    impl.root.Children().Append(impl.tree);

    impl.rebuild();
}

FileTreePane::~FileTreePane() {
    impl_->alive.reset();
}

mux::UIElement FileTreePane::element() const {
    return impl_->root;
}

const std::string& FileTreePane::rootPath() const {
    return impl_->rootPath;
}

void FileTreePane::setRootPath(const std::string& path) {
    if (path.empty()) {
        return;
    }
    std::error_code error;
    if (!fs::is_directory(fs::path(winrt::to_hstring(path).c_str()), error)) {
        return;
    }
    impl_->rootPath = path;
    impl_->rebuild();
}

void FileTreePane::chooseRootPath() {
    impl_->choose();
}

}  // namespace xpcog::winui
