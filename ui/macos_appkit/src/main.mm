// main.mm — điểm vào app AppKit (Phase 2). Dựng NSApplication + menu + cửa sổ chính.
#import <Cocoa/Cocoa.h>

#import "AppController.h"

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        AppController *controller = [AppController new];
        app.delegate = controller;

        // Menu tối thiểu (đủ phím tắt Cmd+S / Cmd+Enter / Cmd+Q).
        NSMenu *mainMenu = [NSMenu new];
        NSMenuItem *appItem = [NSMenuItem new];
        [mainMenu addItem:appItem];
        NSMenu *appMenu = [NSMenu new];
        [appMenu addItemWithTitle:@"Quit deed" action:@selector(terminate:) keyEquivalent:@"q"];
        appItem.submenu = appMenu;

        NSMenuItem *fileItem = [NSMenuItem new];
        [mainMenu addItem:fileItem];
        NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
        [[fileMenu addItemWithTitle:@"Open Folder…" action:@selector(openFolder:) keyEquivalent:@"o"] setTarget:controller];
        [[fileMenu addItemWithTitle:@"Save" action:@selector(saveRequest:) keyEquivalent:@"s"] setTarget:controller];
        NSMenuItem *send = [fileMenu addItemWithTitle:@"Send" action:@selector(sendRequest:) keyEquivalent:@"\r"];
        [send setTarget:controller];
        fileItem.submenu = fileMenu;

        // Edit menu: cần thiết để Cmd+C/X/V/A/Z định tuyến tới first responder
        // (NSTextView) -> bật copy/paste với dữ liệu từ NGOÀI app. target=nil = responder chain.
        NSMenuItem *editItem = [NSMenuItem new];
        [mainMenu addItem:editItem];
        NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
        [editMenu addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
        [editMenu addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"Z"];
        [editMenu addItem:[NSMenuItem separatorItem]];
        [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
        [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
        [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
        [editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
        editItem.submenu = editMenu;

        // Window menu: minimize / zoom (close đã có trên title bar tùy biến).
        NSMenuItem *winItem = [NSMenuItem new];
        [mainMenu addItem:winItem];
        NSMenu *winMenu = [[NSMenu alloc] initWithTitle:@"Window"];
        [winMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
        [winMenu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
        [winMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
        winItem.submenu = winMenu;
        app.windowsMenu = winMenu;

        app.mainMenu = mainMenu;

        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
