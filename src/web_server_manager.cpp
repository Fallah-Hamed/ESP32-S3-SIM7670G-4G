#include "web_server_manager.h"
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include "SD_MMC.h"
#include "camera_manager.h"
#include "sd_card_manager.h"
#include "rgb_led_manager.h"
#include "cellular_manager.h"
#include "azure_iot_manager.h"
// wifi_manager.h removed — Azure ops use cellular now

static WebServer        httpSrv(HTTP_PORT);
static WebSocketsServer wsSrv(WS_PORT);

// ─── HTML page ───────────────────────────────────────────────────────────────
static const char HTML[] = R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-S3 Camera</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{background:#111;color:#eee;font-family:Arial,sans-serif;
         display:flex;flex-direction:column;align-items:center;padding:20px;gap:14px}
    h1{color:#4fc3f7;font-size:1.3rem}
    #wrap{width:100%;max-width:800px;border:2px solid #4fc3f7;
          border-radius:8px;overflow:hidden;background:#000}
    #stream{width:100%;display:block}
    .panel{width:100%;max-width:800px;background:#1a1a1a;border-radius:8px;
           padding:14px;display:flex;flex-direction:column;gap:12px}
    .ptitle{font-size:11px;color:#4fc3f7;text-transform:uppercase;
            letter-spacing:1px;margin-bottom:2px}
    .row{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
    .row label{font-size:13px;color:#aaa;min-width:90px}
    select,input[type=range]{background:#222;color:#eee;border:1px solid #4fc3f7;
                              border-radius:5px;padding:6px;font-size:13px}
    select{flex:1}
    button{background:#4fc3f7;color:#111;border:none;padding:8px 18px;
           border-radius:6px;cursor:pointer;font-size:13px;font-weight:bold;
           transition:background .2s}
    button:hover:not(:disabled){background:#81d4fa}
    button:disabled,select:disabled{opacity:.5;cursor:not-allowed}
    input[type=color]{width:42px;height:34px;border:1px solid #4fc3f7;
                      border-radius:5px;cursor:pointer;padding:2px;background:#222}
    input[type=range]{flex:1;min-width:80px}
    .badge{font-size:12px;padding:4px 10px;border-radius:12px;
           background:#222;border:1px solid #555;white-space:nowrap}
    .ok{border-color:#4caf50!important;color:#4caf50}
    .err{border-color:#f44336!important;color:#f44336}
    .warn{border-color:#ff9800!important;color:#ff9800}
    .note{font-size:11px;color:#777}
    #conn{font-size:12px;padding:4px 12px;border-radius:12px;
          background:#222;border:1px solid #555}
    .toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%);
           padding:10px 22px;border-radius:8px;font-size:13px;color:#fff;
           opacity:0;transition:opacity .3s;pointer-events:none;z-index:99}
    .toast.show{opacity:1}
    .ts{background:#2e7d32}.te{background:#c62828}.ti{background:#1565c0}
    .igrid{font-size:12px;color:#ccc;display:grid;
           grid-template-columns:auto 1fr;gap:4px 16px}
    .igrid span:nth-child(odd){color:#888}
    .igrid span:nth-child(even){font-family:monospace;word-break:break-all}
    footer{font-size:11px;color:#555}
    .tog{display:inline-flex;align-items:center;gap:6px;background:#222;
         border:1px solid #555;border-radius:6px;padding:5px 10px;cursor:pointer;
         font-size:12px;color:#aaa;user-select:none;transition:all .2s}
    .tog.on{border-color:#4caf50;color:#4caf50;background:#1a2a1a}
    .tog input{display:none}
    .rng-val{font-size:12px;color:#4fc3f7;min-width:28px;text-align:right;font-family:monospace}
    .rng-wrap{display:flex;align-items:center;gap:8px;flex:1}
    .rng-wrap label{min-width:70px!important;font-size:12px!important}
    .rng-wrap input[type=range]{flex:1}
    details.panel{width:100%;max-width:800px}
    details.panel summary{cursor:pointer;color:#4fc3f7;font-size:11px;
                           text-transform:uppercase;letter-spacing:1px;padding:4px 0}
  </style>
</head>
<body>
  <h1>ESP32-S3 Camera</h1>
  <div class="row" style="justify-content:center">
    <span id="conn" class="err">WS: Connecting…</span>
  </div>
  <div id="wrap"><img id="stream" alt="Connecting…"></div>

  <!-- Camera controls -->
  <div class="panel">
    <div class="row">
      <label>Resolution</label>
      <select id="res" onchange="sendFS(this.value)" disabled>
        <option value="QVGA">QVGA  320×240</option>
        <option value="VGA">VGA   640×480</option>
        <option value="SVGA" selected>SVGA  800×600</option>
        <option value="XGA">XGA  1024×768</option>
        <option value="HD">HD   1280×720</option>
        <option value="SXGA">SXGA 1280×1024</option>
        <option value="UXGA">UXGA 1600×1200</option>
        <option value="FHD">FHD  1920×1080 ⚠</option>
        <option value="QXGA">QXGA 2048×1536 ⚠</option>
        <option value="QSXGA">5MP  2560×1920 ⚠</option>
      </select>
      <span class="note">⚠ slow; best for snapshot</span>
    </div>
    <div class="row">
      <label>Snapshot</label>
      <button id="sbtn" onclick="snap()" disabled>📷 Save to SD</button>
      <span id="sdbadge" class="badge">SD: …</span>
      <span id="phbadge" class="badge"></span>
    </div>
    <div class="row">
      <label>RGB LED</label>
      <input type="color" id="lc" value="#4fc3f7" oninput="schedLED()">
      <input type="range"  id="lb" min="0" max="255" value="64" oninput="schedLED()">
      <button id="led-toggle" onclick="toggleLED()">Off</button>
      <span id="led-status" class="badge ok">ON</span>
    </div>
  </div>

  <!-- Camera Image Settings -->
  <div class="panel">
    <div class="ptitle">📷 Camera Image Settings</div>
    <div class="row">
      <span class="rng-wrap"><label>Quality</label>
        <input type="range" id="cq" min="0" max="63" value="10" oninput="camRng('quality',this.value,this.nextElementSibling)">
        <span class="rng-val">10</span></span>
    </div>
    <div class="row">
      <span class="rng-wrap"><label>Brightness</label>
        <input type="range" id="cbr" min="-2" max="2" value="0" oninput="camRng('brightness',this.value,this.nextElementSibling)">
        <span class="rng-val">0</span></span>
      <span class="rng-wrap"><label>Contrast</label>
        <input type="range" id="cco" min="-2" max="2" value="0" oninput="camRng('contrast',this.value,this.nextElementSibling)">
        <span class="rng-val">0</span></span>
    </div>
    <div class="row">
      <span class="rng-wrap"><label>Saturation</label>
        <input type="range" id="csa" min="-2" max="2" value="0" oninput="camRng('saturation',this.value,this.nextElementSibling)">
        <span class="rng-val">0</span></span>
      <span class="rng-wrap"><label>Sharpness</label>
        <input type="range" id="csh" min="-2" max="2" value="0" oninput="camRng('sharpness',this.value,this.nextElementSibling)">
        <span class="rng-val">0</span></span>
    </div>
    <div class="row">
      <span class="rng-wrap"><label>AE Level</label>
        <input type="range" id="cae" min="-2" max="2" value="0" oninput="camRng('ae_level',this.value,this.nextElementSibling)">
        <span class="rng-val">0</span></span>
      <label>Effect</label>
      <select id="csfx" onchange="send({cmd:'cam_ctrl',var:'special_effect',val:this.value})" disabled>
        <option value="0">No Effect</option>
        <option value="1">Negative</option>
        <option value="2">Grayscale</option>
        <option value="3">Red Tint</option>
        <option value="4">Green Tint</option>
        <option value="5">Blue Tint</option>
        <option value="6">Sepia</option>
      </select>
    </div>
    <div class="row">
      <label>WB Mode</label>
      <select id="cwb" onchange="send({cmd:'cam_ctrl',var:'wb_mode',val:this.value})" disabled>
        <option value="0">Auto</option>
        <option value="1">Sunny</option>
        <option value="2">Cloudy</option>
        <option value="3">Office</option>
        <option value="4">Home</option>
      </select>
      <label>Gain Ceil</label>
      <select id="cgc" onchange="send({cmd:'cam_ctrl',var:'gainceiling',val:this.value})" disabled>
        <option value="0">2x</option><option value="1">4x</option>
        <option value="2">8x</option><option value="3">16x</option>
        <option value="4">32x</option><option value="5">64x</option>
        <option value="6">128x</option>
      </select>
    </div>
  </div>

  <!-- Camera Processing Toggles -->
  <div class="panel">
    <div class="ptitle">🔧 Camera Processing</div>
    <div class="row" style="gap:6px">
      <label class="tog" id="t-hmirror" onclick="camTog('hmirror')"><input type="checkbox">H-Mirror</label>
      <label class="tog" id="t-vflip" onclick="camTog('vflip')"><input type="checkbox">V-Flip</label>
      <label class="tog" id="t-awb" onclick="camTog('awb')"><input type="checkbox">AWB</label>
      <label class="tog" id="t-agc" onclick="camTog('agc')"><input type="checkbox">AGC</label>
      <label class="tog" id="t-aec" onclick="camTog('aec')"><input type="checkbox">AEC</label>
      <label class="tog" id="t-awbgain" onclick="camTog('awb_gain')"><input type="checkbox">AWB Gain</label>
    </div>
    <div class="row" style="gap:6px">
      <label class="tog" id="t-aec2" onclick="camTog('aec2')"><input type="checkbox">AEC2</label>
      <label class="tog" id="t-dcw" onclick="camTog('dcw')"><input type="checkbox">DCW</label>
      <label class="tog" id="t-bpc" onclick="camTog('bpc')"><input type="checkbox">BPC</label>
      <label class="tog" id="t-wpc" onclick="camTog('wpc')"><input type="checkbox">WPC</label>
      <label class="tog" id="t-rawgma" onclick="camTog('raw_gma')"><input type="checkbox">Raw γ</label>
      <label class="tog" id="t-lenc" onclick="camTog('lenc')"><input type="checkbox">Lens Corr</label>
    </div>
  </div>

  <!-- Cloud upload -->
  <div class="panel">
    <div class="ptitle">☁ Azure Cloud Upload</div>
    <div class="row">
      <label>SD Photo</label>
      <select id="photo-sel" disabled>
        <option value="">– select photo –</option>
      </select>
      <button onclick="refreshPhotos()" title="Refresh list">↻</button>
    </div>
    <div class="row">
      <label></label>
      <button id="upload-btn" onclick="uploadPhoto()" disabled>☁ Upload to Hub</button>
      <button id="telemetry-btn" onclick="sendTelemetry()" disabled>📶 Send Telemetry</button>
      <span id="upload-status" class="badge">–</span>
    </div>
    <div class="note" style="padding-left:100px">
      ⚠ Stream pauses during cellular upload (~10–60 s). Resumes automatically.
    </div>
  </div>

  <!-- Modem info -->
  <div class="panel">
    <div class="row">
      <span class="ptitle" style="flex:1">📡 SIM7670G Modem</span>
      <span id="mdm-badge" class="badge">Initializing…</span>
      <button onclick="send({cmd:'modem_info'})" title="Refresh">↻</button>
    </div>
    <div class="igrid">
      <span>Manufacturer</span><span id="mdm-mfr">–</span>
      <span>Model</span><span id="mdm-model">–</span>
      <span>Firmware</span><span id="mdm-fw">–</span>
      <span>IMEI</span><span id="mdm-imei">–</span>
      <span>ICCID</span><span id="mdm-iccid">–</span>
      <span>Network</span><span id="mdm-net">–</span>
      <span>Signal</span><span id="mdm-rssi">–</span>
      <span>IP Address</span><span id="mdm-ip">–</span>
      <span>GPRS</span><span id="mdm-gprs">–</span>
    </div>
  </div>

  <footer>Waveshare ESP32-S3-SIM7670G-4G v2</footer>
  <div id="toast" class="toast"></div>
<script>
let ws=null, ledTmr=null, snapBusy=false, ledIsOff=false, uploading=false;

function connect(){
  ws=new WebSocket('ws://'+location.hostname+':81');
  ws.onopen=()=>{
    setConn(true);
    document.getElementById('res').disabled=false;
    document.getElementById('sbtn').disabled=false;
    document.getElementById('telemetry-btn').disabled=false;
    send({cmd:'status'});
    applyLED();
    send({cmd:'list_photos'});
    send({cmd:'modem_info'});
  };
  ws.onclose=()=>{
    setConn(false);
    document.getElementById('res').disabled=true;
    document.getElementById('sbtn').disabled=true;
    document.getElementById('telemetry-btn').disabled=true;
    enableCam(false);
    setTimeout(connect,3000);
  };
  ws.onerror=()=>ws.close();
  ws.onmessage=e=>onMsg(JSON.parse(e.data));
}

function send(o){ if(ws&&ws.readyState===1) ws.send(JSON.stringify(o)); }

function setConn(ok){
  const el=document.getElementById('conn');
  el.textContent=ok?'WS: Connected':'WS: Disconnected';
  el.className=ok?'ok':'err';
}

function onMsg(d){
  if(d.type==='settings'){
    document.getElementById('res').value=d.framesize;
    if(d.cam){
      setCamVal('quality',d.cam.quality);
      setCamVal('brightness',d.cam.brightness);
      setCamVal('contrast',d.cam.contrast);
      setCamVal('saturation',d.cam.saturation);
      setCamVal('sharpness',d.cam.sharpness);
      setCamVal('ae_level',d.cam.ae_level);
      setCamSel('special_effect',d.cam.special_effect);
      setCamSel('wb_mode',d.cam.wb_mode);
      setCamSel('gainceiling',d.cam.gainceiling);
      setCamTog('hmirror',d.cam.hmirror);
      setCamTog('vflip',d.cam.vflip);
      setCamTog('awb',d.cam.awb);
      setCamTog('agc',d.cam.agc);
      setCamTog('aec',d.cam.aec);
      setCamTog('awb_gain',d.cam.awb_gain);
      setCamTog('aec2',d.cam.aec2);
      setCamTog('dcw',d.cam.dcw);
      setCamTog('bpc',d.cam.bpc);
      setCamTog('wpc',d.cam.wpc);
      setCamTog('raw_gma',d.cam.raw_gma);
      setCamTog('lenc',d.cam.lenc);
    }
    enableCam(true);
  } else if(d.type==='ack'&&d.cmd==='cam_ctrl'){
    // Server confirmed — sync UI to actual applied value
    if(d.var){
      setCamTog(d.var, d.val);
      setCamVal(d.var, d.val);
      setCamSel(d.var, d.val);
    }
  } else if(d.type==='ack'&&d.cmd==='framesize'){
    toast('Changing resolution…','ti');
    const sel=document.getElementById('res');
    sel.disabled=true;
    setTimeout(()=>{
      document.getElementById('stream').src=
        'http://'+location.hostname+'/stream?t='+Date.now();
      sel.disabled=false;
    },900);
  } else if(d.type==='snapshot_ok'){
    toast('Saved: '+d.file,'ts');
    send({cmd:'status'});
    send({cmd:'list_photos'});
    snapDone();
  } else if(d.type==='snapshot_err'){
    toast('Error: '+d.msg,'te');
    snapDone();
  } else if(d.type==='status'){
    const se=document.getElementById('sdbadge');
    const pe=document.getElementById('phbadge');
    if(d.sd){
      se.textContent='SD: '+d.sd_free_mb+' MB free';
      se.className='badge ok';
      pe.textContent=d.photos+' photo'+(d.photos===1?'':'s');
      pe.className='badge';
    } else {
      se.textContent='SD: not found';
      se.className='badge err';
      pe.textContent='';
    }
  } else if(d.type==='photo_list'){
    const sel=document.getElementById('photo-sel');
    const prev=sel.value;
    sel.innerHTML='<option value="">– select photo –</option>';
    (d.photos||[]).forEach(p=>{
      const o=document.createElement('option');
      o.value=p; o.textContent=p; sel.appendChild(o);
    });
    sel.disabled=false;
    if(prev) sel.value=prev;
    document.getElementById('upload-btn').disabled=
      (d.photos||[]).length===0||uploading;
    document.getElementById('telemetry-btn').disabled=uploading;
  } else if(d.type==='upload_progress'){
    document.getElementById('upload-status').textContent=d.msg;
    document.getElementById('upload-status').className='badge warn';
  } else if(d.type==='upload_done'){
    uploading=false;
    document.getElementById('upload-status').textContent='✓ '+d.msg;
    document.getElementById('upload-status').className='badge ok';
    document.getElementById('upload-btn').disabled=false;
    document.getElementById('telemetry-btn').disabled=false;
    toast('Upload complete!','ts');
    setTimeout(()=>{
      document.getElementById('stream').src=
        'http://'+location.hostname+'/stream?t='+Date.now();
    },600);
  } else if(d.type==='upload_error'){
    uploading=false;
    document.getElementById('upload-status').textContent='✗ '+d.msg;
    document.getElementById('upload-status').className='badge err';
    document.getElementById('upload-btn').disabled=false;
    document.getElementById('telemetry-btn').disabled=false;
    toast('Upload failed: '+d.msg,'te');
    setTimeout(()=>{
      document.getElementById('stream').src=
        'http://'+location.hostname+'/stream?t='+Date.now();
    },600);
  } else if(d.type==='modem_info'){
    document.getElementById('mdm-mfr').textContent=d.manufacturer||'–';
    document.getElementById('mdm-model').textContent=d.model||'–';
    document.getElementById('mdm-fw').textContent=d.firmware||'–';
    document.getElementById('mdm-imei').textContent=d.imei||'–';
    document.getElementById('mdm-iccid').textContent=d.iccid||'–';
    document.getElementById('mdm-net').textContent=d.network||'–';
    document.getElementById('mdm-rssi').textContent=
      (d.rssi!==undefined&&d.rssi!==0)?d.rssi+' dBm':'–';
    document.getElementById('mdm-ip').textContent=d.ip||'–';
    document.getElementById('mdm-gprs').textContent=
      d.gprs?'Connected':'Disconnected';
    const badge=document.getElementById('mdm-badge');
    if(d.ready){
      badge.textContent=d.gprs?'LTE Connected':'Ready (no data)';
      badge.className='badge '+(d.gprs?'ok':'warn');
    } else {
      badge.textContent='Initializing…';
      badge.className='badge';
    }
  }
}

function sendFS(v){ send({cmd:'framesize',val:v}); }

function schedLED(){
  if(ledIsOff) return;
  clearTimeout(ledTmr);
  ledTmr=setTimeout(applyLED,50);
}
function applyLED(){
  const hex=document.getElementById('lc').value;
  const br=parseInt(document.getElementById('lb').value);
  const r=Math.round(parseInt(hex.slice(1,3),16)*br/255);
  const g=Math.round(parseInt(hex.slice(3,5),16)*br/255);
  const b=Math.round(parseInt(hex.slice(5,7),16)*br/255);
  send({cmd:'led',val:[r,g,b].map(v=>v.toString(16).padStart(2,'0')).join('')});
}
function toggleLED(){
  ledIsOff=!ledIsOff;
  const btn=document.getElementById('led-toggle');
  const stat=document.getElementById('led-status');
  if(ledIsOff){
    btn.textContent='On'; stat.textContent='OFF'; stat.className='badge err';
    send({cmd:'led',val:'000000'});
  } else {
    btn.textContent='Off'; stat.textContent='ON'; stat.className='badge ok';
    applyLED();
  }
}

function snap(){
  if(snapBusy) return;
  snapBusy=true;
  const b=document.getElementById('sbtn');
  b.disabled=true; b.textContent='⏳ Saving…';
  send({cmd:'snapshot'});
}
function snapDone(){
  snapBusy=false;
  const b=document.getElementById('sbtn');
  b.textContent='📷 Save to SD'; b.disabled=false;
}

function refreshPhotos(){ send({cmd:'list_photos'}); }
function uploadPhoto(){
  const f=document.getElementById('photo-sel').value;
  if(!f){ toast('Select a photo first','te'); return; }
  uploading=true;
  document.getElementById('upload-btn').disabled=true;
  document.getElementById('telemetry-btn').disabled=true;
  document.getElementById('upload-status').textContent='Queued…';
  document.getElementById('upload-status').className='badge warn';
  send({cmd:'upload_photo',filename:f});
}
function sendTelemetry(){
  uploading=true;
  document.getElementById('upload-btn').disabled=true;
  document.getElementById('telemetry-btn').disabled=true;
  document.getElementById('upload-status').textContent='Sending…';
  document.getElementById('upload-status').className='badge warn';
  send({cmd:'send_telemetry'});
}

function toast(msg,cls){
  const t=document.getElementById('toast');
  t.textContent=msg; t.className='toast show '+cls;
  clearTimeout(t._t); t._t=setTimeout(()=>{t.className='toast';},3500);
}

let camRngTmr={};
function camRng(v,val,el){
  if(el) el.textContent=val;
  clearTimeout(camRngTmr[v]);
  camRngTmr[v]=setTimeout(()=>{send({cmd:'cam_ctrl',var:v,val:parseInt(val)});},80);
}
function camTog(v){
  const btn=document.getElementById('t-'+v.replace(/_/g,''));
  const on=btn.classList.contains('on');
  // Send the opposite of current state; ack handler syncs UI
  send({cmd:'cam_ctrl',var:v,val:on?0:1});
}
function setCamVal(v,val){
  const el=document.getElementById({'quality':'cq','brightness':'cbr','contrast':'cco',
    'saturation':'csa','sharpness':'csh','ae_level':'cae'}[v]);
  if(el){el.value=val;el.nextElementSibling.textContent=val;}
}
function setCamTog(v,val){
  const btn=document.getElementById('t-'+v.replace(/_/g,''));
  if(btn){if(val==1)btn.classList.add('on');else btn.classList.remove('on');}
}
function setCamSel(v,val){
  const el=document.getElementById({special_effect:'csfx',wb_mode:'cwb',gainceiling:'cgc'}[v]);
  if(el) el.value=val;
}
function enableCam(en){
  document.getElementById('cq').disabled=!en;
  document.getElementById('csfx').disabled=!en;
  document.getElementById('cwb').disabled=!en;
  document.getElementById('cgc').disabled=!en;
}

document.getElementById('stream').src=
  'http://'+location.hostname+'/stream?t='+Date.now();
connect();
setInterval(()=>send({cmd:'status'}),15000);
</script>
</body>
</html>
)html";

// ─── Public API ──────────────────────────────────────────────────────────────
void webServerSendTXT(uint8_t clientNum, const char* msg) {
    wsSrv.sendTXT(clientNum, msg);
}

// ─── HTTP handlers ───────────────────────────────────────────────────────────
static void handleRoot() {
    httpSrv.send(200, "text/html", HTML);
}

static void handleStream() {
    WiFiClient client = httpSrv.client();
    if (!client.connected()) return;

    client.print(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace;boundary=frame\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n");

    char hdr[64];
    while (client.connected()) {
        camera_fb_t *fb = nullptr;
        if (xQueueReceive(frameQ, &fb, pdMS_TO_TICKS(100)) == pdTRUE && fb) {
            snprintf(hdr, sizeof(hdr),
                     "--frame\r\nContent-Type: image/jpeg\r\n"
                     "Content-Length: %u\r\n\r\n", (unsigned)fb->len);
            client.print(hdr);
            client.write(fb->buf, fb->len);
            client.print("\r\n");
            esp_camera_fb_return(fb);
        }
        if (snapRequested) {
            snapRequested = false;
            doSnapshot(snapClientNum);
        }
        wsSrv.loop();
        if (telemetryRequested || uploadState == UPLOAD_REQUESTED) {
            Serial.println("[STREAM] Breaking for cloud operation...");
            break;
        }
    }
    camera_fb_t *leftover = nullptr;
    while (xQueueReceive(frameQ, &leftover, 0) == pdTRUE && leftover) {
        esp_camera_fb_return(leftover);
        leftover = nullptr;
    }
    client.stop();
}

// ─── WebSocket event handler ─────────────────────────────────────────────────
static void onWsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
    case WStype_DISCONNECTED:
        Serial.printf("[WS%u] Disconnected\n", num);
        break;

    case WStype_CONNECTED: {
        Serial.printf("[WS%u] Connected\n", num);
        sensor_t *s = esp_camera_sensor_get();
        char buf[1024];
        if (s) {
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"settings\",\"framesize\":\"%s\","
                     "\"cam\":{"
                     "\"quality\":%d,\"brightness\":%d,\"contrast\":%d,"
                     "\"saturation\":%d,\"sharpness\":%d,\"ae_level\":%d,"
                     "\"special_effect\":%d,\"wb_mode\":%d,\"gainceiling\":%d,"
                     "\"hmirror\":%d,\"vflip\":%d,"
                     "\"awb\":%d,\"agc\":%d,\"aec\":%d,"
                     "\"awb_gain\":%d,\"aec2\":%d,\"dcw\":%d,"
                     "\"bpc\":%d,\"wpc\":%d,\"raw_gma\":%d,\"lenc\":%d}}",
                     curSizeName,
                     s->status.quality, s->status.brightness, s->status.contrast,
                     s->status.saturation, s->status.sharpness, s->status.ae_level,
                     s->status.special_effect, s->status.wb_mode, s->status.gainceiling,
                     s->status.hmirror, s->status.vflip,
                     s->status.awb, s->status.agc, s->status.aec,
                     s->status.awb_gain, s->status.aec2, s->status.dcw,
                     s->status.bpc, s->status.wpc, s->status.raw_gma, s->status.lenc);
        } else {
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"settings\",\"framesize\":\"%s\"}", curSizeName);
        }
        wsSrv.sendTXT(num, buf);
        break;
    }

    case WStype_TEXT: {
        String msg((char *)payload, len);
        int ci = msg.indexOf("\"cmd\":\"") + 7;
        if (ci < 7) break;
        String cmd = msg.substring(ci, msg.indexOf('"', ci));

        if (cmd == "framesize") {
            int vi = msg.indexOf("\"val\":\"") + 7;
            if (vi < 7) break;
            String val = msg.substring(vi, msg.indexOf('"', vi));
            if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(50))) {
                pendingSize   = sizeEnum(val.c_str());
                pendingChange = true;
                strncpy(curSizeName, val.c_str(), sizeof(curSizeName) - 1);
                xSemaphoreGive(camMutex);
            }
            wsSrv.sendTXT(num, "{\"type\":\"ack\",\"cmd\":\"framesize\"}");

        } else if (cmd == "led") {
            int vi = msg.indexOf("\"val\":\"") + 7;
            if (vi < 7) break;
            String val = msg.substring(vi, msg.indexOf('"', vi));
            unsigned long c = strtoul(val.c_str(), nullptr, 16);
            pixel.setPixelColor(0, pixel.Color(
                (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
            pixel.show();

        } else if (cmd == "cam_ctrl") {
            int vi = msg.indexOf("\"var\":\"") + 7;
            if (vi < 7) break;
            String var = msg.substring(vi, msg.indexOf('"', vi));
            int wi = msg.indexOf("\"val\":") + 6;
            if (wi < 6) break;
            int val = msg.substring(wi).toInt();

            sensor_t *s = esp_camera_sensor_get();
            if (!s) break;

            bool applied = false;
            if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(200))) {
                if      (var == "brightness")     s->set_brightness(s, val);
                else if (var == "contrast")       s->set_contrast(s, val);
                else if (var == "saturation")     s->set_saturation(s, val);
                else if (var == "sharpness")      s->set_sharpness(s, val);
                else if (var == "quality")        s->set_quality(s, val);
                else if (var == "ae_level")       s->set_ae_level(s, val);
                else if (var == "special_effect") s->set_special_effect(s, val);
                else if (var == "wb_mode")        s->set_wb_mode(s, val);
                else if (var == "gainceiling")    s->set_gainceiling(s, (gainceiling_t)val);
                else if (var == "hmirror")        s->set_hmirror(s, val);
                else if (var == "vflip")          s->set_vflip(s, val);
                else if (var == "awb")            s->set_whitebal(s, val);
                else if (var == "agc")            s->set_gain_ctrl(s, val);
                else if (var == "aec")            s->set_exposure_ctrl(s, val);
                else if (var == "awb_gain")       s->set_awb_gain(s, val);
                else if (var == "aec2")           s->set_aec2(s, val);
                else if (var == "dcw")            s->set_dcw(s, val);
                else if (var == "bpc")            s->set_bpc(s, val);
                else if (var == "wpc")            s->set_wpc(s, val);
                else if (var == "raw_gma")        s->set_raw_gma(s, val);
                else if (var == "lenc")           s->set_lenc(s, val);
                xSemaphoreGive(camMutex);
                applied = true;
                Serial.printf("[CAM] %s = %d\n", var.c_str(), val);
            } else {
                Serial.printf("[CAM] mutex timeout for %s\n", var.c_str());
            }

            if (applied) {
                char ack[64];
                snprintf(ack, sizeof(ack),
                         "{\"type\":\"ack\",\"cmd\":\"cam_ctrl\",\"var\":\"%s\",\"val\":%d}",
                         var.c_str(), val);
                wsSrv.sendTXT(num, ack);
            }

        } else if (cmd == "snapshot") {
            snapRequested = true;
            snapClientNum = num;

        } else if (cmd == "status") {
            char buf[96];
            if (sdOK) {
                uint64_t fm = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024ULL*1024ULL);
                snprintf(buf, sizeof(buf),
                         "{\"type\":\"status\",\"sd\":true,"
                         "\"sd_free_mb\":%llu,\"photos\":%d}",
                         fm, photoCount());
            } else {
                strcpy(buf, "{\"type\":\"status\",\"sd\":false}");
            }
            wsSrv.sendTXT(num, buf);

        } else if (cmd == "list_photos") {
            String json = "{\"type\":\"photo_list\",\"photos\":[";
            bool first = true;
            if (sdOK && SD_MMC.exists("/photos")) {
                File d = SD_MMC.open("/photos");
                for (File f = d.openNextFile(); f; f = d.openNextFile()) {
                    if (f.isDirectory()) continue;
                    String name = f.name();
                    int sl2 = name.lastIndexOf('/');
                    if (sl2 >= 0) name = name.substring(sl2 + 1);
                    if (!name.endsWith(".jpg") && !name.endsWith(".jpeg")) continue;
                    if (!first) json += ",";
                    json += "\"" + name + "\"";
                    first = false;
                }
                d.close();
            }
            json += "]}";
            wsSrv.sendTXT(num, json.c_str());

        } else if (cmd == "upload_photo") {
            if (uploadState != UPLOAD_IDLE) {
                wsSrv.sendTXT(num,
                    "{\"type\":\"upload_error\","
                    "\"msg\":\"Upload already in progress\"}");
                break;
            }
            if (!gprsReady) {
                wsSrv.sendTXT(num,
                    "{\"type\":\"upload_error\","
                    "\"msg\":\"Cellular not connected\"}");
                break;
            }
            int fi = msg.indexOf("\"filename\":\"") + 12;
            if (fi < 12) {
                wsSrv.sendTXT(num,
                    "{\"type\":\"upload_error\",\"msg\":\"No filename\"}");
                break;
            }
            String fname = msg.substring(fi, msg.indexOf('"', fi));
            fname.toCharArray(uploadFilename, sizeof(uploadFilename));
            uploadClientNum = num;
            uploadState = UPLOAD_REQUESTED;
            wsSrv.sendTXT(num,
                "{\"type\":\"upload_progress\",\"msg\":\"Upload queued via cellular...\"}");

        } else if (cmd == "send_telemetry") {
            Serial.printf("[WS%u] send_telemetry command\n", num);
            if (uploadState != UPLOAD_IDLE) {
                wsSrv.sendTXT(num,
                    "{\"type\":\"upload_error\","
                    "\"msg\":\"Upload already in progress\"}");
                break;
            }
            if (!gprsReady) {
                wsSrv.sendTXT(num,
                    "{\"type\":\"upload_error\","
                    "\"msg\":\"Cellular not connected\"}");
                break;
            }
            telemetryClientNum = num;
            telemetryRequested = true;
            wsSrv.sendTXT(num,
                "{\"type\":\"upload_progress\",\"msg\":\"Telemetry queued via cellular...\"}");

        } else if (cmd == "modem_info") {
            char buf[440];
            if (xSemaphoreTake(modemMutex, pdMS_TO_TICKS(100))) {
                snprintf(buf, sizeof(buf),
                         "{\"type\":\"modem_info\","
                         "\"manufacturer\":\"%s\",\"model\":\"%s\","
                         "\"firmware\":\"%s\",\"imei\":\"%s\","
                         "\"iccid\":\"%s\",\"network\":\"%s\","
                         "\"rssi\":%d,\"ip\":\"%s\","
                         "\"ready\":%s,\"gprs\":%s}",
                         modemInfo.manufacturer, modemInfo.model,
                         modemInfo.firmware,     modemInfo.imei,
                         modemInfo.iccid,        modemInfo.network,
                         modemInfo.rssi_dbm,     modemInfo.ip,
                         modemReady ? "true" : "false",
                         gprsReady  ? "true" : "false");
                xSemaphoreGive(modemMutex);
                wsSrv.sendTXT(num, buf);
            }
        }
        break;
    }
    default: break;
    }
}

// ─── Init / loop ─────────────────────────────────────────────────────────────
void initWebServer() {
    wsSrv.begin();
    wsSrv.onEvent(onWsEvent);
    httpSrv.on("/",       handleRoot);
    httpSrv.on("/stream", handleStream);
    httpSrv.begin();
    Serial.println("Servers started (HTTP:80, WS:81)");
}

void handleClients() {
    httpSrv.handleClient();
    wsSrv.loop();
}
