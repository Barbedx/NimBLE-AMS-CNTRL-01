#include "bluetooth.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <time.h>
#include <sys/time.h>
#include <algorithm>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "apple_media_service.h"
#include "current_time_service.h"

#define APPLE_COMPANY_ID 0x004C

namespace Bluetooth
{
    namespace
    {
        // --- Connection state ---
        bool Ended = true;
        bool Connected = false;

        BLEClient *Client = nullptr; // NimBLE client (BLEClient macro)
        RTC_DATA_ATTR bool TimeSet = false;

        // --- Discovery candidate list (shared with the setup portal) ---
        struct CandidateEntry
        {
            NimBLEAddress addr;
            std::string   name;
            int           rssi;
            bool          isApple;
            bool          bonded;
        };

        SemaphoreHandle_t StateMutex = nullptr;
        std::vector<CandidateEntry> Candidates; // guarded by StateMutex

        // Pending connect request, set by the scan callback (bonded auto-connect)
        // or by the portal (user pick). Guarded by StateMutex.
        bool          ConnectPending = false;
        NimBLEAddress PendingAddr;

        uint32_t lastReconnectAttempt = 0;

        struct Lock
        {
            Lock() { if (StateMutex) xSemaphoreTake(StateMutex, portMAX_DELAY); }
            ~Lock() { if (StateMutex) xSemaphoreGive(StateMutex); }
        };

        void startScan(); // fwd

        void requestConnect(const NimBLEAddress &addr)
        {
            Lock l;
            PendingAddr = addr;
            ConnectPending = true;
        }

        // --- Scan callbacks ---
        class AmsScanCallbacks : public NimBLEScanCallbacks
        {
        public:
            void onResult(const NimBLEAdvertisedDevice *dev) override
            {
                if (!dev->isConnectable())
                    return;

                const NimBLEAddress addr = dev->getAddress();

                // Apple company id (first two bytes of manufacturer data).
                bool isApple = false;
                const std::string mfg = dev->getManufacturerData();
                if (mfg.size() >= 2)
                {
                    uint16_t company = (uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8);
                    isApple = (company == APPLE_COMPANY_ID);
                }

                const bool bonded = NimBLEDevice::isBonded(addr);

                // Record / refresh the candidate for the portal.
                {
                    Lock l;
                    auto it = std::find_if(Candidates.begin(), Candidates.end(),
                                           [&](const CandidateEntry &c) { return c.addr == addr; });
                    if (it == Candidates.end())
                    {
                        if (Candidates.size() < 40)
                            Candidates.push_back({addr, dev->getName(), dev->getRSSI(), isApple, bonded});
                    }
                    else
                    {
                        it->rssi = dev->getRSSI();
                        it->bonded = bonded;
                        if (!dev->getName().empty())
                            it->name = dev->getName();
                    }
                }

                // If this is a device we are already bonded to, reconnect to it
                // automatically (this is the "remembers my iPhone" path).
                if (bonded && !Connected)
                {
                    bool already;
                    { Lock l; already = ConnectPending; }
                    if (!already)
                    {
                        Serial.print("Bonded device in range, reconnecting: ");
                        Serial.println(addr.toString().c_str());
                        NimBLEDevice::getScan()->stop();
                        requestConnect(addr);
                    }
                }
            }

            void onScanEnd(const NimBLEScanResults &results, int reason) override
            {
                // Keep scanning while we are disconnected and have no pending work.
                if (!Ended && !Connected)
                {
                    bool pending;
                    { Lock l; pending = ConnectPending; }
                    if (!pending)
                        startScan();
                }
            }
        };

        // --- Client callbacks ---
        class AmsClientCallbacks : public NimBLEClientCallbacks
        {
        public:
            void onDisconnect(NimBLEClient *pClient, int reason) override
            {
                Serial.printf("Client disconnected, reason=%d\n", reason);
                Connected = false;
            }

            void onConnectFail(NimBLEClient *pClient, int reason) override
            {
                Serial.printf("Client connect failed, reason=%d\n", reason);
            }

            void onAuthenticationComplete(NimBLEConnInfo &connInfo) override
            {
                Serial.printf("Auth complete: bonded=%d encrypted=%d authenticated=%d\n",
                              connInfo.isBonded(), connInfo.isEncrypted(), connInfo.isAuthenticated());
            }
        };

        AmsScanCallbacks   scanCb;
        AmsClientCallbacks clientCb;

        void startScan()
        {
            NimBLEScan *scan = NimBLEDevice::getScan();
            if (!scan)
                return;
            if (scan->isScanning())
                return;
            scan->start(0, /*isContinue=*/false, /*restart=*/false); // forever
        }

        void teardownClient()
        {
            if (Client)
            {
                if (Client->isConnected())
                    Client->disconnect();
                NimBLEDevice::deleteClient(Client);
                Client = nullptr;
            }
            Connected = false;
        }

        // Connect, secure (pair/bond), and start AMS + CTS on the given address.
        bool connectTo(const NimBLEAddress &addr)
        {
            NimBLEScan *scan = NimBLEDevice::getScan();
            if (scan && scan->isScanning())
                scan->stop();

            teardownClient();

            Serial.print("Connecting to: ");
            Serial.println(addr.toString().c_str());

            Client = NimBLEDevice::createClient();
            if (!Client)
            {
                Serial.println("Failed to create client");
                return false;
            }
            Client->setClientCallbacks(&clientCb, false);

            if (!Client->connect(addr))
            {
                Serial.println("connect() failed");
                teardownClient();
                return false;
            }

            // Encrypt + pair/bond. With bonding enabled iOS prompts only on the
            // first pairing; afterwards the stored keys make this silent.
            if (!Client->secureConnection())
            {
                Serial.println("secureConnection() failed");
                // A stale bond (we kept keys, the phone forgot them) wedges
                // reconnection. Drop our copy so the next attempt pairs fresh.
                if (NimBLEDevice::isBonded(addr))
                {
                    Serial.println("Deleting stale bond to allow re-pairing");
                    NimBLEDevice::deleteBond(addr);
                }
                teardownClient();
                return false;
            }

            if (!AppleMediaService::StartMediaService(Client))
            {
                Serial.println("StartMediaService failed (not an AMS device?)");
                teardownClient();
                return false;
            }
            Serial.println("AMS started");

            CurrentTimeService::CurrentTime ct;
            if (CurrentTimeService::StartTimeService(Client, &ct))
            {
                timeval new_time;
                new_time.tv_sec = ct.ToTimeT();
                new_time.tv_usec = static_cast<long>(ct.mSecondsFraction * 1000000.0f);
                if (settimeofday(&new_time, nullptr) == 0)
                    TimeSet = true;
                else
                    Serial.println("Error setting time of day");
                ct.Dump();
                Serial.println();
            }
            else
            {
                Serial.println("StartTimeService failed (continuing)");
            }

            Connected = true;
            Serial.println("Connected and services started");
            return true;
        }

    } // anonymous namespace

    // ---------------------------------------------------------------------
    // Public API
    // ---------------------------------------------------------------------

    void Begin(const std::string &device_name)
    {
        Serial.println("Bluetooth::Begin() AMS central + bonding");
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

        if (!StateMutex)
            StateMutex = xSemaphoreCreateMutex();

        Ended = false;
        Connected = false;
        ConnectPending = false;

        NimBLEDevice::init(device_name);

        // Bonding ON, no MITM, LE Secure Connections -> iOS "Just Works" pairing
        // whose keys are persisted in NVS, so reconnection is automatic/silent.
        NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

        NimBLEScan *scan = NimBLEDevice::getScan();
        scan->setScanCallbacks(&scanCb);
        scan->setActiveScan(true);
        scan->setInterval(45);
        scan->setWindow(30);
        scan->setDuplicateFilter(false);
        scan->setFilterPolicy(BLE_HCI_SCAN_FILT_NO_WL);
        startScan();

        Serial.printf("Bluetooth::Begin finished (bonds stored: %d)\n", NimBLEDevice::getNumBonds());
    }

    void End()
    {
        Serial.println("Bluetooth::End()");
        Ended = true;

        NimBLEScan *scan = NimBLEDevice::getScan();
        if (scan)
            scan->stop();

        teardownClient();
        NimBLEDevice::deinit(true);
    }

    void Service()
    {
        if (Ended)
            return;

        // Detect a dropped link and clean up.
        if (Client && !Client->isConnected())
        {
            Serial.println("Service: cleaning up dropped client");
            teardownClient();
            { Lock l; ConnectPending = false; }
        }

        // Honour a pending connect request (user pick or bonded auto-connect).
        bool doConnect = false;
        NimBLEAddress target;
        {
            Lock l;
            if (ConnectPending && !Connected)
            {
                doConnect = true;
                target = PendingAddr;
                ConnectPending = false;
            }
        }
        if (doConnect)
        {
            if (!connectTo(target))
                startScan(); // failed, resume discovery
            return;
        }

        if (!Connected)
        {
            // Belt-and-suspenders reconnect: if bonded but the scan hasn't
            // surfaced the phone (RPA not resolved), try the stored identity
            // directly every few seconds.
            int bonds = NimBLEDevice::getNumBonds();
            if (bonds > 0 && (millis() - lastReconnectAttempt > 8000))
            {
                lastReconnectAttempt = millis();
                requestConnect(NimBLEDevice::getBondedAddress(0));
                return;
            }
            startScan(); // keep discovery alive for the portal
        }
    }

    bool IsConnected()
    {
        return Connected && Client && Client->isConnected();
    }

    bool IsTimeSet() { return TimeSet; }

    bool HasBond() { return NimBLEDevice::getNumBonds() > 0; }

    std::string ConnectedAddress()
    {
        if (IsConnected())
            return Client->getPeerAddress().toString();
        return "";
    }

    void ClearBonds()
    {
        Serial.println("[BLE] Clearing all bonds...");
        NimBLEDevice::deleteAllBonds();
        teardownClient();
        { Lock l; ConnectPending = false; }
        lastReconnectAttempt = 0;
        startScan();
        Serial.println("[BLE] Bonds cleared. On iPhone: Settings -> Bluetooth -> Forget this device, then re-pair from the setup page.");
    }

    std::vector<ScanCandidate> GetCandidates()
    {
        std::vector<ScanCandidate> out;
        Lock l;
        out.reserve(Candidates.size());
        for (const auto &c : Candidates)
            out.push_back({c.addr.toString(), c.name, c.rssi, c.isApple, c.bonded});
        return out;
    }

    bool RequestConnect(const std::string &address)
    {
        Lock l;
        for (const auto &c : Candidates)
        {
            if (c.addr.toString() == address)
            {
                PendingAddr = c.addr;
                ConnectPending = true;
                Serial.printf("Portal requested connect to %s\n", address.c_str());
                return true;
            }
        }
        Serial.printf("Portal requested unknown address %s\n", address.c_str());
        return false;
    }

} // namespace Bluetooth
