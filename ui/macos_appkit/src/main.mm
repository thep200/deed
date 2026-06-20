// main.mm — điểm vào app AppKit (Phase 2). Dựng NSApplication + menu + cửa sổ chính.
#import <Cocoa/Cocoa.h>

#import "app/AppController.h"
#import "app/AppStrings.h"

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
        [appMenu addItemWithTitle:StrMenuQuit action:@selector(terminate:) keyEquivalent:@"q"];
        appItem.submenu = appMenu;

        NSMenuItem *fileItem = [NSMenuItem new];
        [mainMenu addItem:fileItem];
        NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:StrMenuFile];
        [[fileMenu addItemWithTitle:StrOpenFolder action:@selector(openFolder:) keyEquivalent:@"o"] setTarget:controller];
        [[fileMenu addItemWithTitle:StrSave action:@selector(saveRequest:) keyEquivalent:@"s"] setTarget:controller];
        NSMenuItem *send = [fileMenu addItemWithTitle:StrMenuSend action:@selector(sendRequest:) keyEquivalent:@"\r"];
        [send setTarget:controller];
        fileItem.submenu = fileMenu;

        // Edit menu: cần thiết để Cmd+C/X/V/A/Z định tuyến tới first responder
        // (NSTextView) -> bật copy/paste với dữ liệu từ NGOÀI app. target=nil = responder chain.
        NSMenuItem *editItem = [NSMenuItem new];
        [mainMenu addItem:editItem];
        NSMenu *editMenu = [[NSMenu alloc] initWithTitle:StrMenuEdit];
        [editMenu addItemWithTitle:StrMenuUndo action:@selector(undo:) keyEquivalent:@"z"];
        [editMenu addItemWithTitle:StrMenuRedo action:@selector(redo:) keyEquivalent:@"Z"];
        [editMenu addItem:[NSMenuItem separatorItem]];
        [editMenu addItemWithTitle:StrMenuCut action:@selector(cut:) keyEquivalent:@"x"];
        [editMenu addItemWithTitle:StrMenuCopy action:@selector(copy:) keyEquivalent:@"c"];
        [editMenu addItemWithTitle:StrMenuPaste action:@selector(paste:) keyEquivalent:@"v"];
        [editMenu addItemWithTitle:StrMenuSelectAll action:@selector(selectAll:) keyEquivalent:@"a"];
        editItem.submenu = editMenu;

        // Window menu: minimize / zoom (close đã có trên title bar tùy biến).
        NSMenuItem *winItem = [NSMenuItem new];
        [mainMenu addItem:winItem];
        NSMenu *winMenu = [[NSMenu alloc] initWithTitle:StrMenuWindow];
        [winMenu addItemWithTitle:StrMenuMinimize action:@selector(performMiniaturize:) keyEquivalent:@"m"];
        [winMenu addItemWithTitle:StrMenuZoom action:@selector(performZoom:) keyEquivalent:@""];
        [winMenu addItemWithTitle:StrMenuClose action:@selector(performClose:) keyEquivalent:@"w"];
        winItem.submenu = winMenu;
        app.windowsMenu = winMenu;

        app.mainMenu = mainMenu;

        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
