#include "Chrome.hpp"

#include <memory>
#include <string_view>
#include <utility>

namespace xpcog::winui {
namespace {

/// The descendant of `root` called `name`, depth first, or null. For parts of
/// a control's template, which are not reachable through the control's own
/// namescope from out here.
mux::FrameworkElement findNamed(const mux::DependencyObject& root, std::wstring_view name) {
    const int count = mux::Media::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        const auto child = mux::Media::VisualTreeHelper::GetChild(root, i);
        if (const auto element = child.try_as<mux::FrameworkElement>();
            element && element.Name() == name) {
            return element;
        }
        if (auto found = findNamed(child, name)) {
            return found;
        }
    }
    return nullptr;
}

}  // namespace

mux::Controls::Button glyphButton(const wchar_t* glyph, const std::string& tooltip,
                                  std::function<void()> action) {
    auto icon = mux::Controls::FontIcon();
    icon.Glyph(glyph);
    icon.FontSize(16);

    // Transparent rather than null, because the template only swaps its own
    // resources in for the pointer states.
    auto button = mux::Controls::Button();
    button.Content(icon);
    button.Width(40);
    button.Height(36);
    button.Padding(mux::ThicknessHelper::FromUniformLength(0));
    button.Background(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
    button.BorderBrush(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
    setGlyph(button, glyph, tooltip);
    button.Click([action = std::move(action)](auto&&, auto&&) { action(); });
    return button;
}

void setGlyph(const mux::Controls::Button& button, const wchar_t* glyph, const std::string& tooltip) {
    button.Content().as<mux::Controls::FontIcon>().Glyph(glyph);
    const winrt::hstring name = toH(tooltip);
    mux::Controls::ToolTipService::SetToolTip(button, winrt::box_value(name));
    // The tooltip is not an accessible name; a glyph button needs one.
    mux::Automation::AutomationProperties::SetName(button, name);
}

void fitTitleBarContent(const mux::Controls::TitleBar& bar, const mux::FrameworkElement& content) {
    // Weak, because the handlers live on the bar and the column: strong
    // references from there would keep the bar alive from inside itself.
    struct State {
        winrt::weak_ref<mux::Controls::TitleBar> bar;
        winrt::weak_ref<mux::FrameworkElement>   content;
        mux::FrameworkElement                    column{nullptr};
    };
    auto state = std::make_shared<State>(State{winrt::make_weak(bar), winrt::make_weak(content)});

    // The column is deferred-load, realised once the bar has content and its
    // template, so it is looked up until found and then followed.
    auto fit = std::make_shared<std::function<void()>>();
    *fit = [state, weakFit = std::weak_ptr<std::function<void()>>(fit)] {
        const auto bar     = state->bar.get();
        const auto content = state->content.get();
        if (!bar || !content) {
            return;
        }
        if (!state->column) {
            state->column = findNamed(bar, L"PART_ContentPresenterGrid");
            if (!state->column) {
                return;
            }
            state->column.SizeChanged([weakFit](auto&&, auto&&) {
                if (const auto again = weakFit.lock()) {
                    (*again)();
                }
            });
        }
        double inset = 0;
        if (const auto presenter = findNamed(state->column, L"PART_ContentPresenter")) {
            const auto margin = presenter.Margin();
            inset += margin.Left + margin.Right;
        }
        const auto margin = content.Margin();
        inset += margin.Left + margin.Right;
        const double width = state->column.ActualWidth() - inset;
        if (width > 0 && width != content.Width()) {
            content.Width(width);
        }
    };
    // The bar's handler owns the function; the column's reaches it weakly.
    bar.SizeChanged([fit](auto&&, auto&&) { (*fit)(); });
}

std::filesystem::path besideExecutable(const wchar_t* name) {
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path() / name;
}

}  // namespace xpcog::winui
