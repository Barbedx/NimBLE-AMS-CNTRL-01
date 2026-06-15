#pragma once
#include <string>

// Owns all Wi-Fi networking, independent of the web layer:
//  - tries to join a saved network (STA) using credentials stored in NVS;
//  - falls back to an access point with a captive DNS if that fails / none saved;
//  - advertises mDNS (`<hostname>.local`) so the dashboard is reachable by name
//    regardless of the DHCP-assigned IP.
namespace WiFiManager {

void Begin(const std::string& apSsid, const std::string& apPass, const std::string& hostname);
void Handle(); // pump captive DNS while in AP mode

// Scan for nearby networks (async). StartScan() kicks it off; ScanJson() returns
// {"scanning":bool,"nets":[{"ssid","rssi","secure"}...]} (top 10 by signal).
void        StartScan();
std::string ScanJson();

bool        IsStation();   // true = joined a network, false = AP fallback
std::string ModeStr();     // "STA" | "AP"
std::string IP();          // current IP as string
std::string SSID();        // joined SSID (STA) or AP SSID
std::string Hostname();    // mDNS hostname (without ".local")

// Persist new STA credentials to NVS (staticIp "" = DHCP). Does NOT reboot —
// the caller applies them (typically by restarting).
void SaveCredentials(const std::string& ssid, const std::string& pass, const std::string& staticIp);
void ClearCredentials();

} // namespace WiFiManager
