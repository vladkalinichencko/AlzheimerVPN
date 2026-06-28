/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "macosnetworkwatcher.h"
#include "daemon/macosroutemonitor.h"
#include "leakdetector.h"
#include "logger.h"

#include <QMetaObject>
#include <pthread.h>

#import <CoreWLAN/CoreWLAN.h>
#import <Network/Network.h>

namespace {
Logger logger("MacOSNetworkWatcher");
}

// Global variables for CFRunLoop thread
static pthread_t g_powerThread;
static CFRunLoopRef g_powerRunLoop = nullptr;
static bool g_shouldStopPowerThread = false;
static PowerNotificationsListener* g_powerListener = nullptr;

// Thread function for dedicated CFRunLoop
void* powerMonitoringThread(void* arg) {
    logger.debug() << "Power monitoring thread started";
    
    PowerNotificationsListener* listener = static_cast<PowerNotificationsListener*>(arg);
    
    // Get the runloop for this thread
    g_powerRunLoop = CFRunLoopGetCurrent();
    
    // Register for power notifications in this thread
    listener->registerForNotifications();
    
    // Run the CFRunLoop - this will block until CFRunLoopStop is called
    while (!g_shouldStopPowerThread) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
    }
    
    // Cleanup
    listener->cleanup();
    g_powerRunLoop = nullptr;
    
    logger.debug() << "Power monitoring thread finished";
    return nullptr;
}

@interface MacOSNetworkWatcherDelegate : NSObject <CWEventDelegate> {
  MacOSNetworkWatcher* m_watcher;
}
@end

@implementation MacOSNetworkWatcherDelegate

- (id)initWithObject:(MacOSNetworkWatcher*)watcher {
  self = [super init];
  if (self) {
    m_watcher = watcher;
  }
  return self;
}

- (void)bssidDidChangeForWiFiInterfaceWithName:(NSString*)interfaceName {
  logger.debug() << "BSSID changed!" << QString::fromNSString(interfaceName);

  if (m_watcher) {
    if (m_watcher->checkInterface()) {
      emit m_watcher->networkChanged(QString::fromNSString(interfaceName));
    }
  }
}

@end

void PowerNotificationsListener::registerForNotifications()
{
    logger.debug() << "Registering for system power notifications in dedicated thread";
    
    rootPowerDomain = IORegisterForSystemPower(this, &notifyPortRef, sleepWakeupCallBack, &notifierObj);
    if (rootPowerDomain == IO_OBJECT_NULL) {
        logger.error() << "Failed to register for system power notifications!";
        return;
    }

    // Add the notification port to the current runloop (dedicated thread)
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(notifyPortRef), kCFRunLoopCommonModes);
    logger.debug() << "Power notifications registered successfully";
}

void PowerNotificationsListener::cleanup()
{
    if (notifyPortRef != nullptr) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(notifyPortRef), kCFRunLoopCommonModes);
        IONotificationPortDestroy(notifyPortRef);
        notifyPortRef = nullptr;
    }
    
    if (notifierObj != IO_OBJECT_NULL) {
        IODeregisterForSystemPower(&notifierObj);
        notifierObj = IO_OBJECT_NULL;
    }
    
    if (rootPowerDomain != IO_OBJECT_NULL) {
        IOServiceClose(rootPowerDomain);
        rootPowerDomain = IO_OBJECT_NULL;
    }
}

void PowerNotificationsListener::sleepWakeupCallBack(void *refParam, io_service_t service, natural_t messageType, void *messageArgument)
{
	Q_UNUSED(service)

    auto listener = static_cast<PowerNotificationsListener *>(refParam);

    logger.debug() << "Power callback received, messageType:" << messageType;
    switch (messageType) {
    case kIOMessageCanSystemSleep:
        /* Idle sleep is about to kick in. This message will not be sent for forced sleep.
         * Applications have a chance to prevent sleep by calling IOCancelPowerChange.
         * Most applications should not prevent idle sleep. Power Management waits up to
         * 30 seconds for you to either allow or deny idle sleep. If you don’t acknowledge
         * this power change by calling either IOAllowPowerChange or IOCancelPowerChange,
         * the system will wait 30 seconds then go to sleep.
         */

        logger.debug() << "System power message: can system sleep?";

        // Uncomment to cancel idle sleep
        // IOCancelPowerChange(thiz->rootPowerDomain, reinterpret_cast<long>(messageArgument));

        // Allow idle sleep
        IOAllowPowerChange(listener->rootPowerDomain, reinterpret_cast<long>(messageArgument));
        break;

    case kIOMessageSystemWillNotSleep:
        /* Announces that the system has retracted a previous attempt to sleep; it
         * follows `kIOMessageCanSystemSleep`.
         */
        logger.debug() << "System power message: system will NOT sleep.";
        break;

    case kIOMessageSystemWillSleep:
        /* The system WILL go to sleep. If you do not call IOAllowPowerChange or
         * IOCancelPowerChange to acknowledge this message, sleep will be delayed by
         * 30 seconds.
         *
         * NOTE: If you call IOCancelPowerChange to deny sleep it returns kIOReturnSuccess,
         * however the system WILL still go to sleep.
         */

        logger.debug() << "System power message: system WILL sleep";
        if (listener->m_watcher) {
            listener->m_watcher->prepareForSleep();
        }
        IOAllowPowerChange(listener->rootPowerDomain, reinterpret_cast<long>(messageArgument));
        break;

    case kIOMessageSystemWillPowerOn:
        /* Announces that the system is beginning to power the device tree; most devices
         * are still unavailable at this point.
         */
        /* From the documentation:
         *
         * - kIOMessageSystemWillPowerOn is delivered at early wakeup time, before most hardware
         * has been powered on. Be aware that any attempts to access disk, network, the display,
         * etc. may result in errors or blocking your process until those resources become
         * available.
         *
         * So we do NOT log this event.
         */
        break;

    case kIOMessageSystemHasPoweredOn:
        logger.debug() << "System has powered on - waiting for physical default route";
        if (listener->m_watcher) {
            QMetaObject::invokeMethod(listener->m_watcher, [watcher = listener->m_watcher]() {
                watcher->requestWakeup();
            }, Qt::QueuedConnection);
        }
        break;

    default:
        logger.debug() << "System power message: other event: " << messageType;
        /* Not a system sleep and wake notification. */
        break;
    }
}

MacOSNetworkWatcher::MacOSNetworkWatcher(QObject* parent)
    : IOSNetworkWatcher(parent),
      m_routeMonitor(new MacosRouteMonitor(QString(), this)),
      m_powerlistener(this) {
  MZ_COUNT_CTOR(MacOSNetworkWatcher);
  connect(m_routeMonitor, &MacosRouteMonitor::physicalDefaultRouteChanged,
          this, &MacOSNetworkWatcher::physicalDefaultRouteChanged);
}

void MacOSNetworkWatcher::prepareForSleep() {
  m_wakeupPending = false;
}

void MacOSNetworkWatcher::requestWakeup() {
  m_wakeupPending = true;
  m_routeMonitor->refreshPhysicalDefaultRoutes();
  physicalDefaultRouteChanged(m_routeMonitor->hasPhysicalDefaultRoute());
}

bool MacOSNetworkWatcher::physicalNetworkReady() const {
  return m_routeMonitor->hasPhysicalDefaultRoute();
}

void MacOSNetworkWatcher::physicalDefaultRouteChanged(bool ready) {
  emit physicalNetworkReadyChanged(ready);
  if (ready && m_wakeupPending) {
    m_wakeupPending = false;
    logger.debug() << "Physical default route ready after wakeup";
    emit wakeup();
  }
}

MacOSNetworkWatcher::~MacOSNetworkWatcher() {
  MZ_COUNT_DTOR(MacOSNetworkWatcher);
  
  // Stop the dedicated power monitoring thread
  if (g_powerListener) {
    logger.debug() << "Stopping dedicated power monitoring thread";
    g_shouldStopPowerThread = true;
    
    if (g_powerRunLoop) {
      CFRunLoopStop(g_powerRunLoop);
    }
    
    // Wait for thread to finish
    pthread_join(g_powerThread, nullptr);
    g_powerListener = nullptr;
  }
  
  if (m_delegate) {
    CWWiFiClient* client = CWWiFiClient.sharedWiFiClient;
    if (!client) {
      logger.debug() << "Unable to retrieve the CWWiFiClient shared instance";
      return;
    }

    [client stopMonitoringAllEventsAndReturnError:nullptr];
    [static_cast<MacOSNetworkWatcherDelegate*>(m_delegate) dealloc];
    m_delegate = nullptr;
  }
}

void MacOSNetworkWatcher::start() {
  NetworkWatcherImpl::start();

  checkInterface();

  if (m_delegate) {
    logger.debug() << "Delegate already registered";
    return;
  }
  
  // Start dedicated power monitoring thread with CFRunLoop
  if (!g_powerListener) {
    g_powerListener = &m_powerlistener;
    g_shouldStopPowerThread = false;
    
    int result = pthread_create(&g_powerThread, nullptr, powerMonitoringThread, &m_powerlistener);
    if (result != 0) {
      logger.error() << "Failed to create power monitoring thread:" << result;
      g_powerListener = nullptr;
    } else {
      logger.debug() << "Power monitoring enabled";
    }
  } 

  CWWiFiClient* client = CWWiFiClient.sharedWiFiClient;
  if (!client) {
    logger.error() << "Unable to retrieve the CWWiFiClient shared instance";
    return;
  }

  logger.debug() << "Registering delegate";
  m_delegate = [[MacOSNetworkWatcherDelegate alloc] initWithObject:this];
  [client setDelegate:static_cast<MacOSNetworkWatcherDelegate*>(m_delegate)];
  [client startMonitoringEventWithType:CWEventTypeBSSIDDidChange error:nullptr];
  
  logger.debug() << "MacOSNetworkWatcher started successfully";
}

bool MacOSNetworkWatcher::checkInterface() {
  logger.debug() << "Checking interface";

  if (!isActive()) {
    logger.debug() << "Feature disabled";
    return false;
  }

  CWWiFiClient* client = CWWiFiClient.sharedWiFiClient;
  if (!client) {
    logger.debug() << "Unable to retrieve the CWWiFiClient shared instance";
    return false;
  }

  for (CWInterface* interface in client.interfaces) {
    if (!interface.ssid) {
      continue;
    }

    logger.debug() << "Found active WiFi connection on"
                   << QString::fromNSString(interface.interfaceName)
                   << "SSID:" << QString::fromNSString(interface.ssid);
    return true;
  }

  logger.debug() << "No active WiFi connection found";
  return false;
}
