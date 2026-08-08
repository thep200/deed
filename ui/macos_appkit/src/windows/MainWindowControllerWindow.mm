#import "windows/MainWindowControllerPrivate.h"
#import <QuartzCore/QuartzCore.h>

static const CGFloat kTitleH = 21;        // title bar fixed at 21px tall (incl. border)
static const CGFloat kToastTopGap = 10;   // toast top slot sits BELOW the title bar (clears zoom/hide)

// Build a non-anti-aliased circular alpha mask of side (2*rpx+1) device pixels (SQUARE_CORNERS=2). Used as a
// 9-slice CALayer mask: the 4 quarter-circle corners stay pixel-exact (chunky/retro, no smoothing) while the
// 1px center cross stretches to fill the window on resize. Caller owns the returned image (CGImageRelease).
static CGImageRef OS9CreateCornerMask(int rpx) {
    const int S = 2 * rpx + 1;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(NULL, S, S, 8, 0, cs, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx) return NULL;
    unsigned char *data = (unsigned char *)CGBitmapContextGetData(ctx);
    const size_t bpr = CGBitmapContextGetBytesPerRow(ctx);
    const long r2 = (long)rpx * rpx;
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            const long dx = x - rpx, dy = y - rpx;
            const unsigned char a = (dx * dx + dy * dy) <= r2 ? 255 : 0;   // hard threshold -> pixel corners
            unsigned char *p = data + (size_t)y * bpr + (size_t)x * 4;
            p[0] = a; p[1] = a; p[2] = a; p[3] = a;                        // premultiplied white; mask uses alpha
        }
    }
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    return img;
}

@implementation MainWindowController (WindowChrome)

#pragma mark Window / misc

// performClose: is a no-op on borderless windows -> call windowShouldClose: (autosaves) then close directly.
- (void)closeWindow:(id)sender {
    if ([self windowShouldClose:_window]) {
        // Explicitly stop the spinner timer (the block self-cancels via weak self, but clean up now).
        [_spinTimer invalidate]; _spinTimer = nil;
        // Release the input context of every text view/field BEFORE closing -> updateWindows
        // won't re-activate the context of a view being torn down.
        OS9SafeEndEditing(_window, nil);
        [_reqText teardown];
        [_respText teardown];
        [_settingEditor teardown];
        [_window close];
    }
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
    [self autosaveCurrent];
    // Stop the spinner + clear the sending flag on EVERY close path (not just the OS9 close button),
    // so closing mid-send doesn't leave a live timer / stuck _sending state.
    [_spinTimer invalidate]; _spinTimer = nil;
    _sending = NO;
    [self flushCaches];   // persist cache index before the window (and likely the app) goes away
    return YES;
}

// Persist deferred cache metadata. Called on window close + applicationWillTerminate because C++ dtors
// (which would otherwise flush) do NOT run on macOS app terminate.
- (void)flushCaches {
    if (_apiClient) _apiClient->cache().flush();
}
- (void)windowDidResize:(NSNotification *)note { [self relayout]; }

// SQUARE_CORNERS=2: mask the content layer with a 9-slice non-AA corner image so the window has
// pixel-rounded corners. Layer-backing the content view makes the mask clip the whole subtree.
- (void)applyPixelCorners {
    NSView *content = _window.contentView;
    if (!content) return;
    content.wantsLayer = YES;
    const CGFloat scale = _window.backingScaleFactor > 0 ? _window.backingScaleFactor : 1.0;
    int rpx = (int)lround(_cornerRadiusPts * scale);
    if (rpx < 1) rpx = 1;
    const int S = 2 * rpx + 1;
    CGImageRef img = OS9CreateCornerMask(rpx);
    if (!img) return;
    if (!_cornerMask) _cornerMask = [CALayer layer];
    _cornerMask.contents = (__bridge id)img;
    _cornerMask.contentsScale = scale;
    _cornerMask.contentsCenter = CGRectMake((CGFloat)rpx / S, (CGFloat)rpx / S, 1.0 / S, 1.0 / S);  // 9-slice
    _cornerMask.magnificationFilter = kCAFilterNearest;   // stretched edges stay crisp (no blur)
    _cornerMask.frame = content.bounds;
    content.layer.mask = _cornerMask;
    CGImageRelease(img);
    _window.opaque = NO;
    _window.backgroundColor = [NSColor clearColor];
    [_window invalidateShadow];   // shadow follows the rounded shape
}

// Moving to a display with a different scale -> regenerate the mask at the new device resolution.
- (void)windowDidChangeBackingProperties:(NSNotification *)note {
    if (_cornerRadiusPts > 0) [self applyPixelCorners];
}
- (void)windowDidBecomeKey:(NSNotification *)note { [_titleBar setNeedsDisplay:YES]; }
- (void)windowDidResignKey:(NSNotification *)note { [_titleBar setNeedsDisplay:YES]; }

- (NSString *)abbreviatePath:(NSString *)path {
    NSString *p = path;
    NSString *home = NSHomeDirectory();
    BOOL underHome = [p hasPrefix:home];
    if (underHome) p = [p substringFromIndex:home.length];
    NSMutableArray<NSString *> *parts = [[p pathComponents] mutableCopy];
    [parts removeObject:@"/"];
    if (parts.count == 0) return underHome ? @"~" : path;
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    if (underHome) [out addObject:@"~"];
    for (NSUInteger i = 0; i < parts.count; i++) {
        NSString *c = parts[i];
        [out addObject:(i == parts.count - 1) ? c : (c.length ? [c substringToIndex:1] : c)];
    }
    return [out componentsJoinedByString:@"/"];
}

#pragma mark Toast (flat retro, stack top-right, pushed down)

- (void)toast:(NSString *)msg     { [self showToast:msg kind:0]; } // info (gray)
- (void)toastOk:(NSString *)msg   { [self showToast:msg kind:1]; } // success (green)
- (void)toastWarn:(NSString *)msg { [self showToast:msg kind:2]; } // fail (red)

- (void)showToast:(NSString *)msg kind:(NSInteger)kind {
    if (!_toasts) _toasts = [NSMutableArray array];
    NSView *cv = _window.contentView;
    OS9Toast *t = [[OS9Toast alloc] initWithMessage:msg kind:kind];
    NSSize sz = [OS9Toast sizeForMessage:msg];
    // start off-screen to the right, in the top slot -> reflow slides it in.
    t.frame = NSMakeRect(cv.bounds.size.width, kTitleH + kToastTopGap, sz.width, sz.height);
    __weak MainWindowController *ws = self;
    __weak OS9Toast *wt = t;
    t.onClose = ^{ [ws dismissToast:wt]; };
    [cv addSubview:t positioned:NSWindowAbove relativeTo:nil];
    [_toasts addObject:t];
    while (_toasts.count > 5) {                       // cap the stack
        OS9Toast *old = _toasts.firstObject;
        [_toasts removeObjectAtIndex:0]; [old removeFromSuperview];
    }
    [self reflowToasts];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.8 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ [ws dismissToast:wt]; });
}

- (void)dismissToast:(OS9Toast *)t {
    if (!t || ![_toasts containsObject:t]) return;
    [_toasts removeObject:t];
    NSRect away = t.frame; away.origin.x = _window.contentView.bounds.size.width;  // slide right + fade
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
        ctx.duration = 0.28; t.animator.frame = away; t.animator.alphaValue = 0.0;
    } completionHandler:^{ [t removeFromSuperview]; }];
    [self reflowToasts];   // remaining toasts slide down to fill the gap
}

// Stack toasts from the top-RIGHT downward: newest (end of array) on top (content flipped: small y = top).
// Top slot starts under the title bar so a toast never covers the zoom/hide buttons.
- (void)reflowToasts {
    NSView *cv = _window.contentView;
    CGFloat W = cv.bounds.size.width;
    const CGFloat margin = 14, gap = 8;
    CGFloat top = kTitleH + kToastTopGap;
    for (NSInteger i = (NSInteger)_toasts.count - 1; i >= 0; i--) {
        OS9Toast *t = _toasts[i];
        CGFloat tw = t.frame.size.width, th = t.frame.size.height;
        NSRect target = NSMakeRect(W - tw - margin, top, tw, th);
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.2; t.animator.frame = target; t.animator.alphaValue = 1.0;
        } completionHandler:nil];
        top = top + th + gap;
    }
}

- (void)positionToast { [self reflowToasts]; }

@end
