#include "web_portal.h"

#include <Arduino.h>
#include <PsychicHttp.h>
#include <time.h>

#include "bluetooth.h"
#include "wifi_manager.h"
#include "apple_media_service.h"
#include "apple_notification_service.h"

namespace WebPortal
{
    namespace
    {
        PsychicHttpServer server;

        // --- minimal JSON string escaping (", \, control chars; UTF-8 passes through) ---
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
<title>CTRL 01</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}
.wrap{max-width:640px;margin:0 auto;padding:16px}
h1{font-size:20px}
.card{background:#1c1c1e;border-radius:12px;padding:14px;margin:12px 0}
.muted{color:#888;font-size:13px}
button{background:#0a84ff;color:#fff;border:0;border-radius:8px;padding:8px 12px;font-size:14px;cursor:pointer}
button.sec{background:#3a3a3c}
input,select{width:100%;box-sizing:border-box;background:#2a2a2c;color:#eee;border:1px solid #3a3a3c;border-radius:8px;padding:8px;margin:4px 0;font-size:14px}
.controls button{margin-right:6px;margin-top:6px}
.now{font-size:16px;font-weight:600}
</style></head><body><div class="wrap">
<h1>CTRL 01 &mdash; iPhone Link</h1>

<div class="card" id="status">
  <div class="now" id="conn">Loading…</div>
  <div class="muted" id="clock"></div>
</div>

<div class="card" id="npCard" style="display:none">
  <div class="muted" id="player"></div>
  <div class="now" id="title">—</div>
  <div id="artist" class="muted"></div>
  <div id="album" class="muted"></div>
  <progress id="bar" value="0" max="100" style="width:100%;margin-top:8px"></progress>
  <div id="prog" class="muted"></div>
  <div id="meta2" class="muted"></div>
  <div class="controls" id="ctrls">
    <button onclick="cmd('prev')">⏮ Prev</button>
    <button onclick="cmd('toggle')">⏯ Play/Pause</button>
    <button onclick="cmd('next')">⏭ Next</button>
    <button class="sec" onclick="cmd('vol-')">Vol −</button>
    <button class="sec" onclick="cmd('vol+')">Vol +</button>
  </div>
</div>

<div class="card" id="notifCard" style="display:none">
  <div class="muted">Notifications</div>
  <div id="notifs"></div>
</div>

<div class="card">
  <b>How to pair (first time):</b>
  <ol class="muted">
    <li>iPhone → <b>Settings → Bluetooth</b>.</li>
    <li>Under "Other Devices" tap <b>CTRL 01</b> → <b>Pair</b>.</li>
  </ol>
</div>

<div class="card">
  <b>WiFi</b>
  <div class="muted" id="wifi"></div>
  <select id="ssidSel" onchange="document.getElementById('ssid').value=this.value">
    <option value="">— scanning… —</option>
  </select>
  <button class="sec" onclick="scanNets()">Rescan</button>
  <input id="ssid" placeholder="SSID (pick above or type manually)">
  <input id="pass" placeholder="Password" type="password">
  <input id="sip" placeholder="Static IP (optional — blank = DHCP)">
  <button onclick="saveWifi()">Save &amp; reboot</button>
</div>

<div class="card">
  <button class="sec" onclick="clearBonds()">Clear BLE bonds / forget phone</button>
</div>

<script>
function cmd(c){fetch('/api/cmd?c='+encodeURIComponent(c))}
function clearBonds(){if(confirm('Forget the paired phone?'))fetch('/api/clearbonds').then(refresh)}
function saveWifi(){
  var q='/api/wifi/save?ssid='+encodeURIComponent(ssid.value)+'&pass='+encodeURIComponent(pass.value)+'&ip='+encodeURIComponent(sip.value);
  fetch(q).then(r=>r.text()).then(t=>alert(t));
}
function scanNets(){
  var sel=document.getElementById('ssidSel');
  sel.innerHTML='<option value="">— scanning… —</option>';
  fetch('/api/wifi/scan');
  var tries=0,t=setInterval(function(){
    fetch('/api/wifi/networks').then(r=>r.json()).then(d=>{
      if(!d.scanning){
        clearInterval(t);
        sel.innerHTML='<option value="">— choose network —</option>';
        d.nets.forEach(function(nw){
          var q=nw.rssi>-55?'▮▮▮▮':nw.rssi>-65?'▮▮▮':nw.rssi>-75?'▮▮':'▮';
          var o=document.createElement('option');
          o.value=nw.ssid; o.textContent=nw.ssid+'  '+q+(nw.secure?' 🔒':'');
          sel.appendChild(o);
        });
        if(!d.nets.length) sel.innerHTML='<option value="">— none found, Rescan —</option>';
      }
      if(++tries>10){clearInterval(t);}
    }).catch(()=>{});
  },1200);
}
function mmss(s){s=Math.max(0,Math.floor(s));return Math.floor(s/60)+':'+('0'+(s%60)).slice(-2)}
function setTxt(id,t){document.getElementById(id).textContent=t}
function esc(t){return (t||'').replace(/[&<>]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;'}[c]})}
var SH=['Off','One','All'];
function refresh(){
  fetch('/api/status').then(r=>r.json()).then(s=>{
    setTxt('conn', s.connected ? ('Connected: '+s.address) : (s.bonded?'Bonded — waiting for phone…':'Not paired — pair from iOS Settings'));
    setTxt('clock', (s.timeSet?'🕑 ':'🕑 (unset) ')+s.time);
    setTxt('wifi', s.wifiMode+'  ·  '+s.wifiSsid+'  ·  http://'+s.wifiHost+'.local  ('+s.wifiIp+')');
    document.getElementById('npCard').style.display = s.connected?'block':'none';
    if(s.connected){
      setTxt('player', s.player?('▶ '+s.player+'  ·  '+s.state):s.state);
      setTxt('title', s.title||'—');
      setTxt('artist', s.artist||'');
      setTxt('album', s.album||'');
      document.getElementById('bar').value = s.duration>0 ? (100*s.elapsed/s.duration) : 0;
      setTxt('prog', mmss(s.elapsed)+' / '+mmss(s.duration));
      setTxt('meta2', 'Vol '+Math.round(s.volume*100)+'%  ·  Track '+(s.queueIndex+1)+'/'+s.queueCount+'  ·  Shuffle '+SH[s.shuffle]+'  ·  Repeat '+SH[s.repeat]);
    }
    var nl=s.notifs||[];
    document.getElementById('notifCard').style.display=(s.connected&&nl.length)?'block':'none';
    var h='';
    nl.forEach(function(n){
      h+='<div style="border-bottom:1px solid #2a2a2c;padding:6px 0">'
        +'<div class="now">'+esc(n.title||'(no title)')+'</div>'
        +'<div class="muted">'+esc(n.msg||'')+'</div>'
        +'<div class="muted">'+esc(n.cat+' · '+n.app)+'</div></div>';
    });
    document.getElementById('notifs').innerHTML=h;
  });
}
setInterval(refresh,1000);refresh();scanNets();
</script>
</div></body></html>)HTML";

        std::string buildStatusJson()
        {
            bool connected = Bluetooth::IsConnected();

            char timebuf[32] = "";
            time_t now = time(nullptr);
            struct tm tm;
            localtime_r(&now, &tm);
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

            std::string json = "{";
            json += std::string("\"connected\":") + (connected ? "true" : "false") + ",";
            json += std::string("\"bonded\":") + (Bluetooth::HasBond() ? "true" : "false") + ",";
            json += std::string("\"timeSet\":") + (Bluetooth::IsTimeSet() ? "true" : "false") + ",";
            json += "\"time\":\"" + std::string(timebuf) + "\",";
            json += "\"wifiMode\":\"" + WiFiManager::ModeStr() + "\",";
            json += "\"wifiSsid\":\"" + jsonEscape(WiFiManager::SSID()) + "\",";
            json += "\"wifiIp\":\"" + WiFiManager::IP() + "\",";
            json += "\"wifiHost\":\"" + jsonEscape(WiFiManager::Hostname()) + "\",";
            json += "\"address\":\"" + jsonEscape(Bluetooth::ConnectedAddress()) + "\"";
            if (connected)
            {
                const auto &m = AppleMediaService::GetMediaInformation();
                json += ",\"player\":\"" + jsonEscape(m.mPlayerName) + "\"";
                json += ",\"title\":\"" + jsonEscape(m.mTitle) + "\"";
                json += ",\"artist\":\"" + jsonEscape(m.mArtist) + "\"";
                json += ",\"album\":\"" + jsonEscape(m.mAlbum) + "\"";
                json += std::string(",\"state\":\"") + playbackStateStr(m.mPlaybackState) + "\"";
                json += ",\"elapsed\":" + std::to_string(AppleMediaService::CurrentElapsedTime());
                json += ",\"duration\":" + std::to_string(m.mDuration);
                json += ",\"volume\":" + std::to_string(m.mVolume);
                json += ",\"queueIndex\":" + std::to_string(m.mQueueIndex);
                json += ",\"queueCount\":" + std::to_string(m.mQueueCount);
                json += ",\"shuffle\":" + std::to_string((int)m.mShuffleMode);
                json += ",\"repeat\":" + std::to_string((int)m.mRepeatMode);

                auto recents = AppleNotificationService::GetRecent();
                json += ",\"notifs\":[";
                for (size_t i = 0; i < recents.size(); i++)
                {
                    const auto &n = recents[i];
                    if (i) json += ",";
                    json += "{\"cat\":\"" + std::string(AppleNotificationService::CategoryName(n.categoryId)) + "\",";
                    json += "\"app\":\"" + jsonEscape(n.appId) + "\",";
                    json += "\"title\":\"" + jsonEscape(n.title) + "\",";
                    json += "\"msg\":\"" + jsonEscape(n.message) + "\"}";
                }
                json += "]";
            }
            json += "}";
            return json;
        }
    } // anonymous namespace

    void Begin()
    {
        server.config.max_uri_handlers = 16;
        server.config.stack_size = 8192;
        server.listen(80);

        server.on("/", HTTP_GET, [](PsychicRequest *r) {
            return r->reply(200, "text/html", INDEX_HTML);
        });

        server.on("/api/status", HTTP_GET, [](PsychicRequest *r) {
            std::string j = buildStatusJson();
            return r->reply(200, "application/json", j.c_str());
        });

        server.on("/api/clearbonds", HTTP_GET, [](PsychicRequest *r) {
            Bluetooth::ClearBonds();
            return r->reply(200, "text/plain", "ok");
        });

        server.on("/api/cmd", HTTP_GET, [](PsychicRequest *r) {
            using ID = AppleMediaService::RemoteCommandID;
            if (!r->hasParam("c"))
                return r->reply(400, "text/plain", "missing c");
            if (!Bluetooth::IsConnected())
                return r->reply(409, "text/plain", "not connected");
            String c = r->getParam("c")->value();
            bool ok = true;
            if (c == "play")        AppleMediaService::SendRemoteCommand(ID::Play);
            else if (c == "pause")  AppleMediaService::SendRemoteCommand(ID::Pause);
            else if (c == "toggle") AppleMediaService::SendRemoteCommand(ID::TogglePlayPause);
            else if (c == "next")   AppleMediaService::SendRemoteCommand(ID::NextTrack);
            else if (c == "prev")   AppleMediaService::SendRemoteCommand(ID::PreviousTrack);
            else if (c == "vol+")   AppleMediaService::SendRemoteCommand(ID::VolumeUp);
            else if (c == "vol-")   AppleMediaService::SendRemoteCommand(ID::VolumeDown);
            else ok = false;
            return r->reply(ok ? 200 : 400, "text/plain", ok ? "ok" : "unknown cmd");
        });

        server.on("/api/wifi/scan", HTTP_GET, [](PsychicRequest *r) {
            WiFiManager::StartScan();
            return r->reply(200, "text/plain", "ok");
        });

        server.on("/api/wifi/networks", HTTP_GET, [](PsychicRequest *r) {
            std::string j = WiFiManager::ScanJson();
            return r->reply(200, "application/json", j.c_str());
        });

        // Save WiFi credentials, then reboot to apply (reply is flushed first).
        server.on("/api/wifi/save", HTTP_GET, [](PsychicRequest *r) {
            std::string ssid = r->hasParam("ssid") ? std::string(r->getParam("ssid")->value().c_str()) : "";
            std::string pass = r->hasParam("pass") ? std::string(r->getParam("pass")->value().c_str()) : "";
            std::string ip   = r->hasParam("ip")   ? std::string(r->getParam("ip")->value().c_str())   : "";
            if (ssid.empty())
                return r->reply(400, "text/plain", "missing ssid");
            WiFiManager::SaveCredentials(ssid, pass, ip);
            esp_err_t e = r->reply(200, "text/plain", "Saved. Rebooting — reconnect to your network, then open http://ctrl01.local");
            delay(400);
            ESP.restart();
            return e;
        });

        // Captive-portal: redirect anything unknown to the dashboard (AP mode).
        server.onNotFound([](PsychicRequest *r) {
            return r->redirect("http://192.168.4.1/");
        });

        Serial.println("WebPortal (PsychicHttp) started");
    }

} // namespace WebPortal
