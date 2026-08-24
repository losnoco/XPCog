// An immutable string whose storage is shared between every copy of it.
//
// The promoted tags on a PlaylistEntry repeat enormously across a library: an
// album's tracks share an artist, a genre and a date, and a comment can be
// shared by far more than that. The file stores each of them once and refers to
// them by id. Memory used not to: loading expanded every reference back into its
// own std::string, so a library holding two distinct comments between a hundred
// entries spent 1.5 GiB saying so.
//
// Sharing happens where the duplication did -- at the point strings are handed
// out. Library::readEntries resolves each id to one SharedString and gives that
// same one to every entry referring to it, so a load allocates per distinct
// string rather than per reference. There is deliberately no global intern
// table: it would need a lock on every construction, on paths that are hot for
// reasons unrelated to sharing, to save allocations the next save-and-load would
// collapse anyway.
//
// Empty costs nothing -- no allocation, no shared block. Most entries leave most
// of these fields empty, so that is the common case rather than a corner.

#pragma once

#include <compare>
#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace xpcog {

class SharedString {
public:
    SharedString() noexcept = default;

    // Explicit, all of them, and not for tidiness: the comparisons below take a
    // string_view, and an implicit constructor would make `shared == "text"`
    // ambiguous between converting the literal up to a SharedString and
    // converting it across to a string_view. Assignment stays implicit through
    // the operator= overloads, which is the form nearly every caller uses.
    explicit SharedString(std::string text) { assign(std::move(text)); }
    explicit SharedString(std::string_view text) { assign(std::string{text}); }
    explicit SharedString(const char* text)
        : SharedString(text != nullptr ? std::string{text} : std::string{}) {}

    SharedString& operator=(std::string text) {
        assign(std::move(text));
        return *this;
    }
    SharedString& operator=(std::string_view text) {
        assign(std::string{text});
        return *this;
    }
    SharedString& operator=(const char* text) {
        assign(text != nullptr ? std::string{text} : std::string{});
        return *this;
    }

    /// Implicit, so a field that used to be a std::string still reads like one.
    operator const std::string&() const noexcept { return str(); }  // NOLINT
    operator std::string_view() const noexcept { return str(); }    // NOLINT

    [[nodiscard]] const std::string& str() const noexcept {
        return text_ ? *text_ : emptyString();
    }
    [[nodiscard]] std::string_view view() const noexcept { return str(); }
    [[nodiscard]] bool             empty() const noexcept { return str().empty(); }
    [[nodiscard]] std::size_t      size() const noexcept { return str().size(); }
    [[nodiscard]] const char*      c_str() const noexcept { return str().c_str(); }

    void clear() noexcept { text_.reset(); }

    /// The storage itself, for a reader handing the same one to many entries.
    [[nodiscard]] const std::shared_ptr<const std::string>& handle() const noexcept {
        return text_;
    }
    [[nodiscard]] static SharedString fromHandle(
        std::shared_ptr<const std::string> text) {
        SharedString shared;
        if (text && !text->empty()) {
            shared.text_ = std::move(text);
        }
        return shared;
    }

    /// Defined here rather than left to std::string's, which are templates --
    /// template argument deduction does not look through the conversions above,
    /// so inheriting them is not an option however implicit they are.
    [[nodiscard]] friend bool operator==(const SharedString& left,
                                         const SharedString& right) noexcept {
        // The whole point of sharing: equal strings are usually the same one.
        return left.text_ == right.text_ || left.str() == right.str();
    }
    [[nodiscard]] friend std::strong_ordering operator<=>(
        const SharedString& left, const SharedString& right) noexcept {
        return left.str() <=> right.str();
    }

    [[nodiscard]] friend bool operator==(const SharedString& left,
                                         std::string_view    right) noexcept {
        return left.view() == right;
    }
    [[nodiscard]] friend std::strong_ordering operator<=>(
        const SharedString& left, std::string_view right) noexcept {
        return left.view() <=> right;
    }

    /// So a diagnostic can print one. Needed explicitly for the same reason the
    /// comparisons are: the standard library's is a template.
    friend std::ostream& operator<<(std::ostream& out, const SharedString& text) {
        return out << text.view();
    }

private:
    void assign(std::string text) {
        text_ = text.empty() ? nullptr
                             : std::make_shared<const std::string>(std::move(text));
    }

    [[nodiscard]] static const std::string& emptyString() noexcept {
        static const std::string value;
        return value;
    }

    std::shared_ptr<const std::string> text_;
};

}  // namespace xpcog
