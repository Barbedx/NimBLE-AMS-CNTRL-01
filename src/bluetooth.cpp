#include "bluetooth.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <vector>

#include "apple_media_service.h"
#include "apple_notification_service.h"
#include "current_time_service.h"

// Apple Media Service (hosted by the iPhone; we are its client).
#define APPLE_MEDIA_SERVICE_UUID "89D3502B-0F36-433A-8EF4-C502AD55F8DC"

namespace Bluetooth
{
    namespace
    {
        NimBLEServer *Server = nullptr;
        NimBLEClient *Client = nullptr; // GATT client bound to the phone-initiated connection

        volatile bool Connected = false;  // AMS is up
        volatile bool NeedSetup = false;  // phone connected, services not started yet
        volatile bool Secured   = false;  // link encrypted/bonded
        RTC_DATA_ATTR bool TimeSet = false;

        std::string PeerAddr;
        uint32_t    lastSetupAttempt = 0;

        // Bring up AMS + CTS once the link is secured. Returns true on success.
        bool startServices(NimBLEClient *c)
        {
            if (!AppleMediaService::StartMediaService(c))
            {
                Serial.println("StartMediaService not ready yet (will retry)");
                return false;
            }
            Serial.println("AMS started");

            // ANCS (notifications) is optional — don't fail the connection if the
            // phone doesn't expose it; just log and continue.
            AppleNotificationService::StartNotificationService(c);

            CurrentTimeService::CurrentTime ct;
            if (CurrentTimeService::StartTimeService(c, &ct))
            {
                timeval tv;
                tv.tv_sec  = ct.ToTimeT();
                tv.tv_usec = static_cast<long>(ct.mSecondsFraction * 1000000.0f);
                if (settimeofday(&tv, nullptr) == 0)
                    TimeSet = true;
                ct.Dump();
                Serial.println();
            }
            else
            {
                Serial.println("StartTimeService failed (continuing)");
            }
            return true;
        }

        class ServerCallbacks : public NimBLEServerCallbacks
        {
            void onConnect(NimBLEServer *s, NimBLEConnInfo &connInfo) override
            {
                Serial.printf("Phone connected: %s\n", connInfo.getAddress().toString().c_str());
                Client    = s->getClient(connInfo); // client over the inbound connection
                PeerAddr  = connInfo.getAddress().toString();
                Secured   = connInfo.isEncrypted();
                NeedSetup = true;
                Connected = false;
            }

            void onDisconnect(NimBLEServer *s, NimBLEConnInfo &connInfo, int reason) override
            {
                Serial.printf("Phone disconnected: %s reason=%d\n",
                              connInfo.getAddress().toString().c_str(), reason);
                Connected = false;
                NeedSetup = false;
                Secured   = false;
                Client    = nullptr;
                PeerAddr.clear();
                // advertiseOnDisconnect(true) restarts advertising automatically.
            }

            void onAuthenticationComplete(NimBLEConnInfo &connInfo) override
            {
                Serial.printf("Auth complete: bonded=%d encrypted=%d authenticated=%d\n",
                              connInfo.isBonded(), connInfo.isEncrypted(), connInfo.isAuthenticated());
                Secured = connInfo.isEncrypted();
            }
        };

        ServerCallbacks serverCb;

        void startAdvertising(const std::string &name)
        {
            NimBLEAdvertising *adv = Server->getAdvertising();

            // Everything goes in the PRIMARY advertising packet (fits in 31 bytes:
            // 3 flags + 9 name + 18 solicitation = 30). iOS Settings only lists a
            // device whose name is in the primary packet — a scan-response-only name
            // is often hidden. The AMS UUID is advertised as a *solicited* service
            // (AD type 0x15, 128-bit) so iOS knows we want its Apple Media Service
            // (NimBLE issue #1033). AD structure = [len][type=0x15][16 UUID bytes LE].
            NimBLEAdvertisementData advData;
            advData.setFlags(0x06); // LE General Discoverable, BR/EDR not supported
            advData.setName(name);  // Complete Local Name in the primary packet

            NimBLEUUID ams(APPLE_MEDIA_SERVICE_UUID);
            uint8_t sol[18];
            sol[0] = 0x11; // length: 1 (type) + 16 (uuid)
            sol[1] = 0x15; // 128-bit service solicitation
            memcpy(&sol[2], ams.getValue(), 16);
            advData.addData(sol, sizeof(sol));

            adv->setAdvertisementData(advData);

            // Log the exact bytes going on air so we can verify with a BLE scanner.
            std::vector<uint8_t> pl = advData.getPayload();
            Serial.printf("Adv payload (%u bytes): ", (unsigned)pl.size());
            for (uint8_t b : pl) Serial.printf("%02X ", b);
            Serial.println();

            adv->start();
            Serial.printf("Advertising as '%s' — pair from iPhone Settings > Bluetooth\n", name.c_str());
        }
    } // anonymous namespace

    void Begin(const std::string &device_name)
    {
        Serial.println("Bluetooth::Begin() AMS peripheral (pair from phone)");
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

        NimBLEDevice::init(device_name);
        NimBLEDevice::setMTU(247); // larger ATT MTU so ANCS message bodies fit

        // Bonding ON, no MITM, LE Secure Connections -> iOS "Just Works" pairing
        // (single Pair tap), keys persisted in NVS so reconnection is silent.
        NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

        Server = NimBLEDevice::createServer();
        Server->setCallbacks(&serverCb);
        Server->advertiseOnDisconnect(true);
        startAdvertising(device_name);

        Serial.printf("Bluetooth::Begin finished (bonds stored: %d)\n", NimBLEDevice::getNumBonds());
    }

    void Service()
    {
        // Once connected, drive deferred ANCS Control Point writes from this
        // (loop) task — never from inside the notify callback.
        if (Connected && Client && Client->isConnected())
            AppleNotificationService::Process();

        if (!(NeedSetup && Client && Client->isConnected() && !Connected))
            return;

        if (millis() - lastSetupAttempt < 800)
            return;
        lastSetupAttempt = millis();

        // Order matters (NimBLE issue #1033): secure the link first, then discover.
        if (!Secured)
        {
            Serial.println("Securing link — accept the pairing prompt on your iPhone...");
            if (!Client->secureConnection())
            {
                Serial.println("Pairing not complete yet, will retry");
                return;
            }
            Secured = true;
        }

        if (startServices(Client))
        {
            Connected = true;
            NeedSetup = false;
            Serial.println("Connected — AMS + CTS up");
        }
    }

    bool IsConnected() { return Connected && Client && Client->isConnected(); }
    bool IsTimeSet()   { return TimeSet; }
    bool HasBond()     { return NimBLEDevice::getNumBonds() > 0; }

    std::string ConnectedAddress() { return IsConnected() ? PeerAddr : std::string(); }

    void ClearBonds()
    {
        Serial.println("[BLE] Clearing all bonds...");
        NimBLEDevice::deleteAllBonds();
        if (Client && Client->isConnected())
            Client->disconnect();
        Connected = false;
        NeedSetup = false;
        Secured   = false;
        Serial.println("[BLE] Bonds cleared. Also 'Forget this device' for CTRL 01 in iOS Settings, then re-pair.");
    }

} // namespace Bluetooth
