#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include "platform/platform.h"
#include "util/str.h"

@interface WSAPNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation WSAPNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    (void)center;
    (void)notification;
    completionHandler(UNNotificationPresentationOptionBanner);
}
@end

static WSAPNotificationDelegate *notification_delegate;
static bool auth_requested;

/* UNUserNotificationCenter asserts when the executable is not in an app bundle. */
static bool notifications_available(void)
{
    static int available = -1;
    if (available < 0) {
        NSBundle *bundle = [NSBundle mainBundle];
        available = bundle.bundleIdentifier &&
                    [bundle.bundlePath.pathExtension isEqualToString:@"app"];
    }
    return available;
}

void platform_notify(const char *title, const char *body)
{
    if (!notifications_available()) return;
    @autoreleasepool {
        NSString *heading = [NSString stringWithUTF8String:title ? title : ""];
        NSString *message = [NSString stringWithUTF8String:body ? body : ""];
        if (!heading || !message) return;
        UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
        content.title = heading;
        content.body = message;
        UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
        if (!notification_delegate) {
            notification_delegate = [[WSAPNotificationDelegate alloc] init];
            center.delegate = notification_delegate;
        }
        if (!auth_requested) {
            auth_requested = true;
            [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert
                                  completionHandler:^(BOOL granted, NSError *error) {
                (void)granted; (void)error;
            }];
        }
        NSString *identifier = [[NSUUID UUID] UUIDString];
        UNNotificationRequest *request =
            [UNNotificationRequest requestWithIdentifier:identifier
                                                 content:content trigger:nil];
        [center addNotificationRequest:request withCompletionHandler:nil];
        [content release];
    }
}

void platform_open_url(const char *url)
{
    if (!url || !str_url_scheme_allowed(url))
        return;
    @autoreleasepool {
        NSString *str = [NSString stringWithUTF8String:url];
        if (!str)
            return;
        NSURL *ns_url = [NSURL URLWithString:str];
        if (!ns_url)
            return;
        [[NSWorkspace sharedWorkspace] openURL:ns_url];
    }
}

void platform_bell(void)
{
    NSBeep();
}
