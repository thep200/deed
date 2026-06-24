// OS9Toggle.mm — retro slide switch. Fills its frame (as tall as a toolbar button), square corners,
// crisp 1px bevels. A raised KNOB (half width) carries the label (e.g. "TLS") and slides left (off) /
// right (on) with a short animation; the cell to the knob's left is accent-filled when on. Click toggles.
#import "widgets/OS9Toggle.h"
#import "theme/OS9Theme.h"

@implementation OS9Toggle {
    CGFloat _knobFrac;   // animated knob position 0 (left/off) .. 1 (right/on)
    NSTimer *_anim;
}

- (instancetype)initWithLabel:(NSString *)label target:(id)target action:(SEL)action {
    if ((self = [super initWithFrame:NSZeroRect])) {
        _label = [label copy];
        self.target = target;
        self.action = action;
    }
    return self;
}

- (BOOL)isFlipped { return NO; }   // bottom-up: "top" edge = NSMaxY

// Width = two knob-cells; a knob = label + snug padding (+ the 1px frame/bevel on each side).
- (CGFloat)preferredWidth {
    CGFloat lw = _label.length
        ? [_label sizeWithAttributes:@{NSFontAttributeName : [OS9Theme uiFont]}].width : 0;
    CGFloat knob = ceil(lw) + 10;   // ~5px each side inside the knob
    return knob * 2;
}

// Programmatic set (e.g. from layout): snap, no animation.
- (void)setOn:(BOOL)on {
    _on = on;
    [_anim invalidate]; _anim = nil;
    _knobFrac = on ? 1.0 : 0.0;
    [self setNeedsDisplay:YES];
}
- (void)setLabel:(NSString *)label { _label = [label copy]; [self setNeedsDisplay:YES]; }

// Animate the knob toward the current _on state (~0.12s ease-out).
- (void)startAnim {
    [_anim invalidate];
    __weak OS9Toggle *ws = self;
    _anim = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer *t) {
        OS9Toggle *s = ws; if (!s) { [t invalidate]; return; }
        CGFloat target = s->_on ? 1.0 : 0.0;
        CGFloat d = target - s->_knobFrac;
        if (fabs(d) < 0.05) { s->_knobFrac = target; [t invalidate]; s->_anim = nil; }
        else s->_knobFrac += d * 0.34;
        [s setNeedsDisplay:YES];
    }];
}

// 1px bevel: top+left = tl, bottom+right = br (crisp, non-antialiased).
static void bevel(NSRect r, NSColor *tl, NSColor *br) {
    [tl set];
    NSRectFill(NSMakeRect(r.origin.x, NSMaxY(r) - 1, r.size.width, 1));  // top
    NSRectFill(NSMakeRect(r.origin.x, r.origin.y, 1, r.size.height));    // left
    [br set];
    NSRectFill(NSMakeRect(r.origin.x, r.origin.y, r.size.width, 1));     // bottom
    NSRectFill(NSMakeRect(NSMaxX(r) - 1, r.origin.y, 1, r.size.height)); // right
}

- (void)drawRect:(NSRect)dirty {
    [[NSGraphicsContext currentContext] setShouldAntialias:NO];
    NSRect b = self.bounds;
    CGFloat knobW = floor(b.size.width / 2);
    CGFloat knobX = floor(b.origin.x + _knobFrac * (b.size.width - knobW));

    // Track: sunken platinum cell; accent fills the strip to the LEFT of the knob when on (grows as it slides).
    [[OS9Theme buttonFace] set];
    NSRectFill(b);
    if (_on && knobX > b.origin.x + 1) {
        [[OS9Theme accent] set];
        NSRectFill(NSMakeRect(b.origin.x + 1, b.origin.y + 1, knobX - b.origin.x - 1, b.size.height - 2));
    }
    bevel(b, [OS9Theme shadow], [OS9Theme highlight]);   // sunken
    [[OS9Theme frame] set];
    NSFrameRect(b);

    // Knob: raised square carrying the label.
    NSRect knob = NSMakeRect(knobX, b.origin.y, knobW, b.size.height);
    NSRect inner = NSInsetRect(knob, 1, 1);
    [[OS9Theme buttonFace] set];
    NSRectFill(inner);
    bevel(inner, [OS9Theme highlight], [OS9Theme shadow]);  // raised
    [[OS9Theme frame] set];
    NSFrameRect(knob);

    if (_label.length) {
        NSDictionary *attrs = @{NSFontAttributeName : [OS9Theme uiFont],
                                NSForegroundColorAttributeName : [NSColor blackColor]};
        NSSize ls = [_label sizeWithAttributes:attrs];
        NSPoint p = NSMakePoint(floor(NSMidX(knob) - ls.width / 2), floor(NSMidY(knob) - ls.height / 2));
        [_label drawAtPoint:p withAttributes:attrs];
    }
}

- (void)mouseDown:(NSEvent *)e {
    if (!self.isEnabled) return;
    _on = !_on;
    [self startAnim];
    [self sendAction:self.action to:self.target];
}

@end
