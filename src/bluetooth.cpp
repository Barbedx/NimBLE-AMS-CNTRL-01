#include "bluetooth.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <time.h>
#include <sys/time.h>

#if defined(CONFIG_NIMBLE_CPP_IDF)
#include <host/ble_gap.h>
#else
#include <nimble/nimble/host/include/host/ble_gap.h>
#endif
#include "apple_media_service.h"
#include "current_time_service.h"

// <<< set this to your phone's BLE name if you know it >>>
// static const char *const kTargetName = "IPhone 13"; // change to your phone name

// "Підпис" твого айфона – префікс manufacturer data
static const uint8_t kMyIphoneMfgPrefix[] = {
    0x4c, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00
    // можна скоротити / розширити при потребі
};
// AMS candidate (first Apple device): Name: , Address: dc:b5:4f:a6:69:9c, manufacturer data: 4c0010077f1fd4c3a8f948, txPower: 12
// Service: trying to connect to AMS target
// Connecting to AMS device: dc:b5:4f:a6:69:9c
// E NimBLEClient: Connection failed; status=13
// Client connect failed
// Restarting scan after failed connect
// [TIME] 2036-12-28 07:42:44
// I NimBLEScan: New advertiser: 1c:86:9a:ae:a5:43
// Result: Name: , Address: 1c:86:9a:ae:a5:43, manufacturer data: 750042040180661c869aaea5431e869aaea54201000000000000
// I NimBLEScan: New advertiser: 63:f1:9e:a2:05:0c
// Result: Name: , Address: 63:f1:9e:a2:05:0c, manufacturer data: 4c0009081375c0a864231b581608001537abdbe6a113
// AMS candidate (first Apple device): Name: , Address: 63:f1:9e:a2:05:0c, manufacturer data: 4c0009081375c0a864231b581608001537abdbe6a113
// Service: trying to connect to AMS target
// Connecting to AMS device: 63:f1:9e:a2:05:0c

#define APPLE_MUSIC_SERVICE_UUID "89D3502B-0F36-433A-8EF4-C502AD55F8DC"

#define ANCS_SERVICE_UUID "7905F431-B5CE-4E99-A40F-4B1E122D00D0"

#define DELSOL_VEHICLE_SERVICE_UUID "8fb88487-73cf-4cce-b495-505a4b54b802"
#define DELSOL_STATUS_CHARACTERISTIC_UUID "40d527f5-3204-44a2-a4ee-d8d3c16f970e"
#define DELSOL_BATTERY_CHARACTERISTIC_UUID "5c258bb8-91fc-43bb-8944-b83d0edc9b43"

#define DELSOL_LOCATION_SERVICE_UUID "61d33c70-e3cd-4b31-90d8-a6e14162fffd"
#define DELSOL_NAVIGATION_SERVICE_UUID "77f5d2b5-efa1-4d55-b14a-cc92b72708a0"
namespace Bluetooth
{
    namespace
    {
        // --- State ---

        bool Ended = true;
        bool Connected = false;

        BLEClient *Client = nullptr; // NimBLE client (BLEClient macro)
        BLEAddress TargetAddr;       // AMS device address
        bool TargetFound = false;

        RTC_DATA_ATTR bool TimeSet = false;

        // --- Scan callback: look for Apple Media Service UUID ---
        // --- Connect to AMS peripheral and start AMS + CTS ---
        static void dumpMfgHex(const std::string &mfg)
        {
            Serial.print("  mfg hex: ");
            for (uint8_t c : mfg)
            {
                char buf[4];
                sprintf(buf, "%02X", c);
                Serial.print(buf);
            }
            Serial.println();
        }
        class AmsScanCallbacks : public NimBLEScanCallbacks
        {
        public:
            // Optional – you can ignore this if you don’t care about early hits
            void onDiscovered(const NimBLEAdvertisedDevice *dev) override
            {
                // You can leave empty or log:
                // Serial.printf("Discovered: %s\n", dev->toString().c_str());
            }

            // This is where we actually look for AMS
            void onResult(const NimBLEAdvertisedDevice *dev) override
            {
                if (TargetFound)
                    return;

                const std::string name = dev->getName();

                // Debug
                //  Serial.printf("Result: %s\n", dev->toString().c_str());

                // Always log what we see – this is super useful:
        //        Serial.printf("Result: %s\n", dev->toString().c_str());
                // Serial.printf("Name='%s', Address='%s', MfgData='%s'\n",
                //                               name.c_str(),
                //                               address.c_str(),
                //                               mfg.c_str());
                // 0) Переконаймось, що пристрій взагалі коннектимий
                if (!dev->isConnectable())
                {
                 //   Serial.println("  -> not connectable, ignoring");
                    return;
                } 
                const std::string address = dev->getAddress().toString();
                const std::string mfg = dev->getManufacturerData();
          //      dumpMfgHex(mfg);
                
                if (mfg.size() < sizeof(kMyIphoneMfgPrefix))
                {
                 //   Serial.println("  -> mfg too short, ignoring");
                    return;
                }

                // 1) Перевіряємо Apple company ID (перші 2 байти)
                uint16_t company =
                    (uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8);

                bool isApple = (company == 0x004C);
        //        Serial.printf("  IsApple=%d\n", isApple ? 1 : 0);

                if (!isApple)
                {
                    return;
                }

                // 2) Перевіряємо, чи збігається префікс з нашим айфоном
                if (memcmp(mfg.data(), kMyIphoneMfgPrefix,
                           sizeof(kMyIphoneMfgPrefix)) != 0)
                {
  //                  Serial.println("  -> Apple but not my mfg prefix, continue but");
                return;
                }

                // 3) (Опційно) Додатково звузити по serviceUUID its spotify id????
                static const NimBLEUUID kMyService("3e1d50cd-7e3e-427d-8e1c-b78aa87fe624");

                if (dev->haveServiceUUID() && dev->isAdvertisingService(kMyService))
                {
                    Serial.println("  -> mfg prefix + my serviceUUID match, selecting target");
                }
                else
                {
   //                 Serial.println("  -> mfg prefix ok but serviceUUID mismatch, but continue");
                   return;// return;
                }

                Serial.print("AMS candidate founded: ");
                Serial.println(dev->toString().c_str());
                TargetAddr = dev->getAddress();
                TargetFound = true;
                NimBLEDevice::getScan()->stop();
            }

            // NOTE: signature must match: const ref + reason
            void onScanEnd(const NimBLEScanResults &results, int reason) override
            {
                Serial.printf("Scan ended, found %d devices, reason=%d\n",
                              results.getCount(), reason);
            }
        };

        bool ConnectToAms()
        {
            if (!TargetFound)
            {
                Serial.println("ConnectToAms: no target found yet");
                return false;
            }

            if (Client)
            {
                Serial.println("ConnectToAms: deleting existing client");
                NimBLEDevice::deleteClient(Client);
                Client = nullptr;
            }

            Serial.print("Connecting to AMS device: ");
            Serial.println(TargetAddr.toString().c_str());

            Client = NimBLEDevice::createClient();
            if (!Client)
            {
                Serial.println("Failed to create client");
                return false;
            }

            if (!Client->connect(TargetAddr))
            {
                Serial.println("Client connect failed");
                NimBLEDevice::deleteClient(Client);
                Client = nullptr;

                Connected = false;
                TargetFound = false;

                NimBLEScan *s = NimBLEDevice::getScan();
                if (s)
                {
                    Serial.println("Restarting scan after failed connect");
                    s->start(0, false, false);
                }
                return false;
            }

            Serial.println("Client connected to AMS device");

            // Start encryption / bonding
            // if(!NimBLEDevice::startSecurity(Client->getConnHandle())){
            //     Serial.println("Failed to start security (bonding)");
            //     Client->disconnect();
            //     NimBLEDevice::deleteClient(Client);
            //     Client = nullptr;

            //     Connected = false;
            //     TargetFound = false;

            //     NimBLEScan *s = NimBLEDevice::getScan();
            //     if (s)
            //     {
            //         Serial.println("Restarting scan after failed bonding");
            //         s->start(0, false, false);
            //     }
            //     return false;
            // };

            // Synchronous security: fail fast if pairing/encryption fails
            if (!Client->secureConnection()) {
                Serial.println("Failed to secure connection (pairing/encryption)");
                Client->disconnect();
                NimBLEDevice::deleteClient(Client);
                Client = nullptr;

                Connected   = false;
                TargetFound = false;

                NimBLEScan *s = NimBLEDevice::getScan();
                if (s) {
                    Serial.println("Restarting scan after failed secureConnection");
                    s->start(0, false, false);
                }
                return false;
            }
            // --- Apple Media Service ---
            if (!AppleMediaService::StartMediaService(Client))
            {
                Serial.println("StartMediaService failed (AMS not available?)");

                Client->disconnect();
                NimBLEDevice::deleteClient(Client);
                Client = nullptr;

                Connected = false;
                TargetFound = false;

                NimBLEScan *s = NimBLEDevice::getScan();
                if (s)
                {
                    Serial.println("Restarting scan after bad AMS device");
                    s->start(0, false, false);
                }
                return false;
            }
            else
            {
                Serial.println("AMS started");
            }

            // --- Current Time Service ---
            CurrentTimeService::CurrentTime time;
            if (!CurrentTimeService::StartTimeService(Client, &time))
            {
                Serial.println("StartTimeService failed");
            }
            else
            {
                timeval new_time;
                new_time.tv_sec = time.ToTimeT();
                new_time.tv_usec = static_cast<long>(time.mSecondsFraction * 1000000.0f);

                Serial.printf("TIME: unix=%ld, usec=%ld\n",
                              (long)new_time.tv_sec,
                              (long)new_time.tv_usec);

                if (settimeofday(&new_time, nullptr) != 0)
                {
                    Serial.println("Error setting time of day");
                }
                else
                {
                    TimeSet = true;
                }

                time.Dump();
            }

            Connected = true;
            Serial.println("ConnectToAms finished");
            return true;
        }

    } // anonymous namespace

    // ---------------------------------------------------------------------
    // Public API
    // ---------------------------------------------------------------------
 void ClearBonds()
    {
        Serial.println("[BLE] Clearing all BLE bonds...");

        // Remove all stored bonds from NVS
        NimBLEDevice::deleteAllBonds();

        // If we have a client, disconnect and delete it
        if (Client) {
            if (Client->isConnected()) {
                Serial.println("[BLE] Disconnecting active client...");
                Client->disconnect();
            }
            NimBLEDevice::deleteClient(Client);
            Client = nullptr;
        }

        // Reset state flags
        Connected   = false;
        TargetFound = false;
        TimeSet     = false;

        // Restart scan if BLE still initialized
        NimBLEScan *scan = NimBLEDevice::getScan();
        if (scan) {
            Serial.println("[BLE] Restarting scan after clearbonds...");
            scan->start(0, false, false);
        }

        Serial.println("[BLE] Bonds cleared. On iPhone: Settings → Bluetooth → Forget this device, then re-pair.");
    }
    void Begin(const std::string &device_name)
    {
        Serial.println("Bluetooth::Begin() AMS central-only");
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

        Ended = false;
        Connected = false;
        TimeSet = false;
        TargetFound = false;

        NimBLEDevice::init(device_name);
        // Serial.println("Deleting all BLE bonds...");
       //  NimBLEDevice::deleteAllBonds();

        // // // Security: bonding + LE Secure Connections, no MITM
        // static MySecurityCallbacks securityCallbacks;
        // NimBLEDevice::startSecurity setSecurityCallbacks(&securityCallbacks);
        // Security: bonding + LE Secure Connections, no MITM
        // NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
        NimBLEDevice::setSecurityAuth(/*bonding=*/false, /*mitm=*/false, /*sc=*/true); 
        //AMS NO BOUNDING  its actually woirks, but asks for permission each time. I'm ok with that for now.
        //to init connection we have to open spotify on mobile. for some reasone it will translate some ble service with spiotify uid 
        // LEts figure out! 
        // NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
        // NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
        // NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
        // NimBLEDevice::setSecurityPasskey(123456); // optional fixed PIN

        // No input/output → iOS does “Just Works” pairing
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

        // Clear any old, possibly broken bonds
        //   NimBLEDevice::deleteAllBonds();

        NimBLEScan *scan = NimBLEDevice::getScan();
        static AmsScanCallbacks cb;

        scan->setScanCallbacks(&cb);
        scan->setActiveScan(true);
        scan->setInterval(45);
        scan->setWindow(30);
        scan->setDuplicateFilter(false);
        scan->setFilterPolicy(BLE_HCI_SCAN_FILT_NO_WL);
        scan->start(0, /*isContinue=*/false, /*restart=*/false); // ms; 0 = forever

        Serial.println("Bluetooth::Begin finished (scanning for AMS)");
    }

    void End()
    {
        Serial.println("Bluetooth::End()");

        Ended = true;
        Connected = false;
        TargetFound = false;

        NimBLEScan *scan = NimBLEDevice::getScan();
        if (scan)
            scan->stop();

        if (Client)
        {
            Client->disconnect();
            NimBLEDevice::deleteClient(Client);
            Client = nullptr;
        }

        NimBLEDevice::deinit(true);
    }

    void Service()
    {
        if (Ended)
            return;

        // If we had a client and it dropped, clean up and rescan
        if (Client && !Client->isConnected())
        {
            Serial.println("Service: client disconnected, cleaning up");
            NimBLEDevice::deleteClient(Client);
            Client = nullptr;

            Connected = false;
            TimeSet = false;
            TargetFound = false;

            NimBLEScan *scan = NimBLEDevice::getScan();
            if (scan && !scan->isScanning())
            {
                Serial.println("Service: restarting scan");
                scan->start(0, false, false);
            }
        }

        // If not connected but target is found → try to connect
        if ((!Client || !Client->isConnected()) && TargetFound)
        {
            Serial.println("Service: trying to connect to AMS target");
            ConnectToAms();
        }
    }

    bool IsConnected()
    {
        return Connected && Client && Client->isConnected();
    }

    bool IsTimeSet()
    {
        return TimeSet;
    }

} // namespace Bluetooth
