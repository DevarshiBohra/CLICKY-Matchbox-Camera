#include "FS.h"
#include "SD_MMC.h"
#include "wifi_server.h"
#include "config.h"
#include "display.h"
#include "camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Update.h>
#include "driver/gpio.h"
#include <SPI.h>

extern WebServer  server;
extern DNSServer  dnsServer;
extern bool       wifiModeActive;
extern TFT_eSPI   tft;

// ===================== HTML PAGES =====================
static const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CLICKY Gallery</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,sans-serif;background:#111;color:#eee;padding:16px}
  h1{font-size:1.3rem;margin-bottom:4px;color:#fff}
  .sub{font-size:.8rem;color:#888;margin-bottom:8px}
  .tagline{font-size:.8rem;color:#6366f1;font-style:italic;margin-bottom:16px;letter-spacing:.03em}
  .toolbar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:16px;align-items:center}
  button{padding:8px 16px;border:none;border-radius:8px;cursor:pointer;font-size:.85rem;font-weight:600}
  .btn-dl{background:#2563eb;color:#fff}
  .btn-del{background:#dc2626;color:#fff}
  .btn-stop{background:#374151;color:#eee}
  .btn-all{background:#7c3aed;color:#fff}
  button:disabled{opacity:.4;cursor:not-allowed}
  .sel-count{font-size:.85rem;color:#aaa;margin-left:4px}
  .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:12px}
  .card{background:#1e1e1e;border-radius:10px;overflow:hidden;cursor:pointer;transition:outline .1s}
  .card:has(input:checked){outline:3px solid #2563eb}
  .card img{width:100%;aspect-ratio:4/3;object-fit:cover;display:block;background:#333}
  .card-footer{display:flex;align-items:center;gap:6px;padding:6px 8px}
  .card-footer input{width:16px;height:16px;accent-color:#2563eb;flex-shrink:0}
  .fname{font-size:.75rem;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#ccc}
  .empty{color:#666;text-align:center;padding:40px;grid-column:1/-1}
  .toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%);background:#22c55e;color:#fff;
         padding:10px 20px;border-radius:10px;font-weight:600;opacity:0;transition:opacity .4s;pointer-events:none}
  .toast.show{opacity:1}
  .credit{position:fixed;bottom:16px;left:16px;font-size:.75rem;color:#555;line-height:1.5;pointer-events:none}
  @media(prefers-color-scheme:light){
    body{background:#f3f4f6;color:#111}
    .card{background:#fff}
    .fname{color:#444}
    .empty{color:#999}
    .btn-stop{background:#e5e7eb;color:#333}
    .credit{color:#bbb}
  }
</style>
</head>
<body>
<h1>CLICKY Gallery</h1>
<p class="sub" id="sub">Loading...</p>
<p class="tagline">A photo is forever, diamonds ke advertisment se churai hue line</p>
<div class="toolbar">
  <button class="btn-dl"  id="btn-dl"  onclick="downloadSelected()" disabled>Download selected</button>
  <button class="btn-del" id="btn-del" onclick="deleteSelected()"  disabled>Delete selected</button>
  <button class="btn-all" onclick="deleteAll()">Delete all</button>
  <button class="btn-stop" onclick="stopWifi()">Stop WiFi</button>
  <span class="sel-count" id="sel-count"></span>
</div>
<div class="grid" id="grid"></div>
<div class="toast" id="toast"></div>
<div class="credit">Made by:<br>Devarshi Bohra</div>
<script>
let files = [];
async function load() {
  const r = await fetch('/list');
  files = await r.json();
  render();
}
function render() {
  const grid = document.getElementById('grid');
  document.getElementById('sub').textContent =
    files.length + ' photo' + (files.length !== 1 ? 's' : '') + ' on SD card';
  if (!files.length) {
    grid.innerHTML = '<p class="empty">No photos found.</p>';
    return;
  }
  grid.innerHTML = files.map(f => `
    <label class="card">
      <img src="/download?f=${encodeURIComponent(f)}" alt="${f}" loading="lazy">
      <div class="card-footer">
        <input type="checkbox" value="${f}" onchange="updateToolbar()">
        <span class="fname">${f}</span>
      </div>
    </label>`).join('');
}
function selected() {
  return [...document.querySelectorAll('input[type=checkbox]:checked')].map(c => c.value);
}
function updateToolbar() {
  const s = selected();
  const any = s.length > 0;
  document.getElementById('btn-dl').disabled  = !any;
  document.getElementById('btn-del').disabled = !any;
  document.getElementById('sel-count').textContent = any ? s.length + ' selected' : '';
}
function toast(msg, color = '#22c55e') {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.style.background = color;
  el.classList.add('show');
  setTimeout(() => el.classList.remove('show'), 2500);
}
function isIOS() {
  return /iPad|iPhone|iPod/.test(navigator.userAgent) && !window.MSStream;
}
function downloadSelected() {
  const sel = selected();
  if (!sel.length) return;
  if (isIOS()) {
    sel.forEach((f, i) => {
      setTimeout(() => {
        window.open('/download?f=' + encodeURIComponent(f), '_blank');
      }, i * 400);
    });
  } else {
    sel.forEach(f => {
      const a = document.createElement('a');
      a.href = '/download?f=' + encodeURIComponent(f);
      a.download = f;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
    });
  }
}
async function deleteSelected() {
  const sel = selected();
  if (!sel.length) return;
  if (!confirm('Delete ' + sel.length + ' photo(s)?')) return;
  const fd = new FormData();
  sel.forEach(f => fd.append('files', f));
  const r = await fetch('/delete', { method: 'POST', body: fd });
  const j = await r.json();
  toast('Deleted ' + j.deleted + ' file(s)');
  await load();
}
async function deleteAll() {
  if (!files.length) return;
  if (!confirm('Delete ALL ' + files.length + ' photos? This cannot be undone.')) return;
  const r = await fetch('/deleteall', { method: 'POST' });
  const j = await r.json();
  toast('Deleted ' + j.deleted + ' file(s)');
  await load();
}
async function stopWifi() {
  if (!confirm('Stop WiFi and return to camera mode?')) return;
  toast('Restarting...', '#6366f1');
  await fetch('/stop', { method: 'POST' }).catch(() => {});
}
load();
</script>
</body>
</html>
)rawhtml";

static const char OTA_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CLICKY — Firmware Update</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,sans-serif;background:#111;color:#eee;
       display:flex;flex-direction:column;align-items:center;
       justify-content:center;min-height:100vh;padding:24px}
  .card{background:#1e1e1e;border-radius:16px;padding:32px;
        width:100%;max-width:400px;text-align:center}
  h1{font-size:1.2rem;margin-bottom:4px;color:#fff}
  .sub{font-size:.8rem;color:#666;margin-bottom:24px}
  .drop{border:2px dashed #374151;border-radius:12px;padding:32px 16px;
        cursor:pointer;transition:border-color .2s;margin-bottom:16px}
  .drop:hover,.drop.over{border-color:#6366f1}
  .drop input{display:none}
  .drop-icon{font-size:2rem;margin-bottom:8px}
  .drop-label{font-size:.85rem;color:#9ca3af}
  .fname{font-size:.8rem;color:#6366f1;margin-top:6px;min-height:18px}
  button{width:100%;padding:12px;border:none;border-radius:10px;
         background:#6366f1;color:#fff;font-size:.95rem;font-weight:600;
         cursor:pointer;transition:opacity .2s}
  button:disabled{opacity:.4;cursor:not-allowed}
  .bar-wrap{background:#374151;border-radius:8px;height:8px;overflow:hidden;
            margin-top:16px;display:none}
  .bar{height:100%;background:#6366f1;width:0%;transition:width .2s}
  .status{margin-top:12px;font-size:.85rem;color:#9ca3af;min-height:20px}
  .ok{color:#22c55e}.err{color:#ef4444}
</style>
</head>
<body>
<div class="card">
  <h1>Firmware Update</h1>
  <p class="sub">CLICKY — OTA</p>
  <div class="drop" id="drop" onclick="document.getElementById('fw').click()">
    <div class="drop-icon">📦</div>
    <div class="drop-label">Click or drag a .bin file here</div>
    <div class="fname" id="fname"></div>
    <input type="file" id="fw" accept=".bin">
  </div>
  <button id="btn" disabled onclick="upload()">Flash firmware</button>
  <div class="bar-wrap" id="bar-wrap"><div class="bar" id="bar"></div></div>
  <div class="status" id="status"></div>
</div>
<script>
const drop=document.getElementById('drop');
const fw=document.getElementById('fw');
const btn=document.getElementById('btn');
drop.addEventListener('dragover',e=>{e.preventDefault();drop.classList.add('over')});
drop.addEventListener('dragleave',()=>drop.classList.remove('over'));
drop.addEventListener('drop',e=>{
  e.preventDefault();drop.classList.remove('over');
  if(e.dataTransfer.files[0]){fw.files=e.dataTransfer.files;onFile();}
});
fw.addEventListener('change',onFile);
function onFile(){
  if(!fw.files[0])return;
  document.getElementById('fname').textContent=fw.files[0].name;
  btn.disabled=false;
}
function upload(){
  const file=fw.files[0];if(!file)return;
  btn.disabled=true;
  const wrap=document.getElementById('bar-wrap');
  const bar=document.getElementById('bar');
  const st=document.getElementById('status');
  wrap.style.display='block';st.textContent='Uploading...';
  const xhr=new XMLHttpRequest();
  xhr.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);bar.style.width=p+'%';st.textContent='Uploading: '+p+'%';}};
  xhr.onload=()=>{
    if(xhr.status===200){st.innerHTML='<span class="ok">✓ Done — board is restarting</span>';}
    else{st.innerHTML='<span class="err">✗ Upload failed ('+xhr.status+')</span>';btn.disabled=false;}
  };
  xhr.onerror=()=>{st.innerHTML='<span class="err">✗ Network error</span>';btn.disabled=false;};
  const fd=new FormData();fd.append('firmware',file);
  xhr.open('POST','/update');xhr.send(fd);
}
</script>
</body>
</html>
)rawhtml";

// ===================== HTTP HANDLERS =====================

void handleCaptivePortal() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", HTML_PAGE);
}

void handleNotFound() {
  String host = server.hostHeader();
  if (host == "captive.apple.com" ||
      host == "www.apple.com" ||
      host == "apple.com") {
    server.send(200, "text/html",
      "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
      "<BODY>Success</BODY></HTML>");
    return;
  }
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void handleList() {
  File root = SD_MMC.open("/");
  String json = "[";
  bool first = true;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String name = entry.name();
    entry.close();
    if (name.endsWith(".jpg") || name.endsWith(".jpeg") ||
        name.endsWith(".JPG") || name.endsWith(".JPEG")) {
      if (name.startsWith("/")) name = name.substring(1);
      if (!first) json += ",";
      json += "\"" + name + "\"";
      first = false;
    }
  }
  root.close();
  json += "]";
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "application/json", json);
}

void handleDownload() {
  if (!server.hasArg("f")) {
    server.send(400, "text/plain", "Missing file param");
    return;
  }
  String filename = "/" + server.arg("f");
  File file = SD_MMC.open(filename);
  if (!file) {
    server.send(404, "text/plain", "Not found");
    return;
  }
  String disposition = "attachment";
  if (server.hasHeader("User-Agent")) {
    String ua = server.header("User-Agent");
    if (ua.indexOf("iPhone") >= 0 || ua.indexOf("iPad") >= 0 || ua.indexOf("iPod") >= 0) {
      disposition = "inline";
    }
  }
  server.sendHeader("Content-Disposition", disposition + "; filename=\"" + server.arg("f") + "\"");
  server.sendHeader("Cache-Control", "no-cache");
  server.streamFile(file, "image/jpeg");
  file.close();
}

void handleDelete() {
  int deleted = 0;
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "files") {
      String path = "/" + server.arg(i);
      if (SD_MMC.remove(path)) {
        Serial.println("Deleted: " + path);
        deleted++;
      }
    }
  }
  server.send(200, "application/json", "{\"deleted\":" + String(deleted) + "}");
}

void handleDeleteAll() {
  File root = SD_MMC.open("/");
  int deleted = 0;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String name = entry.name();
    entry.close();
    if (name.endsWith(".jpg") || name.endsWith(".jpeg") ||
        name.endsWith(".JPG") || name.endsWith(".JPEG")) {
      if (!name.startsWith("/")) name = "/" + name;
      if (SD_MMC.remove(name)) deleted++;
    }
  }
  root.close();
  server.send(200, "application/json", "{\"deleted\":" + String(deleted) + "}");
}

void handleStop() {
  server.send(200, "text/plain", "Restarting...");
  delay(300);
  stopWifiMode();
}

void handleOTAPage() {
  if (!server.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
    return server.requestAuthentication();
  }
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", OTA_PAGE);
}

void handleOTAUpload() {
  server.on("/update", HTTP_POST,
    []() {
      if (!server.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
        return server.requestAuthentication();
      }
      if (Update.hasError()) {
        server.send(500, "text/plain", "Update failed!");
      } else {
        server.send(200, "text/plain", "OK");
        delay(500);
        esp_restart();
      }
    },
    []() {
      if (!server.authenticate(OTA_USERNAME, OTA_PASSWORD)) return;
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("OTA start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
          Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("OTA success: %u bytes\n", upload.totalSize);
        else Update.printError(Serial);
      }
    }
  );
}

// ===================== WIFI MODE: START / STOP =====================

void startWifiMode() {
  Serial.println("Entering WiFi mode...");
  esp_camera_deinit();
  delay(100);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, strlen(AP_PASSWORD) > 0 ? AP_PASSWORD : nullptr, 6, 0, 4);
  bool apOk = WiFi.softAP(AP_SSID, strlen(AP_PASSWORD) > 0 ? AP_PASSWORD : nullptr);
  delay(500);
  Serial.printf("AP %s | IP: %s\n", apOk ? "OK" : "FAILED", WiFi.softAPIP().toString().c_str());

  tft.fillScreen(TFT_NAVY);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("WiFi Active!", 46, 10);
  tft.setTextColor(TFT_CYAN, TFT_NAVY);
  tft.drawString("SSID:", 10, 30);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString(AP_SSID, 10, 41);
  tft.setTextColor(TFT_CYAN, TFT_NAVY);
  tft.drawString("Open browser:", 78, 30);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("192.168.4.1", 78, 41);
  if (!apOk) {
    tft.setTextColor(TFT_RED, TFT_NAVY);
    tft.drawString("AP FAILED!", 7, 57);
  }

  SPI.end();
  pinMode(2,  INPUT_PULLUP);
  pinMode(4,  INPUT_PULLUP);
  pinMode(12, INPUT_PULLUP);
  pinMode(13, INPUT_PULLUP);
  delay(100);

  gpio_reset_pin(GPIO_NUM_2);
  gpio_set_pull_mode(GPIO_NUM_2, GPIO_PULLUP_ONLY);
  delay(50);

  bool sdOk = SD_MMC.begin("/sdcard", true);
  if (!sdOk) Serial.println("SD mount failed — gallery will be empty.");
  else        Serial.println("SD mounted OK.");

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  server.on("/hotspot-detect.html",       HTTP_GET, handleCaptivePortal);
  server.on("/library/test/success.html", HTTP_GET, handleCaptivePortal);
  server.on("/generate_204",              HTTP_GET, handleCaptivePortal);
  server.on("/connecttest.txt",           HTTP_GET, handleCaptivePortal);
  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/list",      HTTP_GET,  handleList);
  server.on("/download",  HTTP_GET,  handleDownload);
  server.on("/delete",    HTTP_POST, handleDelete);
  server.on("/deleteall", HTTP_POST, handleDeleteAll);
  server.on("/stop",      HTTP_POST, handleStop);
  server.on("/update",    HTTP_GET,  handleOTAPage);
  server.onNotFound(handleNotFound);
  handleOTAUpload();

  const char* headerKeys[] = {"User-Agent"};
  server.collectHeaders(headerKeys, 1);

  server.begin();
  Serial.println("Web server started.");
  wifiModeActive = true;
}

void stopWifiMode() {
  Serial.println("Stopping WiFi mode...");
  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  SD_MMC.end();
  wifiModeActive = false;
  Serial.println("Restarting...");
  delay(500);
  esp_restart();
}