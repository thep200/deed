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
