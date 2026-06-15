#pragma once
#include <string>
#include <functional>
#include <vector>
#include <Arduino.h>

class BLEClient;

// Apple Notification Center Service (ANCS): the iPhone pushes notifications
// (messages, calls, app alerts) to us as a bonded peer. Sibling of AMS — runs
// over the same connection. Spec:
// https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/
namespace AppleNotificationService
{
    struct NotificationInfo
    {
        uint8_t     categoryId = 0;
        uint32_t    uid = 0;
        std::string appId;   // e.g. "com.apple.MobileSMS"
        std::string title;   // sender / contact
        std::string message; // body
    };

    using NotificationCb = std::function<void(const NotificationInfo &)>;

    void RegisterForNotifications(const NotificationCb &callback);
    // Recent notifications, newest first (current ANCS state — entries are removed
    // when iOS sends a Removed event). Kept in RAM, capped to a small history.
    std::vector<NotificationInfo> GetRecent();
    const char *CategoryName(uint8_t categoryId);

    bool StartNotificationService(BLEClient *client);

    // Must be called from the main loop (NOT a BLE callback). Performs the
    // deferred Control Point write to fetch attributes for a new notification.
    // Doing that write inside the notify callback deadlocks the NimBLE host.
    void Process();
}
