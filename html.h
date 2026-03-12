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
  table{width:100%;border-collapse:collapse}
  td{padding:5px 2px;font-size:.95em}
  td:last-child{text-align:right;font-weight:600;font-family:monospace}
  .uid{font-family:monospace;font-size:1.15em;letter-spacing:2px;word-break:break-all;margin-top:6px}
  .lbl{color:#888;font-size:.8em}
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#555;margin-right:6px}
  .dot.ok{background:#4caf50}
</style>
</head><body>
<h1>🎨 Color Sensor Node</h1>

<div class="card">
  <div class="swatch" id="sw"></div>
  <table>
    <tr><td>Red (raw)</td>  <td id="r">—</td></tr>
    <tr><td>Green (raw)</td><td id="g">—</td></tr>
    <tr><td>Blue (raw)</td> <td id="b">—</td></tr>
    <tr><td>Clear</td>      <td id="c">—</td></tr>
    <tr><td>Hex</td>        <td id="hex">—</td></tr>
  </table>
</div>

<div class="card">
  <div class="lbl">RFID — Last Tag UID</div>
  <div class="uid" id="uid">—</div>
</div>

<div style="text-align:right;font-size:.75em;color:#555">
  <span class="dot" id="dot"></span><span id="ts">—</span>
</div>

<script>
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
  }).catch(()=>{ document.getElementById('dot').className='dot'; });
}
update();
setInterval(update, 1000);
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
