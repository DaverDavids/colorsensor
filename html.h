#pragma once

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Color Sensor</title>
<style>
  *{box-sizing:border-box}
  body{font-family:sans-serif;background:#111;color:#ddd;max-width:480px;margin:32px auto;padding:0 16px}
  h1{font-size:1.3em;color:#aaa;margin-bottom:20px}
  .card{background:#1e1e1e;border-radius:10px;padding:16px;margin:12px 0}
  .swatch{height:80px;border-radius:8px;border:1px solid #333;margin-bottom:14px;transition:background .4s}
  .swlive{height:40px;border-radius:6px;border:1px solid #333;margin-bottom:10px;transition:background .15s}
  table{width:100%;border-collapse:collapse}
  td{padding:4px 2px;font-size:.9em}
  td:last-child{text-align:right;font-weight:600;font-family:monospace}
  .uid{font-family:monospace;font-size:1.15em;letter-spacing:2px;word-break:break-all;margin-top:6px}
  .lbl{color:#888;font-size:.8em}
  .sublbl{color:#666;font-size:.75em;margin-bottom:4px}
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#555;margin-right:6px}
  .dot.ok{background:#4caf50}
  .evdot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#555;margin-right:4px;vertical-align:middle}
  .evdot.active{background:#ff9800;box-shadow:0 0 6px #ff9800}
  .calbtn{flex:1;padding:9px;background:#2a2a2a;color:#eee;border:1px solid #555;border-radius:5px;cursor:pointer;font-size:.85em;line-height:1.4}
  .calbtn:hover{background:#383838}
  .calbtn.rst{color:#888;border-color:#444}
</style>
</head><body>
<h1>🎨 Color Sensor Node</h1>

<!-- Committed color (last peak event) -->
<div class="card">
  <div class="lbl">Last Detected Color &nbsp;<span class="evdot" id="evdot"></span><span id="evlbl" style="font-size:.75em;color:#666">idle</span></div>
  <div class="swatch" id="sw"></div>
  <table>
    <tr><td>R</td><td id="r">—</td></tr>
    <tr><td>G</td><td id="g">—</td></tr>
    <tr><td>B</td><td id="b">—</td></tr>
    <tr><td>Clear</td><td id="c">—</td></tr>
    <tr><td>Hex</td><td id="hex">—</td></tr>
  </table>
</div>

<!-- Live raw readout -->
<div class="card">
  <div class="lbl">Live Raw</div>
  <div class="swlive" id="swlive"></div>
  <table>
    <tr><td>R</td><td id="lr">—</td></tr>
    <tr><td>G</td><td id="lg">—</td></tr>
    <tr><td>B</td><td id="lb">—</td></tr>
    <tr><td>Clear</td><td id="lc">—</td></tr>
    <tr><td>Deviation</td><td id="dev">—</td></tr>
    <tr><td style="color:#555">Baseline R/G/B</td><td id="base" style="color:#555;font-size:.8em">—</td></tr>
  </table>
</div>

<!-- RFID -->
<div class="card">
  <div class="lbl">RFID — Last Tag UID</div>
  <div class="uid" id="uid">—</div>
</div>

<!-- Calibration -->
<div class="card">
  <div class="lbl">Calibration &nbsp;<span id="calst" style="color:#f44336">uncalibrated</span></div>
  <div style="display:flex;gap:8px;margin-top:10px">
    <button class="calbtn" onclick="doCalStep('black')">⬛ Set Black<br><small>Cover sensor</small></button>
    <button class="calbtn" onclick="doCalStep('white')">⬜ Set White<br><small>Point at white paper</small></button>
    <button class="calbtn rst" onclick="doCalStep('reset')">↺ Reset</button>
  </div>
  <div id="calmsg" style="font-size:.8em;color:#aaa;margin-top:8px;min-height:1.2em"></div>
</div>

<div style="text-align:right;font-size:.75em;color:#555">
  <span class="dot" id="dot"></span><span id="ts">—</span>
</div>

<script>
function toHex(r,g,b,max){
  if(!max||max===0) return '#000000';
  var R=Math.min(255,Math.round(r*255/max));
  var G=Math.min(255,Math.round(g*255/max));
  var B=Math.min(255,Math.round(b*255/max));
  return '#'+[R,G,B].map(v=>v.toString(16).padStart(2,'0')).join('');
}

function update(){
  fetch('/data').then(r=>r.json()).then(d=>{
    document.getElementById('r').textContent   = d.r;
    document.getElementById('g').textContent   = d.g;
    document.getElementById('b').textContent   = d.b;
    document.getElementById('c').textContent   = d.c;
    document.getElementById('hex').textContent = d.hex;
    document.getElementById('uid').textContent = d.uid;
    document.getElementById('sw').style.background = d.hex;
    document.getElementById('dot').className   = 'dot ok';
    document.getElementById('ts').textContent  = new Date().toLocaleTimeString();
    var cs = document.getElementById('calst');
    if(d.calOK){ cs.textContent='✓ calibrated'; cs.style.color='#4caf50'; }
    else        { cs.textContent='uncalibrated';  cs.style.color='#f44336'; }
    var ev = document.getElementById('evdot');
    var evlbl = document.getElementById('evlbl');
    if(d.event){ ev.className='evdot active'; evlbl.textContent='scanning…'; }
    else       { ev.className='evdot';         evlbl.textContent='idle'; }
  }).catch(()=>{ document.getElementById('dot').className='dot'; });
}

function updateLive(){
  fetch('/livedata').then(r=>r.json()).then(d=>{
    document.getElementById('lr').textContent  = d.lr;
    document.getElementById('lg').textContent  = d.lg;
    document.getElementById('lb').textContent  = d.lb;
    document.getElementById('lc').textContent  = d.lc;
    document.getElementById('dev').textContent = d.dev;
    document.getElementById('base').textContent= d.bR+' / '+d.bG+' / '+d.bB;
    var max = Math.max(d.lr, d.lg, d.lb, 1);
    document.getElementById('swlive').style.background = toHex(d.lr, d.lg, d.lb, max);
  }).catch(()=>{});
}

function doCalStep(type){
  fetch('/cal/'+type,{method:'POST'}).then(r=>r.json()).then(()=>{
    var msg = document.getElementById('calmsg');
    if(type==='black') msg.textContent='Black saved — now point at white paper and tap Set White.';
    else if(type==='white') msg.textContent='White saved — calibration active!';
    else msg.textContent='Calibration cleared.';
    update();
  }).catch(()=>{ document.getElementById('calmsg').textContent='Error — try again.'; });
}

update();
setInterval(update, 1000);      // committed color + cal status
setInterval(updateLive, 250);   // live raw strip updates 4x/sec
</script>
</body></html>
)rawliteral";

// ── Captive portal / WiFi config page ────────────────────────────────────────
const char WIFI_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi Setup</title>
<style>
  body{font-family:sans-serif;background:#111;color:#ddd;max-width:400px;margin:40px auto;padding:0 16px}
  h2{color:#aaa}
  input{width:100%;padding:9px;margin:5px 0 14px;background:#222;border:1px solid #444;color:#eee;border-radius:5px;font-size:1em}
  button{width:100%;padding:11px;background:#1976d2;color:#fff;border:none;border-radius:5px;font-size:1em;cursor:pointer}
  button:hover{background:#1565c0}
  .note{font-size:.8em;color:#666;margin-top:18px}
</style>
</head><body>
<h2>📶 WiFi Setup</h2>
<form method="POST" action="/setwifi">
  <label>SSID</label>
  <input type="text" name="ssid" autocomplete="off" required>
  <label>Password</label>
  <input type="password" name="psk">
  <button type="submit">Save &amp; Connect</button>
</form>
<p class="note">Settings are saved to flash. Device will reboot and connect.</p>
</body></html>
)rawliteral";
