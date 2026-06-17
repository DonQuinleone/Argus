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
- (void)exportAll:(id)sender;
- (void)exportBatchPdf:(id)sender;
- (void)toggleSettings:(id)sender;
- (void)toggleReportInfo:(id)sender;
- (void)toggleTheme:(id)sender;
- (void)quit:(id)sender;
@end

@implementation ArgusMenuTarget
- (void)openFile:(id)sender { if (cb.openFile) cb.openFile(); }
- (void)openFolder:(id)sender { if (cb.openFolder) cb.openFolder(); }
- (void)exportPdf:(id)sender { if (cb.exportPdf) cb.exportPdf(); }
- (void)exportCsv:(id)sender { if (cb.exportCsv) cb.exportCsv(); }
- (void)exportJson:(id)sender { if (cb.exportJson) cb.exportJson(); }
- (void)exportAll:(id)sender { if (cb.exportAll) cb.exportAll(); }
- (void)exportBatchPdf:(id)sender { if (cb.exportBatchPdf) cb.exportBatchPdf(); }
- (void)toggleSettings:(id)sender { if (cb.toggleSettings) cb.toggleSettings(); }
- (void)toggleReportInfo:(id)sender { if (cb.toggleReportInfo) cb.toggleReportInfo(); }
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

        // File menu.
        NSMenuItem* fileBarItem = [[NSMenuItem alloc] init];
        [bar addItem:fileBarItem];
        NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
        addItem(fileMenu, @"Open File...", @selector(openFile:), @"o", g_target);
        addItem(fileMenu, @"Open Folder...", @selector(openFolder:), @"O", g_target);
        [fileMenu addItem:[NSMenuItem separatorItem]];
        addItem(fileMenu, @"Export PDF...", @selector(exportPdf:), @"e", g_target);
        addItem(fileMenu, @"Export CSV...", @selector(exportCsv:), @"", g_target);
        addItem(fileMenu, @"Export JSON...", @selector(exportJson:), @"", g_target);
        [fileMenu addItem:[NSMenuItem separatorItem]];
        addItem(fileMenu, @"Export All Reports (PDF)...", @selector(exportAll:), @"", g_target);
        addItem(fileMenu, @"Export Combined Batch PDF...", @selector(exportBatchPdf:), @"", g_target);
        [fileBarItem setSubmenu:fileMenu];

        // View menu.
        NSMenuItem* viewBarItem = [[NSMenuItem alloc] init];
        [bar addItem:viewBarItem];
        NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
        addItem(viewMenu, @"Spectrogram Settings", @selector(toggleSettings:), @"", g_target);
        addItem(viewMenu, @"QA Report Details", @selector(toggleReportInfo:), @"", g_target);
        addItem(viewMenu, @"Toggle Light / Dark Theme", @selector(toggleTheme:), @"", g_target);
        [viewBarItem setSubmenu:viewMenu];

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
