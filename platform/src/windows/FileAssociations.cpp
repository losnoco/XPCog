// Windows file associations, written straight to the registry.
//
// This used to go through QSettings, whose NativeFormat paths *are* registry
// paths -- a genuinely neat trick that meant no windows.h in the middle of it.
// With Qt gone the trick goes too, and what replaces it is the API QSettings was
// standing in for. The mapping is mechanical: a QSettings path component is a
// subkey, a trailing "." was the key's default value, and childKeys() was
// RegEnumValue. The one Win32 call that was already here, SHChangeNotify, is
// unchanged.
//
// What gets written, and what deliberately does not:
//
//   Software\Classes\XPCog.AudioFile              a ProgID: name, icon, command
//   Software\Classes\Applications\XPCog.exe       the "Open with" entry, plus a
//                                                 FriendlyAppName and the list of
//                                                 types this program accepts
//   Software\Classes\.<ext>\OpenWithProgids       XPCog *added* to each extension
//
// Not written: `.<ext>`'s own default value, which is the extension's current
// handler. Adding to OpenWithProgids offers XPCog; overwriting the default would
// take the file type from whatever has it. And it would not work anyway -- since
// Windows 8 the effective default lives in a hash-protected UserChoice key that
// only the Settings UI can write, so "make me the default" is not a thing an
// application can do, only a thing a user can choose.
//
// HKCU throughout, so none of this needs administrator rights, and it applies to
// the user who asked for it rather than to the machine.

#include "xpcog/platform/FileAssociations.hpp"

#include "WinString.hpp"

#include <shlobj.h>

#include <filesystem>
#include <string>
#include <vector>

namespace xpcog::platform {
namespace {

constexpr const wchar_t* kClasses = L"Software\\Classes";
constexpr const wchar_t* kProgId  = L"XPCog.AudioFile";

/// This process's own image path, in native form.
[[nodiscard]] std::wstring executablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            return buffer;
        }
        // Truncated: GetModuleFileNameW fills the buffer and does not say how
        // much it wanted, so the only way forward is to ask again with more.
        buffer.resize(buffer.size() * 2);
    }
}

/// `Applications\<basename>` -- keyed by the executable's file name, which is
/// what the shell looks up, not by its path.
[[nodiscard]] std::wstring applicationsKey() {
    const std::filesystem::path exe{executablePath()};
    return std::wstring{L"Applications\\"} + exe.filename().wstring();
}

/// `"C:\path\XPCog.exe" "%1"`.
///
/// Built by concatenation, and the `%1` is a literal the shell substitutes -- any
/// formatting helper applied here would consume it and produce a command line
/// that passes the executable to itself.
[[nodiscard]] std::wstring openCommand() {
    return L"\"" + executablePath() + L"\" \"%1\"";
}

/// Creates `Software\Classes\<subPath>` and writes `valueName` (empty for the
/// key's default value). Returns false on the first refusal.
bool writeValue(const std::wstring& subPath, const wchar_t* valueName,
                const std::wstring& value) {
    const std::wstring full = std::wstring{kClasses} + L"\\" + subPath;

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, full.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }

    const LSTATUS status =
        RegSetValueExW(key, valueName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

/// The value *names* under `Software\Classes\<subPath>`, which is how both of the
/// lists here are expressed: the presence of the name is the whole content.
[[nodiscard]] std::vector<std::wstring> valueNames(const std::wstring& subPath) {
    const std::wstring full = std::wstring{kClasses} + L"\\" + subPath;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, full.c_str(), 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return {};
    }

    std::vector<std::wstring> names;
    // 16383 is the documented maximum length of a registry value name.
    std::wstring buffer(16384, L'\0');
    for (DWORD index = 0;; ++index) {
        DWORD length = static_cast<DWORD>(buffer.size());
        const LSTATUS status =
            RegEnumValueW(key, index, buffer.data(), &length, nullptr, nullptr, nullptr, nullptr);
        if (status != ERROR_SUCCESS) {
            break;
        }
        names.emplace_back(buffer.data(), length);
    }
    RegCloseKey(key);
    return names;
}

void deleteValue(const std::wstring& subPath, const std::wstring& valueName) {
    const std::wstring full = std::wstring{kClasses} + L"\\" + subPath;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, full.c_str(), 0, KEY_WRITE, &key) !=
        ERROR_SUCCESS) {
        return;
    }
    RegDeleteValueW(key, valueName.c_str());
    RegCloseKey(key);
}

void deleteTree(const std::wstring& subPath) {
    const std::wstring full = std::wstring{kClasses} + L"\\" + subPath;
    // Absent is the desired end state, so "no such key" is a success.
    RegDeleteTreeW(HKEY_CURRENT_USER, full.c_str());
    RegDeleteKeyExW(HKEY_CURRENT_USER, full.c_str(), 0, 0);
}

void fail(std::string* error) {
    if (error != nullptr) {
        *error = "the registry refused the change";
    }
}

/// Tells the shell the association table moved. Without it Explorer keeps showing
/// the old "Open with" list until it is restarted, which reads as the
/// registration having silently failed.
void notifyShell() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
}

}  // namespace

bool fileAssociationsSupported() { return true; }

bool registerFileAssociations(std::span<const std::string> extensions, std::string* error) {
    const std::wstring exe     = executablePath();
    const std::wstring appKey  = applicationsKey();
    const std::wstring command = openCommand();
    const std::wstring progId{kProgId};

    if (exe.empty()) {
        if (error != nullptr) {
            *error = "this program could not determine its own path";
        }
        return false;
    }

    bool ok = true;
    ok = writeValue(progId, nullptr, L"Audio File") && ok;
    ok = writeValue(progId + L"\\DefaultIcon", nullptr, exe + L",0") && ok;
    ok = writeValue(progId + L"\\shell\\open\\command", nullptr, command) && ok;

    // FriendlyAppName is what "Open with" shows instead of the raw image name.
    // Worth noting for a different reason too: the SMTC card's "Unknown app"
    // caption comes from app identity rather than from this, but this is what
    // AssocQueryString(ASSOCSTR_FRIENDLYAPPNAME) answers with -- so having set it,
    // that caption is worth re-checking rather than assumed unchanged.
    ok = writeValue(appKey, L"FriendlyAppName", L"XPCog") && ok;
    ok = writeValue(appKey + L"\\shell\\open\\command", nullptr, command) && ok;

    for (const std::string& extension : extensions) {
        const std::wstring dotted = L"." + toWide(extension);
        ok = writeValue(appKey + L"\\SupportedTypes", dotted.c_str(), L"") && ok;
        ok = writeValue(dotted + L"\\OpenWithProgids", kProgId, L"") && ok;
    }

    if (!ok) {
        fail(error);
        return false;
    }
    notifyShell();
    return true;
}

bool unregisterFileAssociations(std::string* error) {
    (void)error;

    const std::wstring appKey = applicationsKey();
    const std::wstring progId{kProgId};

    // The extension list is read back out of SupportedTypes rather than taken from
    // the caller. That way this removes exactly what was registered, even if the
    // set of built-in codecs has changed since -- an extension added by a later
    // build would otherwise be missed, and one removed would be left behind.
    for (const std::wstring& dotted : valueNames(appKey + L"\\SupportedTypes")) {
        deleteValue(dotted + L"\\OpenWithProgids", progId);
    }

    deleteTree(progId);
    deleteTree(appKey);

    notifyShell();
    return true;
}

}  // namespace xpcog::platform
