#include "setup_portal.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#include "bluetooth.h"
#include "apple_media_service.h"

namespace SetupPortal
{
    namespace
    {
        WebServer server(80);
        DNSServer dns;
        const byte DNS_PORT = 53;
        IPAddress apIP(192, 168, 4, 1);

        // --- minimal JSON string escaping (handles ", \, control chars; UTF-8 passes through) ---
        std::string jsonEscape(const std::string &in)
        {
            std::string out;
            out.reserve(in.size() + 8);
            for (unsigned char c : in)
            {
                switch (c)
                {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    }
                    else
                        out += (char)c;
                }
            }
            return out;
        }

        const char *playbackStateStr(AppleMediaService::MediaInformation::PlaybackState s)
        {
            using St = AppleMediaService::MediaInformation::PlaybackState;
            switch (s)
            {
            case St::Paused: return "Paused";
            case St::Playing: return "Playing";
            case St::Rewinding: return "Rewinding";
            case St::FastForwarding: return "FastForwarding";
            default: return "Unknown";
            }
        }

        const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>CTRL 01 Setup</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}
.wrap{max-width:640px;margin:0 auto;padding:16px}
h1{font-size:20px}
.card{background:#1c1c1e;border-radius:12px;padding:14px;margin:12px 0}
.muted{color:#888;font-size:13px}
table{width:100%;border-collapse:collapse}
td,th{padding:8px;border-bottom:1px solid #2a2a2c;text-align:left;font-size:14px}
button{background:#0a84ff;color:#fff;border:0;border-radius:8px;padding:8px 12px;font-size:14px;cursor:pointer}
button.sec{background:#3a3a3c}
.badge{font-size:11px;padding:2px 6px;border-radius:6px;background:#333;margin-left:4px}
.apple{background:#0a84ff}
.bond{background:#34c759}
.controls button{margin-right:6px;margin-top:6px}
.now{font-size:16px;font-weight:600}
</style></head><body><div class="wrap">
<h1>CTRL 01 &mdash; iPhone Media Controller</h1>

<div class="card" id="status">
  <div class="now" id="conn">Loading…</div>
  <div id="track" class="muted"></div>
  <div class="controls" id="ctrls" style="display:none">
    <button onclick="cmd('prev')">⏮ Prev</button>
    <button onclick="cmd('toggle')">⏯ Play/Pause</button>
    <button onclick="cmd('next')">⏭ Next</button>
    <button class="sec" onclick="cmd('vol-')">Vol −</button>
    <button class="sec" onclick="cmd('vol+')">Vol +</button>
  </div>
</div>

<div class="card">
  <p class="muted">Bring your iPhone right next to this device, then tap <b>Connect</b> on the
  strongest Apple entry below and accept the pairing prompt on your phone. After that it
  reconnects automatically.</p>
  <label class="muted"><input type="checkbox" id="appleOnly" checked onchange="refresh()"> Show Apple devices only</label>
  <table><thead><tr><th>Device</th><th>Signal</th><th></th></tr></thead>
  <tbody id="devs"><tr><td colspan="3" class="muted">Scanning…</td></tr></tbody></table>
</div>

<div class="card">
  <button class="sec" onclick="clearBonds()">Clear bonds / forget phone</button>
</div>

<script>
function cmd(c){fetch('/api/cmd?c='+encodeURIComponent(c))}
function conn(a){fetch('/api/connect?addr='+encodeURIComponent(a)).then(refresh)}
function clearBonds(){if(confirm('Forget the paired phone?'))fetch('/api/clearbonds').then(refresh)}
function refresh(){
  fetch('/api/status').then(r=>r.json()).then(s=>{
    document.getElementById('conn').textContent = s.connected ? ('Connected: '+s.address) : (s.bonded?'Bonded — reconnecting…':'Not connected');
    document.getElementById('ctrls').style.display = s.connected?'block':'none';
    document.getElementById('track').textContent = s.connected ? (s.title+' — '+s.artist+'  ['+s.state+']') : '';
  });
  fetch('/api/devices').then(r=>r.json()).then(list=>{
    if(document.getElementById('appleOnly').checked) list=list.filter(d=>d.apple);
    // Apple first, then strongest signal first.
    list.sort((a,b)=>(b.apple-a.apple)||(b.rssi-a.rssi));
    var t=document.getElementById('devs');
    if(!list.length){t.innerHTML='<tr><td colspan=3 class=muted>Scanning… bring your phone close</td></tr>';return}
    t.innerHTML='';
    list.forEach(d=>{
      var name=d.name||'(unknown device)';
      var b=(d.apple?'<span class="badge apple">Apple</span>':'')+(d.bonded?'<span class="badge bond">Bonded</span>':'');
      // RSSI: ~-50 near, ~-90 far. Map to bars.
      var bars=d.rssi>-55?'▮▮▮▮':d.rssi>-65?'▮▮▮▯':d.rssi>-75?'▮▮▯▯':d.rssi>-85?'▮▯▯▯':'▯▯▯▯';
      t.innerHTML+='<tr><td>'+name+'<br><span class=muted>'+d.address+'</span>'+b+'</td><td>'+bars+'<br><span class=muted>'+d.rssi+' dBm</span></td><td><button onclick="conn(\''+d.address+'\')">Connect</button></td></tr>';
    });
  });
}
setInterval(refresh,2000);refresh();
</script>
</div></body></html>)HTML";

        void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }

        void handleDevices()
        {
            auto cands = Bluetooth::GetCandidates();
            std::string json = "[";
            bool first = true;
            for (const auto &c : cands)
            {
                if (!first) json += ",";
                first = false;
                json += "{\"address\":\"" + jsonEscape(c.address) + "\",";
                json += "\"name\":\"" + jsonEscape(c.name) + "\",";
                json += "\"rssi\":" + std::to_string(c.rssi) + ",";
                json += std::string("\"apple\":") + (c.isApple ? "true" : "false") + ",";
                json += std::string("\"bonded\":") + (c.isBonded ? "true" : "false") + "}";
            }
            json += "]";
            server.send(200, "application/json", json.c_str());
        }

        void handleStatus()
        {
            bool connected = Bluetooth::IsConnected();
            std::string json = "{";
            json += std::string("\"connected\":") + (connected ? "true" : "false") + ",";
            json += std::string("\"bonded\":") + (Bluetooth::HasBond() ? "true" : "false") + ",";
            json += "\"address\":\"" + jsonEscape(Bluetooth::ConnectedAddress()) + "\"";
            if (connected)
            {
                const auto &m = AppleMediaService::GetMediaInformation();
                json += ",\"title\":\"" + jsonEscape(m.mTitle) + "\"";
                json += ",\"artist\":\"" + jsonEscape(m.mArtist) + "\"";
                json += ",\"album\":\"" + jsonEscape(m.mAlbum) + "\"";
                json += std::string(",\"state\":\"") + playbackStateStr(m.mPlaybackState) + "\"";
            }
            json += "}";
            server.send(200, "application/json", json.c_str());
        }

        void handleConnect()
        {
            if (!server.hasArg("addr"))
            {
                server.send(400, "text/plain", "missing addr");
                return;
            }
            bool ok = Bluetooth::RequestConnect(server.arg("addr").c_str());
            server.send(ok ? 200 : 404, "text/plain", ok ? "ok" : "unknown device");
        }

        void handleClearBonds()
        {
            Bluetooth::ClearBonds();
            server.send(200, "text/plain", "ok");
        }

        void handleCmd()
        {
            using ID = AppleMediaService::RemoteCommandID;
            if (!server.hasArg("c"))
            {
                server.send(400, "text/plain", "missing c");
                return;
            }
            String c = server.arg("c");
            if (!Bluetooth::IsConnected())
            {
                server.send(409, "text/plain", "not connected");
                return;
            }
            bool ok = true;
            if (c == "play")        AppleMediaService::SendRemoteCommand(ID::Play);
            else if (c == "pause")  AppleMediaService::SendRemoteCommand(ID::Pause);
            else if (c == "toggle") AppleMediaService::SendRemoteCommand(ID::TogglePlayPause);
            else if (c == "next")   AppleMediaService::SendRemoteCommand(ID::NextTrack);
            else if (c == "prev")   AppleMediaService::SendRemoteCommand(ID::PreviousTrack);
            else if (c == "vol+")   AppleMediaService::SendRemoteCommand(ID::VolumeUp);
            else if (c == "vol-")   AppleMediaService::SendRemoteCommand(ID::VolumeDown);
            else ok = false;
            server.send(ok ? 200 : 400, "text/plain", ok ? "ok" : "unknown cmd");
        }

        void handleNotFound()
        {
            // Captive-portal style: send everything to the setup page.
            server.sendHeader("Location", "http://192.168.4.1/", true);
            server.send(302, "text/plain", "");
        }
    } // anonymous namespace

    void Begin(const std::string &ssid, const std::string &password)
    {
        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        bool ok = WiFi.softAP(ssid.c_str(), password.empty() ? nullptr : password.c_str());
        Serial.printf("WiFi AP '%s' %s, IP: %s\n", ssid.c_str(),
                      ok ? "started" : "FAILED", WiFi.softAPIP().toString().c_str());

        dns.start(DNS_PORT, "*", apIP);

        server.on("/", handleRoot);
        server.on("/api/devices", handleDevices);
        server.on("/api/status", handleStatus);
        server.on("/api/connect", handleConnect);
        server.on("/api/clearbonds", handleClearBonds);
        server.on("/api/cmd", handleCmd);
        server.onNotFound(handleNotFound);
        server.begin();
        Serial.println("Setup portal web server started");
    }

    void Handle()
    {
        dns.processNextRequest();
        server.handleClient();
    }

} // namespace SetupPortal
