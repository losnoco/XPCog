// Windows: the shell, twice.
//
// **Revealing** is SHOpenFolderAndSelectItems, which needs the file as a PIDL
// rather than as text -- so the path goes through SHParseDisplayName first.
// `explorer /select,"..."` would be shorter and is what most applications do; it
// starts a process, quotes badly around commas and does nothing at all when the
// path contains one. This is the API that command line is a wrapper for.
//
// **Trashing** is SHFileOperation with FOF_ALLOWUNDO, which is what "move to the
// Recycle Bin" means here: without that flag the same call deletes. The newer
// IFileOperation is the documented replacement and would be a COM object, an
// apartment and a sink to write for a call that has exactly one operand; the
// older function is still supported and is honest about what it does.
//
// Both need a COM apartment on the calling thread. wxMSW calls OleInitialize
// during wxApp startup, so the interface thread already has one -- which is the
// only thread either of these is called from.

#include "xpcog/platform/FileManager.hpp"

#include <windows.h>

#include <shlobj.h>

#include <string>

namespace xpcog::platform {

bool revealInFileManager(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }

    PIDLIST_ABSOLUTE item = nullptr;
    if (FAILED(SHParseDisplayName(path.native().c_str(), nullptr, &item, 0, nullptr)) ||
        item == nullptr) {
        return false;
    }

    const HRESULT opened = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    CoTaskMemFree(item);
    return SUCCEEDED(opened);
}

bool moveToTrash(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }

    // pFrom is a *list*, terminated by a second null. A std::wstring already
    // supplies one of those, so one is appended by hand and c_str() answers with
    // both.
    std::wstring from = path.native();
    from.push_back(L'\0');

    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    // ALLOWUNDO is the Recycle Bin. The rest suppress the shell's own dialogs:
    // the caller has already asked, and a second confirmation from a different
    // process is how a command ends up looking broken.
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    return SHFileOperationW(&operation) == 0 && operation.fAnyOperationsAborted == FALSE;
}

}  // namespace xpcog::platform
