// Turning bytes of unknown encoding into UTF-8.
//
// Extracted from PlaylistText.cpp when the HTTP source needed the same rule:
// ICY headers and StreamTitle blocks carry no charset and are frequently
// Latin-1 or CP1251 in practice, exactly like the playlist files this was
// written for.
//
// Cog's guess_encoding_of_string() (Audio/PluginController.mm:856) tries UTF-8
// and then hands the bytes to -[NSString stringEncodingForData:...], a
// statistical detector with no portable equivalent and no specified behaviour to
// port. The rule here is the deterministic half of that: valid UTF-8 is taken
// as-is, anything else is Latin-1. Latin-1 cannot fail, so the result is always
// valid UTF-8 and no input is ever rejected -- which is the property that
// matters, since the alternative is a stream title that vanishes rather than one
// that is merely wrong about which accented letter it is.

#pragma once

#include <string>

namespace xpcog::codecs {

/// Validates UTF-8 strictly enough to decide whether to fall back to Latin-1.
[[nodiscard]] bool isValidUtf8(const std::string& text);

/// Every byte becomes a codepoint. Cannot fail.
[[nodiscard]] std::string latin1ToUtf8(const std::string& text);

/// `text` unchanged when it is already valid UTF-8, otherwise decoded as Latin-1.
[[nodiscard]] std::string toUtf8(std::string text);

}  // namespace xpcog::codecs
