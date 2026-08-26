// macOS: NSWorkspace and NSFileManager, both of which do exactly what is asked
// for with no arrangement around them.
//
// -activateFileViewerSelectingURLs: is the modern spelling of what Cog calls
// (-selectFile:inFileViewerRootedAtPath:, PlaylistController.m:1849). It takes a
// list, so a multiple selection could be revealed in one window; the caller
// reveals one file, as Cog does, because a Finder window per track is not what
// anybody means by "show me where this is".
//
// -trashItemAtURL:resultingItemURL:error: is the whole of the trash on this
// platform, including the volume's own .Trashes and the undo record the Finder
// reads. There is no fallback path to write.

#include "xpcog/platform/FileManager.hpp"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

namespace xpcog::platform {
namespace {

/// The path as an NSURL, or nil when it cannot be one.
NSURL* fileUrl(const std::filesystem::path& path) {
    if (path.empty()) {
        return nil;
    }
    NSString* text = [NSString stringWithUTF8String:path.c_str()];
    return text != nil ? [NSURL fileURLWithPath:text] : nil;
}

}  // namespace

bool revealInFileManager(const std::filesystem::path& path) {
    @autoreleasepool {
        NSURL* url = fileUrl(path);
        if (url == nil) {
            return false;
        }
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ url ]];
        return true;
    }
}

bool moveToTrash(const std::filesystem::path& path) {
    @autoreleasepool {
        NSURL* url = fileUrl(path);
        if (url == nil) {
            return false;
        }
        NSError* error = nil;
        return [[NSFileManager defaultManager] trashItemAtURL:url
                                             resultingItemURL:nil
                                                        error:&error] == YES;
    }
}

}  // namespace xpcog::platform
