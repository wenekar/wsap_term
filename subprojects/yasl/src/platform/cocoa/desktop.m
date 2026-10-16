#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

#include "common/desktop.h"
#include "common/private_desktop.h"

#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t copy_str(char *buf, size_t cap, const char *s)
{
    size_t len = s ? strlen(s) : 0;
    if (!len || len >= cap) return 0;
    memcpy(buf, s, len + 1);
    return len;
}

bool desktop_open_uri(const char *uri)
{
    @autoreleasepool {
        NSString *str = [NSString stringWithUTF8String:uri];
        NSURL *url = str ? [NSURL URLWithString:str] : nil;
        return url && [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

/* Show banners while the application is in front, too. */
@interface YaslNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation YaslNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    (void)center;
    (void)notification;
    completionHandler(UNNotificationPresentationOptionBanner);
}
@end

static YaslNotificationDelegate *notification_delegate;
/* Main-thread state. Notifications made before the answer to the one
 * authorization request wait in the queue and are posted when it is granted. */
static NSMutableArray<UNNotificationRequest *> *notification_queue;
static bool authorization_requested;
static bool authorization_granted;
static bool authorization_denied;

/* UNUserNotificationCenter raises an exception outside an app bundle. */
static bool notifications_available(void)
{
    NSBundle *bundle = [NSBundle mainBundle];
    return bundle.bundleIdentifier && [bundle.bundlePath.pathExtension isEqualToString:@"app"];
}

bool desktop_notify(const char *app_id, const char *title, const char *body)
{
    (void)app_id;
    if (!notifications_available()) {
        errno = ENOTSUP;
        return false;
    }
    if (authorization_denied) {
        errno = EACCES;
        return false;
    }
    @autoreleasepool {
        NSString *heading = [NSString stringWithUTF8String:title];
        NSString *message = [NSString stringWithUTF8String:body];
        if (!heading || !message) {
            errno = EINVAL;
            return false;
        }
        UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
        if (!notification_delegate) {
            notification_delegate = [[YaslNotificationDelegate alloc] init];
            center.delegate = notification_delegate;
        }
        UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
        content.title = heading;
        content.body = message;
        UNNotificationRequest *request =
            [UNNotificationRequest requestWithIdentifier:[[NSUUID UUID] UUIDString]
                                                 content:content
                                                 trigger:nil];
        [content release];
        if (authorization_granted) {
            [center addNotificationRequest:request withCompletionHandler:nil];
            return true;
        }
        if (!notification_queue)
            notification_queue = [[NSMutableArray alloc] init];
        [notification_queue addObject:request];
        if (!authorization_requested) {
            authorization_requested = true;
            [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert
                                  completionHandler:^(BOOL granted, NSError *error) {
                (void)error;
                /* The handler runs on a background queue; the state is main-thread only. */
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (!granted) {
                        authorization_denied = true;
                        [notification_queue removeAllObjects];
                        return;
                    }
                    authorization_granted = true;
                    UNUserNotificationCenter *granted_center = [UNUserNotificationCenter currentNotificationCenter];
                    for (UNNotificationRequest *queued in notification_queue)
                        [granted_center addNotificationRequest:queued withCompletionHandler:nil];
                    [notification_queue removeAllObjects];
                });
            }];
        }
        return true;
    }
}

uint32_t desktop_appearance(void)
{
    @autoreleasepool {
        uint32_t flags = 0;
        NSAppearanceName name = [NSApp.effectiveAppearance
            bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
        if ([name isEqualToString:NSAppearanceNameDarkAqua])
            flags |= YASL_PRIVATE_APPEARANCE_DARK;
        else if (name)
            flags |= YASL_PRIVATE_APPEARANCE_LIGHT;
        if ([NSWorkspace sharedWorkspace].accessibilityDisplayShouldIncreaseContrast)
            flags |= YASL_PRIVATE_APPEARANCE_HIGH_CONTRAST;
        return flags;
    }
}

size_t desktop_executable_path(char *buf, size_t cap)
{
    char raw[PATH_MAX];
    uint32_t size = sizeof(raw);
    char resolved[PATH_MAX];
    if (_NSGetExecutablePath(raw, &size) != 0 || !realpath(raw, resolved)) return 0;
    return copy_str(buf, cap, resolved);
}

size_t desktop_resource_dir(char *buf, size_t cap)
{
    @autoreleasepool {
        NSBundle *bundle = [NSBundle mainBundle];
        if (!bundle.bundleIdentifier || ![bundle.bundlePath.pathExtension isEqualToString:@"app"])
            return 0;
        NSString *path = bundle.resourcePath;
        return path ? copy_str(buf, cap, path.fileSystemRepresentation) : 0;
    }
}

size_t desktop_default_locale(char *buf, size_t cap)
{
    /* Identifiers such as "zh-Hans_CN" are not POSIX names, so build the name
     * from the language and region codes alone. */
    char name[64] = "en_US.UTF-8";
    CFLocaleRef locale = CFLocaleCopyCurrent();
    if (locale) {
        CFStringRef language = CFLocaleGetValue(locale, kCFLocaleLanguageCode);
        CFStringRef region = CFLocaleGetValue(locale, kCFLocaleCountryCode);
        char lang[16], reg[16];
        if (language && CFStringGetCString(language, lang, sizeof(lang), kCFStringEncodingUTF8) && lang[0]) {
            if (region && CFStringGetCString(region, reg, sizeof(reg), kCFStringEncodingUTF8) && reg[0])
                snprintf(name, sizeof(name), "%s_%s.UTF-8", lang, reg);
            else
                snprintf(name, sizeof(name), "%s.UTF-8", lang);
        }
        CFRelease(locale);
    }
    return copy_str(buf, cap, name);
}
