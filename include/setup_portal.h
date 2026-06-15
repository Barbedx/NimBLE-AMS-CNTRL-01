#pragma once
#include <string>

// A WiFi access point + web page used to pick which BLE device to pair with
// (the iPhone advertises with a rotating random address and no name, so the
// user identifies it by signal strength from the live list rather than the
// firmware guessing). The page also shows now-playing status and offers
// play/pause/next/prev controls. Runs alongside BLE the whole time.
namespace SetupPortal
{
    void Begin(const std::string &ssid, const std::string &password);
    void Handle(); // call from loop()
}
