#import "dialogs/OS9Dialog.h"

#import "app/OS9Lifecycle.h"
#import "theme/OS9Theme.h"
#import "widgets/OS9BevelButton.h"

// --- Metrics (CUSTOM_DIALOG §8) ---
static const CGFloat kPad = 16;
static const CGFloat kBtnGap = 8;
static const CGFloat kBtnMinW = 70;
static const CGFloat kBtnH = 22;
static const CGFloat kMinW = 300;
static const CGFloat kMaxW = 460;
static const CGFloat kTitleH = 20;     // pinstripe title bar (movable)
static const CGFloat kIconW = 40;      // cột icon cảnh báo
static const CGFloat kFieldH = 22;

#pragma mark - OS9DialogWindow (borderless, nhận phím được)

@interface OS9DialogWindow : NSWindow
@property(nonatomic, copy) void (^onReturn)(void);   // Enter -> nút mặc định
@property(nonatomic, copy) void (^onEscape)(void);   // Esc/Cmd-. -> cancel
@end

@implementation OS9DialogWindow
- (BOOL)canBecomeKeyWindow { return YES; }    // bắt buộc cho borderless để nhận phím/focus field
- (BOOL)canBecomeMainWindow { return YES; }
- (void)keyDown:(NSEvent *)e {
    NSString *chars = e.charactersIgnoringModifiers;
    unichar c = chars.length ? [chars characterAtIndex:0] : 0;
    if (c == NSCarriageReturnCharacter || c == NSEnterCharacter) { if (_onReturn) _onReturn(); return; }
    if (c == 27) { if (_onEscape) _onEscape(); return; }
    if ((e.modifierFlags & NSEventModifierFlagCommand) && c == '.') { if (_onEscape) _onEscape(); return; }
    [super keyDown:e];
}
- (void)cancelOperation:(id)sender { if (_onEscape) _onEscape(); }   // Esc qua responder chain
@end

#pragma mark - OS9DialogView (vẽ Platinum + kéo title)

@interface OS9DialogView : NSView
@property(nonatomic) BOOL movable;            // có title bar pinstripe -> kéo di chuyển
@property(nonatomic, copy) NSString *titleText;
@property(nonatomic) OS9AlertIcon icon;
@end

@implementation OS9DialogView
- (BOOL)isFlipped { return YES; }             // layout từ trên xuống cho dễ

- (void)drawRect:(NSRect)dirty {
    NSRect b = self.bounds;
    // Nền Platinum + viền 1px + bevel (sáng trên-trái / tối dưới-phải). KHÔNG bóng mềm.
    [[OS9Theme windowBg] set];
    NSRectFill(b);
    [[OS9Theme highlight] set];
    NSRectFill(NSMakeRect(0, 0, b.size.width, 1));
    NSRectFill(NSMakeRect(0, 0, 1, b.size.height));
    [[OS9Theme shadow] set];
    NSRectFill(NSMakeRect(0, b.size.height - 1, b.size.width, 1));
    NSRectFill(NSMakeRect(b.size.width - 1, 0, 1, b.size.height));
    [[OS9Theme frame] set];
    NSFrameRect(b);

    if (_movable) {   // title bar kẻ sọc Platinum
        NSRect tr = NSMakeRect(1, 1, b.size.width - 2, kTitleH);
        [OS9Theme drawStripedTitleInRect:tr stripesInRect:tr active:YES];
        if (_titleText.length) {
            NSDictionary *a = @{NSFontAttributeName : [OS9Theme uiFont],
                                NSForegroundColorAttributeName : [NSColor blackColor]};
            NSSize sz = [_titleText sizeWithAttributes:a];
            // nền nhỏ sau chữ để tách khỏi sọc (đọc rõ)
            NSRect lblBg = NSMakeRect((b.size.width - sz.width) / 2 - 6, 2, sz.width + 12, kTitleH - 3);
            [[OS9Theme windowBg] set]; NSRectFill(lblBg);
            [_titleText drawAtPoint:NSMakePoint((b.size.width - sz.width) / 2,
                                                (kTitleH - sz.height) / 2 + 1) withAttributes:a];
        }
    }

    if (_icon != OS9AlertNone) [self drawAlertIconAt:NSMakePoint(kPad, (_movable ? kTitleH : 0) + kPad)];
}

// Icon cảnh báo tự vẽ đơn giản (note=ⓘ, caution=⚠, stop=⊘). Đặt trong cột rộng kIconW.
- (void)drawAlertIconAt:(NSPoint)p {
    NSRect r = NSMakeRect(p.x, p.y, 28, 28);
    NSColor *ink = [NSColor blackColor];
    if (_icon == OS9AlertCaution) {
        NSBezierPath *tri = [NSBezierPath bezierPath];
        [tri moveToPoint:NSMakePoint(NSMidX(r), r.origin.y)];
        [tri lineToPoint:NSMakePoint(NSMaxX(r), NSMaxY(r))];
        [tri lineToPoint:NSMakePoint(r.origin.x, NSMaxY(r))];
        [tri closePath];
        [[NSColor colorWithCalibratedRed:0.95 green:0.82 blue:0.20 alpha:1] set]; [tri fill];
        [ink set]; tri.lineWidth = 1.5; [tri stroke];
    } else if (_icon == OS9AlertStop) {
        NSBezierPath *c = [NSBezierPath bezierPathWithOvalInRect:r];
        [[NSColor colorWithCalibratedRed:0.75 green:0.15 blue:0.15 alpha:1] set]; [c fill];
        [[NSColor whiteColor] set]; NSRectFill(NSMakeRect(r.origin.x + 5, NSMidY(r) - 2, 18, 4));
    } else {  // note
        NSBezierPath *c = [NSBezierPath bezierPathWithOvalInRect:r];
        [[NSColor colorWithCalibratedRed:0.25 green:0.45 blue:0.85 alpha:1] set]; [c fill];
        NSDictionary *a = @{NSFontAttributeName : [NSFont boldSystemFontOfSize:18],
                            NSForegroundColorAttributeName : [NSColor whiteColor]};
        [@"i" drawAtPoint:NSMakePoint(NSMidX(r) - 2, r.origin.y + 4) withAttributes:a];
    }
    (void)ink;
}

// Kéo title bar -> di chuyển cửa sổ (movable-modal).
- (void)mouseDown:(NSEvent *)e {
    if (!_movable) return;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if (p.y <= kTitleH + 1) {     // trong vùng title bar (flipped: y nhỏ = trên)
        [self.window performWindowDragWithEvent:e];
    }
}
@end

#pragma mark - OS9DialogController (driver modal + layout)

@interface OS9DialogController : NSObject <NSTextFieldDelegate>
@end

@implementation OS9DialogController {
    OS9DialogWindow *_win;
    OS9DialogView *_view;
    NSTextField *_field;        // chỉ PROMPT
    NSTextField *_errorLabel;   // chỉ PROMPT (dialogErrorText)
    NSArray<OS9BevelButton *> *_buttons;
    NSInteger _defaultIdx, _cancelIdx;
    NSInteger _result;
    BOOL _isPrompt;
    NSString *(^_validate)(NSString *);
}

- (NSColor *)errorColor { return [NSColor colorWithCalibratedRed:0.6 green:0.0 blue:0.0 alpha:1.0]; }

// Đo bề rộng chữ message (wrap trong maxW) -> tính kích thước dialog.
- (NSRect)measureMessage:(NSString *)msg width:(CGFloat)maxTextW {
    if (!msg.length) return NSZeroRect;
    NSDictionary *a = @{NSFontAttributeName : [OS9Theme uiFont]};
    return [msg boundingRectWithSize:NSMakeSize(maxTextW, 10000)
                             options:NSStringDrawingUsesLineFragmentOrigin attributes:a];
}

- (OS9BevelButton *)makeButton:(NSString *)title index:(NSInteger)idx {
    OS9BevelButton *b = [[OS9BevelButton alloc] initWithTitle:title target:self
                                                       action:@selector(buttonClicked:)];
    b.tag = idx;
    b.isDefault = (idx == _defaultIdx);
    return b;
}

- (void)buttonClicked:(OS9BevelButton *)b { [self resolveIndex:b.tag]; }
- (void)fireDefault { [self resolveIndex:_defaultIdx]; }
- (void)fireCancel { [self resolveIndex:_cancelIdx]; }

// Quyết định đóng dialog: prompt + nút mặc định -> validate trước; fail -> giữ dialog + báo lỗi.
- (void)resolveIndex:(NSInteger)idx {
    if (_isPrompt && idx == _defaultIdx) {
        NSString *input = _field.stringValue;
        if (_validate) {
            NSString *err = _validate(input);
            if (err.length) {
                _errorLabel.stringValue = err;
                _errorLabel.hidden = NO;
                NSBeep();
                [_win makeFirstResponder:_field];
                return;   // KHÔNG đóng
            }
        }
    }
    _result = idx;
    [NSApp stopModalWithCode:idx];
}

// Đặt cửa sổ canh giữa parent (hoặc màn hình chứa parent / main screen).
- (void)positionRelativeTo:(NSWindow *)parent size:(NSSize)sz {
    NSRect area;
    if (parent) area = parent.frame;
    else { NSScreen *s = [NSScreen mainScreen]; area = s.visibleFrame; }
    NSScreen *scr = parent ? (parent.screen ?: [NSScreen mainScreen]) : [NSScreen mainScreen];
    (void)scr;
    NSPoint origin = NSMakePoint(NSMidX(area) - sz.width / 2, NSMidY(area) - sz.height / 2);
    [_win setFrame:NSMakeRect(origin.x, origin.y, sz.width, sz.height) display:YES];
}

- (NSInteger)runConfirmTitle:(NSString *)title message:(NSString *)message
                     buttons:(NSArray<NSString *> *)titles
               defaultButton:(NSInteger)def cancelButton:(NSInteger)cancel
                        icon:(OS9AlertIcon)icon parent:(NSWindow *)parent {
    _isPrompt = NO;
    _defaultIdx = def; _cancelIdx = cancel; _result = cancel;
    BOOL movable = (title.length > 0);
    CGFloat iconW = (icon != OS9AlertNone) ? kIconW : 0;
    CGFloat topInset = (movable ? kTitleH : 0) + kPad;

    NSRect tb = [self measureMessage:message width:(kMaxW - 2 * kPad - iconW)];
    CGFloat textW = ceil(tb.size.width), textH = MAX(ceil(tb.size.height), iconW ? 28 : 16);

    // bề rộng theo nút.
    NSMutableArray<OS9BevelButton *> *btns = [NSMutableArray array];
    CGFloat btnsTotal = 0;
    for (NSInteger i = 0; i < (NSInteger)titles.count; i++) {
        OS9BevelButton *b = [self makeButton:titles[i] index:i];
        NSSize ts = [titles[i] sizeWithAttributes:@{NSFontAttributeName : [OS9Theme uiFont]}];
        CGFloat w = MAX(kBtnMinW, ceil(ts.width) + 24);
        b.frame = NSMakeRect(0, 0, w, kBtnH);
        [btns addObject:b];
        btnsTotal += w + (i ? kBtnGap : 0);
    }
    _buttons = btns;

    CGFloat contentW = MAX(MAX(kMinW, textW + 2 * kPad + iconW), btnsTotal + 2 * kPad);
    contentW = MIN(contentW, MAX(kMaxW, btnsTotal + 2 * kPad));
    CGFloat contentH = topInset + textH + kPad + kBtnH + kPad;

    [self buildWindowWidth:contentW height:contentH movable:movable title:title icon:icon];

    // message label
    NSTextField *lbl = [self labelWithText:message
                                     frame:NSMakeRect(kPad + iconW, topInset, contentW - 2 * kPad - iconW, textH)];
    [_view addSubview:lbl];

    [self layoutButtons:btns inWidth:contentW bottomY:contentH - kPad - kBtnH];

    [self positionRelativeTo:parent size:NSMakeSize(contentW, contentH)];
    return [self present:parent firstResponder:nil];
}

- (NSString *)runPromptTitle:(NSString *)title message:(NSString *)message
                 defaultText:(NSString *)text placeholder:(NSString *)placeholder
                    okButton:(NSString *)ok cancelButton:(NSString *)cancel
                    validate:(NSString *(^)(NSString *))validate parent:(NSWindow *)parent {
    _isPrompt = YES;
    _validate = [validate copy];
    _defaultIdx = 1; _cancelIdx = 0; _result = 0;   // [Cancel, OK]
    CGFloat topInset = kTitleH + kPad;

    CGFloat contentW = 360;
    CGFloat y = topInset;
    NSRect mb = message.length ? [self measureMessage:message width:contentW - 2 * kPad] : NSZeroRect;
    CGFloat msgH = message.length ? MAX(16, ceil(mb.size.height)) : 0;

    CGFloat contentH = topInset + (msgH ? msgH + kBtnGap : 0) + kFieldH + 4 + 16 /*err*/ + kPad + kBtnH + kPad;
    [self buildWindowWidth:contentW height:contentH movable:YES title:title icon:OS9AlertNone];

    if (msgH) {
        [_view addSubview:[self labelWithText:message
                                        frame:NSMakeRect(kPad, y, contentW - 2 * kPad, msgH)]];
        y += msgH + kBtnGap;
    }

    // input field (bevel lõm Platinum vẽ sau; field borderless nền trắng).
    _field = [[NSTextField alloc] initWithFrame:NSMakeRect(kPad, y, contentW - 2 * kPad, kFieldH)];
    _field.stringValue = text ?: @"";
    if (placeholder.length) _field.placeholderString = placeholder;
    _field.font = [OS9Theme uiFont];
    _field.bordered = YES;
    _field.bezeled = YES;
    _field.bezelStyle = NSTextFieldSquareBezel;
    _field.focusRingType = NSFocusRingTypeNone;   // tắt focus ring xanh
    _field.drawsBackground = YES;
    _field.delegate = self;
    [_view addSubview:_field];
    y += kFieldH + 4;

    _errorLabel = [self labelWithText:@"" frame:NSMakeRect(kPad, y, contentW - 2 * kPad, 16)];
    _errorLabel.textColor = [self errorColor];
    _errorLabel.font = [NSFont systemFontOfSize:10];
    _errorLabel.hidden = YES;
    [_view addSubview:_errorLabel];

    NSArray<OS9BevelButton *> *btns = @[ [self makeButton:cancel index:0], [self makeButton:ok index:1] ];
    for (NSInteger i = 0; i < 2; i++) {
        NSString *t = (i == 0) ? cancel : ok;
        NSSize ts = [t sizeWithAttributes:@{NSFontAttributeName : [OS9Theme uiFont]}];
        btns[i].frame = NSMakeRect(0, 0, MAX(kBtnMinW, ceil(ts.width) + 24), kBtnH);
    }
    _buttons = btns;
    [self layoutButtons:btns inWidth:contentW bottomY:contentH - kPad - kBtnH];

    [self positionRelativeTo:parent size:NSMakeSize(contentW, contentH)];
    NSInteger code = [self present:parent firstResponder:_field];
    if (code != _defaultIdx) return nil;
    return [_field.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

#pragma mark helpers

- (NSTextField *)labelWithText:(NSString *)text frame:(NSRect)f {
    NSTextField *l = [[NSTextField alloc] initWithFrame:f];
    l.stringValue = text ?: @"";
    l.editable = NO; l.selectable = NO; l.bordered = NO; l.drawsBackground = NO;
    l.font = [OS9Theme uiFont];
    l.textColor = [NSColor blackColor];
    l.cell.wraps = YES; l.cell.lineBreakMode = NSLineBreakByWordWrapping;
    return l;
}

- (void)layoutButtons:(NSArray<OS9BevelButton *> *)btns inWidth:(CGFloat)contentW bottomY:(CGFloat)y {
    CGFloat total = 0;
    for (OS9BevelButton *b in btns) total += b.frame.size.width;
    total += kBtnGap * (btns.count - 1);
    CGFloat x = contentW - kPad - total;       // canh phải-dưới; array order trái->phải
    for (OS9BevelButton *b in btns) {
        b.frame = NSMakeRect(x, y, b.frame.size.width, kBtnH);
        [_view addSubview:b];
        x += b.frame.size.width + kBtnGap;
    }
}

- (void)buildWindowWidth:(CGFloat)w height:(CGFloat)h movable:(BOOL)movable
                   title:(NSString *)title icon:(OS9AlertIcon)icon {
    _win = [[OS9DialogWindow alloc] initWithContentRect:NSMakeRect(0, 0, w, h)
                                              styleMask:NSWindowStyleMaskBorderless
                                                backing:NSBackingStoreBuffered defer:NO];
    _win.opaque = NO;
    _win.hasShadow = YES;
    _win.releasedWhenClosed = NO;   // §2.3: controller giữ strong ref suốt modal; ARC quản vòng đời
    _win.level = NSModalPanelWindowLevel;
    __weak OS9DialogController *ws = self;
    _win.onReturn = ^{ [ws fireDefault]; };
    _win.onEscape = ^{ [ws fireCancel]; };
    _view = [[OS9DialogView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
    _view.movable = movable;
    _view.titleText = title;
    _view.icon = icon;
    _win.contentView = _view;
}

- (NSInteger)present:(NSWindow *)parent firstResponder:(NSView *)fr {
    [_win makeKeyAndOrderFront:nil];
    if (fr) { [_win makeFirstResponder:fr]; if ([fr isKindOfClass:[NSTextField class]]) [(NSTextField *)fr selectText:nil]; }
    NSInteger code = [NSApp runModalForWindow:_win];
    // §2.3: deactivate input context của ô nhập (rename) KHI window còn sống, RỒI mới đóng.
    OS9SafeEndEditing(_win, _field);
    [_win orderOut:nil];
    return code;
}

#pragma mark NSTextFieldDelegate (prompt: Enter=OK, Esc=Cancel)

- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    if (sel == @selector(insertNewline:)) { [self fireDefault]; return YES; }
    if (sel == @selector(cancelOperation:)) { [self fireCancel]; return YES; }
    return NO;
}
- (void)controlTextDidChange:(NSNotification *)n { _errorLabel.hidden = YES; }  // gõ lại -> ẩn lỗi
@end

#pragma mark - OS9Dialog (public API)

@implementation OS9Dialog

+ (NSInteger)confirmWithTitle:(NSString *)title message:(NSString *)message
                      buttons:(NSArray<NSString *> *)buttons
                defaultButton:(NSInteger)defaultIdx cancelButton:(NSInteger)cancelIdx
                         icon:(OS9AlertIcon)icon parent:(NSWindow *)parent {
    OS9DialogController *c = [OS9DialogController new];   // giữ sống suốt modal
    if (icon == OS9AlertStop || icon == OS9AlertCaution) NSBeep();
    return [c runConfirmTitle:title message:message buttons:buttons
                defaultButton:defaultIdx cancelButton:cancelIdx icon:icon parent:parent];
}

+ (NSString *)promptWithTitle:(NSString *)title message:(NSString *)message
                  defaultText:(NSString *)text placeholder:(NSString *)placeholder
                     okButton:(NSString *)ok cancelButton:(NSString *)cancel
                     validate:(NSString *(^)(NSString *))validate parent:(NSWindow *)parent {
    OS9DialogController *c = [OS9DialogController new];
    return [c runPromptTitle:title message:message defaultText:text placeholder:placeholder
                    okButton:ok cancelButton:cancel validate:validate parent:parent];
}
@end
