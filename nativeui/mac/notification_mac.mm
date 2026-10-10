// Copyright 2021 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#include "nativeui/notification.h"

#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>

#include "base/logging.h"
#include "base/apple/scoped_nsobject.h"
#include "base/strings/sys_string_conversions.h"
#include "nativeui/gfx/image.h"
#include "nativeui/mac/notification_center_mac.h"

namespace nu {

namespace {

// Identifier of the reply action. A notification with a reply button carries
// this action in its category; the delegate recognizes the reply by the type of
// the response instead of by this identifier.
NSString* const kReplyActionIdentifier = @"yue.notification.reply";

}  // namespace

bool CanUseUserNotifications() {
  // The framework delivers notifications only for an application with a bundle
  // identity: for a bare executable its notification center raises an
  // exception, and the identity carrying APIs are unavailable. Such processes
  // keep the legacy path - a process the system does not know cannot show
  // notifications, whichever API it calls.
  return [[NSBundle mainBundle] bundleIdentifier] != nil;
}

void Notification::Show() {
  if (!CanUseUserNotifications()) {
    [NSUserNotificationCenter.defaultUserNotificationCenter
        deliverNotification:notification_];
    return;
  }

  NSString* identifier = base::SysUTF8ToNSString(mac_content_.identifier);
  base::apple::scoped_nsobject<UNMutableNotificationContent> content(
      [[UNMutableNotificationContent alloc] init]);
  [content setTitle:base::SysUTF8ToNSString(mac_content_.title)];
  [content setBody:base::SysUTF8ToNSString(mac_content_.body)];
  if (!mac_content_.silent)
    [content setSound:[UNNotificationSound defaultSound]];
  [content setUserInfo:@{
    @"info": base::SysUTF8ToNSString(mac_content_.info),
    @"actionInfo": @"",
  }];
  if (!mac_content_.image_path.empty()) {
    base::apple::scoped_nsobject<NSURL> url([[NSURL alloc]
        initFileURLWithPath:base::SysUTF8ToNSString(mac_content_.image_path)]);
    NSError* error = nil;
    UNNotificationAttachment* attachment = [UNNotificationAttachment
        attachmentWithIdentifier:@"image"
                             URL:url.get()
                         options:nil
                           error:&error];
    if (attachment)
      [content setAttachments:@[ attachment ]];
    else
      LOG(ERROR) << "Failed to attach notification image: "
                 << mac_content_.image_path;
  }

  // The actions and the reply button of a notification live in a category which
  // has to be registered before the request is added; using the notification
  // identifier as the category identifier keeps the two in sync.
  NSMutableArray* actions = [NSMutableArray array];
  if (mac_content_.has_reply) {
    UNTextInputNotificationAction* reply = [UNTextInputNotificationAction
        actionWithIdentifier:kReplyActionIdentifier
                       title:NSLocalizedString(@"Reply", nil)
                     options:UNNotificationActionOptionNone
        textInputButtonTitle:NSLocalizedString(@"Send", nil)
        textInputPlaceholder:base::SysUTF8ToNSString(
                                   mac_content_.response_placeholder)];
    [actions addObject:reply];
  }
  for (const Action& action : mac_content_.actions) {
    // The info of an action is its identifier, which is how the delegate tells
    // the actions apart.
    NSString* action_identifier = base::SysUTF8ToNSString(action.info);
    NSString* action_title = base::SysUTF8ToNSString(action.title);
    UNNotificationAction* nsaction = [UNNotificationAction
        actionWithIdentifier:action_identifier
                       title:action_title
                     options:UNNotificationActionOptionNone];
    [actions addObject:nsaction];
  }
  [content setCategoryIdentifier:identifier];
  UNUserNotificationCenter* center =
      [UNUserNotificationCenter currentNotificationCenter];
  [center
      setNotificationCategories:[NSSet setWithObject:
          [UNNotificationCategory categoryWithIdentifier:identifier
                                                 actions:actions
                                       intentIdentifiers:@[]
                                                options:
                          UNNotificationCategoryOptionCustomDismissAction]]];

  UNNotificationRequest* request =
      [UNNotificationRequest requestWithIdentifier:identifier
                                           content:content.get()
                                           trigger:nil];
  [center addNotificationRequest:request
           withCompletionHandler:^(NSError* error) {
    if (error) {
      LOG(ERROR) << "Failed to show notification: "
                 << base::SysNSStringToUTF8(error.localizedDescription);
    }
  }];
}

void Notification::Close() {
  if (!CanUseUserNotifications()) {
    [NSUserNotificationCenter.defaultUserNotificationCenter
        removeDeliveredNotification:notification_];
    return;
  }
  [[UNUserNotificationCenter currentNotificationCenter]
      removeDeliveredNotificationsWithIdentifiers:
          @[ base::SysUTF8ToNSString(mac_content_.identifier) ]];
}

void Notification::SetTitle(const std::string& title) {
  mac_content_.title = title;
  [notification_ setTitle:base::SysUTF8ToNSString(title)];
}

void Notification::SetBody(const std::string& body) {
  mac_content_.body = body;
  [notification_ setInformativeText:base::SysUTF8ToNSString(body)];
}

void Notification::SetInfo(const std::string& info) {
  mac_content_.info = info;
  [notification_ setUserInfo:@{
      @"info": base::SysUTF8ToNSString(info),
      @"actionInfo": [notification_ userInfo][@"actionInfo"],
  }];
}

std::string Notification::GetInfo() const {
  return base::SysNSStringToUTF8([notification_ userInfo][@"info"]);
}

void Notification::SetSilent(bool silent) {
  mac_content_.silent = silent;
  [notification_ setSoundName:(silent ? nil
                                      : NSUserNotificationDefaultSoundName)];
}

void Notification::SetImagePath(const base::FilePath& path) {
  mac_content_.image_path = path.value();
  NSString* imagePath = base::SysUTF8ToNSString(path.value());
  base::apple::scoped_nsobject<NSImage> image(
      [[NSImage alloc] initByReferencingFile:imagePath]);
  [notification_ setContentImage:image.get()];
}

void Notification::SetHasReplyButton(bool has) {
  mac_content_.has_reply = has;
  [notification_ setHasReplyButton:has];
}

void Notification::SetResponsePlaceholder(const std::string& placeholder) {
  mac_content_.response_placeholder = placeholder;
  [notification_ setResponsePlaceholder:base::SysUTF8ToNSString(placeholder)];
}

void Notification::SetIdentifier(const std::string& identifier) {
  mac_content_.identifier = identifier;
  [notification_ setIdentifier:base::SysUTF8ToNSString(identifier)];
}

std::string Notification::GetIdentifier() const {
  return base::SysNSStringToUTF8([notification_ identifier]);
}

void Notification::SetActions(const std::vector<Action>& actions) {
  mac_content_.actions = actions;
  if (actions.empty()) {
    [notification_ setHasActionButton:NO];
    [notification_ setAdditionalActions:@[]];
    return;
  }

  // When having reply button, the action button does not work and we have to
  // put all actions as additionalActions.
  // Otherwise make the first action as actionButton, and the reset as
  // additionalActions.
  size_t first_as_action = ![notification_ hasReplyButton];
  if (first_as_action) {
    const Action& a = actions[0];
    [notification_ setHasActionButton:YES];
    [notification_ setActionButtonTitle:base::SysUTF8ToNSString(a.title)];
    [notification_ setUserInfo:@{
        @"info": [notification_ userInfo][@"info"],
        @"actionInfo": base::SysUTF8ToNSString(a.info),
    }];
    if (actions.size() == 1) {
      [notification_ setAdditionalActions:@[]];
      return;
    }
  }

  auto* nsactions = [NSMutableArray arrayWithCapacity:actions.size()];
  for (size_t i = first_as_action ? 1 : 0; i < actions.size(); ++i) {
    const auto& action = actions[i];
    auto* nsaction = [NSUserNotificationAction
        actionWithIdentifier:base::SysUTF8ToNSString(action.info)
                       title:base::SysUTF8ToNSString(action.title)];
    [nsactions addObject:nsaction];
  }
  [notification_ setAdditionalActions:nsactions];
}

void Notification::PlatformInit() {
  notification_ = [[NSUserNotification alloc] init];
  [notification_ setHasActionButton:NO];
  [notification_ setUserInfo:@{@"info": @"", @"actionInfo": @""}];

  // The identifier names the notification in the notification center. Without a
  // bundle identifier the prefix would read "(null)", which is of no use when
  // debugging, so a neutral one is used instead.
  NSString* prefix = [[NSBundle mainBundle] bundleIdentifier];
  if (prefix == nil)
    prefix = @"yue";
  NSString* identifier = [NSString
      stringWithFormat:@"%@.notification.%@",
                       prefix, [[NSUUID UUID] UUIDString]];
  [notification_ setIdentifier:identifier];
  mac_content_.identifier = base::SysNSStringToUTF8(identifier);

  if (CanUseUserNotifications()) {
    // The framework delivers nothing before the user has authorized the
    // application, so the request is made as early as possible; the system only
    // shows the prompt once.
    [[UNUserNotificationCenter currentNotificationCenter]
        requestAuthorizationWithOptions:UNAuthorizationOptionAlert |
                                        UNAuthorizationOptionSound |
                                        UNAuthorizationOptionBadge
                      completionHandler:^(BOOL granted, NSError* error) {
      if (!granted)
        LOG(WARNING) << "Notifications are not authorized for this "
                        "application, the user can enable them in System "
                        "Settings";
    }];
  }
}

void Notification::PlatformDestroy() {
  [notification_ release];
}

void Notification::PlatformSetImage(Image* image) {
  [notification_ setContentImage:(image ? image->GetNative() : nil)];
}

}  // namespace nu
