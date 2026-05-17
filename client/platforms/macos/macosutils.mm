/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "macosutils.h"
#include "logger.h"

#import <Cocoa/Cocoa.h>
#import <ServiceManagement/ServiceManagement.h>

namespace {
Logger logger("MacOSUtils");
}

// static
NSString* MacOSUtils::appId() {
  NSString* appId = [[NSBundle mainBundle] bundleIdentifier];
  if (!appId) {
    // Fallback. When an unsigned/un-notarized app is executed in
    // command-line mode, it could fail the fetching of its own bundle id.
    appId = @"org.amnezia.AmneziaVPN";
  }

  return appId;
}

// static
QString MacOSUtils::computerName() {
  NSString* name = [[NSHost currentHost] localizedName];
  return QString::fromNSString(name);
}

// static
void MacOSUtils::enableLoginItem(bool startAtBoot) {
  logger.debug() << "Enabling login-item";

  NSString* appId = MacOSUtils::appId();
  Q_ASSERT(appId);

  NSError* error = nil;
  if (startAtBoot) {
    if (![[SMAppService mainAppService] registerAndReturnError: & error]) {
      logger.error() << "Failed to register Amnezia VPN LoginItem: " << error.localizedDescription;
    } else {
      logger.debug() << "Amnezia VPN LoginItem registered successfully.";
    }
  } else {
    if (![[SMAppService mainAppService] unregisterAndReturnError: & error]) {
      logger.error() << "Failed to unregister Amnezia VPN LoginItem: " << error.localizedDescription;
    } else {
      logger.debug() << "LoginItem unregistered successfully.";
    }
  }
}

// static
void MacOSUtils::setDockClickHandler() {
  logger.debug() << "Dock click handling is owned by the Qt application delegate.";
}

void MacOSUtils::hideDockIcon() {
  [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}

void MacOSUtils::showDockIcon() {
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp activateIgnoringOtherApps:YES];
}

void MacOSUtils::patchNSStatusBarSetImageForBigSur() {
  logger.debug() << "Skipping obsolete Qt 5 status bar image patch.";
}
