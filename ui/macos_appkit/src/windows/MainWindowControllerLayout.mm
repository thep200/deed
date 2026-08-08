#import "windows/MainWindowControllerPrivate.h"
#import <QuartzCore/QuartzCore.h>

static const CGFloat kTitleH = 21;        // title bar fixed at 21px tall (incl. border)

@implementation MainWindowController (Layout)

#pragma mark Layout

- (void)relayout {
    NSRect cb = [_window.contentView bounds];
    if (_cornerMask) {   // keep the 9-slice corner mask sized to the window (no implicit animation on resize)
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        _cornerMask.frame = cb;
        [CATransaction commit];
    }
    CGFloat W = cb.size.width, H = cb.size.height;
    CGFloat titleH = kTitleH;
    _titleBar.frame = NSMakeRect(0, 0, W, titleH);
    _mainPane.frame = NSMakeRect(0, titleH, W, H - titleH);
    _configPane.frame = NSMakeRect(0, titleH, W, H - titleH);
    if (_configMode) { [self layoutConfig]; [self positionToast]; return; }

    DeedConfig *cfg = [DeedConfig shared];
    CGFloat MW = _mainPane.bounds.size.width, MH = _mainPane.bounds.size.height;
    CGFloat pad = [cfg floatFor:@"PADDING" def:8];
    // Outer side margins = outer edge of title icons -> panes/buttons align with close/zoom/hide.
    CGFloat side = [OS9TitleBar iconSideInset];
    CGFloat tabH = [cfg floatFor:@"TAB_HEIGHT" def:22];
    CGFloat toolH = [cfg floatFor:@"TOOLBAR_HEIGHT" def:40];
    CGFloat btnH = [cfg floatFor:@"BUTTON_HEIGHT" def:22];
    CGFloat statusH = 18;
    CGFloat dw = 6;

    CGFloat top = pad;
    CGFloat statusY = top + tabH + 2;
    CGFloat panesY = statusY + statusH + 2;            // closer to status -> pane extends higher
    CGFloat panesBottom = MH - toolH - 2;              // closer to toolbar -> pane extends lower

    // clamp pane widths
    CGFloat minTree = 140, minReq = 200, minResp = 220;
    CGFloat avail = MW - 2 * side - 2 * dw;
    if (_treeW < minTree) _treeW = minTree;
    if (_treeW > avail - minReq - minResp) _treeW = avail - minReq - minResp;
    CGFloat remain = avail - _treeW; // for req + resp
    if (_reqW <= 0) _reqW = remain * 3 / 7;   // default: left (request) : right (response) = 3:4
    if (_reqW < minReq) _reqW = minReq;
    if (_reqW > remain - minResp) _reqW = remain - minResp;
    CGFloat respW = remain - _reqW;

    CGFloat treeX = side;
    CGFloat divTreeX = treeX + _treeW;
    CGFloat reqX = divTreeX + dw;
    CGFloat divRespX = reqX + _reqW;
    CGFloat respX = divRespX + dw;

    // gear + Open (collection path) + tree — wrapped in serrated border
    CGFloat wGear = [cfg floatFor:@"BTN_SETTING_W" def:26];
    _settingButton.frame = NSMakeRect(treeX, top, wGear, tabH);
    _openButton.frame = NSMakeRect(treeX + wGear + 4, top, _treeW - wGear - 4, tabH);
    _treeInset.frame = NSMakeRect(treeX, statusY, _treeW, panesBottom - statusY);
    _treeScroll.frame = NSInsetRect(_treeInset.bounds, 2, 2);

    // dividers (full height of panes region)
    _divTree.frame = NSMakeRect(divTreeX, statusY, dw, panesBottom - statusY);
    _divResp.frame = NSMakeRect(divRespX, panesY, dw, panesBottom - panesY);

    // LEFT pane group: request tabs + editor
    [self layoutTabButtons:_reqTabButtons atX:reqX y:top width:_reqW height:tabH extra:0];
    _reqInset.frame = NSMakeRect(reqX, panesY, _reqW, panesBottom - panesY);
    _reqText.frame = NSInsetRect(_reqInset.bounds, 2, 2);

    // RIGHT pane group: response tabs + Pretty (same row) + editor
    NSMutableArray<OS9BevelButton *> *rightTabGroup = [_respTabButtons mutableCopy];
    if (_prettyButton) [rightTabGroup addObject:_prettyButton];
    [self layoutTabButtons:rightTabGroup atX:respX y:top width:respW height:tabH extra:0];
    _respInset.frame = NSMakeRect(respX, panesY, respW, panesBottom - panesY);
    _respText.frame = NSInsetRect(_respInset.bounds, 2, 2);

    // status line (span req + resp)
    CGFloat slX = reqX, slW = (respX + respW) - reqX;
    _statusBar.frame = NSMakeRect(slX, statusY, slW, statusH);
    _statusLabel.frame = NSMakeRect(slX + 8, statusY + 1, slW - 16, statusH - 2);

    // toolbar (1 row): ENV | Method/Proto | URL (stretches) | Cancel(when sending) | Send
    CGFloat ty = MH - toolH + (toolH - btnH) / 2;
    CGFloat x = side;
    CGFloat wEnv = [cfg floatFor:@"BTN_ENV_W" def:120];
    CGFloat wMethod = [cfg floatFor:@"BTN_METHOD_W" def:92];
    CGFloat wProto = [cfg floatFor:@"BTN_PROTO_W" def:104];   // just wider than "Reflection"
    CGFloat wService = [cfg floatFor:@"BTN_SERVICE_W" def:200];
    CGFloat wSend = [cfg floatFor:@"BTN_SEND_W" def:54];
    CGFloat wCancel = [cfg floatFor:@"BTN_CANCEL_W" def:64];

    _envButton.frame = NSMakeRect(x, ty, wEnv, btnH); x += wEnv + 6;
    RequestTypeUi *tui = TypeUiFor([self requestType]);
    BOOL grpc = tui.showsProtoPopup;
    _methodPopup.frame = NSMakeRect(x, ty, wMethod, btnH);
    _protoPopup.frame = NSMakeRect(x, ty, wProto, btnH);
    _methodPopup.hidden = !tui.showsMethodPopup;
    _protoPopup.hidden = !tui.showsProtoPopup;
    // HTTP advances by method width, gRPC by proto width, popup-less types by 0.
    x += (tui.showsProtoPopup ? wProto : (tui.showsMethodPopup ? wMethod : 0)) + 6;

    // Kafka client-kind selector: sits right before the brokers/URL field (i.e. right where the
    // method/proto popup would otherwise be).
    _kafkaModeToggle.hidden = !tui.showsKafkaToggle;
    if (tui.showsKafkaToggle) {
        BOOL isConsumer = ([self kafkaClientKind] == core::domain::KafkaClientKind::Consumer);
        _kafkaModeToggle.on = isConsumer;
        _kafkaModeToggle.label = isConsumer ? StrKafkaConsumer : StrKafkaProducer;
        CGFloat wToggle = [_kafkaModeToggle preferredWidth];
        _kafkaModeToggle.frame = NSMakeRect(x, ty, wToggle, btnH);
        x += wToggle + 6;
    }

    _cancelButton.hidden = !_sending;
    _servicePopup.hidden = !grpc;
    // Right group: [servicePopup (gRPC)] [Cancel (when sending)] [Send].
    CGFloat rightGroup = wSend + 6 + (_sending ? wCancel + 6 : 0) + (grpc ? wService + 6 : 0);
    CGFloat urlW = (MW - side) - x - rightGroup;
    if (urlW < 140) urlW = 140;
    _urlInset.frame = NSMakeRect(x, ty, urlW, btnH);
    // field sits inside the inset, leaving room for the border + vertically centered for one line.
    CGFloat fh = ceil([[OS9Theme monoFont] ascender] - [[OS9Theme monoFont] descender]) + 2;
    _urlField.frame = NSMakeRect(4, floor((btnH - fh) / 2), urlW - 8, fh);
    CGFloat rx = MW - side - wSend;           // right edge of the Send button
    _sendButton.frame = NSMakeRect(rx, ty, wSend, btnH);
    if (_sending) { rx -= 6 + wCancel; _cancelButton.frame = NSMakeRect(rx, ty, wCancel, btnH); }
    if (grpc) { rx -= 6 + wService; _servicePopup.frame = NSMakeRect(rx, ty, wService, btnH); }

    [self positionToast];
}

- (void)layoutTabButtons:(NSArray<OS9BevelButton *> *)buttons atX:(CGFloat)x y:(CGFloat)y width:(CGFloat)width height:(CGFloat)h extra:(CGFloat)extra {
    if (buttons.count == 0) return;
    CGFloat bw = width / buttons.count;
    CGFloat cx = x;
    for (OS9BevelButton *btn in buttons) { btn.frame = NSMakeRect(cx, y, bw - 2, h); cx += bw; }
}

- (void)layoutConfig {
    DeedConfig *cfg = [DeedConfig shared];
    CGFloat W = _configPane.bounds.size.width, H = _configPane.bounds.size.height;
    CGFloat pad = [cfg floatFor:@"PADDING" def:8];
    CGFloat side = [OS9TitleBar iconSideInset];   // align L/R margins with the title-bar close/hide icons
    CGFloat btnH = [cfg floatFor:@"BUTTON_HEIGHT" def:22];
    _backButton.frame = NSMakeRect(side, pad, 90, btnH);             // ← Back (top-left); title in the title bar
    _manageEnvButton.frame = NSMakeRect(side + 90 + 6, pad, 110, btnH);   // next to Back (6pt toolbar gap)
    _manageEnvButton.hidden = (_configKind != 1);                    // Settings only

    CGFloat top = pad + btnH + pad;
    NSRect body = NSMakeRect(side, top, W - 2 * side, H - top - pad);
    if (_configKind == 0) {                                          // Environments
        if (_envVC.view) { _envVC.view.frame = body; [_envVC layout]; }
    } else {                                                         // Settings
        _settingInset.frame = body;
        _settingEditor.frame = NSInsetRect(_settingInset.bounds, 2, 2);   // leave room for serrated border
    }
}

@end
