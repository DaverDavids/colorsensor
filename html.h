#pragma once

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Color Sensor</title>
<style>
  *{box-sizing:border-box}
  body{font-family:sans-serif;background:#111;color:#ddd;max-width:540px;margin:24px auto;padding:0 16px}
  h1{font-size:1.2em;color:#aaa;margin-bottom:16px}
  h2{font-size:.95em;color:#666;margin:0 0 10px;text-transform:uppercase;letter-spacing:1px}
  .card{background:#1e1e1e;border-radius:10px;padding:14px;margin:10px 0}
  .swatch{height:70px;border-radius:7px;border:1px solid #333;margin-bottom:10px;transition:background .4s}
  .swlive{height:28px;border-radius:5px;border:1px solid #2a2a2a;margin-bottom:8px;transition:background .12s}
  table{width:100%;border-collapse:collapse}
  td{padding:3px 2px;font-size:.85em}
  td:last-child{text-align:right;font-family:monospace;font-weight:600}
  .lbl{color:#777;font-size:.75em;margin-bottom:5px}
  .namelabel{font-size:1.5em;font-weight:700;letter-spacing:1px;margin:2px 0}
  .conflabel{font-size:.82em;color:#aaa;margin-bottom:4px}
  .evdot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#333;margin-right:5px;vertical-align:middle}
  .evdot.on{background:#ff9800;box-shadow:0 0 5px #ff9800;animation:pulse .6s infinite alternate}
  @keyframes pulse{from{opacity:1}to{opacity:.4}}
  .uid{font-family:monospace;font-size:1em;letter-spacing:2px}
  .dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:#333;margin-right:5px}
  .dot.ok{background:#4caf50}
  .row{display:flex;gap:8px;margin-top:8px}
  .calbtn{flex:1;padding:8px 4px;background:#252525;color:#eee;border:1px solid #444;border-radius:5px;cursor:pointer;font-size:.8em}
  .calbtn:hover{background:#333}
  .tbtn{padding:7px 14px;background:#1565c0;color:#fff;border:none;border-radius:5px;cursor:pointer;font-size:.85em}
  .tbtn:hover{background:#1976d2}
  .sbtn{padding:7px 14px;background:#2e7d32;color:#fff;border:none;border-radius:5px;cursor:pointer;font-size:.85em}
  .sbtn:hover{background:#388e3c}
  .dbtn{padding:4px 8px;background:#333;color:#e57373;border:1px solid #555;border-radius:4px;cursor:pointer;font-size:.78em}
  .dbtn:hover{background:#b71c1c;color:#fff;border-color:#b71c1c}
  .pfrow{display:flex;align-items:center;gap:8px;padding:5px 0;border-bottom:1px solid #252525}
  .pfswatch{width:26px;height:26px;border-radius:4px;border:1px solid #333;flex-shrink:0}
  .pfname{flex:1;font-size:.9em}
  .pfrgb{font-family:monospace;font-size:.75em;color:#666}
  input[type=text],input[type=number]{width:100%;padding:7px;background:#222;border:1px solid #444;color:#eee;border-radius:5px;font-size:.9em}
  .setrow{display:flex;align-items:center;gap:10px;margin:5px 0}
  .setrow label{flex:1;font-size:.82em;color:#aaa}
  .setrow input[type=number]{width:90px;flex:none}
  .hint{font-size:.72em;color:#555;margin-top:2px}
  .msg{font-size:.8em;color:#aaa;min-height:1.2em;margin-top:5px}
  .calst{font-size:.75em}
  .bar{height:6px;border-radius:3px;background:#333;margin-top:4px;overflow:hidden}
  .barfill{height:100%;border-radius:3px;background:#4caf50;transition:width .3s}
  .dbgrow td{color:#555;font-size:.75em}
  .dbgrow td:last-child{color:#888}
</style>
</head><body>
<h1>🎨 Color Sensor</h1>

<!-- Detection result -->
<div class="card">
  <div class="lbl">Last Detection &nbsp;<span class="evdot" id="evdot"></span><span id="evlbl" style="font-size:.75em;color:#555">idle</span></div>
  <div class="swatch" id="sw" style="background:#222"></div>
  <div class="namelabel" id="detname">—</div>
  <div class="conflabel" id="detconf"></div>
  <div class="bar"><div class="barfill" id="confbar" style="width:0%"></div></div>
  <table style="margin-top:8px">
    <tr><td>Avg R / G / B</td><td id="rgb">—</td></tr>
    <tr><td>Hex</td><td id="hex">—</td></tr>
    <tr><td>Distance</td><td id="dist">—</td></tr>
    <tr><td>Event duration</td><td id="evdur">—</td></tr>
  </table>
</div>

<!-- Live -->
<div class="card">
  <div class="lbl">Live</div>
  <div class="swlive" id="swlive" style="background:#222"></div>
  <table>
    <tr><td>Norm R/G/B</td><td id="norm">—</td></tr>
    <tr><td>Raw R/G/B/C</td><td id="raw">—</td></tr>
    <tr><td>Clear / Base C</td><td id="clearc">—</td></tr>
    <tr><td>C ratio (trig @ <span id="trigdisp">—</span>)</td><td id="cratio">—</td></tr>
    <tr><td style="color:#555">Last avg RGB</td><td id="lavg" style="color:#555">—</td></tr>
    <tr><td style="color:#555">Last event dur</td><td id="ldur" style="color:#555">—</td></tr>
  </table>
</div>

<!-- RFID -->
<div class="card">
  <div class="lbl">RFID</div>
  <table>
    <tr>
      <td>13.56 MHz (MFRC522)</td>
      <td class="uid" id="uid">—</td>
    </tr>
    <tr>
      <td>125 kHz (EM4100)</td>
      <td class="uid" id="id125">—</td>
    </tr>
    <tr class="dbgrow">
      <td>125k ISR edges / halfbuf</td>
      <td id="em_dbg">—</td>
    </tr>
    <tr class="dbgrow">
      <td>125k frame ready</td>
      <td id="em_frdy">—</td>
    </tr>
  </table>
</div>

<!-- Training -->
<div class="card">
  <h2>Color Profiles &nbsp;<span id="calst" class="calst" style="color:#f44336">uncalibrated</span></h2>
  <div id="pflist"></div>
  <div class="row" style="align-items:flex-end;margin-top:10px">
    <div style="flex:1">
      <div class="lbl">Name for current live reading</div>
      <input type="text" id="trainname" placeholder="e.g. Green Ball" maxlength="19">
    </div>
    <button class="tbtn" onclick="trainProfile()">+ Train</button>
  </div>
  <div class="msg" id="trainmsg"></div>
</div>

<!-- Detection Settings -->
<div class="card">
  <h2>Detection Settings</h2>
  <div class="setrow">
    <label>Trigger ratio<br><span class="hint">Clear channel must drop to X% of baseline. Lower = needs bigger drop.</span></label>
    <input type="number" id="s_trig" step="0.01" min="0.5" max="0.99">
  </div>
  <div class="setrow">
    <label>Min event ms<br><span class="hint">Shorter events are discarded as noise.</span></label>
    <input type="number" id="s_minms" step="1" min="5" max="500">
  </div>
  <div class="setrow">
    <label>Max event ms<br><span class="hint">Longer events = object just sitting there — ignored.</span></label>
    <input type="number" id="s_maxms" step="10" min="100" max="10000">
  </div>
  <div class="setrow">
    <label>Match distance<br><span class="hint">Max RGB Euclidean distance to a trained profile. Higher = looser.</span></label>
    <input type="number" id="s_dist" step="1" min="10" max="441">
  </div>
  <div class="setrow">
    <label>EMA alpha<br><span class="hint">Baseline drift speed. Lower = slower drift.</span></label>
    <input type="number" id="s_ema" step="0.005" min="0.01" max="0.30">
  </div>
  <div class="row" style="margin-top:10px">
    <button class="sbtn" onclick="saveSettings()">Save Settings</button>
  </div>
  <div class="msg" id="setmsg"></div>
</div>

<!-- Calibration -->
<div class="card">
  <h2>White Balance Cal</h2>
  <div class="row">
    <button class="calbtn" onclick="doCalStep('black')">&#11035; Set Black<br><small>Cover sensor</small></button>
    <button class="calbtn" onclick="doCalStep('white')">&#11036; Set White<br><small>Open / LED on</small></button>
    <button class="calbtn" style="color:#555;border-color:#333" onclick="doCalStep('reset')">&#8634; Reset</button>
  </div>
  <div class="msg" id="calmsg"></div>
</div>

<div style="text-align:right;font-size:.7em;color:#333;margin-top:4px">
  <span class="dot" id="dot"></span><span id="ts">—</span>
</div>

<script>
function update(){
  fetch('/data').then(r=>r.json()).then(d=>{
    document.getElementById('sw').style.background=d.hex;
    var nm=document.getElementById('detname');
    nm.textContent=d.name; nm.style.color=d.match?'#fff':'#777';
    var conf=d.match?d.conf:0;
    document.getElementById('detconf').textContent=d.match?('Confidence: '+conf.toFixed(1)+'%  \u00b7  dist: '+d.dist.toFixed(1)):(d.name==='\u2014'?'':'\u2014');
    document.getElementById('confbar').style.width=conf.toFixed(0)+'%';
    document.getElementById('confbar').style.background=conf>70?'#4caf50':conf>40?'#ff9800':'#f44336';
    document.getElementById('rgb').textContent=d.r+' / '+d.g+' / '+d.b;
    document.getElementById('hex').textContent=d.hex;
    document.getElementById('dist').textContent=d.dist.toFixed(1);
    document.getElementById('evdur').textContent=d.evDur+'ms';
    document.getElementById('uid').textContent=d.uid||'\u2014';
    document.getElementById('id125').textContent=d.id125||'\u2014';
    document.getElementById('dot').className='dot ok';
    document.getElementById('ts').textContent=new Date().toLocaleTimeString();
    var cs=document.getElementById('calst');
    if(d.calOK){cs.textContent='\u2713 calibrated';cs.style.color='#4caf50';}else{cs.textContent='uncalibrated';cs.style.color='#f44336';}
    var ev=document.getElementById('evdot'),el=document.getElementById('evlbl');
    if(d.event){ev.className='evdot on';el.textContent='scanning\u2026';el.style.color='#ff9800';}
    else{ev.className='evdot';el.textContent='idle';el.style.color='#555';}
  }).catch(()=>{document.getElementById('dot').className='dot';});
}

function updateLive(){
  fetch('/livedata').then(r=>r.json()).then(d=>{
    document.getElementById('swlive').style.background=d.hex;
    document.getElementById('norm').textContent=d.nr+' / '+d.ng+' / '+d.nb;
    document.getElementById('raw').textContent=d.lr+' / '+d.lg+' / '+d.lb+' / '+d.lc;
    document.getElementById('clearc').textContent=d.lc+' / base: '+d.baseC;
    document.getElementById('trigdisp').textContent=(d.trigRatio*100).toFixed(0)+'%';
    var rat=d.cRatio;
    var rc=document.getElementById('cratio');
    rc.textContent=rat.toFixed(3)+(rat<d.trigRatio?' \u26a0\ufe0f triggering':'');
    rc.style.color=rat<d.trigRatio?'#ff9800':'#ddd';
    document.getElementById('lavg').textContent=d.avgR+' / '+d.avgG+' / '+d.avgB;
    document.getElementById('ldur').textContent=d.lastDur+'ms';
  }).catch(()=>{});
}

function updateDebug125(){
  fetch('/debug125').then(r=>r.json()).then(d=>{
    document.getElementById('em_dbg').textContent=d.edges+' / '+d.halfCount;
    document.getElementById('em_frdy').textContent=d.frameReady?'YES ✓':'no';
  }).catch(()=>{});
}

function loadProfiles(){
  fetch('/profiles').then(r=>r.json()).then(list=>{
    var el=document.getElementById('pflist');
    if(!list.length){el.innerHTML='<div style="color:#444;font-size:.85em;padding:4px 0">No profiles yet \u2014 hold a ball in front and tap Train.</div>';return;}
    el.innerHTML=list.map(p=>
      '<div class="pfrow">'+
      '<div class="pfswatch" style="background:'+p.hex+'"></div>'+
      '<div class="pfname">'+p.name+'<br><span class="pfrgb">'+p.hex+' &nbsp;'+p.r+'/'+p.g+'/'+p.b+'</span></div>'+
      '<button class="dbtn" onclick="delProfile('+p.i+')">Del</button>'+
      '</div>').join('');
  });
}

function trainProfile(){
  var name=document.getElementById('trainname').value.trim();
  if(!name){document.getElementById('trainmsg').textContent='Enter a name first.';return;}
  var fd=new FormData();fd.append('name',name);
  fetch('/profiles/train',{method:'POST',body:fd}).then(r=>r.json()).then(d=>{
    if(d.ok){document.getElementById('trainmsg').textContent='\u2713 Saved "'+d.name+'" as '+d.hex;document.getElementById('trainname').value='';loadProfiles();}
    else document.getElementById('trainmsg').textContent='Error: '+d.error;
  }).catch(()=>{document.getElementById('trainmsg').textContent='Request failed.';});
}

function delProfile(i){
  var fd=new FormData();fd.append('i',i);
  fetch('/profiles/delete',{method:'POST',body:fd}).then(()=>loadProfiles());
}

function loadSettings(){
  fetch('/settings').then(r=>r.json()).then(d=>{
    document.getElementById('s_trig').value=d.trigRatio;
    document.getElementById('s_minms').value=d.minEventMs;
    document.getElementById('s_maxms').value=d.maxEventMs;
    document.getElementById('s_dist').value=d.matchDist;
    document.getElementById('s_ema').value=d.emaAlpha;
  });
}

function saveSettings(){
  var fd=new FormData();
  fd.append('trigRatio', document.getElementById('s_trig').value);
  fd.append('minEventMs',document.getElementById('s_minms').value);
  fd.append('maxEventMs',document.getElementById('s_maxms').value);
  fd.append('matchDist', document.getElementById('s_dist').value);
  fd.append('emaAlpha',  document.getElementById('s_ema').value);
  fetch('/settings',{method:'POST',body:fd}).then(r=>r.json()).then(d=>{
    document.getElementById('setmsg').textContent=d.ok?'\u2713 Saved':'Error saving';
    loadSettings();
  }).catch(()=>{document.getElementById('setmsg').textContent='Request failed.';});
}

function doCalStep(type){
  fetch('/cal/'+type,{method:'POST'}).then(r=>r.json()).then(()=>{
    var m=document.getElementById('calmsg');
    if(type==='black') m.textContent='Black saved. Now point at white LED and tap Set White.';
    else if(type==='white') m.textContent='White saved \u2014 calibration active! Re-train profiles if needed.';
    else m.textContent='Calibration cleared.';
    update();
  }).catch(()=>{document.getElementById('calmsg').textContent='Error \u2014 try again.';});
}

update(); loadProfiles(); loadSettings(); updateDebug125();
setInterval(update, 1000);
setInterval(updateLive, 250);
setInterval(updateDebug125, 500);
</script>
</body></html>
)rawliteral";

const char WIFI_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi Setup</title>
<style>
  body{font-family:sans-serif;background:#111;color:#ddd;max-width:400px;margin:40px auto;padding:0 16px}
  h2{color:#aaa} input{width:100%;padding:9px;margin:5px 0 14px;background:#222;border:1px solid #444;color:#eee;border-radius:5px;font-size:1em}
  button{width:100%;padding:11px;background:#1976d2;color:#fff;border:none;border-radius:5px;font-size:1em;cursor:pointer}
  .note{font-size:.8em;color:#555;margin-top:18px}
</style>
</head><body>
<h2>&#x1F4F6; WiFi Setup</h2>
<form method="POST" action="/setwifi">
  <label>SSID</label><input type="text" name="ssid" autocomplete="off" required>
  <label>Password</label><input type="password" name="psk">
  <button type="submit">Save &amp; Connect</button>
</form>
<p class="note">Saved to flash. Device reboots and connects.</p>
</body></html>
)rawliteral";
