#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "ui/yasl/accessibility/accessibility.h"

#include "util/geom.h"
#include "yasl/yasl.h"

#include <stdlib.h>
#include <string.h>

@interface YaslView : NSView
@end

@class WsapAccessibilityProvider;

@interface WsapAccessibilityElement : NSAccessibilityElement {
@public
    __unsafe_unretained WsapAccessibilityProvider *provider;
    uint64_t nodeID;
}
- (instancetype)initWithProvider:(WsapAccessibilityProvider *)owner
                           nodeID:(uint64_t)identifier;
@end

@interface WsapAccessibilityProvider : NSObject {
@public
    YaslView *view;
    struct yasl_surface *surface;
    ui_accessibility_action_sink_fn actionSink;
    void *actionUserdata;
    struct ui_accessibility_tree snapshot;
    NSMutableDictionary *elements;
    uint64_t focusedID;
}
- (NSArray *)childrenOf:(uint64_t)parentID;
- (WsapAccessibilityElement *)elementFor:(uint64_t)identifier;
- (const struct ui_accessibility_node *)nodeFor:(uint64_t)identifier;
- (BOOL)perform:(uint64_t)identifier
         action:(enum ui_accessibility_action)action
          value:(const char *)value
   selectionStart:(size_t)start
     selectionEnd:(size_t)end
           caret:(size_t)caret;
@end

static char provider_key;

#ifndef YASL_SCALE_UNIT
#define YASL_SCALE_UNIT 120
#endif

static struct rect2i rect_to_logical(const struct yasl_surface *s,
                                    struct rect2i device)
{
    double s_scale = s ? yasl_surface_get_info(s).scale : 1.0;
    uint32_t scale = (uint32_t)(s_scale * (double)YASL_SCALE_UNIT + 0.5);
    if (!scale)
        scale = YASL_SCALE_UNIT;
    return (struct rect2i){
        .pos = {
            .x = (int)(((int64_t)device.x * YASL_SCALE_UNIT) / scale),
            .y = (int)(((int64_t)device.y * YASL_SCALE_UNIT) / scale),
        },
        .size = {
            .x = (int)(((int64_t)device.width * YASL_SCALE_UNIT) / scale),
            .y = (int)(((int64_t)device.height * YASL_SCALE_UNIT) / scale),
        },
    };
}

static NSString *role_for_node(enum ui_accessibility_role role)
{
    switch (role) {
    case UI_ACCESSIBILITY_ROLE_WINDOW: return NSAccessibilityWindowRole;
    case UI_ACCESSIBILITY_ROLE_GROUP: return NSAccessibilityGroupRole;
    case UI_ACCESSIBILITY_ROLE_DIALOG: return NSAccessibilitySheetRole;
    case UI_ACCESSIBILITY_ROLE_LABEL: return NSAccessibilityStaticTextRole;
    case UI_ACCESSIBILITY_ROLE_HEADING: return @"AXHeading";
    case UI_ACCESSIBILITY_ROLE_BUTTON:
    case UI_ACCESSIBILITY_ROLE_TOGGLE_BUTTON: return NSAccessibilityButtonRole;
    case UI_ACCESSIBILITY_ROLE_CHECKBOX: return NSAccessibilityCheckBoxRole;
    case UI_ACCESSIBILITY_ROLE_RADIO_BUTTON: return NSAccessibilityRadioButtonRole;
    case UI_ACCESSIBILITY_ROLE_TAB_LIST: return NSAccessibilityTabGroupRole;
    case UI_ACCESSIBILITY_ROLE_TAB: return NSAccessibilityButtonRole;
    case UI_ACCESSIBILITY_ROLE_TEXT_FIELD:
    case UI_ACCESSIBILITY_ROLE_SEARCH_FIELD: return NSAccessibilityTextFieldRole;
    case UI_ACCESSIBILITY_ROLE_LINK: return NSAccessibilityLinkRole;
    case UI_ACCESSIBILITY_ROLE_LIST: return NSAccessibilityListRole;
    case UI_ACCESSIBILITY_ROLE_LIST_ITEM: return NSAccessibilityRowRole;
    case UI_ACCESSIBILITY_ROLE_MENU: return NSAccessibilityMenuRole;
    case UI_ACCESSIBILITY_ROLE_MENU_ITEM: return NSAccessibilityMenuItemRole;
    case UI_ACCESSIBILITY_ROLE_SLIDER: return NSAccessibilitySliderRole;
    case UI_ACCESSIBILITY_ROLE_PROGRESS_INDICATOR:
        return NSAccessibilityProgressIndicatorRole;
    case UI_ACCESSIBILITY_ROLE_SCROLL_BAR: return NSAccessibilityScrollBarRole;
    case UI_ACCESSIBILITY_ROLE_IMAGE: return NSAccessibilityImageRole;
    case UI_ACCESSIBILITY_ROLE_TERMINAL: return NSAccessibilityTextAreaRole;
    case UI_ACCESSIBILITY_ROLE_COMBO_BOX: return NSAccessibilityComboBoxRole;
    case UI_ACCESSIBILITY_ROLE_UNKNOWN: return NSAccessibilityUnknownRole;
    }
    return NSAccessibilityUnknownRole;
}

static NSString *string_or_nil(const char *text)
{
    return text ? [NSString stringWithUTF8String:text] : nil;
}

static NSNumber *element_key(uint64_t identifier)
{
    return @(identifier);
}

static bool strings_equal(const char *a, const char *b)
{
    return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

static NSUInteger utf16_offset_for_utf8(const char *text, size_t offset)
{
    if (!text)
        return 0;
    size_t length = strlen(text);
    if (offset > length)
        offset = length;
    while (offset > 0 &&
           ((const unsigned char)text[offset] & 0xc0) == 0x80)
        offset--;
    NSString *prefix = [[[NSString alloc] initWithBytes:text
                                                 length:offset
                                               encoding:NSUTF8StringEncoding]
        autorelease];
    return prefix ? prefix.length : 0;
}

static NSUInteger utf16_boundary(NSString *text, NSUInteger offset)
{
    if (offset > 0 && offset < text.length &&
        CFStringIsSurrogateHighCharacter([text characterAtIndex:offset - 1]) &&
        CFStringIsSurrogateLowCharacter([text characterAtIndex:offset]))
        offset--;
    return offset;
}

static struct rect2i accessibility_bounds(
    const struct ui_accessibility_node *node)
{
    if (!node)
        return (struct rect2i){ .pos = { .x = 0, .y = 0 },
                               .size = { .x = 0, .y = 0 } };
    return node->bounds;
}

@implementation WsapAccessibilityElement

- (instancetype)initWithProvider:(WsapAccessibilityProvider *)owner
                           nodeID:(uint64_t)identifier
{
    self = [super init];
    if (self) {
        provider = owner;
        nodeID = identifier;
    }
    return self;
}

- (void)dealloc
{
    [super dealloc];
}

- (BOOL)isAccessibilityElement
{
    return [provider nodeFor:nodeID] != NULL;
}

- (BOOL)isAccessibilitySelectorAllowed:(SEL)selector
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    if (!node)
        return NO;
    enum ui_accessibility_action action = 0;
    if (selector == @selector(accessibilityPerformPress))
        action = UI_ACCESSIBILITY_ACTION_INVOKE;
    else if (selector == @selector(accessibilityPerformIncrement))
        action = UI_ACCESSIBILITY_ACTION_INCREMENT;
    else if (selector == @selector(accessibilityPerformDecrement))
        action = UI_ACCESSIBILITY_ACTION_DECREMENT;
    else if (selector == @selector(setAccessibilityValue:))
        action = UI_ACCESSIBILITY_ACTION_SET_VALUE;
    else if (selector == @selector(setAccessibilitySelectedTextRange:) ||
             selector == @selector(setAccessibilitySelectedTextRanges:))
        action = UI_ACCESSIBILITY_ACTION_SET_SELECTION;
    else if (selector == @selector(setAccessibilityFocused:))
        action = UI_ACCESSIBILITY_ACTION_FOCUS;
    if (action)
        return provider->actionSink && (node->actions & action) &&
               !(node->states & (UI_ACCESSIBILITY_STATE_DISABLED |
                                 UI_ACCESSIBILITY_STATE_HIDDEN));
    return [super isAccessibilitySelectorAllowed:selector];
}

- (BOOL)isAccessibilityEnabled
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    return node && !(node->states & UI_ACCESSIBILITY_STATE_DISABLED);
}

- (BOOL)isAccessibilityFocused
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    return node && (node->states & UI_ACCESSIBILITY_STATE_FOCUSED);
}

- (void)setAccessibilityFocused:(BOOL)focused
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    if (focused && node && (node->actions & UI_ACCESSIBILITY_ACTION_FOCUS))
        [provider perform:nodeID action:UI_ACCESSIBILITY_ACTION_FOCUS
                     value:NULL selectionStart:0 selectionEnd:0 caret:0];
}

- (BOOL)isAccessibilitySelected
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    return node && (node->states & UI_ACCESSIBILITY_STATE_SELECTED);
}

- (BOOL)isAccessibilityExpanded
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    return node && (node->states & UI_ACCESSIBILITY_STATE_EXPANDED);
}

- (NSString *)accessibilityRole
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    return role_for_node(node ? node->role : UI_ACCESSIBILITY_ROLE_UNKNOWN);
}

- (NSString *)accessibilitySubrole
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    if (node && (node->states & UI_ACCESSIBILITY_STATE_PASSWORD))
        return NSAccessibilitySecureTextFieldSubrole;
    if (node && node->role == UI_ACCESSIBILITY_ROLE_SEARCH_FIELD)
        return NSAccessibilitySearchFieldSubrole;
    return node && node->role == UI_ACCESSIBILITY_ROLE_TAB
        ? NSAccessibilityTabButtonSubrole : nil;
}

- (NSString *)accessibilityIdentifier
{
    return [NSString stringWithFormat:@"wsap-%llu", (unsigned long long)nodeID];
}

- (NSString *)accessibilityLabel
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    return string_or_nil(node ? node->name : NULL);
}

- (NSString *)accessibilityHelp
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    return string_or_nil(node ? node->description : NULL);
}

- (id)accessibilityValue
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    if (!node)
        return nil;
    if (node->role == UI_ACCESSIBILITY_ROLE_CHECKBOX ||
        node->role == UI_ACCESSIBILITY_ROLE_RADIO_BUTTON ||
        node->role == UI_ACCESSIBILITY_ROLE_TOGGLE_BUTTON)
        return @((node->states & UI_ACCESSIBILITY_STATE_CHECKED) != 0);
    return string_or_nil(node->value);
}

- (NSInteger)accessibilityNumberOfCharacters
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    NSString *value = string_or_nil(node ? node->value : NULL);
    if (!value)
        return 0;
    return value.length > (NSUInteger)NSIntegerMax
        ? NSIntegerMax : (NSInteger)value.length;
}

- (NSString *)accessibilityStringForRange:(NSRange)range
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    NSString *value = string_or_nil(node ? node->value : NULL);
    if (!value || range.location > value.length ||
        range.length > value.length - range.location)
        return nil;
    return [value substringWithRange:range];
}

- (NSInteger)accessibilityInsertionPointLineNumber
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    NSString *value = string_or_nil(node ? node->value : NULL);
    if (!node || !value)
        return 0;
    NSUInteger caret = utf16_offset_for_utf8(node->value, node->caret_offset);
    NSUInteger line = 0;
    for (NSUInteger i = 0; i < caret; i++) {
        if ([value characterAtIndex:i] == '\n')
            line++;
    }
    return line > (NSUInteger)NSIntegerMax ? NSIntegerMax : (NSInteger)line;
}

- (id)accessibilityParent
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    if (!node)
        return nil;
    const struct ui_accessibility_node *parent =
        [provider nodeFor:node->parent_id];
    if (!node->parent_id ||
        (parent && parent->role == UI_ACCESSIBILITY_ROLE_WINDOW &&
         !parent->parent_id))
        return provider->view;
    return [provider elementFor:node->parent_id];
}

- (NSArray *)accessibilityChildren
{
    return provider ? [provider childrenOf:nodeID] : @[];
}

- (NSRect)accessibilityFrame
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    YaslView *view = provider ? provider->view : nil;
    if (!node || !view || !view.window)
        return NSZeroRect;

    struct rect2i bounds = accessibility_bounds(node);
    if (provider->surface)
        bounds = rect_to_logical(provider->surface, bounds);
    NSRect local = NSMakeRect((CGFloat)bounds.x,
                              view.bounds.size.height -
                                  ((CGFloat)bounds.y + (CGFloat)bounds.height),
                              (CGFloat)bounds.width,
                              (CGFloat)bounds.height);
    NSRect windowRect = [view convertRect:local toView:nil];
    return [view.window convertRectToScreen:windowRect];
}

- (NSRange)accessibilitySelectedTextRange
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    if (!node || !node->value)
        return NSMakeRange(0, 0);
    NSString *value = string_or_nil(node->value);
    if (!value)
        return NSMakeRange(0, 0);
    size_t begin = node->selection_start;
    size_t end = node->selection_end;
    if (begin == end)
        begin = end = node->caret_offset;
    if (end < begin) {
        size_t swap = begin;
        begin = end;
        end = swap;
    }
    NSUInteger location = utf16_offset_for_utf8(node->value, begin);
    NSUInteger limit = utf16_offset_for_utf8(node->value, end);
    if (limit < location)
        limit = location;
    return NSMakeRange(location, limit - location);
}

- (NSArray *)accessibilitySelectedTextRanges
{
    return @[ [NSValue valueWithRange:[self accessibilitySelectedTextRange]] ];
}

- (NSString *)accessibilitySelectedText
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    NSString *value = string_or_nil(node ? node->value : NULL);
    if (!value)
        return nil;
    NSRange range = [self accessibilitySelectedTextRange];
    if (NSMaxRange(range) > value.length)
        return @"";
    return [value substringWithRange:range];
}

- (void)setAccessibilitySelectedTextRange:(NSRange)range
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    if (!node || !node->value ||
        !(node->actions & UI_ACCESSIBILITY_ACTION_SET_SELECTION))
        return;
    NSString *value = string_or_nil(node->value);
    if (!value)
        return;
    if (range.location == NSNotFound)
        return;
    NSUInteger start = MIN(range.location, value.length);
    NSUInteger end = start + MIN(range.length, value.length - start);
    start = utf16_boundary(value, start);
    end = utf16_boundary(value, end);
    if (end < start)
        end = start;
    size_t startBytes = [[value substringToIndex:start]
        lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    size_t endBytes = [[value substringToIndex:end]
        lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    [provider perform:nodeID action:UI_ACCESSIBILITY_ACTION_SET_SELECTION
                 value:NULL selectionStart:startBytes selectionEnd:endBytes
                 caret:endBytes];
}

- (void)setAccessibilitySelectedTextRanges:(NSArray *)ranges
{
    id range = ranges.count ? ranges[0] : nil;
    if ([range isKindOfClass:[NSValue class]] &&
        strcmp([range objCType], @encode(NSRange)) == 0)
        [self setAccessibilitySelectedTextRange:[range rangeValue]];
}

- (BOOL)accessibilityPerformPress
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    if (!node || !(node->actions & UI_ACCESSIBILITY_ACTION_INVOKE))
        return NO;
    return [provider perform:nodeID action:UI_ACCESSIBILITY_ACTION_INVOKE
                       value:NULL selectionStart:0 selectionEnd:0 caret:0];
}

- (BOOL)accessibilityPerformIncrement
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    if (!node || !(node->actions & UI_ACCESSIBILITY_ACTION_INCREMENT))
        return NO;
    return [provider perform:nodeID action:UI_ACCESSIBILITY_ACTION_INCREMENT
                       value:NULL selectionStart:0 selectionEnd:0 caret:0];
}

- (BOOL)accessibilityPerformDecrement
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    if (!node || !(node->actions & UI_ACCESSIBILITY_ACTION_DECREMENT))
        return NO;
    return [provider perform:nodeID action:UI_ACCESSIBILITY_ACTION_DECREMENT
                       value:NULL selectionStart:0 selectionEnd:0 caret:0];
}

- (void)setAccessibilityValue:(id)value
{
    const struct ui_accessibility_node *node = [provider nodeFor:nodeID];
    if (!node || !(node->actions & UI_ACCESSIBILITY_ACTION_SET_VALUE))
        return;
    NSString *text = [value isKindOfClass:[NSString class]] ? value
                                                            : [value description];
    const char *utf8 = text.UTF8String;
    if (utf8) {
        size_t end = strlen(utf8);
        [provider perform:nodeID action:UI_ACCESSIBILITY_ACTION_SET_VALUE
                     value:utf8 selectionStart:end selectionEnd:end caret:end];
    }
}

- (void)accessibilitySetValue:(id)value forAttribute:(NSString *)attribute
{
    if ([attribute isEqualToString:NSAccessibilityValueAttribute])
        [self setAccessibilityValue:value];
}

@end

@implementation WsapAccessibilityProvider

- (instancetype)initWithView:(YaslView *)nativeView
                    actionSink:(ui_accessibility_action_sink_fn)sink
                    userdata:(void *)userdata
{
    self = [super init];
    if (self) {
        view = nativeView;
        actionSink = sink;
        actionUserdata = userdata;
        elements = [[NSMutableDictionary alloc] init];
    }
    return self;
}

- (void)dealloc
{
    for (WsapAccessibilityElement *element in [elements allValues])
        element->provider = nil;
    ui_accessibility_tree_destroy(&snapshot);
    [elements release];
    [super dealloc];
}

- (const struct ui_accessibility_node *)nodeFor:(uint64_t)identifier
{
    return ui_accessibility_tree_find(&snapshot, identifier);
}

- (WsapAccessibilityElement *)elementFor:(uint64_t)identifier
{
    if (!identifier)
        return nil;
    NSNumber *key = element_key(identifier);
    WsapAccessibilityElement *element = [elements objectForKey:key];
    if (!element) {
        element = [[WsapAccessibilityElement alloc] initWithProvider:self
                                                              nodeID:identifier];
        [elements setObject:element forKey:key];
        [element release];
        element = [elements objectForKey:key];
    }
    return element;
}

- (NSArray *)childrenOf:(uint64_t)parentID
{
    NSMutableArray *children = [NSMutableArray array];
    for (size_t i = 0; i < snapshot.count; i++) {
        const struct ui_accessibility_node *node = &snapshot.nodes[i];
        if (node->parent_id == parentID) {
            if (!parentID && node->role == UI_ACCESSIBILITY_ROLE_WINDOW) {
                [children addObjectsFromArray:[self childrenOf:node->id]];
                continue;
            }
            WsapAccessibilityElement *element = [self elementFor:node->id];
            if (element)
                [children addObject:element];
        }
    }
    if (parentID && children.count > 1) {
        [children sortUsingComparator:^NSComparisonResult(id firstObject,
                                                          id secondObject) {
            WsapAccessibilityElement *a = firstObject;
            WsapAccessibilityElement *b = secondObject;
            uint32_t first = [self nodeFor:a->nodeID]->order;
            uint32_t second = [self nodeFor:b->nodeID]->order;
            return first < second ? NSOrderedAscending :
                   first > second ? NSOrderedDescending : NSOrderedSame;
        }];
    }
    return children;
}

- (BOOL)perform:(uint64_t)identifier
         action:(enum ui_accessibility_action)action
          value:(const char *)value
   selectionStart:(size_t)start
     selectionEnd:(size_t)end
           caret:(size_t)caret
{
    const struct ui_accessibility_node *node = [self nodeFor:identifier];
    if (!actionSink || !node || !(node->actions & action) ||
        (node->states & (UI_ACCESSIBILITY_STATE_DISABLED |
                         UI_ACCESSIBILITY_STATE_HIDDEN)))
        return NO;
    struct ui_accessibility_action_request request = {
        .node_id = identifier,
        .action = action,
        .value = value,
        .caret_offset = caret,
        .selection_start = start,
        .selection_end = end,
    };
    actionSink(actionUserdata, &request);
    return YES;
}

- (BOOL)replaceSnapshot:(const struct ui_accessibility_tree *)tree
{
    bool layoutChanged = snapshot.count != tree->count;
    uint64_t newFocused = 0;
    NSMutableArray *valueChanges = [NSMutableArray array];
    NSMutableArray *selectionChanges = [NSMutableArray array];
    size_t sharedCount = snapshot.count < tree->count
        ? snapshot.count : tree->count;
    for (size_t i = 0; i < tree->count; i++)
        if (tree->nodes[i].states & UI_ACCESSIBILITY_STATE_FOCUSED)
            newFocused = tree->nodes[i].id;
    for (size_t i = 0; i < sharedCount; i++) {
        const struct ui_accessibility_node *old = &snapshot.nodes[i];
        const struct ui_accessibility_node *next = &tree->nodes[i];
        if (old->id != next->id || old->parent_id != next->parent_id ||
            old->order != next->order || old->role != next->role ||
            old->states != next->states || old->actions != next->actions ||
            !rect2i_eq(old->bounds, next->bounds) ||
            !strings_equal(old->name, next->name) ||
            !strings_equal(old->description, next->description))
            layoutChanged = true;
        if (old->id == next->id &&
            !strings_equal(old->value, next->value))
            [valueChanges addObject:element_key(next->id)];
        if (old->id == next->id &&
            (old->caret_offset != next->caret_offset ||
             old->selection_start != next->selection_start ||
             old->selection_end != next->selection_end))
            [selectionChanges addObject:element_key(next->id)];
    }
    if (!layoutChanged && valueChanges.count == 0 && selectionChanges.count == 0 &&
        focusedID == newFocused)
        return YES;

    struct ui_accessibility_tree copy = {0};
    for (size_t i = 0; i < tree->count; i++) {
        const struct ui_accessibility_node *node = &tree->nodes[i];
        const struct ui_accessibility_node_spec spec = {
            .id = node->id,
            .parent_id = node->parent_id,
            .order = node->order,
            .role = node->role,
            .states = node->states,
            .actions = node->actions,
            .bounds = node->bounds,
            .name = node->name,
            .description = node->description,
            .value = node->value,
            .caret_offset = node->caret_offset,
            .selection_start = node->selection_start,
            .selection_end = node->selection_end,
        };
        if (!ui_accessibility_tree_add(&copy, &spec)) {
            ui_accessibility_tree_destroy(&copy);
            return NO;
        }
    }

    uint64_t oldFocused = focusedID;
    focusedID = newFocused;
    ui_accessibility_tree_destroy(&snapshot);
    snapshot = copy;

    NSMutableSet *live = [NSMutableSet setWithCapacity:snapshot.count];
    for (size_t i = 0; i < snapshot.count; i++) {
        const struct ui_accessibility_node *node = &snapshot.nodes[i];
        NSNumber *key = element_key(node->id);
        [live addObject:key];
        WsapAccessibilityElement *element = [self elementFor:node->id];
        [element setAccessibilityRole:role_for_node(node->role)];
        [element setAccessibilityLabel:string_or_nil(node->name)];
        [element setAccessibilityHelp:string_or_nil(node->description)];
    }

    NSArray *oldKeys = [[elements allKeys] copy];
    for (NSNumber *key in oldKeys) {
        if (![live containsObject:key]) {
            WsapAccessibilityElement *element = [elements objectForKey:key];
            element->provider = nil;
            [elements removeObjectForKey:key];
        }
    }
    [oldKeys release];

    if (layoutChanged)
        NSAccessibilityPostNotification(view,
                                        NSAccessibilityLayoutChangedNotification);
    for (NSNumber *key in valueChanges) {
        uint64_t identifier = (uint64_t)[key unsignedLongLongValue];
        NSAccessibilityPostNotification([self elementFor:identifier],
                                        NSAccessibilityValueChangedNotification);
    }
    for (NSNumber *key in selectionChanges) {
        uint64_t identifier = (uint64_t)[key unsignedLongLongValue];
        NSAccessibilityPostNotification(
            [self elementFor:identifier],
            NSAccessibilitySelectedTextChangedNotification);
    }
    if (focusedID != oldFocused) {
        id target = focusedID ? [self elementFor:focusedID] : view;
        if (target)
            NSAccessibilityPostNotification(
                target, NSAccessibilityFocusedUIElementChangedNotification);
    }
    return YES;
}

@end

@interface YaslView (WSAPAccessibility)
- (NSArray *)accessibilityChildren;
- (id)accessibilityFocusedUIElement;
- (id)accessibilityHitTest:(NSPoint)point;
@end

@implementation YaslView (WSAPAccessibility)

- (BOOL)isAccessibilityElement
{
    return NO;
}

- (NSArray *)accessibilityChildren
{
    WsapAccessibilityProvider *provider =
        objc_getAssociatedObject(self, &provider_key);
    return provider ? [provider childrenOf:0] : @[];
}

- (id)accessibilityFocusedUIElement
{
    WsapAccessibilityProvider *provider =
        objc_getAssociatedObject(self, &provider_key);
    return provider && provider->focusedID
        ? [provider elementFor:provider->focusedID] : nil;
}

- (id)accessibilityHitTest:(NSPoint)screenPoint
{
    WsapAccessibilityProvider *provider =
        objc_getAssociatedObject(self, &provider_key);
    if (!provider || !self.window)
        return nil;
    NSPoint windowPoint = [self.window convertPointFromScreen:screenPoint];
    NSPoint point = [self convertPoint:windowPoint fromView:nil];
    CGFloat scale = self.window.backingScaleFactor;
    if (scale < 1.0)
        scale = 1.0;
    struct vector2i devicePoint = {
        .x = (int)(point.x * scale),
        .y = (int)((self.bounds.size.height - point.y) * scale),
    };
    for (size_t i = provider->snapshot.count; i > 0; i--) {
        const struct ui_accessibility_node *node =
            &provider->snapshot.nodes[i - 1];
        if (rect2i_contains(node->bounds, devicePoint))
            return [provider elementFor:node->id];
    }
    return nil;
}

@end

static void *provider_attach(void *native_handle,
                             ui_accessibility_action_sink_fn action_sink,
                             void *action_userdata)
{
    struct yasl_surface *surface = native_handle;
    struct yasl_native native = {0};
    YaslView *view = surface && yasl_surface_get_native(surface, &native) == 0
        ? (YaslView *)native.ns_view : nil;
    if (!view || objc_getAssociatedObject(view, &provider_key))
        return NULL;
    WsapAccessibilityProvider *provider =
        [[WsapAccessibilityProvider alloc] initWithView:view
                                             actionSink:action_sink
                                               userdata:action_userdata];
    if (!provider)
        return NULL;
    provider->surface = surface;
    objc_setAssociatedObject(view, &provider_key, provider,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [provider release];
    return objc_getAssociatedObject(view, &provider_key);
}

static bool provider_publish(void *instance,
                             const struct ui_accessibility_tree *snapshot)
{
    if (!instance || !snapshot)
        return false;
    return [(WsapAccessibilityProvider *)instance replaceSnapshot:snapshot];
}

static void provider_detach(void *instance)
{
    if (!instance)
        return;
    WsapAccessibilityProvider *provider = [(WsapAccessibilityProvider *)instance retain];
    YaslView *view = provider->view;
    provider->view = nil;
    provider->surface = NULL;
    provider->actionSink = NULL;
    provider->actionUserdata = NULL;
    for (WsapAccessibilityElement *element in [provider->elements allValues])
        element->provider = nil;
    [provider->elements removeAllObjects];
    ui_accessibility_tree_destroy(&provider->snapshot);
    objc_setAssociatedObject(view, &provider_key, nil,
                             OBJC_ASSOCIATION_ASSIGN);
    [provider release];
}

static const struct ui_accessibility_provider_ops provider_ops = {
    .attach = provider_attach,
    .publish_snapshot = provider_publish,
    .detach = provider_detach,
};

const struct ui_accessibility_provider_ops *ui_accessibility_provider_ops(void)
{
    return &provider_ops;
}

void *ui_accessibility_native_handle(struct yasl_surface *s)
{
    return s;
}

void ui_accessibility_set_menu_popup_depth(
    const struct ui_accessibility_provider *provider, int depth)
{
    (void)provider;
    (void)depth;
}
