#include <Arduino.h>
#include <esp_sleep.h>
#include "apple_media_service.h"
#include "Bluetooth.h"
#include "setup_portal.h"
#include "esp_system.h"

const unsigned long sleepTimeout = 1000 * 60 * 3; // 3 min
const unsigned long displayTimeout = 1000 * 30;   // 30 sec

// DisplayManager displayManager;
unsigned long startTime = 0;
bool disableDisplay = false;
bool isConnected = false;
unsigned long initialFreeHeap = 0;
AppleMediaService::MediaInformation media_info;

void onDataUpdateCallback(const AppleMediaService::MediaInformation &info)
{
 info.dump();
  media_info = info;
}

void setup()
{
  initialFreeHeap = esp_get_free_heap_size() / 1024.0;
  Serial.begin(115200);

  delay(1000); // wait for things to settle

  Bluetooth::Begin("CTRL 01");
  AppleMediaService::RegisterForNotifications(
      onDataUpdateCallback,
      AppleMediaService::NotificationLevel::All);

  // WiFi access point + web page for picking/pairing the phone and for status
  // and remote control. SSID "CTRL 01 Setup", password "ctrl0101" (>=8 chars).
  SetupPortal::Begin("CTRL 01 Setup", "ctrl0101");

  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  Serial.println("==== ESP Chip Information ====");
  Serial.printf("Model: ESP32-%c\n",
                (chip_info.model == CHIP_ESP32) ? 'D' : (chip_info.model == CHIP_ESP32S2) ? 'S'
                                                    : (chip_info.model == CHIP_ESP32S3)   ? 'S'
                                                    : (chip_info.model == CHIP_ESP32C3)   ? 'C'
                                                                                          : '?');
  Serial.printf("Cores: %d\n", chip_info.cores);
  Serial.printf("Revision: %d\n", chip_info.revision);
  Serial.printf("Features bitmask: 0x%X\n", chip_info.features);

  // Check features flags (classic macro from esp_chip_info.h)
  if (chip_info.features & CHIP_FEATURE_BT)
    Serial.println("Classic Bluetooth: ✅ supported");
  else
    Serial.println("Classic Bluetooth: ❌ not available");

  if (chip_info.features & CHIP_FEATURE_BLE)
    Serial.println("BLE (Bluetooth Low Energy): ✅ supported");
  else
    Serial.println("BLE (Bluetooth Low Energy): ❌ not available");

  Serial.println("================================");
}void pollSerial()
{
  static String line;

  struct CommandEntry
  {
    const char *name;
    AppleMediaService::RemoteCommandID id;
  };

  // Commands available over serial
  static const CommandEntry kCommands[] = {
      {"play",   AppleMediaService::RemoteCommandID::Play},
      {"pause",  AppleMediaService::RemoteCommandID::Pause},
      {"toggle", AppleMediaService::RemoteCommandID::TogglePlayPause},
      {"next",   AppleMediaService::RemoteCommandID::NextTrack},
      {"prev",   AppleMediaService::RemoteCommandID::PreviousTrack},
      {"vol+",   AppleMediaService::RemoteCommandID::VolumeUp},
      {"vol-",   AppleMediaService::RemoteCommandID::VolumeDown},
      {"ff",     AppleMediaService::RemoteCommandID::SkipForward},
      {"rew",    AppleMediaService::RemoteCommandID::SkipBackward},
      {"rep",    AppleMediaService::RemoteCommandID::AdvanceRepeatMode},
      {"shuf",   AppleMediaService::RemoteCommandID::AdvanceShuffleMode},
      {"like",   AppleMediaService::RemoteCommandID::LikeTrack},
      {"dislike",AppleMediaService::RemoteCommandID::DislikeTrack},
      {"star",   AppleMediaService::RemoteCommandID::BookmarkTrack},
  };

  while (Serial.available())
  {
    char c = (char)Serial.read();

    if (c == '\n' || c == '\r')
    {
      line.trim();

      if (line.length() > 0)
      {
        // ---- global commands (work even if not connected) ----
        if (line.equalsIgnoreCase("help")) {
          Serial.println("Commands:");
          for (const auto &cmd : kCommands)
          {
            Serial.print("  ");
            Serial.println(cmd.name);
          }
          Serial.println("  clearbonds  (clear BLE bonds + restart scan)");
        }
        else if (line.equalsIgnoreCase("clearbonds")) {
          Bluetooth::ClearBonds();
        }
        // ---- media commands (only when connected) ----
        else if (Bluetooth::IsConnected())
        {
          bool handled = false;

          for (const auto &cmd : kCommands)
          {
            if (line.equalsIgnoreCase(cmd.name))
            {
              AppleMediaService::SendRemoteCommand(cmd.id);
              handled = true;
              break;
            }
          }

          if (!handled) {
            Serial.println("Unknown command. Type 'help' for list.");
          }
        }
        else {
          Serial.println("Not connected. Only 'help' and 'clearbonds' are available.");
        }
      }

      line = "";
    }
    else
    {
      line += c;
    }
  }
}
void loop()
{

  Bluetooth::Service();
  SetupPortal::Handle();

  static uint32_t last = 0;
  if (millis() - last > 10000)
  {
    time_t now = time(nullptr); // time are rewrited from bt
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    Serial.printf("[TIME] %s\n", buf);
    last = millis();
  }

  pollSerial();
}
