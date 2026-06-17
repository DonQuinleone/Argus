// Native macOS menu bar built with AppKit. GLFW already creates the NSApplication;
// here we attach a proper menu so the app shows "Argus" and its items
// in the system menu bar instead of an in-window ImGui menu.
#import "MacMenu.h"

#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>

// Objective-C target that forwards menu actions to the C++ callbacks.
@interface ArgusMenuTarget : NSObject {
@public
    argus::MacMenuCallbacks cb;
}
- (void)openFile:(id)sender;
- (void)openFolder:(id)sender;
- (void)exportPdf:(id)sender;
- (void)exportCsv:(id)sender;
- (void)exportJson:(id)sender;
- (void)exportSpecPng:(id)sender;
- (void)exportAll:(id)sender;
- (void)exportBatchPdf:(id)sender;
- (void)toggleSettings:(id)sender;
- (void)toggleReportInfo:(id)sender;
- (void)toggleProfiles:(id)sender;
- (void)toggleTheme:(id)sender;
- (void)quit:(id)sender;
@end

@implementation ArgusMenuTarget
- (void)openFile:(id)sender { if (cb.openFile) cb.openFile(); }
- (void)openFolder:(id)sender { if (cb.openFolder) cb.openFolder(); }
- (void)exportPdf:(id)sender { if (cb.exportPdf) cb.exportPdf(); }
- (void)exportCsv:(id)sender { if (cb.exportCsv) cb.exportCsv(); }
- (void)exportJson:(id)sender { if (cb.exportJson) cb.exportJson(); }
- (void)exportSpecPng:(id)sender { if (cb.exportSpecPng) cb.exportSpecPng(); }
- (void)exportAll:(id)sender { if (cb.exportAll) cb.exportAll(); }
- (void)exportBatchPdf:(id)sender { if (cb.exportBatchPdf) cb.exportBatchPdf(); }
- (void)toggleSettings:(id)sender { if (cb.toggleSettings) cb.toggleSettings(); }
- (void)toggleReportInfo:(id)sender { if (cb.toggleReportInfo) cb.toggleReportInfo(); }
- (void)toggleProfiles:(id)sender { if (cb.toggleProfiles) cb.toggleProfiles(); }
- (void)toggleTheme:(id)sender { if (cb.toggleTheme) cb.toggleTheme(); }
- (void)quit:(id)sender {
    if (cb.quit)
        cb.quit();  // clean shutdown via the GLFW loop (saves state)
    else
        [NSApp terminate:sender];
}
@end

namespace argus {

std::string macResourcePath(const std::string& name) {
    @autoreleasepool {
        NSString* base = name.empty() ? @"" : @(name.c_str());
        NSString* stem = [base stringByDeletingPathExtension];
        NSString* ext = [base pathExtension];
        NSString* path = [[NSBundle mainBundle] pathForResource:stem ofType:ext];
        return path ? std::string([path UTF8String]) : std::string();
    }
}

// allowedFileTypes is deprecated (macOS 12+) in favour of allowedContentTypes/UTType, but
// it still works and avoids a UniformTypeIdentifiers dependency for our simple extensions.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static NSArray<NSString*>* toExtArray(const std::vector<std::string>& exts) {
    if (exts.empty()) return nil;
    NSMutableArray<NSString*>* a = [NSMutableArray array];
    for (const auto& e : exts) [a addObject:@(e.c_str())];
    return a;
}

std::vector<std::string> macOpenFiles(const std::string& title,
                                      const std::vector<std::string>& exts, bool multiple) {
    @autoreleasepool {
        NSOpenPanel* p = [NSOpenPanel openPanel];
        [p setCanChooseFiles:YES];
        [p setCanChooseDirectories:NO];
        [p setAllowsMultipleSelection:(multiple ? YES : NO)];
        if (!title.empty()) [p setMessage:@(title.c_str())];
        NSArray<NSString*>* types = toExtArray(exts);
        if (types) [p setAllowedFileTypes:types];
        [NSApp activateIgnoringOtherApps:YES];
        std::vector<std::string> out;
        if ([p runModal] == NSModalResponseOK)
            for (NSURL* u in [p URLs]) out.push_back(std::string([[u path] UTF8String]));
        return out;
    }
}

std::string macOpenFolder(const std::string& title, const std::string& defaultDir) {
    @autoreleasepool {
        NSOpenPanel* p = [NSOpenPanel openPanel];
        [p setCanChooseFiles:NO];
        [p setCanChooseDirectories:YES];
        [p setAllowsMultipleSelection:NO];
        if (!title.empty()) [p setMessage:@(title.c_str())];
        if (!defaultDir.empty())
            [p setDirectoryURL:[NSURL fileURLWithPath:@(defaultDir.c_str()) isDirectory:YES]];
        [NSApp activateIgnoringOtherApps:YES];
        if ([p runModal] == NSModalResponseOK) return std::string([[[p URL] path] UTF8String]);
        return std::string();
    }
}

std::string macSaveFile(const std::string& title, const std::string& defaultPath,
                        const std::string& ext) {
    @autoreleasepool {
        NSSavePanel* p = [NSSavePanel savePanel];
        if (!title.empty()) [p setMessage:@(title.c_str())];
        NSString* def = @(defaultPath.c_str());
        NSString* fname = [def lastPathComponent];
        NSString* dir = [def stringByDeletingLastPathComponent];
        if ([fname length]) [p setNameFieldStringValue:fname];
        if ([dir length])
            [p setDirectoryURL:[NSURL fileURLWithPath:dir isDirectory:YES]];
        if (!ext.empty()) [p setAllowedFileTypes:@[ @(ext.c_str()) ]];
        [NSApp activateIgnoringOtherApps:YES];
        if ([p runModal] == NSModalResponseOK) return std::string([[[p URL] path] UTF8String]);
        return std::string();
    }
}

#pragma clang diagnostic pop

// Kept alive for the lifetime of the process (menu retains a raw pointer to it).
static ArgusMenuTarget* g_target = nil;

static NSMenuItem* addItem(NSMenu* menu, NSString* title, SEL action, NSString* key,
                           id target) {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    if (target) [item setTarget:target];
    [menu addItem:item];
    return item;
}

void installMacMenu(const MacMenuCallbacks& cb) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        g_target = [[ArgusMenuTarget alloc] init];
        g_target->cb = cb;

        NSString* appName = @"Argus";
        NSMenu* bar = [[NSMenu alloc] init];

        // Application menu.
        NSMenuItem* appBarItem = [[NSMenuItem alloc] init];
        [bar addItem:appBarItem];
        NSMenu* appMenu = [[NSMenu alloc] init];
        addItem(appMenu, [@"About " stringByAppendingString:appName],
                @selector(orderFrontStandardAboutPanel:), @"", nil);
        [appMenu addItem:[NSMenuItem separatorItem]];
        addItem(appMenu, [@"Hide " stringByAppendingString:appName], @selector(hide:), @"h", nil);
        NSMenuItem* hideOthers =
            addItem(appMenu, @"Hide Others", @selector(hideOtherApplications:), @"h", nil);
        [hideOthers setKeyEquivalentModifierMask:(NSEventModifierFlagOption |
                                                  NSEventModifierFlagCommand)];
        addItem(appMenu, @"Show All", @selector(unhideAllApplications:), @"", nil);
        [appMenu addItem:[NSMenuItem separatorItem]];
        addItem(appMenu, [@"Quit " stringByAppendingString:appName], @selector(quit:), @"q",
                g_target);
        [appBarItem setSubmenu:appMenu];

        // No File/View menus: those actions live in the app's own UI now. Only the standard
        // macOS Application + Window menus remain (the OS always shows a menu bar for the
        // focused app, and Quit/Hide/Minimise are expected there).

        // Window menu (standard).
        NSMenuItem* winBarItem = [[NSMenuItem alloc] init];
        [bar addItem:winBarItem];
        NSMenu* winMenu = [[NSMenu alloc] initWithTitle:@"Window"];
        addItem(winMenu, @"Minimize", @selector(performMiniaturize:), @"m", nil);
        addItem(winMenu, @"Zoom", @selector(performZoom:), @"", nil);
        [winBarItem setSubmenu:winMenu];
        [NSApp setWindowsMenu:winMenu];

        [NSApp setMainMenu:bar];
    }
}

}  // namespace argus
#endif
