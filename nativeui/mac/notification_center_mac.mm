// Copyright 2021 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#include "nativeui/mac/notification_center_mac.h"

#include "base/strings/sys_string_conversions.h"

@implementation NUNotificationCenterDelegate

- (id)initWithShell:(nu::NotificationCenter*)shell {
  if ((self = [super init]))
    shell_ = shell;
  return self;
}

- (std::string)infoForNotification:(NSUserNotification*)nsnotification {
  return base::SysNSStringToUTF8([nsnotification userInfo][@"info"]);
}

- (std::string)infoForResponse:(UNNotificationResponse*)response {
  return base::SysNSStringToUTF8(
      response.notification.request.content.userInfo[@"info"]);
}

#pragma mark - NSUserNotificationCenterDelegate

- (BOOL)userNotificationCenter:(NSUserNotificationCenter*)center
     shouldPresentNotification:(NSUserNotification*)nsnotification {
  return YES;
}

- (void)userNotificationCenter:(NSUserNotificationCenter*)center
        didDeliverNotification:(NSUserNotification*)nsnotification {
  shell_->on_notification_show.Emit([self infoForNotification:nsnotification]);
}

- (void)userNotificationCenter:(NSUserNotificationCenter*)center
       didActivateNotification:(NSUserNotification*)nsnotification {
  switch (nsnotification.activationType) {
    case NSUserNotificationActivationTypeNone:
      break;
    case NSUserNotificationActivationTypeContentsClicked:
      shell_->on_notification_click.Emit(
          [self infoForNotification:nsnotification]);
      break;
    case NSUserNotificationActivationTypeReplied:
      shell_->on_notification_reply.Emit(
          [self infoForNotification:nsnotification],
          base::SysNSStringToUTF8([nsnotification response].string));
      break;
    case NSUserNotificationActivationTypeActionButtonClicked: {
      NSDictionary* userInfo = [nsnotification userInfo];
      shell_->on_notification_action.Emit(
          base::SysNSStringToUTF8(userInfo[@"actionInfo"]));
      break;
    }
    case NSUserNotificationActivationTypeAdditionalActionClicked: {
      auto action = [nsnotification additionalActivationAction];
      shell_->on_notification_action.Emit(
          base::SysNSStringToUTF8([action identifier]));
      break;
    }
  }
  [NSUserNotificationCenter.defaultUserNotificationCenter
      removeDeliveredNotification:nsnotification];
}

- (void)userNotificationCenter:(NSUserNotificationCenter*)center
               didDismissAlert:(NSUserNotification*)nsnotification {
  shell_->on_notification_close.Emit([self infoForNotification:nsnotification]);
}

- (void)userNotificationCenter:(NSUserNotificationCenter*)center
    didRemoveDeliveredNotifications:(NSArray*)nsnotifications {
  for (NSUserNotification* n in nsnotifications)
    shell_->on_notification_close.Emit([self infoForNotification:n]);
}

#pragma mark - UNUserNotificationCenterDelegate

// The application is in the foreground: present the notification anyway, which
// is what shouldPresentNotification: answers YES for on the legacy API.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:
             (void (^)(UNNotificationPresentationOptions))completionHandler {
  shell_->on_notification_show.Emit(base::SysNSStringToUTF8(
      notification.request.content.userInfo[@"info"]));
  // The banner and list options arrived in macOS 11; the deployment target is
  // lower, so the older option (deprecated afterwards) serves the rest.
  if (@available(macOS 11.0, *)) {
    completionHandler(UNNotificationPresentationOptionBanner |
                      UNNotificationPresentationOptionList |
                      UNNotificationPresentationOptionSound);
  } else {
    completionHandler(UNNotificationPresentationOptionAlert |
                      UNNotificationPresentationOptionSound);
  }
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
  std::string info = [self infoForResponse:response];
  NSString* action = response.actionIdentifier;
  if ([action isEqualToString:UNNotificationDefaultActionIdentifier]) {
    shell_->on_notification_click.Emit(info);
  } else if ([action isEqualToString:UNNotificationDismissActionIdentifier]) {
    shell_->on_notification_close.Emit(info);
  } else if ([response isKindOfClass:[UNTextInputNotificationResponse class]]) {
    UNTextInputNotificationResponse* text_response =
        (UNTextInputNotificationResponse*)response;
    shell_->on_notification_reply.Emit(
        info, base::SysNSStringToUTF8(text_response.userText));
  } else {
    // The info of an action is registered as its identifier.
    shell_->on_notification_action.Emit(base::SysNSStringToUTF8(action));
  }
  // The legacy API removed the notification on activation; keep that behavior.
  [center removeDeliveredNotificationsWithIdentifiers:
      @[ response.notification.request.identifier ]];
  completionHandler();
}

@end

namespace nu {

void NotificationCenter::Clear() {
  if (CanUseUserNotifications()) {
    [[UNUserNotificationCenter currentNotificationCenter]
        removeAllDeliveredNotifications];
    return;
  }
  [NSUserNotificationCenter.defaultUserNotificationCenter
      removeAllDeliveredNotifications];
}

void NotificationCenter::PlatformInit() {
  center_ = [[NUNotificationCenterDelegate alloc] initWithShell:this];
  if (CanUseUserNotifications())
    [UNUserNotificationCenter currentNotificationCenter].delegate = center_;
  else
    [NSUserNotificationCenter.defaultUserNotificationCenter
        setDelegate:center_];
}

void NotificationCenter::PlatformDestroy() {
  if (CanUseUserNotifications())
    [UNUserNotificationCenter currentNotificationCenter].delegate = nil;
  else
    [NSUserNotificationCenter.defaultUserNotificationCenter setDelegate:nil];
  [center_ release];
}

}  // namespace nu
