#include "wifi_manager.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <algorithm>
#include <vector>

namespace WiFiManager
{
    namespace
    {
        Preferences prefs;
        DNSServer   dns;
        const byte  DNS_PORT = 53;
        IPAddress   apIP(192, 168, 4, 1);

        bool        gSta = false; // true once joined as station
        std::string gHostname = "ctrl01";
        std::string gApSsid, gApPass;
        std::string gSsid; // current network name (STA) or AP SSID

        std::string jsonEscape(const std::string &in)
        {
            std::string out;
            for (unsigned char c : in)
            {
                if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
                else if (c >= 0x20)        { out += (char)c; }
            }
            return out;
        }

        void startMDNS()
        {
            if (MDNS.begin(gHostname.c_str()))
            {
                MDNS.addService("http", "tcp", 80);
                Serial.printf("mDNS up: http://%s.local\n", gHostname.c_str());
            }
            else
            {
                Serial.println("mDNS start failed");
            }
        }

        void startAP()
        {
            gSta = false;
            WiFi.mode(WIFI_AP);
            WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
            bool ok = WiFi.softAP(gApSsid.c_str(), gApPass.empty() ? nullptr : gApPass.c_str());
            gSsid = gApSsid;
            Serial.printf("WiFi AP '%s' %s, IP: %s\n", gApSsid.c_str(),
                          ok ? "up" : "FAILED", WiFi.softAPIP().toString().c_str());
            dns.start(DNS_PORT, "*", apIP); // captive portal
            startMDNS();
        }

        bool tryStation(const String &ssid, const String &pass, const String &staticIp)
        {
            gSta = false;
            WiFi.mode(WIFI_STA);
            WiFi.setHostname(gHostname.c_str());
            WiFi.setAutoReconnect(true);

            if (staticIp.length())
            {
                IPAddress ip;
                if (ip.fromString(staticIp))
                {
                    // Assume a typical /24 with the gateway at .1 (also used as DNS).
                    IPAddress gw(ip[0], ip[1], ip[2], 1);
                    WiFi.config(ip, gw, IPAddress(255, 255, 255, 0), gw);
                }
            }

            WiFi.begin(ssid.c_str(), pass.c_str());
            Serial.printf("WiFi: joining '%s'", ssid.c_str());
            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 12000)
            {
                delay(250);
                Serial.print(".");
            }
            Serial.println();

            if (WiFi.status() == WL_CONNECTED)
            {
                gSta  = true;
                gSsid = ssid.c_str();
                Serial.printf("WiFi STA connected, IP: %s\n", WiFi.localIP().toString().c_str());
                startMDNS();
                return true;
            }
            Serial.println("WiFi STA connect failed");
            return false;
        }
    } // anonymous namespace

    void Begin(const std::string &apSsid, const std::string &apPass, const std::string &hostname)
    {
        gApSsid   = apSsid;
        gApPass   = apPass;
        gHostname = hostname;

        prefs.begin("wifi", /*readOnly=*/true);
        String ssid = prefs.getString("ssid", "");
        String pass = prefs.getString("pass", "");
        String sip  = prefs.getString("staticip", "");
        prefs.end();

        if (ssid.length() && tryStation(ssid, pass, sip))
            return;
        startAP();
    }

    void Handle()
    {
        if (!gSta)
            dns.processNextRequest();
    }

    void StartScan()
    {
        if (WiFi.scanComplete() == WIFI_SCAN_RUNNING)
            return;
        WiFi.scanDelete();
        WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
    }

    std::string ScanJson()
    {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_FAILED) // -2: never triggered → start one now
        {
            WiFi.scanNetworks(true, false);
            n = WIFI_SCAN_RUNNING;
        }
        if (n < 0) // running
            return "{\"scanning\":true,\"nets\":[]}";

        struct Net { std::string ssid; int rssi; bool sec; };
        std::vector<Net> nets;
        for (int i = 0; i < n; i++)
        {
            String ss = WiFi.SSID(i);
            if (!ss.length())
                continue;
            int  rssi = WiFi.RSSI(i);
            bool sec  = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            bool dup = false;
            for (auto &e : nets)
                if (e.ssid == ss.c_str()) { dup = true; if (rssi > e.rssi) e.rssi = rssi; break; }
            if (!dup)
                nets.push_back({std::string(ss.c_str()), rssi, sec});
        }
        std::sort(nets.begin(), nets.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
        if (nets.size() > 10)
            nets.resize(10);

        std::string j = "{\"scanning\":false,\"nets\":[";
        for (size_t i = 0; i < nets.size(); i++)
        {
            if (i) j += ",";
            j += "{\"ssid\":\"" + jsonEscape(nets[i].ssid) + "\",\"rssi\":" +
                 std::to_string(nets[i].rssi) + ",\"secure\":" + (nets[i].sec ? "true" : "false") + "}";
        }
        j += "]}";
        return j;
    }

    bool        IsStation() { return gSta; }
    std::string ModeStr()   { return gSta ? "STA" : "AP"; }
    std::string IP()        { return (gSta ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str(); }
    std::string SSID()      { return gSsid; }
    std::string Hostname()  { return gHostname; }

    void SaveCredentials(const std::string &ssid, const std::string &pass, const std::string &staticIp)
    {
        prefs.begin("wifi", false);
        prefs.putString("ssid", ssid.c_str());
        prefs.putString("pass", pass.c_str());
        prefs.putString("staticip", staticIp.c_str());
        prefs.end();
        Serial.printf("WiFi creds saved (ssid='%s', staticip='%s')\n", ssid.c_str(), staticIp.c_str());
    }

    void ClearCredentials()
    {
        prefs.begin("wifi", false);
        prefs.clear();
        prefs.end();
        Serial.println("WiFi creds cleared");
    }

} // namespace WiFiManager
