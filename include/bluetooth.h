#pragma once
#include <string>

// BLE role model: the ESP32 advertises as a connectable peripheral named
// "CTRL 01". The user pairs by tapping it in iOS Settings > Bluetooth (this is
// the disambiguator — no guessing which scanned address is the phone). Once the
// phone connects, the ESP32 grabs a GATT *client* for that connection
// (NimBLEServer::getClient) and reads Apple Media Service from the phone.
// Bonding makes every later reconnect automatic and silent.
namespace Bluetooth {

void Begin(const std::string& device_name);
void Service();
bool IsConnected();
bool IsTimeSet();
void ClearBonds();
bool HasBond();
std::string ConnectedAddress(); // "" when not connected

} // namespace Bluetooth
