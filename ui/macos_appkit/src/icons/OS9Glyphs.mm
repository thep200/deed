#import "icons/OS9Glyphs.h"

NSImage *OS9GearImage(CGFloat size) {
    return [NSImage imageWithSize:NSMakeSize(size, size)
                          flipped:NO
                   drawingHandler:^BOOL(NSRect r) {
        CGFloat cx = size / 2, cy = size / 2;
        CGFloat rOut = size * 0.46;   // đỉnh răng
        CGFloat rRoot = size * 0.32;  // chân răng (vành)
        int teeth = 8;
        CGFloat pitch = 2 * M_PI / teeth;
        CGFloat tipHalf = pitch * 0.24;   // nửa bề rộng góc của đỉnh răng (răng vuông, rõ)

        // Răng vuông (flat-top) quanh vành -> bánh răng rõ ràng thay vì hình sao.
        NSBezierPath *gear = [NSBezierPath bezierPath];
        for (int i = 0; i < teeth; i++) {
            CGFloat a = i * pitch;
            NSPoint p1 = NSMakePoint(cx + rRoot * cos(a - tipHalf), cy + rRoot * sin(a - tipHalf));
            NSPoint p2 = NSMakePoint(cx + rOut  * cos(a - tipHalf), cy + rOut  * sin(a - tipHalf));
            NSPoint p3 = NSMakePoint(cx + rOut  * cos(a + tipHalf), cy + rOut  * sin(a + tipHalf));
            NSPoint p4 = NSMakePoint(cx + rRoot * cos(a + tipHalf), cy + rRoot * sin(a + tipHalf));
            if (i == 0) [gear moveToPoint:p1]; else [gear lineToPoint:p1];
            [gear lineToPoint:p2];
            [gear lineToPoint:p3];
            [gear lineToPoint:p4];
        }
        [gear closePath];

        // Kiểu "viền": vẽ nét bao (outline) đậm thay vì tô đặc.
        CGFloat lw = MAX(1.0, floor(size * 0.09));
        gear.lineWidth = lw;
        gear.lineJoinStyle = NSLineJoinStyleMiter;
        [[NSColor blackColor] set];
        [gear stroke];

        // lỗ trục ở giữa (vòng tròn viền)
        CGFloat hr = size * 0.15;
        NSBezierPath *hole = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - hr, cy - hr, hr * 2, hr * 2)];
        hole.lineWidth = lw;
        [hole stroke];
        return YES;
    }];
}

NSImage *OS9SendImage(CGFloat size) {
    return [NSImage imageWithSize:NSMakeSize(size, size) flipped:NO
                   drawingHandler:^BOOL(NSRect r) {
        CGFloat s = size;
        // Máy bay giấy hướng phải — đường VIỀN mảnh (hairline) + nếp gấp giữa.
        NSBezierPath *body = [NSBezierPath bezierPath];
        [body moveToPoint:NSMakePoint(s*0.90, s*0.50)];   // mũi (phải)
        [body lineToPoint:NSMakePoint(s*0.12, s*0.82)];   // góc trên-trái
        [body lineToPoint:NSMakePoint(s*0.40, s*0.50)];   // lõm giữa
        [body lineToPoint:NSMakePoint(s*0.12, s*0.18)];   // góc dưới-trái
        [body closePath];
        body.lineWidth = 0.6;                              // mảnh hơn (trước 1.0)
        body.lineJoinStyle = NSLineJoinStyleMiter;
        [[NSColor blackColor] set];
        [body stroke];
        return YES;
    }];
}

NSImage *OS9FolderImage(CGFloat size) {
    return [NSImage imageWithSize:NSMakeSize(size, size) flipped:NO
                   drawingHandler:^BOOL(NSRect r) {
        [[NSGraphicsContext currentContext] setShouldAntialias:NO];
        CGFloat s = size / 16.0;                 // metric thiết kế cho 16px
        // Thân + tab (non-flipped: y lớn = trên). Tab nhô lên ở góc trên-trái.
        CGFloat x0   = floor(1 * s),  x1 = floor(15 * s);
        CGFloat yBot = floor(2 * s),  yTop = floor(12 * s);   // mép trên thân
        CGFloat tabW = floor(6 * s),  tabH = floor(2 * s);    // tab nhô thêm tabH
        NSColor *fill = [NSColor colorWithCalibratedRed:0.62 green:0.74 blue:0.86 alpha:1.0];
        NSColor *hi   = [NSColor colorWithCalibratedRed:0.82 green:0.90 blue:0.97 alpha:1.0];
        NSColor *line = [NSColor colorWithCalibratedRed:0.27 green:0.38 blue:0.50 alpha:1.0];

        NSBezierPath *p = [NSBezierPath bezierPath];
        [p moveToPoint:NSMakePoint(x0,             yBot)];          // dưới-trái
        [p lineToPoint:NSMakePoint(x1,             yBot)];          // dưới-phải
        [p lineToPoint:NSMakePoint(x1,             yTop)];          // lên mép phải
        [p lineToPoint:NSMakePoint(x0 + tabW + 2*s, yTop)];         // mép trên thân (phải)
        [p lineToPoint:NSMakePoint(x0 + tabW,      yTop + tabH)];   // dốc lên đỉnh tab
        [p lineToPoint:NSMakePoint(x0,             yTop + tabH)];   // đỉnh tab (trái)
        [p closePath];

        [fill set]; [p fill];
        [hi set];                                                   // bevel sáng mép trên thân
        NSRectFill(NSMakeRect(x0 + 1, yTop - 1, (x1 - x0) - 2, 1));
        [line set]; p.lineWidth = 1.0; [p stroke];
        return YES;
    }];
}

NSArray<NSImage *> *OS9SpinnerFrames(CGFloat size, int frameCount) {
    static NSMutableDictionary<NSString *, NSArray<NSImage *> *> *cache;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ cache = [NSMutableDictionary dictionary]; });
    NSString *key = [NSString stringWithFormat:@"%.0fx%d", size, frameCount];
    NSArray<NSImage *> *frames = cache[key];
    if (frames) return frames;                       // đã dựng -> tái dùng (không cấp phát lại)
    NSMutableArray<NSImage *> *fs = [NSMutableArray arrayWithCapacity:frameCount];
    for (int i = 0; i < frameCount; i++)
        [fs addObject:OS9SpinnerImage(size, (CGFloat)i / frameCount)];
    frames = [fs copy];
    cache[key] = frames;
    return frames;
}

NSImage *OS9SpinnerImage(CGFloat size, CGFloat phase) {
    return [NSImage imageWithSize:NSMakeSize(size, size) flipped:NO
                   drawingHandler:^BOOL(NSRect r) {
        CGFloat cx = size/2, cy = size/2;
        CGFloat rIn = size*0.22, rOut = size*0.44;
        int spokes = 8;
        CGFloat lw = MAX(1.5, size*0.12);
        CGFloat base = phase * 2 * M_PI;          // pha quay
        for (int i = 0; i < spokes; i++) {
            CGFloat a = base + i * (2*M_PI/spokes);
            CGFloat alpha = 0.20 + 0.80 * ((CGFloat)i / (spokes-1)); // mờ dần -> hiệu ứng quay
            NSBezierPath *sp = [NSBezierPath bezierPath];
            sp.lineWidth = lw;
            sp.lineCapStyle = NSLineCapStyleRound;
            [sp moveToPoint:NSMakePoint(cx + rIn*cos(a),  cy + rIn*sin(a))];
            [sp lineToPoint:NSMakePoint(cx + rOut*cos(a), cy + rOut*sin(a))];
            [[NSColor colorWithCalibratedWhite:0.0 alpha:alpha] set];
            [sp stroke];
        }
        return YES;
    }];
}
