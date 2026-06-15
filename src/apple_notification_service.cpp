#include "apple_notification_service.h"

#include <NimBLEDevice.h>
#include <Arduino.h>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define ANCS_SERVICE_UUID             "7905F431-B5CE-4E99-A40F-4B1E122D00D0"
#define ANCS_NOTIFICATION_SOURCE_UUID "9FBF120D-6301-42D9-8C58-25E699A21DBD"
#define ANCS_CONTROL_POINT_UUID       "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9"
#define ANCS_DATA_SOURCE_UUID         "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB"

// CommandID
#define CommandIDGetNotificationAttributes 0
// Notification attribute IDs
#define AttrAppIdentifier 0
#define AttrTitle         1
#define AttrSubtitle      2
#define AttrMessage       3
#define AttrDate          5

// Notification Source EventID
#define EventIDNotificationAdded    0
#define EventIDNotificationModified 1
#define EventIDNotificationRemoved  2

namespace AppleNotificationService
{
    namespace
    {
        const size_t MAX_LIST = 8;

        NotificationCb gCallback;
        NimBLERemoteCharacteristic *gControlPoint = nullptr;

        // All shared state below is touched by the BLE host task (notify callbacks),
        // the loop task (Process), and the HTTP task (GetRecent) — guard with a mutex.
        SemaphoreHandle_t gMutex = nullptr;
        std::vector<NotificationInfo> gList;    // oldest..newest, current ANCS state
        std::vector<uint32_t>         gPending; // UIDs awaiting an attribute fetch

        struct Lock
        {
            Lock()  { if (gMutex) xSemaphoreTake(gMutex, portMAX_DELAY); }
            ~Lock() { if (gMutex) xSemaphoreGive(gMutex); }
        };

        // Caller must hold the lock.
        NotificationInfo *findUid(uint32_t uid)
        {
            for (auto &n : gList)
                if (n.uid == uid)
                    return &n;
            return nullptr;
        }

        // Fetch app id + title + message for a notification (loop task only — a
        // blocking write inside a notify callback deadlocks the NimBLE host).
        void requestAttributes(uint32_t uid)
        {
            if (!gControlPoint)
                return;
            uint8_t cmd[] = {
                CommandIDGetNotificationAttributes,
                (uint8_t)(uid & 0xFF), (uint8_t)((uid >> 8) & 0xFF),
                (uint8_t)((uid >> 16) & 0xFF), (uint8_t)((uid >> 24) & 0xFF),
                AttrAppIdentifier,
                AttrTitle,   0x40, 0x00, // max 64
                AttrMessage, 0x80, 0x00, // max 128
            };
            gControlPoint->writeValue(cmd, sizeof(cmd), true);
        }

        // Notification Source: [EventID][Flags][CategoryID][Count][UID(4, LE)]
        void onNotificationSource(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool)
        {
            if (len < 8)
                return;
            uint8_t  eventId    = data[0];
            uint8_t  categoryId = data[2];
            uint32_t uid = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                           ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);

            if (eventId == EventIDNotificationRemoved)
            {
                Lock l;
                gList.erase(std::remove_if(gList.begin(), gList.end(),
                            [&](const NotificationInfo &n) { return n.uid == uid; }), gList.end());
                return;
            }

            if (eventId == EventIDNotificationAdded || eventId == EventIDNotificationModified)
            {
                {
                    Lock l;
                    NotificationInfo *e = findUid(uid);
                    if (!e)
                    {
                        if (gList.size() >= MAX_LIST)
                            gList.erase(gList.begin()); // drop oldest
                        NotificationInfo n;
                        n.uid = uid;
                        n.categoryId = categoryId;
                        gList.push_back(n);
                    }
                    else
                    {
                        e->categoryId = categoryId;
                    }
                    gPending.push_back(uid); // fetch details from the loop task
                }
                Serial.printf("[ANCS] event=%d uid=%u category=%s\n", eventId, uid, CategoryName(categoryId));
            }
        }

        // Data Source: [CmdID][UID(4)] then tuples [AttrID][len(2,LE)][value...]
        void onDataSource(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool)
        {
            if (len < 5)
                return;
            uint32_t uid = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                           ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);

            std::string appId, title, message;
            size_t i = 5;
            while (i + 3 <= len)
            {
                uint8_t  attrId = data[i];
                uint16_t alen   = (uint16_t)data[i + 1] | ((uint16_t)data[i + 2] << 8);
                i += 3;
                if (i + alen > len)
                    alen = len - i;
                std::string val(reinterpret_cast<char *>(data) + i, alen);
                i += alen;
                switch (attrId)
                {
                case AttrAppIdentifier: appId = val; break;
                case AttrTitle:         title = val; break;
                case AttrMessage:       message = val; break;
                }
            }

            NotificationInfo copy;
            bool have = false;
            {
                Lock l;
                NotificationInfo *e = findUid(uid);
                if (e)
                {
                    e->appId = appId;
                    e->title = title;
                    e->message = message;
                    copy = *e;
                    have = true;
                }
            }
            if (have)
            {
                Serial.printf("[ANCS] %s | %s: %s\n", copy.appId.c_str(), copy.title.c_str(), copy.message.c_str());
                if (gCallback)
                    gCallback(copy);
            }
        }
    } // anonymous namespace

    void RegisterForNotifications(const NotificationCb &callback) { gCallback = callback; }

    std::vector<NotificationInfo> GetRecent()
    {
        Lock l;
        std::vector<NotificationInfo> out(gList.rbegin(), gList.rend()); // newest first
        return out;
    }

    void Process()
    {
        uint32_t uid;
        bool has = false;
        {
            Lock l;
            if (!gPending.empty())
            {
                uid = gPending.front();
                gPending.erase(gPending.begin());
                has = true;
            }
        }
        if (has)
            requestAttributes(uid); // safe: runs on the loop task
    }

    const char *CategoryName(uint8_t c)
    {
        switch (c)
        {
        case 0:  return "Other";
        case 1:  return "IncomingCall";
        case 2:  return "MissedCall";
        case 3:  return "Voicemail";
        case 4:  return "Social";
        case 5:  return "Schedule";
        case 6:  return "Email";
        case 7:  return "News";
        case 8:  return "Health";
        case 9:  return "Business";
        case 10: return "Location";
        case 11: return "Entertainment";
        default: return "Unknown";
        }
    }

    bool StartNotificationService(BLEClient *client)
    {
        if (!client || !client->isConnected())
            return false;

        if (!gMutex)
            gMutex = xSemaphoreCreateMutex();

        // Fresh connection: drop stale state (UIDs are only valid per-connection).
        {
            Lock l;
            gList.clear();
            gPending.clear();
        }

        auto svc = client->getService(ANCS_SERVICE_UUID);
        if (!svc)
        {
            Serial.println("ANCS service not found (iOS may not expose it yet)");
            return false;
        }

        gControlPoint = svc->getCharacteristic(ANCS_CONTROL_POINT_UUID);
        auto dataSource = svc->getCharacteristic(ANCS_DATA_SOURCE_UUID);
        auto notifSource = svc->getCharacteristic(ANCS_NOTIFICATION_SOURCE_UUID);
        if (!gControlPoint || !dataSource || !notifSource)
        {
            Serial.println("ANCS characteristics not found");
            return false;
        }

        dataSource->subscribe(true, onDataSource); // subscribe first so responses aren't missed
        notifSource->subscribe(true, onNotificationSource);
        Serial.println("ANCS started");
        return true;
    }
}
