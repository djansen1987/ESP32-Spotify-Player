#include "web_portal.h"
#include "app_log.h"
#include "config.h"
#include "spotify.h"
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <atomic>
#include <mbedtls/sha256.h>

namespace {

const char PAGE_TOP[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Spotify Player Setup</title>
<style>
body{background:#121212;color:#fff;font-family:'Helvetica Neue',Helvetica,Arial,sans-serif;display:flex;justify-content:center;padding:20px;margin:0;box-sizing:border-box}
*,*::before,*::after{box-sizing:border-box}
.container{background:#181818;padding:30px;border-radius:8px;width:100%;max-width:400px;box-shadow:0 4px 12px rgba(0,0,0,.5)}
h2{color:#1DB954;text-align:center;margin-top:0}
p{color:#B3B3B3;line-height:1.5}
.section{margin-bottom:30px;padding-bottom:20px;border-bottom:1px solid #282828}
.section:last-child{border-bottom:none;margin-bottom:0;padding-bottom:0}
label{display:block;margin-bottom:8px;color:#B3B3B3;font-size:14px}
input[type=text],input[type=password],select{width:100%;padding:12px;margin-bottom:15px;background:#282828;border:1px solid #3E3E3E;color:#fff;border-radius:4px;font-size:16px}
input:focus{outline:none;border-color:#1DB954}
button,a.btn{display:block;width:100%;padding:14px;background:#1DB954;color:#fff;border:none;border-radius:500px;font-size:16px;font-weight:bold;cursor:pointer;text-align:center;text-decoration:none}
button:hover,a.btn:hover{background:#1ed760}
.hint{font-size:12px;color:#B3B3B3;margin-bottom:15px;display:block}
code{color:#1DB954;word-break:break-all}
.msg{margin-top:15px;font-size:14px;color:#F5A623;min-height:1em}
details summary{cursor:pointer;font-size:18px;font-weight:bold;margin-bottom:15px}
.row{display:flex;align-items:center;gap:10px;margin-bottom:15px;color:#B3B3B3;font-size:14px}
.row input{width:auto;margin:0}
button.small{width:auto;padding:8px 16px;font-size:14px;background:#282828}
#log{background:#0e0e0e;border:1px solid #282828;border-radius:4px;padding:10px;font:12px/1.4 monospace;max-height:300px;overflow:auto;white-space:pre-wrap;word-break:break-word;margin:0}
#log .W{color:#F5A623}
#log .E{color:#ff6b6b}
#log .D{color:#7f7f7f}
</style>
</head>
<body>
<div class="container">
)rawliteral";

const char PAGE_BOTTOM[] PROGMEM = "</div>\n</body>\n</html>\n";

const char INDEX_BODY[] PROGMEM = R"rawliteral(
<h2>Device Setup</h2>

<div class="section">
<h3>Wi-Fi Settings</h3>
<form action="/save-wifi" method="POST">
<label for="ssid">Network Name (SSID)</label>
<input type="text" id="ssid" name="ssid" required maxlength="32" autocapitalize="none" autocorrect="off">
<label for="password">Password</label>
<input type="password" id="password" name="password" maxlength="63">
<button type="submit">Save Wi-Fi &amp; Restart</button>
</form>
</div>

<div class="section sta-only" hidden>
<h3>Spotify Settings</h3>
<span class="hint">Create an app at developer.spotify.com and add <code>http://127.0.0.1:8080/callback</code> as redirect URI. Playback control requires Spotify Premium.</span>
<label for="client_id">Spotify Client ID</label>
<input type="text" id="client_id" form="spotify-form" name="client_id" maxlength="64" required autocapitalize="none" autocorrect="off">
<button type="button" onclick="login()" style="margin-bottom:15px">1. Login to Spotify</button>
<span class="hint">After logging in the browser fails to load 127.0.0.1. Copy the entire URL from the address bar and paste it below.</span>
<form id="spotify-form">
<label for="redirect_url">Paste URL</label>
<input type="text" id="redirect_url" name="redirect_url" placeholder="http://127.0.0.1:8080/callback?code=..." required autocapitalize="none" autocorrect="off">
<button type="submit">2. Save Spotify Config</button>
</form>
<div class="msg" id="msg"></div>
</div>

<div class="section sta-only" hidden>
<h3>Album Art</h3>
<form id="art-form">
<label for="resolution">Resolution</label>
<select id="resolution" name="resolution">
<option value="64">64 (Fastest, blurry)</option>
<option value="300">300 (Recommended)</option>
<option value="640">640 (Slow download)</option>
</select>
<button type="submit">Save Album Art</button>
</form>
<div class="msg" id="art-msg"></div>
</div>

<div class="section">
<details id="log-section">
<summary>Debug Log</summary>
<div class="row">
<label style="margin:0"><input type="checkbox" id="debug"> Verbose logging</label>
<button type="button" class="small" id="copy-log">Copy</button>
<button type="button" class="small" id="clear-log">Clear</button>
</div>
<pre id="log"></pre>
</details>
</div>

<script>
let challenge=null;
const $=id=>document.getElementById(id);
const msg=t=>$('msg').textContent=t;

fetch('/status').then(r=>r.json()).then(s=>{
  $('resolution').value=s.art;
  if(!s.ap){
    document.querySelectorAll('.sta-only').forEach(e=>e.hidden=false);
    if(s.spotify)msg('Spotify is connected.');
    fetch('/pkce').then(r=>r.json()).then(j=>challenge=j.challenge);
  }
});

function login(){
  const id=$('client_id').value.trim();
  if(!id){msg('Enter your Client ID first.');return;}
  if(!challenge){msg('Not ready yet, try again.');return;}
  const p=new URLSearchParams({client_id:id,response_type:'code',redirect_uri:'http://127.0.0.1:8080/callback',
    code_challenge_method:'S256',code_challenge:challenge,
    scope:'user-read-playback-state user-modify-playback-state'});
  window.open('https://accounts.spotify.com/authorize?'+p,'_blank');
}

async function poll(){
  const s=await (await fetch('/status')).json();
  if(s.busy){setTimeout(poll,1500);return;}
  msg(s.error?('Error: '+s.error):'Spotify connected. The player is ready.');
}

$('spotify-form').addEventListener('submit',async e=>{
  e.preventDefault();
  msg('Saving...');
  const r=await fetch('/save-spotify',{method:'POST',body:new URLSearchParams(new FormData(e.target))});
  if(!r.ok){msg(await r.text());return;}
  poll();
});

$('art-form').addEventListener('submit',async e=>{
  e.preventDefault();
  const r=await fetch('/save-art',{method:'POST',body:new URLSearchParams(new FormData(e.target))});
  $('art-msg').textContent=r.ok?'Saved. The new size is used from the next poll.':await r.text();
});

async function loadLog(){
  const j=await (await fetch('/log')).json();
  $('debug').checked=j.debug;
  logText=j.entries.map(e=>e.t+'s ['+e.l+'] '+e.m).join('\n');
  const box=$('log');
  box.textContent='';
  if(!j.entries.length){box.textContent='No entries.';return;}
  for(const e of j.entries){
    const line=document.createElement('div');
    line.className=e.l;
    line.textContent=e.t+'s ['+e.l+'] '+e.m;
    box.appendChild(line);
  }
}
let logTimer=null;
let logText='';
$('copy-log').addEventListener('click',async()=>{
  let ok=false;
  // The clipboard API is unavailable on plain http, so fall back to execCommand.
  if(navigator.clipboard&&window.isSecureContext){
    try{await navigator.clipboard.writeText(logText);ok=true;}catch(e){}
  }
  if(!ok){
    const ta=document.createElement('textarea');
    ta.value=logText;
    ta.style.position='fixed';
    ta.style.opacity='0';
    document.body.appendChild(ta);
    ta.select();
    ok=document.execCommand('copy');
    document.body.removeChild(ta);
  }
  const b=$('copy-log');
  b.textContent=ok?'Copied':'Copy failed';
  setTimeout(()=>b.textContent='Copy',1500);
});
$('log-section').addEventListener('toggle',()=>{
  clearInterval(logTimer);
  if($('log-section').open){loadLog();logTimer=setInterval(loadLog,3000);}
});
$('debug').addEventListener('change',async e=>{
  await fetch('/debug',{method:'POST',body:new URLSearchParams({enabled:e.target.checked?'1':'0'})});
});
$('clear-log').addEventListener('click',async()=>{await fetch('/log-clear',{method:'POST'});loadLog();});
</script>
)rawliteral";

AsyncWebServer server(80);

String verifier;
String pendingCode;
String pendingVerifier;
String lastError;
std::atomic<bool> hasPending{false};
std::atomic<bool> busy{false};
std::atomic<uint32_t> restartAt{0};

String page(const char *body) {
    String out;
    out.reserve(strlen_P(PAGE_TOP) + strlen_P(body) + 64);
    out += FPSTR(PAGE_TOP);
    out += FPSTR(body);
    out += FPSTR(PAGE_BOTTOM);
    return out;
}

String base64Url(const uint8_t *data, size_t len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    String out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = data[i] << 16;
        if (i + 1 < len) n |= data[i + 1] << 8;
        if (i + 2 < len) n |= data[i + 2];
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        if (i + 1 < len) out += tbl[(n >> 6) & 63];
        if (i + 2 < len) out += tbl[n & 63];
    }
    return out;
}

String newVerifier() {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    String v;
    for (int i = 0; i < 64; i++) v += alphabet[esp_random() % (sizeof(alphabet) - 1)];
    return v;
}

bool isSafeToken(const String &s, size_t maxLen) {
    if (s.isEmpty() || s.length() > maxLen) return false;
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.' && c != '~') return false;
    }
    return true;
}

String extractCode(const String &url) {
    int idx = url.indexOf("?code=");
    if (idx < 0) idx = url.indexOf("&code=");
    if (idx < 0) return "";
    String code = url.substring(idx + 6);
    int end = code.indexOf('&');
    if (end >= 0) code = code.substring(0, end);
    end = code.indexOf('#');
    if (end >= 0) code = code.substring(0, end);
    code.trim();
    return code;
}

void handleStatus(AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["ap"] = WiFi.getMode() == WIFI_AP;
    doc["busy"] = busy.load();
    doc["spotify"] = g_config.refreshToken.length() > 0;
    doc["art"] = g_config.artSize;
    doc["error"] = lastError;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void handlePkce(AsyncWebServerRequest *request) {
    verifier = newVerifier();
    uint8_t digest[32];
    mbedtls_sha256((const unsigned char *)verifier.c_str(), verifier.length(), digest, 0);

    JsonDocument doc;
    doc["challenge"] = base64Url(digest, sizeof(digest));
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void handleSaveWifi(AsyncWebServerRequest *request) {
    if (!request->hasParam("ssid", true) || request->getParam("ssid", true)->value().isEmpty()) {
        request->send(400, "text/plain", "SSID is missing.");
        return;
    }
    g_config.ssid = request->getParam("ssid", true)->value();
    g_config.pass = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";
    config_save();

    static const char body[] PROGMEM =
        "<h2>Wi-Fi saved</h2>"
        "<p>The device is restarting. Reconnect your phone or computer to your own network, "
        "then open the new address shown on the device display to finish the Spotify setup.</p>";
    request->send(200, "text/html", page(body));
    restartAt = millis() + 1500;
}

void handleLog(AsyncWebServerRequest *request) {
    request->send(200, "application/json", log_to_json());
}

void handleDebug(AsyncWebServerRequest *request) {
    bool on = request->hasParam("enabled", true) && request->getParam("enabled", true)->value() == "1";
    log_set_debug(on);
    app_log(LL_INFO, "Verbose logging %s", on ? "enabled" : "disabled");
    request->send(200, "text/plain", "OK");
}

void handleLogClear(AsyncWebServerRequest *request) {
    log_clear();
    request->send(200, "text/plain", "OK");
}

void handleSaveArt(AsyncWebServerRequest *request) {
    int art = request->hasParam("resolution", true) ? request->getParam("resolution", true)->value().toInt() : 0;
    if (art != 64 && art != 300 && art != 640) {
        request->send(400, "text/plain", "Invalid resolution.");
        return;
    }
    g_config.artSize = art;
    config_save();
    request->send(200, "text/plain", "OK");
}

void handleSaveSpotify(AsyncWebServerRequest *request) {
    if (WiFi.status() != WL_CONNECTED) {
        request->send(409, "text/plain", "Connect the device to Wi-Fi first.");
        return;
    }
    if (busy || verifier.isEmpty() || !request->hasParam("redirect_url", true) || !request->hasParam("client_id", true)) {
        request->send(400, "text/plain", "Missing parameters or setup already in progress.");
        return;
    }

    String clientId = request->getParam("client_id", true)->value();
    clientId.trim();
    String redirectUrl = request->getParam("redirect_url", true)->value();
    if (!isSafeToken(clientId, 64)) {
        request->send(400, "text/plain", "Invalid Client ID.");
        return;
    }

    String code = extractCode(redirectUrl);
    if (!isSafeToken(code, 1024)) {
        request->send(400, "text/plain", "Invalid URL pasted. Could not find a valid 'code=' parameter.");
        return;
    }

    g_config.clientId = clientId;
    config_save();

    pendingCode = code;
    pendingVerifier = verifier;
    lastError = "";
    busy = true;
    hasPending = true;
    request->send(200, "text/plain", "OK");
}

} // namespace

void web_portal_begin() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) { request->send(200, "text/html", page(INDEX_BODY)); });
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/pkce", HTTP_GET, handlePkce);
    server.on("/save-wifi", HTTP_POST, handleSaveWifi);
    server.on("/save-art", HTTP_POST, handleSaveArt);
    server.on("/log", HTTP_GET, handleLog);
    server.on("/debug", HTTP_POST, handleDebug);
    server.on("/log-clear", HTTP_POST, handleLogClear);
    server.on("/save-spotify", HTTP_POST, handleSaveSpotify);
    server.begin();
}

void web_portal_loop() {
    uint32_t t = restartAt;
    if (t && (int32_t)(millis() - t) >= 0) ESP.restart();
}

bool web_portal_take_code(String &code, String &verifierOut) {
    if (!hasPending.exchange(false)) return false;
    code = pendingCode;
    verifierOut = pendingVerifier;
    return true;
}

void web_portal_set_result(bool ok, const String &error) {
    lastError = ok ? "" : error;
    busy = false;
}
