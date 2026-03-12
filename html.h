#pragma once

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Color Sensor</title>
<style>
  *{box-sizing:border-box}
  body{font-family:sans-serif;background:#111;color:#ddd;max-width:520px;margin:24px auto;padding:0 16px}
  h1{font-size:1.2em;color:#aaa;margin-bottom:16px}
  .card{background:#1e1e1e;border-radius:10px;padding:14px;margin:10px 0}
  .swatch{height:70px;border-radius:7px;border:1px solid #333;margin-bottom:12px;transition:background .4s}
  .swlive{height:32px;border-radius:5px;border:1px solid #2a2a2a;margin-bottom:8px;transition:background .15s}
  table{width:100%;border-collapse:collapse}
  td{padding:3px 2px;font-size:.88em}
  td:last-child{text-align:right;font-weight:600;font-family:monospace}
  .lbl{color:#888;font-size:.78em;margin-bottom:4px}
  .namelabel{font-size:1.4em;font-weight:700;letter-spacing:1px;margin:4px 0 2px}
  .distlabel{font-size:.78em;color:#888}
  .evdot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#444;margin-right:5px;vertical-align:middle}
  .evdot.on{background:#ff9800;box-shadow:0 0 5px #ff9800}
  .uid{font-family:monospace;font-size:1.05em;letter-spacing:2px;word-break:break-all;margin-top:4px}
  .dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:#444;margin-right:5px}
  .dot.ok{background:#4caf50}
  .calbtn{flex:1;padding:8px;background:#252525;color:#eee;border:1px solid #444;border-radius:5px;cursor:pointer;font-size:.82em;line-height:1.4}
  .calbtn:hover{background:#333}
  .calbtn.rst{color:#777;border-color:#333}
  .tbtn{padding:7px 14px;background:#1565c0;color:#fff;border:none;border-radius:5px;cursor:pointer;font-size:.85em}
  .tbtn:hover{background:#1976d2}
  .dbtn{padding:4px 8px;background:#333;color:#e57373;border:1px solid #555;border-radius:4px;cursor:pointer;font-size:.78em}
  .dbtn:hover{background:#b71c1c;color:#fff;border-color:#b71c1c}
  .pfrow{display:flex;align-items:center;gap:8px;padding:5px 0;border-bottom:1px solid #2a2a2a}
  .pfswatch{width:28px;height:28px;border-radius:4px;border:1px solid #333;flex-shrink:0}
  .pfname{flex:1;font-size:.9em}
  .pfrgb{font-family:monospace;font-size:.78em;color:#888}
  input[type=text]{width:100%;padding:8px;background:#222;border:1px solid #444;color:#eee;border-radius:5px;font-size:.95em;margin-top:4px}
</style>
</head><body>
<h1>🎨 Color Sensor Node</h1>

<!-- Last detected result -->
<div class="card">
  <div class="lbl">Last Detection &nbsp;<span class="evdot" id="evdot"></span><span id="evlbl" style="font-size:.75em;color:#666">idle</span></div>
  <div class="swatch" id="sw"></div>
  <div class="namelabel" id="detname">—</div>
  <div class="distlabel" id="detdist"></div>
  <table style="margin-top:6px">
    <tr><td>R / G / B (norm)</td><td id="rgb">—</td></tr>
    <tr><td>Hex</td><td id="hex">—</td></tr>
  </table>
</div>

<!-- Live raw -->
<div class="card">
  <div class="lbl">Live</div>
  <div class="swlive" id="swlive"></div>
  <table>
    <tr><td>Norm R/G/B</td><td id="norm">—</td></tr>
    <tr><td>Raw R/G/B/C</td><td id="raw">—</td></tr>
    <tr><td>Deviation</td><td id="dev">—</td></tr>
    <tr><td style="color:#555">Baseline</td><td id="base" style="color:#555">—</td></tr>
  </table>
</div>

<!-- RFID -->
<div class="card">
  <div class="lbl">RFID — Last Tag</div>
  <div class="uid" id="uid">—</div>
</div>

<!-- Training -->
<div class="card">
  <div class="lbl">Color Profiles &nbsp;<span id="calst" style="color:#f44336">uncalibrated</span></div>
  <div id="pflist" style="margin:8px 0"></div>
  <div style="display:flex;gap:8px;align-items:flex-end;margin-top:8px">
    <div style="flex:1">
      <div style="font-size:.8em;color:#888;margin-bottom:3px">Name for current reading</div>
      <input type="text" id="trainname" placeholder="e.g. Red Ball" maxlength="19">
    </div>
    <button class="tbtn" onclick="trainProfile()">+ Train</button>
  </div>
  <div id="trainmsg" style="font-size:.8em;color:#aaa;margin-top:6px;min-height:1em"></div>
</div>

<!-- Calibration -->
<div class="card">
  <div class="lbl">White Balance Calibration</div>
  <div style="display:flex;gap:8px;margin-top:8px">
    <button class="calbtn" onclick="doCalStep('black')">⬛ Set Black<br><small>Cover sensor</small></button>
    <button class="calbtn" onclick="doCalStep('white')">⬜ Set White<br><small>Point at white</small></button>
    <button class="calbtn rst" onclick="doCalStep('reset')">↺ Reset</button>
  </div>
  <div id="calmsg" style="font-size:.8em;color:#aaa;margin-top:6px;min-height:1em"></div>
</div>

<div style="text-align:right;font-size:.72em;color:#444;margin-top:4px">
  <span class="dot" id="dot"></span><span id="ts">—</span>
</div>

<script>
function hex3(r,g,b){return '#'+[r,g,b].map(v=>v.toString(16).padStart(2,'0')).join('');}

function update(){
  fetch('/data').then(r=>r.json()).then(d=>{
    document.getElementById('sw').style.background = d.hex;
    document.getElementById('detname').textContent = d.name;
    document.getElementById('detname').style.color = d.match ? '#fff' : '#888';
    document.getElementById('detdist').textContent = d.match ? 'Distance: '+d.dist.toFixed(1) : (d.name==='\u2014'?'':'No trained match');
    document.getElementById('rgb').textContent = d.r+' / '+d.g+' / '+d.b;
    document.getElementById('hex').textContent = d.hex;
    document.getElementById('uid').textContent = d.uid;
    document.getElementById('dot').className = 'dot ok';
    document.getElementById('ts').textContent = new Date().toLocaleTimeString();
    var cs = document.getElementById('calst');
    if(d.calOK){cs.textContent='✓ calibrated';cs.style.color='#4caf50';}
    else{cs.textContent='uncalibrated';cs.style.color='#f44336';}
    var ev=document.getElementById('evdot'), el=document.getElementById('evlbl');
    if(d.event){ev.className='evdot on';el.textContent='scanning…';el.style.color='#ff9800';}
    else{ev.className='evdot';el.textContent='idle';el.style.color='#555';}
  }).catch(()=>{document.getElementById('dot').className='dot';});
}

function updateLive(){
  fetch('/livedata').then(r=>r.json()).then(d=>{
    document.getElementById('swlive').style.background = d.hex;
    document.getElementById('norm').textContent = d.nr+' / '+d.ng+' / '+d.nb;
    document.getElementById('raw').textContent  = d.lr+' / '+d.lg+' / '+d.lb+' / '+d.lc;
    document.getElementById('dev').textContent  = d.dev;
    document.getElementById('base').textContent = d.bR+' / '+d.bG+' / '+d.bB;
  }).catch(()=>{});
}

function loadProfiles(){
  fetch('/profiles').then(r=>r.json()).then(list=>{
    var el=document.getElementById('pflist');
    if(!list.length){el.innerHTML='<div style="color:#555;font-size:.85em">No profiles yet.</div>';return;}
    el.innerHTML=list.map(p=>
      '<div class="pfrow">'+
      '<div class="pfswatch" style="background:'+p.hex+'"></div>'+
      '<div class="pfname">'+p.name+'<br><span class="pfrgb">'+p.hex+' &nbsp; '+p.r+'/'+p.g+'/'+p.b+'</span></div>'+
      '<button class="dbtn" onclick="delProfile('+p.i+')">Delete</button>'+
      '</div>'
    ).join('');
  });
}

function trainProfile(){
  var name=document.getElementById('trainname').value.trim();
  if(!name){document.getElementById('trainmsg').textContent='Enter a name first.';return;}
  var fd=new FormData(); fd.append('name',name);
  fetch('/profiles/train',{method:'POST',body:fd}).then(r=>r.json()).then(d=>{
    if(d.ok){
      document.getElementById('trainmsg').textContent='✓ Saved "'+d.name+'" as '+d.hex;
      document.getElementById('trainname').value='';
      loadProfiles();
    } else {
      document.getElementById('trainmsg').textContent='Error: '+d.error;
    }
  }).catch(()=>{document.getElementById('trainmsg').textContent='Request failed.';});
}

function delProfile(i){
  var fd=new FormData(); fd.append('i',i);
  fetch('/profiles/delete',{method:'POST',body:fd}).then(r=>r.json()).then(()=>loadProfiles());
}

function doCalStep(type){
  fetch('/cal/'+type,{method:'POST'}).then(r=>r.json()).then(()=>{
    var msg=document.getElementById('calmsg');
    if(type==='black') msg.textContent='Black saved — now point at white and tap Set White.';
    else if(type==='white') msg.textContent='White saved — calibration active! Re-train profiles now.';
    else msg.textContent='Calibration cleared.';
    update();
  }).catch(()=>{document.getElementById('calmsg').textContent='Error — try again.';});
}

update();
loadProfiles();
setInterval(update, 1000);
setInterval(updateLive, 250);
</script>
</body></html>
)rawliteral";

// ── Captive portal ────────────────────────────────────────────────────────────────
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
