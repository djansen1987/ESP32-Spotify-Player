#include "web_portal.h"
#include "app_log.h"
#include "config.h"
#include "led.h"
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
.pl{display:flex;align-items:center;gap:8px;padding:8px 0;border-bottom:1px solid #282828}
.pl span{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.pl button{width:auto;padding:6px 12px;font-size:13px;background:#282828}
#pl-results{max-height:320px;overflow-y:auto;margin-top:10px}
[hidden]{display:none!important}
.tabs{display:flex;gap:4px;margin-bottom:24px;background:#222;padding:4px;border-radius:500px}
.tabs button{flex:1;padding:8px 4px;font-size:14px;background:transparent;color:#B3B3B3}
.tabs button:hover{background:#2a2a2a}
.tabs button.active{background:#1DB954;color:#fff}
</style>
</head>
<body>
<div class="container">
)rawliteral";

const char PAGE_BOTTOM[] PROGMEM = "</div>\n</body>\n</html>\n";

const char INDEX_BODY[] PROGMEM = R"rawliteral(
<h2>Device Setup</h2>

<nav class="tabs" id="tabs">
<button type="button" data-go="wifi">Wi-Fi</button>
<button type="button" data-go="spotify" class="sta-only" hidden>Spotify</button>
<button type="button" data-go="playlists" class="sta-only" hidden>Playlists</button>
<button type="button" data-go="debug">Debug</button>
</nav>

<div class="section" data-tab="wifi" hidden>
<h3>Add a network</h3>
<form action="/save-wifi" method="POST">
<label for="ssid">Network Name (SSID)</label>
<input type="text" id="ssid" name="ssid" required maxlength="32" autocapitalize="none" autocorrect="off">
<label for="password">Password</label>
<input type="password" id="password" name="password" maxlength="63">
<button type="submit">Save Wi-Fi &amp; Restart</button>
</form>
</div>

<div class="section" data-tab="wifi" hidden>
<h3>Known networks</h3>
<span class="hint">The device tries these in order of last use and skips networks without internet. Networks you add are remembered.</span>
<div id="wifi-list"></div>
<div class="msg" id="wifi-warn"></div>
<button type="button" class="small" id="wifi-reset" style="margin-top:12px">Forget all &amp; restart in setup mode</button>
<div class="msg" id="wifi-msg"></div>
</div>

<div class="section" data-tab="spotify" hidden>
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

<div class="section" data-tab="spotify" hidden>
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

<div class="section" data-tab="spotify" hidden>
<h3>RGB LED</h3>
<span class="hint">The LED on the back of the board follows the colour of the album art. Set 0 to turn it off.</span>
<label for="led">Brightness: <span id="led-val"></span>%</label>
<input type="range" id="led" min="0" max="100" step="5" style="width:100%;accent-color:#1DB954">
</div>

<div class="section" data-tab="playlists" hidden>
<h3>Playlists</h3>
<span class="hint">Pinned playlists are shown first on the display. Search all your playlists to pin more.</span>
<div id="pins"></div>

<h4>Add by URL or ID</h4>
<div class="row" style="margin-bottom:6px">
<input type="text" id="add-id" placeholder="https://open.spotify.com/playlist/..." autocapitalize="none" autocorrect="off" style="flex:1;min-width:0">
<button type="button" class="small" id="add-btn">Add</button>
</div>
<input type="text" id="add-name" placeholder="Name (optional, otherwise looked up)" maxlength="80" style="margin-bottom:6px">
<div class="msg" id="add-msg" style="margin-top:0"></div>

<h4>Search Spotify</h4>
<div class="row" style="margin-bottom:6px">
<input type="text" id="sp-search" placeholder="Find any playlist, e.g. a radio" maxlength="50" autocapitalize="none" autocorrect="off" style="flex:1;min-width:0">
<button type="button" class="small" id="sp-btn">Search</button>
</div>
<div class="msg" id="sp-msg" style="margin-top:0"></div>
<div id="sp-results"></div>
<button type="button" class="small" id="sp-more" hidden style="margin-top:10px">More results</button>

<h4>My playlists</h4>
<input type="text" id="pl-search" placeholder="Search my playlists" autocapitalize="none" autocorrect="off" style="margin-bottom:0">
<div class="row" style="margin:10px 0 0">
<button type="button" class="small" id="load-playlists">Refresh playlists</button>
<span id="pl-progress"></span>
</div>
<div id="pl-results"></div>
</div>

<div class="section" data-tab="debug" hidden>
<h3>Debug Log</h3>
<div class="row">
<label style="margin:0"><input type="checkbox" id="debug"> Verbose logging</label>
<button type="button" class="small" id="copy-log">Copy</button>
<button type="button" class="small" id="clear-log">Clear</button>
</div>
<pre id="log"></pre>
</div>

<script>
let challenge=null;
const $=id=>document.getElementById(id);
const msg=t=>$('msg').textContent=t;

fetch('/status').then(r=>r.json()).then(s=>{
  $('resolution').value=s.art;
  $('led').value=s.led;
  $('led-val').textContent=s.led;
  $('client_id').value=s.client_id;
  if(!s.ap){
    document.querySelectorAll('.sta-only').forEach(e=>e.hidden=false);
    if(s.spotify)msg('Spotify is connected.');
    fetch('/pkce').then(r=>r.json()).then(j=>challenge=j.challenge);
  }
  const allowed=[...document.querySelectorAll('#tabs button:not([hidden])')].map(b=>b.dataset.go);
  const wanted=location.hash.slice(1);
  showTab(allowed.includes(wanted)?wanted:(s.ap?'wifi':(s.spotify?'playlists':'spotify')));
});

function showTab(name){
  document.querySelectorAll('[data-tab]').forEach(e=>e.hidden=e.dataset.tab!==name);
  document.querySelectorAll('#tabs button').forEach(b=>b.classList.toggle('active',b.dataset.go===name));
  history.replaceState(null,'','#'+name);
  clearInterval(logTimer);
  if(name==='debug'){loadLog();logTimer=setInterval(loadLog,3000);}
  if(name==='playlists')ensurePlaylists();
  if(name==='wifi')loadWifi();
}
document.querySelectorAll('#tabs button').forEach(b=>b.addEventListener('click',()=>showTab(b.dataset.go)));

function login(){
  const id=$('client_id').value.trim();
  if(!id){msg('Enter your Client ID first.');return;}
  if(!challenge){msg('Not ready yet, try again.');return;}
  const p=new URLSearchParams({client_id:id,response_type:'code',redirect_uri:'http://127.0.0.1:8080/callback',
    code_challenge_method:'S256',code_challenge:challenge,
    scope:'user-read-playback-state user-modify-playback-state playlist-read-private'});
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

const sleep=ms=>new Promise(r=>setTimeout(r,ms));
let ledTimer=null;
$('led').addEventListener('input',e=>{
  $('led-val').textContent=e.target.value;
  clearTimeout(ledTimer);
  ledTimer=setTimeout(()=>postForm('/save-led',{value:e.target.value}),120);
});
$('led').addEventListener('change',e=>{
  clearTimeout(ledTimer);
  postForm('/save-led',{value:e.target.value,save:'1'});
});
$('wifi-reset').addEventListener('click',async()=>{
  if(!confirm('Forget all saved networks and restart in setup mode?'))return;
  await postForm('/wifi-reset',{});
  $('wifi-msg').textContent='Restarting. Connect to the Spotify-Player-Setup network and open http://192.168.4.1';
});
async function loadWifi(){
  const j=await (await fetch('/api/wifi')).json();
  const box=$('wifi-list');
  box.textContent='';
  if(!j.networks.length)box.textContent='No saved networks.';
  j.networks.forEach(n=>{
    const isCurrent=n.ssid===j.current;
    const row=makeRow(n.ssid+(isCurrent?(j.internet?' (connected)':' (connected, no internet)'):''));
    row.appendChild(makeButton(isCurrent?'Forget & disconnect':'Remove',async()=>{
      if(!confirm((isCurrent?'Forget and disconnect from ':'Remove ')+n.ssid+'?'))return;
      await postForm('/wifi-remove',{ssid:n.ssid});
      if(isCurrent)$('wifi-msg').textContent='Restarting. The device joins another saved network or starts the Spotify-Player-Setup network.';
      else loadWifi();
    }));
    box.appendChild(row);
  });
  $('wifi-warn').textContent=(j.current&&!j.internet)?'Connected to '+j.current+' but there is no internet access. Forget it to move on to another network.':'';
}
let pinned=[];
let allPlaylists=[];
let playlistsLoaded=false;
let loadingPlaylists=false;
const CACHE_KEY='playlists-v2';
let logTimer=null;

function makeButton(text,onclick,disabled){
  const b=document.createElement('button');
  b.type='button';
  b.textContent=text;
  b.disabled=!!disabled;
  b.addEventListener('click',onclick);
  return b;
}
function makeRow(name){
  const row=document.createElement('div');
  row.className='pl';
  const label=document.createElement('span');
  label.textContent=name;
  row.appendChild(label);
  return row;
}
async function postForm(url,data){
  return fetch(url,{method:'POST',body:new URLSearchParams(data)});
}
function renderPins(){
  const box=$('pins');
  box.textContent='';
  if(!pinned.length){box.textContent='No pinned playlists yet.';return;}
  pinned.forEach((p,i)=>{
    const row=makeRow(p.name);
    if(i>0)row.appendChild(makeButton('Up',async()=>{await postForm('/pin-up',{id:p.id});loadPins();}));
    row.appendChild(makeButton('Rename',async()=>{
      const name=prompt('Name for this playlist',p.name);
      if(name&&name.trim()){await postForm('/pin-rename',{id:p.id,name:name.trim()});loadPins();}
    }));
    row.appendChild(makeButton('Remove',async()=>{await postForm('/pin-remove',{id:p.id});loadPins();}));
    box.appendChild(row);
  });
}
function renderResults(){
  const box=$('pl-results');
  box.textContent='';
  const q=$('pl-search').value.trim().toLowerCase();
  const ids=new Set(pinned.map(p=>p.id));
  allPlaylists.filter(p=>!q||p.name.toLowerCase().includes(q)||(p.owner||'').toLowerCase().includes(q)).slice(0,60).forEach(p=>{
    const row=makeRow(p.owner?p.name+' - '+p.owner:p.name);
    const isPinned=ids.has(p.id);
    row.appendChild(makeButton(isPinned?'Pinned':'Pin',async()=>{
      const r=await postForm('/pin-add',{id:p.id,name:p.name});
      if(!r.ok)$('pl-progress').textContent=await r.text();
      loadPins();
    },isPinned));
    box.appendChild(row);
  });
}
async function loadPins(){
  const j=await (await fetch('/api/pins')).json();
  pinned=j.pins;
  renderPins();
  renderResults();
}
async function pollWeb(url){
  for(let tries=0;tries<40;tries++){
    const j=await (await fetch(url+(tries===0?'&fresh=1':''))).json();
    if(j.status==='ready')return j;
    if(j.status==='error')throw new Error(j.error);
    await sleep(700);
  }
  throw new Error('Timed out waiting for Spotify');
}
const fetchPage=offset=>pollWeb('/api/playlists?offset='+offset);

function parsePlaylistId(text){
  const m=text.match(/playlist[\/:]([A-Za-z0-9]{10,40})/);
  if(m)return m[1];
  const t=text.trim();
  return /^[A-Za-z0-9]{10,40}$/.test(t)?t:null;
}
async function addById(){
  const box=$('add-msg');
  const id=parsePlaylistId($('add-id').value);
  if(!id){box.textContent='Not a valid playlist URL or ID.';return;}
  box.textContent='Looking up playlist...';
  let name=$('add-name').value.trim();
  let note='';
  if(!name){
    name='Playlist '+id.slice(0,6);
    try{
      const j=await pollWeb('/api/lookup?id='+id);
      if(j.items.length&&j.items[0].name)name=j.items[0].name;
    }catch(e){note=' (name not available from Spotify, use Rename to change it)';}
  }
  const r=await postForm('/pin-add',{id:id,name:name});
  if(r.ok){
    box.textContent='Pinned: '+name+note;
    $('add-id').value='';
    $('add-name').value='';
    loadPins();
  }else{
    box.textContent=await r.text();
  }
}
$('add-btn').addEventListener('click',addById);
$('add-id').addEventListener('keydown',e=>{if(e.key==='Enter')addById();});

let spQuery='',spNext=0;
async function searchSpotify(more){
  const box=$('sp-msg');
  if(!more){
    spQuery=$('sp-search').value.trim();
    spNext=0;
    $('sp-results').textContent='';
    $('sp-more').hidden=true;
  }
  if(!spQuery)return;
  box.textContent='Searching...';
  try{
    let j;
    // Spotify returns null for restricted playlists, so a page can be empty while more exist.
    for(let skips=0;skips<4;skips++){
      j=await pollWeb('/api/search?q='+encodeURIComponent(spQuery)+'&offset='+spNext);
      spNext=j.next;
      if(j.items.length||j.next>=j.total)break;
    }
    const ids=new Set(pinned.map(p=>p.id));
    j.items.forEach(p=>{
      const row=makeRow(p.owner?p.name+' - '+p.owner:p.name);
      row.appendChild(makeButton(ids.has(p.id)?'Pinned':'Pin',async e=>{
        const r=await postForm('/pin-add',{id:p.id,name:p.name});
        if(r.ok){e.target.textContent='Pinned';e.target.disabled=true;loadPins();}
        else box.textContent=await r.text();
      },ids.has(p.id)));
      $('sp-results').appendChild(row);
    });
    spNext=j.next;
    $('sp-more').hidden=!(j.next<j.total);
    box.textContent=$('sp-results').children.length?'':'No playlists found.';
  }catch(e){
    box.textContent='Error: '+e.message;
  }
}
$('sp-btn').addEventListener('click',()=>searchSpotify(false));
$('sp-more').addEventListener('click',()=>searchSpotify(true));
$('sp-search').addEventListener('keydown',e=>{if(e.key==='Enter')searchSpotify(false);});
async function loadPlaylists(){
  if(loadingPlaylists)return;
  loadingPlaylists=true;
  const btn=$('load-playlists');
  const progress=$('pl-progress');
  btn.disabled=true;
  allPlaylists=[];
  let complete=false;
  try{
    let offset=0,total=1;
    while(offset<total){
      const page=await fetchPage(offset);
      allPlaylists.push(...page.items);
      total=page.total;
      progress.textContent='Loaded '+allPlaylists.length+' of '+total;
      renderResults();
      if(page.next<=offset)break;
      offset=page.next;
    }
    complete=true;
    progress.textContent=allPlaylists.length+' playlists loaded.';
  }catch(e){
    progress.textContent='Error: '+e.message;
  }
  if(complete){
    playlistsLoaded=true;
    try{localStorage.setItem(CACHE_KEY,JSON.stringify({time:Date.now(),items:allPlaylists}));}catch(e){}
  }
  btn.disabled=false;
  loadingPlaylists=false;
}
async function ensurePlaylists(){
  if(playlistsLoaded||loadingPlaylists)return;
  try{
    const c=JSON.parse(localStorage.getItem(CACHE_KEY));
    if(c&&Array.isArray(c.items)){
      allPlaylists=c.items;
      playlistsLoaded=true;
      $('pl-progress').textContent=allPlaylists.length+' playlists (cached '+new Date(c.time).toLocaleString()+')';
      renderResults();
      return;
    }
  }catch(e){}
  await loadPlaylists();
}
$('load-playlists').addEventListener('click',loadPlaylists);
$('pl-search').addEventListener('input',renderResults);
loadPins();

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
$('debug').addEventListener('change',async e=>{
  await fetch('/debug',{method:'POST',body:new URLSearchParams({enabled:e.target.checked?'1':'0'})});
});
$('clear-log').addEventListener('click',async()=>{await fetch('/log-clear',{method:'POST'});loadLog();});
</script>
)rawliteral";

AsyncWebServer server(80);

struct PagePart {
    const char *data;
    size_t length;
};

// Streams the page straight from flash so the 13 KB document is never copied into heap.
size_t indexChunk(uint8_t *buffer, size_t maxLen, size_t index) {
    static const PagePart parts[] = {
        {PAGE_TOP, sizeof(PAGE_TOP) - 1},
        {INDEX_BODY, sizeof(INDEX_BODY) - 1},
        {PAGE_BOTTOM, sizeof(PAGE_BOTTOM) - 1},
    };
    size_t pos = index;
    size_t written = 0;
    for (const PagePart &part : parts) {
        if (pos >= part.length) {
            pos -= part.length;
            continue;
        }
        size_t n = min(maxLen - written, part.length - pos);
        memcpy(buffer + written, part.data + pos, n);
        written += n;
        pos = 0;
        if (written == maxLen) break;
    }
    return written;
}

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
    doc["led"] = g_config.ledBrightness;
    doc["client_id"] = g_config.clientId;
    doc["ssid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
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

void handleWifiReset(AsyncWebServerRequest *request) {
    config_clear_networks();
    request->send(200, "text/plain", "OK");
    restartAt = millis() + 1500;
}

void handleApiWifi(AsyncWebServerRequest *request) {
    JsonDocument doc;
    bool connected = WiFi.status() == WL_CONNECTED;
    doc["current"] = connected ? WiFi.SSID() : String();
    doc["internet"] = g_internetOk;
    JsonArray networks = doc["networks"].to<JsonArray>();
    for (const WifiNet &net : config_get_networks()) {
        JsonObject o = networks.add<JsonObject>();
        o["ssid"] = net.ssid;
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void handleWifiRemove(AsyncWebServerRequest *request) {
    if (!request->hasParam("ssid", true)) {
        request->send(400, "text/plain", "SSID is missing.");
        return;
    }
    String ssid = request->getParam("ssid", true)->value();
    bool wasCurrent = WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid;
    config_remove_network(ssid);
    request->send(200, "text/plain", "OK");
    if (wasCurrent) restartAt = millis() + 1500;
}

void handleSaveWifi(AsyncWebServerRequest *request) {
    if (!request->hasParam("ssid", true) || request->getParam("ssid", true)->value().isEmpty()) {
        request->send(400, "text/plain", "SSID is missing.");
        return;
    }
    config_add_network(request->getParam("ssid", true)->value(), request->hasParam("password", true) ? request->getParam("password", true)->value() : "");

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

void handlePins(AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray pins = doc["pins"].to<JsonArray>();
    for (const Pin &pin : config_get_pins()) {
        JsonObject o = pins.add<JsonObject>();
        o["id"] = pin.id;
        o["name"] = pin.name;
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

bool pinIdParam(AsyncWebServerRequest *request, String &id) {
    if (!request->hasParam("id", true)) return false;
    id = request->getParam("id", true)->value();
    return isSafeToken(id, 40);
}

void handlePinAdd(AsyncWebServerRequest *request) {
    String id;
    if (!pinIdParam(request, id) || !request->hasParam("name", true)) {
        request->send(400, "text/plain", "Invalid playlist.");
        return;
    }
    String name = request->getParam("name", true)->value();
    name.trim();
    if (name.length() > 80) name = name.substring(0, 80);
    if (!config_add_pin(id, name)) {
        request->send(409, "text/plain", "Pin limit reached (" + String(MAX_PINS) + "). Remove one first.");
        return;
    }
    request->send(200, "text/plain", "OK");
}

void handlePinRemove(AsyncWebServerRequest *request) {
    String id;
    if (!pinIdParam(request, id)) {
        request->send(400, "text/plain", "Invalid playlist.");
        return;
    }
    config_remove_pin(id);
    request->send(200, "text/plain", "OK");
}

void handlePinRename(AsyncWebServerRequest *request) {
    String id;
    if (!pinIdParam(request, id) || !request->hasParam("name", true)) {
        request->send(400, "text/plain", "Invalid playlist.");
        return;
    }
    String name = request->getParam("name", true)->value();
    name.trim();
    if (name.isEmpty()) {
        request->send(400, "text/plain", "Name cannot be empty.");
        return;
    }
    if (name.length() > 80) name = name.substring(0, 80);
    config_rename_pin(id, name);
    request->send(200, "text/plain", "OK");
}

void handlePinUp(AsyncWebServerRequest *request) {
    String id;
    if (!pinIdParam(request, id)) {
        request->send(400, "text/plain", "Invalid playlist.");
        return;
    }
    config_move_pin_up(id);
    request->send(200, "text/plain", "OK");
}

void respondWebList(AsyncWebServerRequest *request, ListKind kind, uint32_t offset, const String &query) {
    JsonDocument doc;
    if (g_config.refreshToken.isEmpty()) {
        doc["status"] = "error";
        doc["error"] = "Spotify is not connected.";
    } else {
        String key = String(offset) + ":" + query;
        bool fresh = request->hasParam("fresh");

        ListData data;
        spotify_get_list(kind, data);
        bool same = data.key == key;
        if (same && data.status == LIST_LOADING) {
            doc["status"] = "loading";
        } else if (same && !fresh && (data.status == LIST_READY || data.status == LIST_ERROR)) {
            if (data.status == LIST_READY) {
                doc["status"] = "ready";
                doc["next"] = data.next;
                doc["total"] = data.total;
                JsonArray items = doc["items"].to<JsonArray>();
                for (const ListItem &item : data.items) {
                    JsonObject o = items.add<JsonObject>();
                    o["id"] = item.id;
                    o["name"] = item.name;
                    o["owner"] = item.owner;
                }
            } else {
                doc["status"] = "error";
                doc["error"] = data.error;
            }
        } else {
            spotify_request_list(kind, offset, query);
            doc["status"] = "loading";
        }
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

uint32_t offsetParam(AsyncWebServerRequest *request, long maxValue) {
    return request->hasParam("offset") ? (uint32_t)constrain(request->getParam("offset")->value().toInt(), 0, maxValue) : 0;
}

void handleWebPlaylists(AsyncWebServerRequest *request) {
    respondWebList(request, LIST_WEB_PLAYLISTS, offsetParam(request, 100000), "");
}

void handleLookup(AsyncWebServerRequest *request) {
    String id = request->hasParam("id") ? request->getParam("id")->value() : "";
    if (!isSafeToken(id, 40)) {
        request->send(400, "text/plain", "Invalid playlist ID.");
        return;
    }
    respondWebList(request, LIST_WEB_LOOKUP, 0, id);
}

void handleSearch(AsyncWebServerRequest *request) {
    String q = request->hasParam("q") ? request->getParam("q")->value() : "";
    q.trim();
    if (q.isEmpty() || q.length() > 50) {
        request->send(400, "text/plain", "Search text must be 1-50 characters.");
        return;
    }
    respondWebList(request, LIST_WEB_SEARCH, offsetParam(request, 1000), q);
}

void handleSaveLed(AsyncWebServerRequest *request) {
    if (!request->hasParam("value", true)) {
        request->send(400, "text/plain", "Missing value.");
        return;
    }
    int value = constrain(request->getParam("value", true)->value().toInt(), 0, 100);
    g_config.ledBrightness = value;
    led_set_brightness(value);
    if (request->hasParam("save", true)) config_save();
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
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(request->beginChunkedResponse("text/html", indexChunk));
    });
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/pkce", HTTP_GET, handlePkce);
    server.on("/save-wifi", HTTP_POST, handleSaveWifi);
    server.on("/wifi-reset", HTTP_POST, handleWifiReset);
    server.on("/wifi-remove", HTTP_POST, handleWifiRemove);
    server.on("/api/wifi", HTTP_GET, handleApiWifi);
    server.on("/save-art", HTTP_POST, handleSaveArt);
    server.on("/save-led", HTTP_POST, handleSaveLed);
    server.on("/api/pins", HTTP_GET, handlePins);
    server.on("/api/playlists", HTTP_GET, handleWebPlaylists);
    server.on("/api/lookup", HTTP_GET, handleLookup);
    server.on("/api/search", HTTP_GET, handleSearch);
    server.on("/pin-add", HTTP_POST, handlePinAdd);
    server.on("/pin-remove", HTTP_POST, handlePinRemove);
    server.on("/pin-up", HTTP_POST, handlePinUp);
    server.on("/pin-rename", HTTP_POST, handlePinRename);
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
