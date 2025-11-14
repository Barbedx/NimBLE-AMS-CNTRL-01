#include "apple_media_service.h"

#include <NimBLEDevice.h>
#include <Arduino.h>
#include <esp32-hal-log.h>

// Apple Media Service Documentation:
// https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleMediaService_Reference/Specification/Specification.html#//apple_ref/doc/uid/TP40014716-CH1-SW7

#define APPLE_SERVICE_UUID "89D3502B-0F36-433A-8EF4-C502AD55F8DC"
#define APPLE_REMOTE_COMMAND_UUID "9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2"
#define APPLE_ENTITY_UPDATE_UUID "2F7CABCE-808D-411F-9A0C-BB92BA96C102"
#define APPLE_ENTITY_ATTRIBUTE_UUID "C6B2F38C-23AB-46D8-A6AB-A3A870BBD5D7"

#define EntityIDPlayer 0
#define EntityIDQueue 1
#define EntityIDTrack 2

#define PlayerAttributeIDName 0
#define PlayerAttributeIDPlaybackInfo 1
#define PlayerAttributeIDVolume 2

#define QueueAttributeIDIndex 0
#define QueueAttributeIDCount 1
#define QueueAttributeIDShuffleMode 2
#define QueueAttributeIDRepeatMode 3

#define TrackAttributeIDArtist 0
#define TrackAttributeIDAlbum 1
#define TrackAttributeIDTitle 2
#define TrackAttributeIDDuration 3

namespace AppleMediaService
{
  namespace
  {
    NotificationCb gCallback;
    NotificationLevel gNotificationLevel;
    MediaInformation gMediaInformation;
    NimBLERemoteCharacteristic *remote_command_characteristic = nullptr;
  }

  void RegisterForNotifications(const NotificationCb &callback, NotificationLevel level)
  {
    gCallback = callback;
    gNotificationLevel = level;
  }

  const MediaInformation &GetMediaInformation()
  {
    return gMediaInformation;
  }

  bool StartMediaService(BLEClient *client)
  {
    assert(client != nullptr);

    if (!client->isConnected())
    {
      Serial.println("client not connected");
      return false;
    }

    auto music_service = client->getService(APPLE_SERVICE_UUID);
    if (!music_service)
    {
      Serial.println("Apple music service not found");
      return false;
    }

    // TODO: Add support for actually sending media control commands
    remote_command_characteristic = music_service->getCharacteristic(APPLE_REMOTE_COMMAND_UUID);
    if (!remote_command_characteristic)
    {
      Serial.println("Apple remote command characteristic not found");
      return false;
    }

    // TODO: Add support for the entity attribute characteristic, which is only used for handling track attributes that are too long for
    // entity update.
    auto entity_attribute_characteristic = music_service->getCharacteristic(APPLE_REMOTE_COMMAND_UUID);
    if (!entity_attribute_characteristic)
    {
      Serial.println("Apple entity attribute characteristic not found");
      return false;
    }

    auto entity_update = music_service->getCharacteristic(APPLE_ENTITY_UPDATE_UUID);
    if (!entity_update)
    {
      Serial.println("Apple entity update characteristic not found");
      return false;
    }

    entity_update->subscribe(true, [](NimBLERemoteCharacteristic *characteristic, uint8_t *data, size_t length, bool is_notify)
                             {
                               bool notify = gNotificationLevel == NotificationLevel::All;
                               if (length < 3)
                               {
                                 Serial.println("entity_update notification less than 3 bytes, ignoring.");
                                 return;
                               }
                               uint8_t entity_id = data[0];
                               uint8_t attribute_id = data[1];
                               uint8_t flags = data[2];
                               std::string value(reinterpret_cast<char *>(data) + 3, length - 3);

                               Serial.printf("entity update. id: %i, attribute: %i, flags: %i, value: %s\n", entity_id, attribute_id, flags, value.c_str());
                               switch (entity_id)
                               {
                               case EntityIDPlayer:
                               {
                                 switch (attribute_id)
                                 {
                                 case PlayerAttributeIDName:
                                   gMediaInformation.mPlayerName = value;
                                   break;
                                 case PlayerAttributeIDPlaybackInfo: // A concatenation of three comma-separated values: //TODO:check later, gpt doubts 
                                 {
                                   auto comma_index = value.find(",");
                                   if (comma_index != std::string::npos)
                                   {
                                     gMediaInformation.mPlaybackState =
                                         static_cast<MediaInformation::PlaybackState>(String(value.substr(0, comma_index).c_str()).toInt());
                                     auto second_comma = value.find(",", comma_index + 1);
                                     if (second_comma != std::string::npos)
                                     {
                                       gMediaInformation.mPlaybackRate = String(value.substr(comma_index + 1, second_comma).c_str()).toFloat();
                                       gMediaInformation.mElapsedTime = String(value.substr(second_comma + 1).c_str()).toFloat();
                                     }
                                   }
                                   break;
                                 }
                                 case PlayerAttributeIDVolume: // A string that represents the floating point value of the volume, ranging from 0
                                   // (silent) to
                                   // 1 (full volume).
                                   gMediaInformation.mVolume = String(value.c_str()).toFloat();
                                   break;
                                 }
                                 break;
                               }
                               case EntityIDQueue:
                               {
                                 switch (attribute_id)
                                 {
                                 case QueueAttributeIDIndex:
                                   gMediaInformation.mQueueIndex = String(value.c_str()).toInt();
                                   break;
                                 case QueueAttributeIDCount:
                                   gMediaInformation.mQueueCount = String(value.c_str()).toInt();
                                   break;
                                 case QueueAttributeIDShuffleMode:
                                   notify = true;
                                   gMediaInformation.mShuffleMode = static_cast<MediaInformation::ShuffleMode>(String(value.c_str()).toInt());
                                   break;
                                 case QueueAttributeIDRepeatMode:
                                   notify = true;
                                   gMediaInformation.mRepeatMode = static_cast<MediaInformation::RepeatMode>(String(value.c_str()).toInt());
                                   break;
                                 }
                                 break;
                               }
                               case EntityIDTrack:
                               {
                                 switch (attribute_id)
                                 {
                                 case TrackAttributeIDArtist:
                                   gMediaInformation.mArtist = value;
                                   notify = true;
                                   break;
                                 case TrackAttributeIDAlbum:
                                   gMediaInformation.mAlbum = value;
                                   break;
                                 case TrackAttributeIDTitle:
                                   gMediaInformation.mTitle = value;
                                   notify = true;
                                   break;
                                 case TrackAttributeIDDuration:
                                   gMediaInformation.mDuration = String(value.c_str()).toFloat();
                                   break;
                                 }
                                 break;
                               }
                               }
                               if (notify && gCallback)
                               {
                                 gCallback(gMediaInformation); 
                               } });

    // Subscribe to all player, queue, and track updates!
    uint8_t player_setup[] = {0, 0, 1, 2};
    uint8_t queue_setup[] = {1, 0, 1, 2, 3};
    uint8_t track_setup[] = {2, 0, 1, 2, 3};

    entity_update->writeValue(player_setup, sizeof(player_setup), true);
    entity_update->writeValue(queue_setup, sizeof(queue_setup), true);
    entity_update->writeValue(track_setup, sizeof(track_setup), true);
    return true;
  }

  bool setRemoteCommandValue(uint8_t commandID)
  {
    if (remote_command_characteristic != nullptr)
    {
      remote_command_characteristic->writeValue(&commandID, sizeof(commandID), true);
      return true;
    }
    else
    {
      Serial.println("Remote command characteristic is not initialized!");
      return false;
    }
  }
}