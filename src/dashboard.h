// Self-contained status dashboard served by the dongle on port 8080.
// Dark-only page (committed look for a device console). Colors follow the
// validated dataviz reference palette (dark mode): series blue/orange/aqua
// pass CVD + contrast checks on surface #1a1a19; target is a muted reference
// line; status colors (critical/warning) are reserved for the fault banner.
#pragma once

static const char DASH_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mini-Split</title>
<style>
:root{
  --page:#0d0d0d; --surface:#1a1a19; --ink:#ffffff; --ink2:#c3c2b7;
  --muted:#898781; --grid:#2c2c2a; --axis:#383835;
  --s-indoor:#3987e5; --s-outdoor:#d95926; --s-hz:#199e70;
  --good:#0ca30c; --warn:#fab219; --crit:#d03b3b;
  --ring:rgba(255,255,255,0.10);
}
*{box-sizing:border-box;margin:0}
body{background:var(--page);color:var(--ink);
  font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif;padding:16px;
  max-width:760px;margin:0 auto}
h1{font-size:19px;font-weight:600;display:flex;align-items:center;gap:8px}
h1 .dot{width:9px;height:9px;border-radius:50%;background:var(--crit)}
h1 .dot.on{background:var(--good)}
h1 small{color:var(--muted);font-weight:400;font-size:12px;margin-left:auto}
.banner{border-radius:10px;padding:10px 14px;margin:12px 0 0;display:none;
  font-size:14px;border:1px solid}
.banner.crit{display:block;color:#ff9d9d;border-color:var(--crit);
  background:rgba(208,59,59,.12)}
.banner.warn{display:block;color:var(--warn);border-color:var(--warn);
  background:rgba(250,178,25,.10)}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));
  gap:10px;margin:14px 0}
.tile{background:var(--surface);border:1px solid var(--ring);border-radius:10px;
  padding:12px 14px}
.tile .lab{font-size:12px;color:var(--muted)}
.tile .val{font-size:26px;font-weight:600;margin-top:2px}
.tile .sub{font-size:12px;color:var(--ink2);margin-top:2px}
.badges{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 14px}
.badge{background:var(--surface);border:1px solid var(--ring);border-radius:999px;
  padding:4px 12px;font-size:13px;color:var(--ink2)}
.badge b{color:var(--ink);font-weight:600}
.card{background:var(--surface);border:1px solid var(--ring);border-radius:10px;
  padding:14px;margin-bottom:14px}
.card h2{font-size:13px;font-weight:600;color:var(--ink2);margin-bottom:8px}
.legend{display:flex;gap:16px;font-size:12px;color:var(--ink2);margin-bottom:6px}
.legend span{display:flex;align-items:center;gap:6px}
.legend i{width:14px;height:3px;border-radius:2px;display:inline-block}
.chartwrap{position:relative}
canvas{width:100%;height:220px;display:block}
#ch2{height:160px}
.tip{position:absolute;pointer-events:none;background:#0d0d0dee;
  border:1px solid var(--ring);border-radius:8px;padding:7px 10px;font-size:12px;
  color:var(--ink2);display:none;white-space:nowrap;z-index:2}
.tip b{color:var(--ink)}
.kgrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));
  gap:4px 18px;font-size:13px}
.kgrid div{display:flex;justify-content:space-between;gap:8px;
  border-bottom:1px solid var(--grid);padding:4px 0}
.kgrid span{color:var(--muted)}
.kgrid b{color:var(--ink);font-weight:600}
.sys{display:flex;flex-wrap:wrap;gap:6px 18px;font-size:12px;color:var(--muted)}
.sys b{color:var(--ink2);font-weight:500}
</style></head><body>
<h1><span class="dot" id="dot"></span>Mini-Split
  <small id="stamp">connecting…</small></h1>
<div class="banner" id="banner"></div>

<div class="tiles">
  <div class="tile"><div class="lab">Indoor</div>
    <div class="val" id="tIn">–</div><div class="sub" id="state">–</div></div>
  <div class="tile"><div class="lab">Outdoor</div>
    <div class="val" id="tOut">–</div><div class="sub" id="outSub">coil sensor</div></div>
  <div class="tile"><div class="lab">Target</div>
    <div class="val" id="tSet">–</div><div class="sub" id="mode">–</div></div>
  <div class="tile" id="hzTile" style="display:none"><div class="lab">Compressor</div>
    <div class="val" id="tHz">–</div><div class="sub" id="hzSub">–</div></div>
</div>

<div class="badges" id="badges"></div>

<div class="card">
  <h2>Temperature — last 4 hours</h2>
  <div class="legend">
    <span><i style="background:var(--s-indoor)"></i>Indoor</span>
    <span><i style="background:var(--s-outdoor)"></i>Outdoor</span>
    <span><i style="background:var(--muted);height:0;border-top:2px dashed var(--muted)"></i>Target</span>
  </div>
  <div class="chartwrap"><canvas id="ch1"></canvas><div class="tip" id="tip1"></div></div>
</div>

<div class="card" id="hzCard" style="display:none">
  <h2>Compressor frequency — last 4 hours</h2>
  <div class="chartwrap"><canvas id="ch2"></canvas><div class="tip" id="tip2"></div></div>
</div>

<div class="card" id="teleCard" style="display:none">
  <h2>Refrigerant circuit &amp; fans</h2>
  <div class="kgrid" id="kgrid"></div>
</div>

<div class="card"><h2>System</h2><div class="sys" id="sys"></div></div>

<script>
"use strict";
var D=null,px=window.devicePixelRatio||1;
var C={mut:'#898781',grid:'#2c2c2a',axis:'#383835',ink2:'#c3c2b7',surf:'#1a1a19'};
function fmt(v,u){return (v==null||isNaN(v))?'–':(Math.round(v*10)/10)+(u||'°')}
function hm(d){return ('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2)}
function upt(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
  return (d?d+'d ':'')+h+'h '+m+'m'}

// generic line chart: cfg.series=[{key,color,dash,lab}], cfg.unit, cfg.zeroBase
function makeChart(cvId,tipId,cfg){
  var cv=document.getElementById(cvId),cx=cv.getContext('2d'),
      tip=document.getElementById(tipId),hoverI=-1;
  function vals(){var o=[];cfg.series.forEach(function(s){
    (D.hist[s.key]||[]).forEach(function(v){if(v!=null)o.push(v)})});return o}
  function draw(){
    if(!D)return;
    var W=cv.clientWidth,H=cv.clientHeight;
    cv.width=W*px;cv.height=H*px;cx.setTransform(px,0,0,px,0,0);
    cx.clearRect(0,0,W,H);
    var h=D.hist,n=(h[cfg.series[0].key]||[]).length;
    var padL=38,padR=54,padT=8,padB=20,pw=W-padL-padR,ph=H-padT-padB;
    if(n<2){cx.fillStyle=C.mut;cx.font='12px system-ui';
      cx.fillText('collecting samples…',padL,H/2);return}
    var all=vals();if(!all.length)return;
    var lo=cfg.zeroBase?0:Math.floor(Math.min.apply(0,all))-1;
    var hi=Math.ceil(Math.max.apply(0,all))+(cfg.zeroBase?5:1);
    var X=function(i){return padL+pw*i/(n-1)};
    var Y=function(v){return padT+ph*(1-(v-lo)/(hi-lo))};
    cx.font='11px system-ui';cx.textAlign='right';cx.textBaseline='middle';
    var step=Math.max(1,Math.ceil((hi-lo)/5));
    for(var v=Math.ceil(lo);v<=hi;v+=step){
      cx.strokeStyle=C.grid;cx.lineWidth=1;cx.beginPath();
      cx.moveTo(padL,Y(v));cx.lineTo(padL+pw,Y(v));cx.stroke();
      cx.fillStyle=C.mut;cx.fillText(v+cfg.unit,padL-6,Y(v));
    }
    cx.textAlign='center';cx.textBaseline='top';
    var t0=Date.now()-(n-1)*h.dt*1000;
    var fh=new Date(t0);fh.setMinutes(0,0,0);
    for(var t=fh.getTime();t<=Date.now();t+=3600000){
      if(t<t0)continue;
      cx.fillStyle=C.mut;cx.fillText(hm(new Date(t)),X((t-t0)/(h.dt*1000)),padT+ph+6);
    }
    cx.strokeStyle=C.axis;cx.beginPath();
    cx.moveTo(padL,padT+ph);cx.lineTo(padL+pw,padT+ph);cx.stroke();
    cfg.series.forEach(function(s){
      var a=h[s.key]||[];
      cx.strokeStyle=s.color;cx.lineWidth=2;cx.setLineDash(s.dash||[]);
      cx.beginPath();var began=false;
      for(var i=0;i<n;i++){if(a[i]==null){began=false;continue}
        began?cx.lineTo(X(i),Y(a[i])):cx.moveTo(X(i),Y(a[i]));began=true}
      cx.stroke();cx.setLineDash([]);
      if(!s.dash)for(var i=n-1;i>=0;i--)if(a[i]!=null){
        cx.fillStyle=s.color;cx.beginPath();cx.arc(X(i),Y(a[i]),3,0,7);cx.fill();
        cx.fillStyle=C.ink2;cx.textAlign='left';cx.textBaseline='middle';
        cx.font='11px system-ui';
        cx.fillText(fmt(a[i],cfg.unit),X(i)+7,Y(a[i]));break}
    });
    if(hoverI>=0&&hoverI<n){
      cx.strokeStyle=C.axis;cx.lineWidth=1;cx.setLineDash([2,3]);
      cx.beginPath();cx.moveTo(X(hoverI),padT);cx.lineTo(X(hoverI),padT+ph);cx.stroke();
      cx.setLineDash([]);
      cfg.series.forEach(function(s){
        var v=(D.hist[s.key]||[])[hoverI];if(v==null)return;
        cx.fillStyle=s.color;cx.strokeStyle=C.surf;cx.lineWidth=2;
        cx.beginPath();cx.arc(X(hoverI),Y(v),4.5,0,7);cx.fill();cx.stroke();
      });
    }
  }
  cv.addEventListener('mousemove',function(e){
    if(!D)return;var r=cv.getBoundingClientRect();
    var n=(D.hist[cfg.series[0].key]||[]).length,padL=38,padR=54,pw=r.width-padL-padR;
    hoverI=Math.round((e.clientX-r.left-padL)/pw*(n-1));
    if(hoverI<0||hoverI>=n){hoverI=-1;tip.style.display='none';draw();return}
    var t=new Date(Date.now()-(n-1-hoverI)*D.hist.dt*1000),s='<b>'+hm(t)+'</b>';
    cfg.series.forEach(function(sr){
      s+=' &nbsp;'+sr.lab+' <b>'+fmt((D.hist[sr.key]||[])[hoverI],cfg.unit)+'</b>'});
    tip.innerHTML=s;tip.style.display='block';
    var tx=e.clientX-r.left+12;
    if(tx+tip.offsetWidth>r.width)tx=e.clientX-r.left-tip.offsetWidth-12;
    tip.style.left=tx+'px';tip.style.top='8px';
    draw();
  });
  cv.addEventListener('mouseleave',function(){hoverI=-1;tip.style.display='none';draw()});
  return {draw:draw};
}

var tempChart=makeChart('ch1','tip1',{unit:'°',series:[
  {key:'target',color:'#898781',dash:[5,4],lab:'set'},
  {key:'indoor',color:'#3987e5',lab:'in'},
  {key:'outdoor',color:'#d95926',lab:'out'}]});
var hzChart=makeChart('ch2','tip2',{unit:'Hz',zeroBase:true,series:[
  {key:'hz',color:'#199e70',lab:'comp'}]});

function badge(k,v){return '<span class="badge">'+k+' <b>'+v+'</b></span>'}
function krow(k,v){return '<div><span>'+k+'</span><b>'+v+'</b></div>'}

function poll(){
  fetch('/api').then(function(r){return r.json()}).then(function(d){
    D=d;
    document.getElementById('dot').className='dot'+(d.link?' on':'');
    document.getElementById('stamp').textContent=d.link?'live':'AC link down';
    var bn=document.getElementById('banner');
    if(d.err){bn.className='banner crit';
      bn.innerHTML='&#9888; <b>Fault '+d.err+'</b> — '+d.errName}
    else if(d.filter){bn.className='banner warn';
      bn.innerHTML='&#9888; Filter needs cleaning'}
    else bn.className='banner';
    document.getElementById('tIn').innerHTML=fmt(d.indoor);
    document.getElementById('tOut').innerHTML=fmt(d.outdoor);
    document.getElementById('tSet').innerHTML=fmt(d.target);
    document.getElementById('mode').textContent=d.power?d.mode:'off';
    document.getElementById('state').textContent=d.power?'running':'standby';
    var t=d.tele||{};
    if(t.hz!=null){
      document.getElementById('hzTile').style.display='';
      document.getElementById('tHz').innerHTML=fmt(t.hz,' Hz');
      document.getElementById('hzSub').textContent=
        (t.hzTarget?'target '+t.hzTarget+' Hz':'inverter drive');
    }
    var bs=badge('power',d.power?'on':'off')+badge('mode',d.mode)+
      badge('fan',d.fan)+badge('swing',d.swing)+badge('preset',d.preset);
    if(d.auxHeat)bs+=badge('aux heat','on');
    if(d.defrost)bs+=badge('defrost','active');
    if(!d.displayOn)bs+=badge('display','off');
    document.getElementById('badges').innerHTML=bs;
    var kg='';
    if(t.amps!=null)kg+=krow('compressor current',fmt(t.amps,' A'));
    if(t.volts!=null)kg+=krow('inverter voltage (raw)',fmt(t.volts,' V'));
    if(t.powW!=null)kg+=krow('outdoor unit power',fmt(t.powW,' W'));
    if(t.discharge!=null)kg+=krow('discharge temp',fmt(t.discharge));
    if(t.coilIn!=null)kg+=krow('indoor coil (T2)',fmt(t.coilIn));
    if(t.coilOut!=null)kg+=krow('outdoor coil (T3)',fmt(t.coilOut));
    if(t.ambOut!=null)kg+=krow('outdoor ambient (T4)',fmt(t.ambOut));
    if(t.t1!=null)kg+=krow('indoor ambient (T1)',fmt(t.t1));
    if(t.fanIn!=null)kg+=krow('indoor fan',fmt(t.fanIn,' rpm'));
    if(t.fanOut!=null)kg+=krow('outdoor fan',fmt(t.fanOut,' rpm'));
    if(t.humIn!=null)kg+=krow('indoor humidity',fmt(t.humIn,'%'));
    document.getElementById('teleCard').style.display=kg?'':'none';
    document.getElementById('kgrid').innerHTML=kg;
    var hzData=(d.hist.hz||[]).some(function(v){return v!=null});
    document.getElementById('hzCard').style.display=hzData?'':'none';
    document.getElementById('sys').innerHTML=
      '<span>wifi <b>'+d.rssi+' dBm</b></span><span>heap <b>'+
      Math.round(d.heap/1024)+' kB</b></span><span>up <b>'+upt(d.uptime)+
      '</b></span><span>fw <b>'+d.fw+'</b></span><span>telemetry <b>g1:'+
      (d.groups.g1?'✓':'–')+' g2:'+(d.groups.g2?'✓':'–')+
      ' g5:'+(d.groups.g5?'✓':'–')+' g7:'+(d.groups.g7?'✓':'–')+
      '</b></span>';
    tempChart.draw();if(hzData)hzChart.draw();
  }).catch(function(){document.getElementById('dot').className='dot';
    document.getElementById('stamp').textContent='dongle unreachable'});
}
window.addEventListener('resize',function(){tempChart.draw();hzChart.draw()});
poll();setInterval(poll,3000);
</script></body></html>
)HTML";
