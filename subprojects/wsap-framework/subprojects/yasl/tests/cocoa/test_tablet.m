#import <AppKit/AppKit.h>
#include <common/private_native.h>
#include <assert.h>

/* A strict event double catches querying tablet-only properties on ordinary
 * mouse events, and proximity-only properties on tablet point events. */
@interface TabletSample : NSObject
@property NSEventType type;
@property NSEventSubtype subtype;
@property NSEventModifierFlags modifierFlags;
@property NSPoint locationInWindow;
@property NSInteger buttonNumber;
@property NSUInteger buttonMask;
@property(getter=isEnteringProximity) BOOL enteringProximity;
@property NSPointingDeviceType tool;
@property float samplePressure;
@property NSPoint sampleTilt;
@property double sampleRotation;
@property double sampleTangential;
@property NSInteger sampleDeviceID;
@property NSInteger sampleToolID;
@end

@implementation TabletSample
{
    NSEventSubtype _subtype;
    NSUInteger _buttonMask;
}
- (NSEventSubtype)subtype
{
    assert(self.type == NSEventTypeLeftMouseDown || self.type == NSEventTypeLeftMouseUp ||
           self.type == NSEventTypeRightMouseDown || self.type == NSEventTypeRightMouseUp ||
           self.type == NSEventTypeOtherMouseDown || self.type == NSEventTypeOtherMouseUp ||
           self.type == NSEventTypeMouseMoved || self.type == NSEventTypeLeftMouseDragged ||
           self.type == NSEventTypeRightMouseDragged || self.type == NSEventTypeOtherMouseDragged);
    return _subtype;
}
- (void)setSubtype:(NSEventSubtype)subtype
{
    _subtype = subtype;
}
- (NSUInteger)buttonMask
{
    assert(self.type == NSEventTypeTabletPoint || self.subtype == NSEventSubtypeTabletPoint);
    return _buttonMask;
}
- (void)setButtonMask:(NSUInteger)buttonMask
{
    _buttonMask = buttonMask;
}
- (float)pressure
{
    assert(self.type == NSEventTypeTabletPoint || self.subtype == NSEventSubtypeTabletPoint);
    return self.samplePressure;
}
- (NSPoint)tilt
{
    assert(self.type == NSEventTypeTabletPoint || self.subtype == NSEventSubtypeTabletPoint);
    return self.sampleTilt;
}
- (NSPointingDeviceType)pointingDeviceType
{
    assert(self.type == NSEventTypeTabletProximity || self.subtype == NSEventSubtypeTabletProximity);
    return self.tool;
}
- (double)rotation { return self.sampleRotation; }
- (double)tangentialPressure { return self.sampleTangential; }
- (NSInteger)pointingDeviceID { return self.sampleDeviceID; }
- (NSInteger)pointingDeviceSerialNumber { return self.sampleToolID; }
@end

static struct yasl_private_event next(struct yasl_surface *surface, enum yasl_private_event_type type)
{
    struct yasl_private_event event;
    assert(yasl_private_display_poll_event(yasl_private_surface_display(surface), &event));
    assert(event.type == type);
    return event;
}

int main(void)
{
    @autoreleasepool {
        struct yasl_private_display *display = yasl_private_display_open(NULL, NULL);
        assert(display && (yasl_private_display_caps(display) & YASL_PRIVATE_CAP_TABLET));
        struct yasl_private_window_spec spec = {.title = "Tablet test", .size = {320, 240}};
        struct yasl_surface *surface = yasl_private_surface_create_toplevel(display, &spec);
        assert(surface);
        NSWindow *window = (NSWindow *)yasl_private_surface_get_native_handle(surface);
        NSView *view = window.contentView;
        struct yasl_private_event event;
        while (yasl_private_display_poll_event(yasl_private_surface_display(surface), &event)) {}
        TabletSample *sample = [TabletSample new];
        NSEvent *native = (NSEvent *)sample;
        sample.type = NSEventTypeTabletProximity;
        sample.enteringProximity = YES;
        sample.tool = NSPointingDeviceTypePen;
        sample.sampleDeviceID = 17;
        sample.sampleToolID = 29;
        [view tabletProximity:native];
        sample.type = NSEventTypeMouseMoved;
        sample.subtype = NSEventSubtypeTabletPoint;
        sample.locationInWindow = NSMakePoint(100, 120);
        sample.sampleTilt = NSMakePoint(-0.5, 0.5);
        sample.sampleRotation = 37.5;
        sample.sampleTangential = -0.25;
        [view mouseMoved:native];
        struct yasl_private_event pen = next(surface, YASL_PRIVATE_EVENT_PEN);
        assert(pen.pen.action == YASL_PRIVATE_PEN_ENTER && pen.pen.device_id == 17 && pen.pen.tool_id == 29);
        struct yasl_private_event ev = next(surface, YASL_PRIVATE_EVENT_POINTER_MOTION);
        assert(ev.pointer.device == YASL_PRIVATE_PTR_DEV_PEN && ev.type == YASL_PRIVATE_EVENT_POINTER_MOTION && !ev.pointer.pen.pressure);
        assert(ev.pointer.pen.tilt_x == -45 && ev.pointer.pen.tilt_y == -45);
        pen = next(surface, YASL_PRIVATE_EVENT_PEN);
        assert(pen.pen.action == YASL_PRIVATE_PEN_MOVE && pen.pen.device_id == 17 && pen.pen.tool_id == 29);
        assert((pen.pen.axes_valid & (YASL_PRIVATE_PEN_PRESSURE | YASL_PRIVATE_PEN_TILT)) ==
               (YASL_PRIVATE_PEN_PRESSURE | YASL_PRIVATE_PEN_TILT));
        assert(pen.pen.rotation == 37.5 && pen.pen.tangential == -0.25);
        assert((pen.pen.axes_valid & (YASL_PRIVATE_PEN_ROTATION | YASL_PRIVATE_PEN_TANGENTIAL)) ==
               (YASL_PRIVATE_PEN_ROTATION | YASL_PRIVATE_PEN_TANGENTIAL));
        sample.type = NSEventTypeLeftMouseDown;
        sample.samplePressure = 0.5;
        sample.buttonMask = NSEventButtonMaskPenTip;
        [view mouseDown:native];
        ev = next(surface, YASL_PRIVATE_EVENT_POINTER_DOWN);
        assert(ev.pointer.device == YASL_PRIVATE_PTR_DEV_PEN && ev.type == YASL_PRIVATE_EVENT_POINTER_DOWN && ev.pointer.button == YASL_PRIVATE_BUTTON_PRIMARY);
        assert(ev.pointer.pen.pressure == 32767);
        pen = next(surface, YASL_PRIVATE_EVENT_PEN);
        assert(pen.pen.action == YASL_PRIVATE_PEN_DOWN && pen.pen.in_contact && pen.pen.pressure == 0.5);
        assert(pen.pen.buttons == YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_PRIMARY));
        assert(pen.pen.changed_button == YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_PRIMARY));
        struct yasl_private_vector2i position = ev.pointer.pos;
        /* Native tablet events cover stationary pressure changes. */
        sample.type = NSEventTypeTabletPoint;
        sample.subtype = NSEventSubtypeMouseEvent;
        sample.buttonMask = NSEventButtonMaskPenTip;
        sample.samplePressure = 1.5;
        [view tabletPoint:native];
        ev = next(surface, YASL_PRIVATE_EVENT_POINTER_MOTION);
        assert(ev.type == YASL_PRIVATE_EVENT_POINTER_MOTION && ev.pointer.pen.pressure == 65535);
        assert(ev.pointer.pos.x == position.x && ev.pointer.pos.y == position.y);
        pen = next(surface, YASL_PRIVATE_EVENT_PEN);
        assert(pen.pen.action == YASL_PRIVATE_PEN_MOVE && pen.pen.pressure == 1.0);
        sample.buttonMask |= NSEventButtonMaskPenLowerSide;
        [view tabletPoint:native];
        next(surface, YASL_PRIVATE_EVENT_POINTER_MOTION);
        pen = next(surface, YASL_PRIVATE_EVENT_PEN);
        assert(pen.pen.action == YASL_PRIVATE_PEN_BUTTON);
        assert(pen.pen.buttons == (YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_PRIMARY) |
                                   YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_SECONDARY)));
        assert(pen.pen.changed_button == YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_SECONDARY));
        sample.buttonMask = NSEventButtonMaskPenTip;
        sample.samplePressure = 0; /* Zero does not release a held tip button. */
        [view tabletPoint:native];
        assert(next(surface, YASL_PRIVATE_EVENT_POINTER_MOTION).type == YASL_PRIVATE_EVENT_POINTER_MOTION);
        next(surface, YASL_PRIVATE_EVENT_PEN);
        sample.buttonMask = 0;
        [view tabletPoint:native];
        ev = next(surface, YASL_PRIVATE_EVENT_POINTER_UP);
        assert(ev.type == YASL_PRIVATE_EVENT_POINTER_UP && !ev.pointer.pen.pressure && ev.pointer.device == YASL_PRIVATE_PTR_DEV_PEN);
        pen = next(surface, YASL_PRIVATE_EVENT_PEN);
        assert(pen.pen.action == YASL_PRIVATE_PEN_UP && !pen.pen.in_contact && pen.pen.pressure == 0.0);
        sample.type = NSEventTypeTabletProximity;
        sample.enteringProximity = NO;
        [view tabletProximity:native];
        assert(next(surface, YASL_PRIVATE_EVENT_POINTER_LEAVE).type == YASL_PRIVATE_EVENT_POINTER_LEAVE);
        assert(next(surface, YASL_PRIVATE_EVENT_PEN).pen.action == YASL_PRIVATE_PEN_LEAVE);
        sample.enteringProximity = YES;
        sample.tool = NSPointingDeviceTypeEraser;
        [view tabletProximity:native];
        sample.type = NSEventTypeTabletPoint;
        sample.samplePressure = 0.75;
        sample.buttonMask = NSEventButtonMaskPenTip;
        [view tabletPoint:native];
        pen = next(surface, YASL_PRIVATE_EVENT_PEN);
        assert(pen.pen.action == YASL_PRIVATE_PEN_ENTER && pen.pen.tool == 2);
        ev = next(surface, YASL_PRIVATE_EVENT_POINTER_DOWN);
        assert(ev.pointer.device == YASL_PRIVATE_PTR_DEV_ERASER && ev.type == YASL_PRIVATE_EVENT_POINTER_DOWN && ev.pointer.pen.pressure > 0);
        pen = next(surface, YASL_PRIVATE_EVENT_PEN);
        assert(pen.pen.action == YASL_PRIVATE_PEN_DOWN && pen.pen.tool == 2 && pen.pen.in_contact);
        sample.type = NSEventTypeTabletProximity;
        sample.enteringProximity = NO;
        [view tabletProximity:native];
        ev = next(surface, YASL_PRIVATE_EVENT_POINTER_LEAVE);
        assert(ev.pointer.device == YASL_PRIVATE_PTR_DEV_ERASER && ev.type == YASL_PRIVATE_EVENT_POINTER_LEAVE && !ev.pointer.pen.pressure);
        assert(next(surface, YASL_PRIVATE_EVENT_PEN).pen.action == YASL_PRIVATE_PEN_LEAVE);
        sample.type = NSEventTypeMouseMoved;
        [view mouseMoved:native];
        ev = next(surface, YASL_PRIVATE_EVENT_POINTER_MOTION);
        assert(ev.pointer.device == YASL_PRIVATE_PTR_DEV_MOUSE && !ev.pointer.pen.pressure);
        sample.type = NSEventTypeMouseExited;
        [view mouseExited:native];
        ev = next(surface, YASL_PRIVATE_EVENT_POINTER_LEAVE);
        assert(ev.pointer.device == YASL_PRIVATE_PTR_DEV_MOUSE && ev.type == YASL_PRIVATE_EVENT_POINTER_LEAVE);
        [sample release];
        yasl_private_surface_destroy(surface);
        yasl_private_display_close(display);
    }
    return 0;
}
