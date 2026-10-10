// Copyright 2021 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#ifndef NATIVEUI_MAC_NOTIFICATION_CENTER_MAC_H_
#define NATIVEUI_MAC_NOTIFICATION_CENTER_MAC_H_

#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>

#include "nativeui/notification_center.h"

namespace nu {

// Whether this process delivers notifications through the UserNotifications
// framework. Only an application with a bundle identity can: for a bare
// executable the notification center of the framework raises an exception, so
// such processes keep the legacy NSUserNotification path instead (see
// notification_mac.mm).
bool CanUseUserNotifications();

}  // namespace nu

// The delegate of both notification APIs: the legacy NSUserNotificationCenter
// and the UserNotifications framework share the class, and the members used at
// runtime depend on CanUseUserNotifications().
@interface NUNotificationCenterDelegate
    : NSObject<NSUserNotificationCenterDelegate,
               UNUserNotificationCenterDelegate> {
 @private
  nu::NotificationCenter* shell_;
}
- (id)initWithShell:(nu::NotificationCenter*)shell;
- (std::string)infoForNotification:(NSUserNotification*)nsnotification;
@end

#endif  // NATIVEUI_MAC_NOTIFICATION_CENTER_MAC_H_
