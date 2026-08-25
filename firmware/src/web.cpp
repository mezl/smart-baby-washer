#include "web.h"

#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include "app.h"
#include "config.h"
#include <cn2core.h>

#include "cn2.h"
#include <Preferences.h>
#include "kasa.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "net.h"

namespace web {

// ---------------------------------------------------------------------------
// USER APP  —  served at /
//
// What the front panel does, in a browser. The engineering page moved to /dev.
//
// It does NOT press panel buttons: nothing the panel does before it commands a
// load reaches the wire, so there is no button code to replay and no way to ask
// the panel to start anything. This drives the ESP32's own cycle runner instead,
// which is why the program list here is the runner's, not the panel's.
//
// Consequence worth knowing: a cycle started here runs under the runner's
// interlocks, not the machine's. See docs/safety.md.
static const char APP_HTML[] PROGMEM = R"HTML(<!doctype html>
<meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Momcozy D8</title>
<style>
 :root{color-scheme:dark}
 *{box-sizing:border-box}
 body{font:15px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;
      padding:14px;background:#0e1116;color:#e6e9ee;max-width:560px;margin:0 auto}
 h1{font-size:13px;margin:0 0 10px;color:#7d8794;letter-spacing:.1em;font-weight:600}
 .card{background:#161b22;border:1px solid #2a323c;border-radius:12px;
       padding:14px;margin-bottom:10px}
 .state{font-size:26px;font-weight:700;letter-spacing:.01em}
 .sub{color:#7d8794;font-size:13px;margin-top:2px}
 .big{display:flex;gap:14px;margin-top:12px}
 .met{flex:1;background:#0e1116;border:1px solid #232a33;border-radius:9px;
      padding:9px 11px}
 .met b{display:block;font-size:22px;font-weight:700}
 .met span{color:#7d8794;font-size:11px;letter-spacing:.06em}
 .modes{display:grid;grid-template-columns:1fr 1fr;gap:7px}
 .m{padding:12px 10px;border:1px solid #38424f;border-radius:9px;background:#1e2630;
    cursor:pointer;text-align:center;font-size:14px;user-select:none}
 .m small{display:block;color:#7d8794;font-size:11px;margin-top:2px}
 .m.on{background:#1d5c39;border-color:#3fb950;color:#eafaf0}
 .go{width:100%;padding:15px;font-size:17px;font-weight:700;border-radius:10px;
     border:1px solid #3fb950;background:#1d5c39;color:#eafaf0;cursor:pointer;
     margin-top:10px}
 .go.stop{background:#5c1d1d;border-color:#f85149;color:#ffecec}
 .go.arm{background:#6b5312;border-color:#d29922;color:#fff6e0}
 .bar{height:7px;background:#232a33;border-radius:4px;overflow:hidden;margin-top:11px}
 .bar i{display:block;height:100%;background:#3fb950;width:0;transition:width .4s}
 .stg{margin-top:9px;font-size:12px;color:#7d8794;max-height:132px;overflow:auto}
 .stg div{padding:2px 0;display:flex;justify-content:space-between}
 .stg .cur{color:#3fb950;font-weight:600}
 .warn{background:#3a2a08;border-color:#7a5a10;color:#ffe6ab}
 .bad{background:#3a1414;border-color:#7a2020;color:#ffd7d7}
 .ok{color:#3fb950}.mut{color:#7d8794}.hot{color:#e8734a}
 a{color:#7d8794;font-size:12px}
 .m,.go,.lb{touch-action:manipulation;-webkit-tap-highlight-color:transparent}
 /* Load buttons. The engineering page renders these as 30 px chips, which is
    fine with a mouse and unusable with a thumb. 64 px and three across. */
 .loads{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-top:10px}
 .lb{min-height:64px;border:1px solid #38424f;border-radius:10px;background:#1e2630;
     display:flex;flex-direction:column;align-items:center;justify-content:center;
     font-size:13px;font-weight:600;user-select:none;cursor:pointer;padding:6px 4px;
     text-align:center;line-height:1.15}
 .lb small{display:block;color:#7d8794;font-size:10px;font-weight:400;margin-top:3px}
 /* forced on by us */
 .lb.on{background:#1d5c39;border-color:#3fb950;color:#eafaf0}
 /* the machine itself is driving it */
 .lb.mach{background:#2a3550;border-color:#5878c8;color:#dce6ff}
 .lb.tap{filter:brightness(1.4)}
 .warn2{background:#3a2a08;border:1px solid #7a5a10;color:#ffe6ab;border-radius:9px;
        padding:9px 11px;font-size:12px;margin-top:10px;line-height:1.35}
 /* :hover latches on a touch screen, so the pressed look is driven by a class
    the tap handler adds and removes rather than by the pointer resting there. */
 .m.tap,.go.tap{filter:brightness(1.4)}
 /* A hairline that fills while a command is in flight. Without it the only
    feedback a tap gets is the next poll, up to a second later, and the natural
    response to that is to tap again. */
 #busy{position:fixed;left:0;top:0;height:2px;width:0;background:#3fb950;
       transition:width .12s;z-index:9;pointer-events:none}
 body.wait #busy{width:100%}
</style>
<div id=busy></div>
<h1>MOMCOZY D8</h1>

<div class=card id=hdr>
  <div class=state id=st>&hellip;</div>
  <div class=sub id=sub></div>
  <div class=bar><i id=pb></i></div>
  <div class=big>
    <div class=met><span>WATER TEMP</span><b id=t1 class=hot>&mdash;</b></div>
    <div class=met><span>LID</span><b id=lid>&mdash;</b></div>
  </div>
</div>

<div class=card id=errcard style="display:none"></div>

<div class=card>
  <div class=modes id=modes></div>
  <button class=go id=go>START</button>
  <div class=stg id=stg></div>
</div>

<div class=card>
  <h1 style="margin:0 0 2px">LOADS &mdash; DIRECT CONTROL</h1>
  <div class=sub id=loadsub></div>
  <div class=loads id=loads></div>
  <button class=go id=lrel style="background:#2a3038;border-color:#4a5462;color:#dde;margin-top:10px">RELEASE ALL</button>
  <button class=go id=lcut style="background:#5c1d1d;border-color:#f85149;color:#ffecec;margin-top:8px;display:none">CUT MAINS</button>
  <div class=warn2 id=cutnote style="display:none">Cuts power at the smart plug &mdash;
    the only way to stop a load the controller has latched on. <b>The machine stays
    off.</b> This board is powered by the plug, so it cannot switch itself back on
    (tested: the plug's countdown is cancelled by the relay change, and a one-off
    schedule does not fire). Restore it in the Kasa app.</div>
  <div class=warn2>Tap drives that load on the real machine &mdash; no cycle, no
    water check, no interlock. Blue means the machine is driving it; green means
    you are. The heater bits will boil a dry sump.</div>
</div>

<div style="text-align:center;padding:4px 0 14px">
  <a href="/dev">engineering page &rarr;</a>
</div>

<script>
const $=i=>document.getElementById(i);
// One request at a time, with a timeout. The device runs the synchronous
// Arduino WebServer and serves exactly one client; overlapping requests just
// queue in the browser's socket pool, and a tap then waits behind them.
let BUSY=0;
async function hit(u,m){
  if(BUSY) return null;
  BUSY=1; const ac=new AbortController(), t=setTimeout(()=>ac.abort(),8000);
  try{ return await fetch(u,{method:m||'GET',signal:ac.signal}) }
  catch(e){ return null }
  finally{ clearTimeout(t); BUSY=0 }
}
function post(u){ document.body.classList.add('wait');
  return hit(u,'POST').then(()=>{document.body.classList.remove('wait');
                                 clearTimeout(TT); return tick()}) }
let MODE=-1, ARM=0, STATE=0;

function pick(m){ if(STATE!==1) post('/api/cycle?mode='+m) }
function go(){
  if(STATE===4) return post('/api/cycle?run=resume');
  if(STATE===1) return post('/api/cycle?run=stop');
  if(!ARM){ ARM=1; draw(LAST); setTimeout(()=>{ARM=0;draw(LAST)},4000); return }
  ARM=0; post('/api/cycle?run=start');
}
$('go').onclick=go;

// Direct load control. Two states per bit: forced ON by us, or passed through.
// A third "force OFF" exists on the engineering page; it is left out here
// because on a phone the useful action is "make this run", and three states in
// one tap target invites the wrong one.
const LOADS=[[0,'Wash pump','b0'],[1,'Drain','b1'],[2,'Water heat','b2'],
             [3,'Air heat','b3'],[4,'Blower','b4'],[5,'Intake','b5']];
let LSET=0;
function lbit(k){
  LSET ^= (1<<k);
  post('/api/panel_ovr?clr='+(LSET?'FF':'0')+'&set='+LSET.toString(16));
}
$('lrel').onclick=()=>{ LSET=0; post('/api/panel_ovr?clr=0&set=0'); };

// Cutting mains needs two taps. It is the one control here that the machine
// cannot undo -- and neither can this board, which the plug also powers.
let CUTARM=0;
$('lcut').onclick=()=>{
  if(!CUTARM){
    CUTARM=1; $('lcut').textContent='TAP AGAIN TO CUT MAINS';
    setTimeout(()=>{CUTARM=0;$('lcut').textContent='CUT MAINS'},4000);
    return;
  }
  CUTARM=0; $('lcut').textContent='cutting...';
  post('/api/kasa?cycle=1');
};
function drawLoads(a){
  LSET = a.p1_set|0;
  const fwd = a.pb1_fwd|0, real = a.pb1|0;
  $('loads').innerHTML = LOADS.map(([k,n,b])=>{
    const forced = (LSET>>k&1), machine = (real>>k&1), live = (fwd>>k&1);
    const cls = forced ? 'lb on' : (machine ? 'lb mach' : 'lb');
    return `<div class="${cls}" onclick="lbit(${k})">${n}`
         + `<small>${b} &middot; ${live?'RUNNING':'off'}</small></div>`;
  }).join('');
  const hasplug = !!(a.plug && a.plug.length);
  $('lcut').style.display = hasplug ? '' : 'none';
  $('cutnote').style.display = hasplug ? '' : 'none';
  $('loadsub').innerHTML = a.locked
    ? '<span class=hot>controller locked &mdash; it will ignore these</span>'
    : (LSET ? '<span class=ok>you are driving '+LOADS.filter(([k])=>LSET>>k&1).length+' load(s)</span>'
            : 'tap a load to drive it directly');
}

// The manual's names, so the page reads like the machine rather than like the
// firmware's internal program list.
const NICE={'Rapid Wash':['Rapid Wash','19 min · 55°C'],
            'Normal Wash':['Normal Wash','29 min · 68°C'],
            'Steam Steril':['Steam Sterilise','9 min · 100°C'],
            'Drying':['Drying','60 min'],
            '72h Storage':['72h Storage','fresh air'],
            'Self-Clean':['Self-Clean','30 min · 70°C']};

// A 20 s drain rendered as "0 min", which reads like a bug in the machine.
const dur=s=> !s ? 'until filled'
  : s<60 ? s+' s'
  : s<3600 ? Math.round(s/60)+' min'
  : (s/3600).toFixed(s%3600?1:0)+' h';

let LAST=null;
function draw(a){
  if(!a) return;
  LAST=a; STATE=a.state;
  const S=['IDLE','RUNNING','COMPLETE','STOPPED','PAUSED'][a.state]||'IDLE';
  // A cycle running at the machine's own panel. The ESP32 identifies the
  // program from its stage sequence; until then it is just "running".
  const pcOn = a.pc && a.state!==1 && a.state!==4;
  $('st').textContent = a.e5 ? 'ERROR' : (pcOn ? 'RUNNING' : S);
  $('st').className='state'+((a.state===1||pcOn)?' ok':(a.state===3||a.e5?' hot':''));

  if(MODE!==a.mode || !$('modes').children.length){
    MODE=a.mode;
    $('modes').innerHTML=a.modes.map((m,i)=>{
      if(m.empty) return '';           // a free slot is not a program
      const n=NICE[m.n]||[m.n,'custom'];
      return `<div class="m${i===a.mode?' on':''}" onclick="pick(${i})">`
            +`${n[0]}<small>${n[1]}</small></div>`;
    }).join('');
  }

  const cur=a.stages[a.stage];
  const mmss=t=>{t=Math.max(0,t|0);const m=(t/60)|0;return m+':'+String(t%60).padStart(2,'0')};
  $('sub').textContent = (a.state===1||a.state===4)
    ? `${a.modes[a.mode].n} — step ${a.stage+1} of ${a.stages.length}: ${cur?cur.name:''}`
    : pcOn
    ? ((a.pc_prog?(NICE[a.pc_prog]||[a.pc_prog])[0]:'cycle from the panel')
       + ' — ' + (a.pc_phase||'') + ' · ' + mmss(a.pc_elapsed) + ' elapsed'
       + (a.pc_prog ? ' · ~' + mmss(a.pc_remain) + ' left' : ''))
    : (a.state===2?'finished — safe to open':
      (a.state===3?(a.why||'stopped'):'ready'));
  $('pb').style.width = (a.state===1||a.state===4)
    ? ((a.stage+1)/a.stages.length*100)+'%'
    : (pcOn && a.pc_total>0)
    ? Math.min(100,100*(a.pc_total-a.pc_remain)/a.pc_total)+'%' : '0';

  drawLoads(a);
  $('t1').textContent=a.temp+' °C';
  $('lid').innerHTML = a.lid ? '<span class=ok>closed</span>'
                             : '<span class=hot>OPEN</span>';

  const b=$('go');
  b.className='go'+(a.state===1?' stop':(a.state===4?' arm':(ARM?' arm':'')));
  b.textContent = a.state===4 ? 'RESUME'
                : a.state===1 ? 'STOP'
                : (ARM?'TAP AGAIN TO START':'START');

  $('stg').innerHTML=a.stages.map((s,i)=>
    `<div class="${a.state===1&&i===a.stage?'cur':''}"><span>${i+1}. ${s.name}</span>`
    +`<span>${dur(s.secs)}</span></div>`).join('');

  // A pause is not a failure, so it reads as an instruction rather than an alarm.
  let e='', bad=false;
  if(a.err){ e=`<b>${a.err}</b><br><span class=sub>${a.errtxt}</span>`; bad=true }
  else if(a.state===4)
    e=`<b>Paused</b><br><span class=sub>${a.why||''}</span>`;
  else if(!a.lid) e='<b>Lid open</b><br><span class=sub>close the lid before starting</span>';
  else if(a.state===3&&a.why){ e=`<b>Cycle stopped</b><br><span class=sub>${a.why}</span>`; bad=true }
  $('errcard').style.display=e?'':'none';
  $('errcard').className='card '+(bad?'bad':'warn');
  $('errcard').innerHTML=e;
}
// Self-rescheduling, so a slow round trip can never stack a second poll on top
// of the first. A plain setInterval did, and the pile-up is what made buttons
// stop answering until the page was reloaded.
let TT=null;
async function tick(){
  clearTimeout(TT);
  if(!document.hidden){
    const r=await hit('/api/app');
    if(r) try{ draw(await r.json()) }catch(e){}
  }
  TT=setTimeout(tick, document.hidden?4000:1000);
}
// A tap has to look like it landed before the device has answered.
addEventListener('pointerdown',e=>{const b=e.target.closest&&e.target.closest('button,.m');
  if(b){b.classList.add('tap'); setTimeout(()=>b.classList.remove('tap'),180)}},
  {passive:true});
document.addEventListener('visibilitychange',()=>{if(!document.hidden)tick()});
tick();
</script>
)HTML";


static WebServer s_server(80);
static bool      s_uploadAuthOk = false;

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<meta charset="utf-8">
<meta name=viewport content="width=device-width,initial-scale=1">
<title>D8 CN2</title>
<style>
 body{font:12px/1.22 ui-monospace,Menlo,Consolas,monospace;margin:0;padding:4px;
      background:#0e1116;color:#dde}
 h1{font-size:13px;margin:0 0 4px;color:#9cf}
 .row{display:flex;gap:8px;flex-wrap:wrap;align-items:flex-start;margin-bottom:8px}
 /* Masonry-style columns. A flex row is as tall as its tallest box, which left a
    large void beside the short ones; columns pack instead. */
 /* Boxes are distributed into columns by JS, shortest-column-first. CSS multicol
    was tried and balances badly here: it fills a column before moving on, so the
    two tallest boxes landed together and the page was as tall as their sum no
    matter how many columns fitted. */
 .cols{display:flex;gap:6px;align-items:flex-start;margin-bottom:6px}
 .colw{flex:1 1 0;min-width:0;display:flex;flex-direction:column;gap:6px}
 .cols .box{width:auto}
 .box{background:#161b22;border:1px solid #2a323c;border-radius:6px;padding:3px 7px}
 .box b{color:#9cf;display:block;margin-bottom:3px;font-size:11px;letter-spacing:.06em}
 button{background:#1e2630;color:#dde;border:1px solid #38424f;padding:2px 7px;
        cursor:pointer;border-radius:4px;margin:1px;font:inherit;
        touch-action:manipulation;-webkit-tap-highlight-color:transparent;
        -webkit-user-select:none;user-select:none}
 /* Hover only where a pointer exists. On a touch screen :hover latches onto the
    last element tapped, so a button pressed a minute ago still looks lit and
    the one you are about to press looks the same as one that is active. */
 @media (hover:hover){button:hover{background:#26313d}}
 button:active,.tap{background:#36434f;border-color:#5a6b7d}
 button.on{background:#2a6;color:#021;border-color:#3c8;font-weight:600}
 button.unk{opacity:.45}
 input,select{background:#0a0d11;color:#dde;border:1px solid #38424f;padding:3px;
       border-radius:3px;text-align:center;box-sizing:border-box;font:inherit}
 input{width:44px}
 select{text-align:left;max-width:100%}
 pre{background:#05070a;border:1px solid #2a323c;padding:8px;overflow:auto;
     max-height:26vh;margin:0;white-space:pre;border-radius:6px;font-size:12px}
 .big{font-size:16px;font-weight:600}
 .lbl{color:#7d8794;font-size:11px}
 .ok{color:#3fb950}.bad{color:#f85149}.warn{color:#d29922}.mut{color:#7d8794}
 table{border-collapse:collapse}td{padding:0 6px 0 0;vertical-align:top}
 /* One byte, eight equal cells across the card. The bit number sits above the
    box and the name below it, so the boxes line up in a row you can read as a
    byte instead of a ragged wrap of variable-width chips. */
 .bits{display:grid;grid-template-columns:repeat(8,1fr);gap:3px;margin-top:3px}
 .bitc{text-align:center;min-width:0}
 .bitn{color:#5c6672;font-size:9px;line-height:1.1}
 .bitb{height:19px;line-height:19px;border-radius:3px;font-size:11px;font-weight:700}
 .bitl{font-size:9px;line-height:1.15;margin-top:1px;color:#7d8794;
       overflow-wrap:anywhere;hyphens:auto}
 button.sm{padding:0 4px;margin:0 1px;font-size:10px;line-height:1.4}
 td.ov{white-space:nowrap}
 td.c{text-align:center}
 td.nw,th.nw{white-space:nowrap}
 .cols input{width:38px;padding:1px}
 .cols table td{padding:0 5px 0 0;line-height:1.3}
 .bset{background:#2a6;color:#021}.bclr{background:#232a33;color:#6b7683}
 .tab{display:inline-block;padding:1px 6px;margin:1px;border:1px solid #38424f;
      border-radius:3px;cursor:pointer;font-size:10px;color:#9aa4b0}
 .tab.on{background:#2a6;color:#021;border-color:#3c8;font-weight:600}
 .tab{touch-action:manipulation;-webkit-tap-highlight-color:transparent}
 .box{max-width:100%;box-sizing:border-box}
 /* A hairline that fills while a command is in flight. Without it the only
    feedback a tap gets is the next poll, up to a second later, and the natural
    response to that is to tap again. */
 #busy{position:fixed;left:0;top:0;height:2px;width:0;background:#3fb950;
       transition:width .12s;z-index:9;pointer-events:none}
 body.wait #busy{width:100%}
 /* --- phones -----------------------------------------------------------
    Laid out for a desktop column pack, this page gave ~18 px tap targets, and
    the fixed min-widths on the log boxes pushed it sideways so taps landed on
    the wrong control. Inputs under 16 px are worse than small: iOS Safari
    zooms the page in on focus and does not zoom back out, after which every
    tap is offset and the page reads as broken. */
 @media (max-width:760px){
   body{padding:6px;font-size:13px}
   .cols,.colw,.row{display:block}
   .box{min-width:0!important;width:auto!important;margin-bottom:6px;padding:5px 8px}
   .row .box{flex:none!important}
   button{padding:7px 11px;font-size:13px;min-height:34px;margin:2px 1px}
   button.sm{padding:5px 9px;font-size:12px;min-height:30px;line-height:1.2}
   .tab{padding:6px 10px;font-size:12px;margin:2px}
   /* 16 px stops the zoom; the padding is what makes a stage row 70 px tall,
      so tighten that instead of the type. */
   input,select{font-size:16px;padding:3px 5px;min-height:30px}
   input{width:56px}
   .cols input{width:54px}
   #cusn{width:auto;min-width:110px;text-align:left}
   /* Title and status on separate lines -- together they wrap into two ragged
      rows and the first thing on the page is a wall of numbers. */
   h1 #hdr{display:block;font-size:11px;margin-top:2px}
   .bitb{height:24px;line-height:24px}
   .bitl{font-size:8px}
   pre{font-size:11px;max-height:40vh}
   h1{font-size:14px}
 }
</style>
<h1>Momcozy D8 &mdash; CN2 <span class=lbl>v)HTML" FW_VERSION R"HTML(</span> <span id=hdr class=mut></span></h1>
<div id=busy></div>

<div class=cols id=cols>
  <div class=box><b>MACHINE</b>
    <div id=state class=big>&hellip;</div>
    <table style="margin-top:6px">
      <tr><td class=lbl>temp (byte1)</td><td id=t1></td></tr>
      <tr><td class=lbl>flow (byte2)</td><td id=t2></td></tr></table>
    <div class=lbl id=srcnote style="margin-top:4px"></div></div>

  <div class=box><b>LID</b>
    <table>
      <tr><td class=lbl>reed (bit 1)</td><td id=lidrd></td></tr>
      <tr><td class=lbl>micro (bit 7)</td><td id=lidm></td></tr>
      <tr><td class=lbl>machine sees</td><td id=lidr></td></tr>
      <tr><td class=lbl>panel is told</td><td id=lidf></td></tr></table>
    <div class=lbl style="margin-top:4px" title="Two sensors, both 'set = lid off'. The machine only calls the lid shut when BOTH read closed, so an override moves both together — moving one would show it half an open lid.">both must read closed</div>
    <div style="margin-top:5px"><span class=lbl>lid status override:</span>
      <button id=lm0 onclick="lidm(0)" title="pass the real sensors through">real</button>
      <button id=lm1 onclick="lidm(1)" title="tell the panel the lid is CLOSED — clears bits 1 and 7">force ON</button>
      <button id=lm2 onclick="lidm(2)" title="tell the panel the lid is OPEN — sets bits 1 and 7">force OFF</button></div></div>

  <div class=box><b>WASH PUMP RELAY</b>
    <div id=wsrbig class=big>&hellip;</div>
    <table>
      <tr><td class=lbl>relay</td><td id=wsrst></td></tr>
      <tr><td class=lbl>reason</td><td id=wsrwhy></td></tr>
      <tr><td class=lbl>closed for</td><td id=wsrms></td></tr>
      <tr><td class=lbl>closes</td><td id=wsrn></td></tr>
      <tr><td class=lbl>wiring</td><td id=wsrpin></td></tr></table>
    <div style="margin-top:5px"><span class=lbl>mode:</span>
      <button id=wm2 onclick="wsr('auto')" title="Follow b0 of the panel frame as forwarded. The machine's own cycles and the cycle runner both drive the pump.">auto</button>
      <button id=wm0 onclick="wsr('off')" title="Contacts held open whatever b0 does.">off</button>
      <button id=wm1 onclick="wsr('on')" title="Force the pump on. Not persisted across a reboot -- comes back as auto.">force ON</button></div>
    <div class=lbl style="margin-top:4px" title="The board's own low-side switch for WS PUMP never closes on b0, so the pump is driven by an external relay instead. The pump reports nothing, so the firmware cannot tell whether it is actually turning.">external relay &mdash; the board's own switch does not close on b0</div></div>

  <div class=box><b>CONTROLLER &rarr; PANEL</b>
    <table><tr><td class=lbl>frame</td><td id=fb class=mono></td></tr></table>
    <div id=fbleg style="margin-top:2px"></div>
    <div class=lbl style="margin-top:6px" title="These are the CONTROLLER's own bits, before any filtering. The MACHINE line above shows what the panel is actually told, which is what the appliance displays. They differ whenever the false-E5 filter is masking -- that is the filter working, not a fault.">byte3 status bits <span class=mut>(as sent by the controller)</span></div>
    <div class=bits id=bits></div>
    <div class=lbl style="margin-top:5px" id=stshow></div>
    <div style="margin-top:6px">
      <b style="color:#9cf;font-size:11px;letter-spacing:.06em">FALSE-E5 FILTER</b>
      <span id=e5fst class=lbl></span></div>
    <div class=lbl style="margin-top:1px" title="This controller raises bit 6 two seconds after every power-on while still warm, and again the instant a dry stage ends -- both on a byte-exact link with no missed frames. AUTO does not hide it; it refuses to relay a claim about OUR link that we can positively disprove, and passes it straight through the moment we cannot (thinning, probe, spoof, virtual, stale frames, or any bad checksum).">disproves it, not hides it &mdash; hover</div>
    <div style="margin-top:3px">
      <button class=sm id=e5f1 onclick="e5f('auto')">auto</button>
      <button class=sm id=e5f0 onclick="e5f('off')">off</button>
      <button class=sm id=e5f2 onclick="e5f('force')">force</button></div>
</div>

  <div class=box><b>ERROR CODES &mdash; BW05 manual, p.29</b>
    <div id=errtbl></div>
    <div style="margin-top:6px">
      <b style="color:#9cf;font-size:11px;letter-spacing:.06em">FAKE THE TEMPERATURE
        <span class=lbl style="font-weight:400;letter-spacing:0">(controller byte 1)</span></b>
      <span id=ntcst class=lbl></span></div>
    <div class=lbl style="margin-top:1px" title="Rewrites the temperature the panel is shown. It does NOT raise E3 or E4 — tested at 0x00 and 0xFF, no reaction, because the controller reads the thermistor itself and reports its verdict in a status bit. Its remaining use is staging E6: pin the value while the heater runs, so the panel sees a heater that is not working.">what the panel is shown &mdash; used to stage E6</div>
    <div style="margin-top:3px">
      <button class=sm id=bnop onclick="tempov(CUR.temp_ovr===255?-1:255)">0xFF</button>
      <button class=sm id=bnsh onclick="tempov(CUR.temp_ovr===0?-1:0)">0x00</button>
      <button class=sm onclick="tempov(CUR.temp_ovr>=0?-1:(CUR.temp_real||30))">pin (E6)</button>
      <button class=sm onclick="tempov(-1)">normal</button>
      <input id=nv size=3 value=99 class=mono>
      <button class=sm onclick="tempov($('nv').value)">set</button></div>
    <div style="margin-top:6px">
      <button onclick="clearErr()">clear all overrides</button></div>
    <div class=lbl id=e5note style="margin-top:2px"></div></div>

  <div class=box style="min-width:250px"><b>PANEL FRAME (panel &rarr; controller)</b>
    <table><tr><td class=lbl>byte1 &mdash; loads</td><td id=pb1 class=mono></td></tr>
      <tr><td class=lbl title="Not independent: across 99,285 frames byte 2 has never varied apart from byte 3, and three of its four values fit the same law one unit finer. Only its top bit is unexplained.">byte2 (tracks byte3)</td><td id=pb2 class=mono></td></tr>
      <tr><td class=lbl>byte3 &mdash; fill target</td><td id=pb3></td></tr>
      <tr><td class=lbl>frame</td><td id=fp class=mono></td></tr></table>
    <div id=fpleg style="margin-top:2px"></div>
    <table style="margin-top:4px"><tr><td class=lbl>last non-idle byte1</td>
      <td id=blast class=mono></td></tr>
      <tr><td class=lbl>seen</td><td id=bage class=lbl></td></tr></table>
    <div style="margin-top:8px">
      <b style="color:#9cf;font-size:11px;letter-spacing:.06em">DRIVE LOADS
        <span class=lbl style="font-weight:400;letter-spacing:0">(panel byte 1)</span></b>
      <button class=sm onclick="povr('0','0')" title="stop forcing every load bit — hand byte 1 back to the panel">clear byte-1 overrides</button>
      <span id=b1gate class=lbl></span></div>
    <div id=b1tbl style="margin-top:3px"></div>
    <div style="margin-top:8px">
      <b style="color:#9cf;font-size:11px;letter-spacing:.06em">BYTE2 / BYTE3 OVERRIDE</b>
      <span id=movr class=lbl></span></div>
    <div class=lbl style="margin-top:1px" title="byte3 sets how much water the next fill draws. It does NOT start a cycle — forcing 40/20 was tried and the machine ignored it.">fill volume, not a cycle trigger</div>
    <div style="margin-top:3px" id=tgtbtn></div>
    <div class=lbl id=tgtnote style="margin-top:2px"></div>
    <div class=lbl style="margin-top:3px">raw hex
      byte2 <input id=m2 size=2 value=D6 class=mono title="Determined by byte 3 in every frame ever captured. Sending an unpaired value is a state the machine has never produced — fine for probing, not for imitating it.">
      byte3 <input id=m3 size=2 value=23 class=mono title="the fill target">
      <button class=sm onclick="modeo($('m2').value,$('m3').value)">set</button>
      <span id=pairhint class=mut></span></div>
    <div style="margin-top:6px">
      <button id=bprobe onclick=probe() title="Feed the CONTROLLER a permanently idle panel frame. Buttons can be pressed with no possibility of starting a cycle.">PROBE &mdash; deafen the controller</button>
      <button id=bvirt onclick="virt()" title="Synthesise the CONTROLLER's frames and drop the real ones, so the panel sees a machine you control. Engages PROBE too, so nothing reaches the real controller.">VIRTUAL &mdash; fake the controller</button>
      <button onclick="post('/api/btnclear')">reset</button></div>
    <div class=lbl id=virtnote style="margin-top:2px"></div>
    <div id=virtset style="margin-top:3px;display:none" class=lbl>
      <button class=sm id=bvauto onclick="vauto()">AUTO</button>
      temp <input id=vt size=2 value=30 class=mono>
      flow <input id=vf size=2 value=0 class=mono>
      status <input id=vs size=2 value=0 class=mono>
      <button class=sm onclick="virtset()">apply</button>
      <span title="AUTO models the machine from measured rates (fill 2.24 counts/s, heat 0.103 °C/s) so the panel can run a whole cycle against nothing. Most error codes are only checked mid-cycle.">(model)</span>
    </div>
</div>

<div class=box><b>CYCLE RUNNER
  <span class=lbl style="float:right;font-weight:400;letter-spacing:0" id=cycst></span></b>
  <div class=lbl title="The ESP32 drives every load and fill target; the panel is held idle. The machine protects none of this — the interlocks in the runner are the only ones that exist. A stage with 0 s runs until its fill target lands."><b class=warn>machine protects none of this</b> &middot; 0 s = until filled</div>
  <div id=cyctabs style="margin-top:3px"></div>
  <div class=lbl style="margin-top:3px" id=cyctemp></div>
  <div id=cyctbl style="margin-top:3px"></div>
  <div style="margin-top:5px">
    <button id=bcycgo onclick="cycgo()">START CYCLE</button>
    <button onclick="post('/api/cycle?run=stop')">STOP</button>
    <button class=sm onclick="cycdef()">restore default times</button>
    <span class=lbl title="fill stall · over temperature · lid open · fault bit · dead link">aborts on 5 guards</span></div>
  <div class=lbl id=cycwhy style="margin-top:3px"></div>
  <div style="margin-top:7px">
    <b style="color:#9cf;font-size:11px;letter-spacing:.06em">CUSTOM PROGRAM</b>
    <span class=lbl title="loads:target:seconds per stage, comma separated, loads and target in hex. loads bits: 01 wash pump, 02 drain, 04 water heat, 08 air heat, 10 dry blower, 20 intake. seconds 0 on a fill means until the target lands.">format &mdash; hover</span></div>
  <div style="margin-top:3px">
    <select id=cusl onchange=cusload()></select>
    <input id=cusn size=14 placeholder="name">
    <button class=sm onclick=cussave()>save</button>
    <button class=sm onclick=cusdel()>delete</button>
    <span class=lbl id=cusmsg></span></div>
  <input id=cuss style="width:100%;margin-top:3px" class=mono placeholder="02:00:20,20:07:0,05:00:120">
  <div class=lbl style="margin-top:2px">Rejected if it would dry-fire the heater,
    fill with no target, or run an untargeted flush with the drain shut &mdash;
    the machine checks none of that.</div></div>

<div id=gbox style="display:contents">
<div class=box><b>TEMPERATURE &mdash; 30 min
  <span class=lbl style="float:right;font-weight:400;letter-spacing:0" id=gtnow></span></b>
  <div id=gtemp></div>
  <div class=lbl title="Orange = water heater (b2). Amber = air heater or dry (b3/b4). They are separate lanes and can overlap. Dashed = last WATER heater cut-out, the only one with a reproducible setpoint. Byte 1 is uncalibrated and reads low."><span style="color:#e8734a">&#9632;</span> water heat
    <span style="color:#d29922;margin-left:8px">&#9632;</span> air heat / dry</div></div>

<div class=box id=gflowbox><b>FLOW PULSES &mdash; 60 s
  <span class=lbl style="float:right;font-weight:400;letter-spacing:0" id=gfnow></span></b>
  <div id=gflow></div></div>
</div>

</div>

<div style="margin-bottom:3px">
  <a href="#" id=gtog onclick="return gtoggle()"
     style="color:#9cf;font-size:12px;text-decoration:none;margin-right:14px">&#9662; graphs</a>
  <a href="#" id=diagtog onclick="return diag()"
     style="color:#9cf;font-size:12px;text-decoration:none">&#9656; diagnostics</a>
  <span class=lbl>autodetect &middot; link quality &middot; change detector &middot; raw frame log</span>
</div>
<div id=diagbox style="display:none">
<div class=box style="margin-bottom:10px"><b>PIN AUTODETECT</b>
  <button class=sm onclick="post('/api/detect')">SCAN (passive)</button>
  <button class=sm onclick="post('/api/detect?phase2=1')">SCAN + RESOLVE TX</button>
  <span class=lbl>phase 1 never drives a line &middot; phase 2 latches E5 on the panel, power-cycle after</span>
  <div id=det class=mono style="margin-top:4px"></div></div>

<div class=box style="margin-bottom:10px"><b>LINK QUALITY &mdash; bad checksums mean a corrupted byte on the wire</b>
  <button class=sm onclick="post('/api/qclear')">reset</button>
  <div id=lq class=mono style="margin-top:4px"></div>
  <div style="margin-top:8px">
    <b style="color:#9cf;font-size:11px;letter-spacing:.06em">TX MARGIN &mdash; starve a far end until it raises E5</b>
    <span id=thintxt class=lbl></span></div>
  <div style="margin-top:3px" class=lbl>to panel 1-in-
    <input id=tp size=2 value=1 class=mono> &nbsp; to controller 1-in-
    <input id=tc size=2 value=1 class=mono>
    <button class=sm onclick="post('/api/thin?panel='+$('tp').value+'&ctrl='+$('tc').value)">apply</button>
    <button class=sm onclick="post('/api/thin?panel=1&ctrl=1')">full rate</button></div></div>

<div class=box style="margin-bottom:10px"><b>CHANGE DETECTOR &mdash; anything unlike idle</b>
  <div>
    <button onclick="post('/api/baseline')">snapshot idle now</button>
    <button onclick="post('/api/deltaclear')">clear</button>
    <span class=lbl id=basetxt></span>
  </div>
  <pre id=dl style="max-height:150px;margin-top:6px">&mdash;</pre>
  <div class=lbl>Snapshot idle, then do the thing. Every frame that differs is listed
    with how many times it was seen and how long ago it first appeared.</div></div>

<div class=box style="margin-bottom:10px"><b>LINK</b>
  <button id=bflow onclick=flow()>FLOW sim</button>
  <input id=hz value=100>
  <button onclick="post('/api/clear')">clear log</button>
  <button onclick=frz()>freeze</button></div>

<div class=row style="align-items:stretch">
  <div class=box style="flex:1;min-width:330px">
    <b>RX &mdash; FROM CONTROLLER</b>
    <pre id=hc style="max-height:26vh">&hellip;</pre></div>
  <div class=box style="flex:1;min-width:330px">
    <b>TX &mdash; TO PANEL (after rewrite)</b>
    <pre id=htp style="max-height:26vh">&hellip;</pre></div>
</div>
<div class=row style="align-items:stretch">
  <div class=box style="flex:1;min-width:330px">
    <b>RX &mdash; FROM PANEL</b>
    <pre id=hp style="max-height:26vh">&hellip;</pre></div>
  <div class=box style="flex:1;min-width:330px">
    <b>TX &mdash; TO CONTROLLER (after rewrite)</b>
    <pre id=htc style="max-height:26vh">&hellip;</pre></div>
</div>
<div class=box><b>RAW ROLLING LOG</b>
  <button onclick="post('/api/histclear')">clear history</button>
  <pre id=f style="max-height:24vh">&hellip;</pre></div>
</div><!-- /diagbox -->

<script>
const $=i=>document.getElementById(i); let frozen=false;
// ---- request pipeline ------------------------------------------------------
// The device runs the synchronous Arduino WebServer: one client, one request at
// a time. This page used to fire a 1 Hz setInterval with no re-entrancy guard,
// and each tick made five to seven sequential fetches. On a phone a tick takes
// longer than a second, so ticks overlapped, the browser's six sockets per host
// filled with requests the device had not reached yet, and a button POST queued
// behind all of them -- which is why a tap did nothing until a reload dropped
// the backlog. Everything now goes through one queue: one request in flight,
// commands jump the line, and a timeout stops a dropped connection wedging the
// slot forever.
let Q=[], QBUSY=0;
function req(url,method,prio,kind){
  return new Promise((res,rej)=>{
    const j={url,m:method||'GET',k:kind||null,res,rej};
    if(prio) Q.unshift(j); else Q.push(j);
    if(Q.length>16){ Q.splice(16).forEach(x=>x.rej(new Error('dropped'))) }
    pump();
  });
}
async function pump(){
  if(QBUSY) return;
  const j=Q.shift(); if(!j) return;
  QBUSY=1;
  const ac=new AbortController(), t=setTimeout(()=>ac.abort(),8000);
  try{
    const r=await fetch(j.url,{method:j.m,signal:ac.signal});
    j.res(j.k==='json'?await r.json():j.k==='text'?await r.text():r);
  }catch(e){ j.rej(e) }
  finally{ clearTimeout(t); QBUSY=0; if(Q.length) pump() }
}
const gj=u=>req(u,'GET',0,'json');
const gt=u=>req(u,'GET',0,'text');
// A command is what the user is waiting on, so it goes to the front, shows the
// progress hairline, and pulls the next poll forward instead of adding one.
function post(u){
  document.body.classList.add('wait');
  return req(u,'POST',1).catch(()=>{})
    .then(()=>{document.body.classList.remove('wait'); return kick()});
}
// A tap has to look like it landed before the device has answered.
addEventListener('pointerdown',e=>{const b=e.target.closest&&e.target.closest('button,.tab');
  if(b){b.classList.add('tap'); setTimeout(()=>b.classList.remove('tap'),180)}},
  {passive:true});
// The panel-button machinery that lived here is gone. It invited you to learn a
// hex "code" per button, which cannot exist: pressing all eight buttons produces
// ZERO bytes on the wire, and byte 1 is a load bitmap, not a button map. Use the
// DRIVE LOADS table to drive them directly.
function probe(){const on=$('bprobe').classList.contains('on');
 post('/api/probe?v='+(on?'off':'on'))}
function frz(){frozen=!frozen;kick()}
function flow(){const on=$('bflow').classList.contains('on');post('/api/flow?hz='+(on?0:($('hz').value|0)))}
function wsr(m){post('/api/wsrelay?mode='+m);}
function lidm(m){post('/api/lid?m='+m)}
function ovr(c,v){post('/api/status_ovr?clr='+c+'&set='+v)}

// Force status bit 6 SET, so the panel sees the controller reporting a comms
// failure and puts E5 on the display.
//
// Two clicks, because this one does not undo. E5 is LATCHED at the panel: once
// raised it stays on the display until the machine is power-cycled, so clearing
// the bit again leaves the error showing. "pass all" restores the stream but not
// the panel.
let e5arm=0;
function fireE5(){
  if(!e5arm){ e5arm=1; setTimeout(()=>{e5arm=0},4000); return false }
  e5arm=0; ovr('0','40'); return false;
}
function flowsp(on){post('/api/flowspoof?v='+(on?'on':'off'))}
function tempov(v){post('/api/tempovr?v='+v)}
function clearErr(){ovr('0','0');flowsp(0);tempov(-1)}

// The manual's full table (BW05 p.29). E2 does not exist -- the manual skips it.
// "trig" is what this firmware can do to provoke each one from the CN2 link; a
// null means the evidence for that fault never crosses this link at all, so no
// amount of rewriting will produce it.
//
// Every trigger except E5 is UNVERIFIED. They are reasoned from what each byte
// carries, not observed. The table says so per row rather than implying more
// than has been tested.
const ERRS=[
 ['E0','Voltage Anomaly',          'b4','status byte 3 bit 4 — CONFIRMED'],
 ['E1','Water Inlet Fault',        'flow','starve byte 2 during a fill — needs a running cycle'],
 ['E3','Sensor Open Circuit',      'b2','status byte 3 bit 2 — CONFIRMED'],
 ['E4','Sensor Short Circuit',     'b3','status byte 3 bit 3 — CONFIRMED'],
 ['E5','Communication Failure',    'b6','status byte 3 bit 6 — CONFIRMED'],
 ['E6','Heating Plate Malfunction','tfrz','pin byte 1 while the heater runs — needs a running cycle'],
 ['E7','Fan Failure',              'b5','status byte 3 bit 5 — CONFIRMED, but only while a cycle runs'],
];
// Not E-codes. The manual lists these separately as indicator alerts, and they
// clear on their own once the condition goes away.
const ALERTS=[['Water Shortage','add water, press Start/Pause; auto-resumes'],
              ['Lid Open','byte 3 bits 1 and 7; auto-resumes when shut']];

function errTbl(s){
  const ss=s.st_set|0, fl=s.flow_spoof, tv=s.temp_ovr;
  const act={b2:!!(ss&0x04),b3:!!(ss&0x08),b4:!!(ss&0x10),b5:!!(ss&0x20),
             b6:!!(ss&0x40), flow:fl, tfrz:(tv>=0)};
  let h='<table><tr><td class=lbl>code</td><td class=lbl>manual</td>'
       +'<td class=lbl>trigger</td></tr>';
  for(const [c,name,k,how] of ERRS){
    const on=k&&act[k];
    const btn = k
      ? `<button class="sm ${on?'on':(armk===k?'on':'')}" title="${how}"`
        +` onclick="trig('${k}')">`
        +`${on?'ACTIVE':(armk===k?'confirm?':'force')}</button>`
      : `<span class=mut title="${how}">not reachable</span>`;
    h+=`<tr><td class=mono style="color:${k?'#dde':'#4a5462'}">${c}</td>`
      +`<td class=lbl>${name}</td><td>${btn}</td></tr>`;
  }
  for(const [n,how] of ALERTS)
    h+=`<tr><td class=mut>&mdash;</td><td class=lbl>${n} `
      +`<span class=mut>(alert, not a code)</span></td>`
      +`<td class=lbl title="${how}">&mdash;</td></tr>`;
  return h+'</table>';
}
const BITOF={b2:0x04,b3:0x08,b4:0x10,b5:0x20,b6:0x40};
// Every fault bit LATCHES at the panel -- one frame is enough and it holds until
// a power cycle -- so each of these is arm-then-fire. The panel shows the LOWEST
// set bit, confirmed by sending 0x0C and getting E3 rather than E4.
let armk=null;
function trig(k){
  if(BITOF[k]!==undefined){
    if(armk!==k){ armk=k; setTimeout(()=>{if(armk===k)armk=null},4000); return false }
    armk=null; ovr('0',BITOF[k].toString(16)); return false;
  }
  if(k==='flow') return flowsp(!CUR.flow_spoof);
  if(k==='tfrz') return tempov((CUR.temp_ovr>=0)?-1:(CUR.temp_real||30));
}

window.addEventListener('DOMContentLoaded',()=>{
  try{ if(localStorage.d8diag==='1') diag(); }catch(e){}
  try{ if(localStorage.d8graphs==='0') gtoggle(); }catch(e){}
  cusget();
  layout(true);
  // and again once the browser has settled, in case the first ran too early
  requestAnimationFrame(()=>layout(true));
  setTimeout(()=>layout(true), 300);
});
// Both graphs are inline SVG built from the hex the device sends. No library,
// no canvas -- the page has to survive being served off an ESP32.
// Per-byte colouring for the two frame layouts, so a glance at the hex tells you
// which byte is which. Same palette is used for the legend under each frame.
const FB_COL = ['#6b7683','#e8734a','#4a9ee8','#3fb950','#4a5462','#a08cee','#4a5462','#7d8794'];
const FB_NAME= ['header 0xA2','TEMPERATURE','FLOW pulse count','STATUS bits',
                'constant 0x04','byte5 — mains volts?','constant 0x02','XOR checksum'];
const FP_COL = ['#6b7683','#e8c547','#4a5462','#4ad0c0','#7d8794'];
const FP_NAME= ['header 0xAA','LOAD bitmap','byte2 unknown','FILL TARGET','XOR checksum'];

function frameHTML(hex, ctrl){
  if(!hex) return '<span class=mut>-</span>';
  const c = ctrl?FB_COL:FP_COL, n = ctrl?FB_NAME:FP_NAME;
  return hex.split(' ').map((b,i)=>
    `<span style="color:${c[i]||'#dde'};font-weight:600" title="byte${i} — ${n[i]||'?'}">${b}</span>`
  ).join(' ');
}
function frameLegend(ctrl){
  const c = ctrl?FB_COL:FP_COL, n = ctrl?FB_NAME:FP_NAME;
  return n.map((t,i)=> (t.startsWith('constant')||t.startsWith('header')||t.startsWith('XOR'))
      ? '' : `<span style="color:${c[i]}">&#9632;</span> <span class=lbl>${t.toLowerCase()}</span>`)
    .filter(Boolean).join(' &nbsp; ');
}

function hexBytes(h){const a=[];for(let i=0;i+1<h.length;i+=2)a.push(parseInt(h.substr(i,2),16));return a}

function svgTemp(raw, air, cutout, W){
  const H=96,L=26,R=6,T=6,Bm=14;
  if(!raw.length) return '<div class=lbl>no samples yet</div>';
  let z=0; while(z<raw.length && (raw[z]&0x7F)===0) z++;   // drop pre-first-frame zeros
  raw=raw.slice(z);
  if(!raw.length) return '<div class=lbl>no samples yet</div>';
  // Two independent lanes now: bit 7 of the temperature byte is the WATER
  // heater, the air array is the air heater or dry. They overlap only briefly.
  const temp=raw.map(v=>v&0x7F), heat=raw.map(v=>(v&0x80)!==0);
  const airOn=(air&&air.length===raw.length)?air.map(v=>v!==0):raw.map(()=>false);
  const lo=0, hi=Math.max(100, Math.max(...temp)+5);
  const x=i=>L+(W-L-R)*(raw.length<2?0:i/(raw.length-1));
  const y=v=>T+(H-T-Bm)*(1-(v-lo)/(hi-lo));
  let o=`<svg width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" style="display:block">`;
  // heater bands first, so the trace draws over them
  const band=(on,col,op)=>{
    let out='',i=0;
    while(i<on.length){
      if(on[i]){let j=i;while(j<on.length&&on[j])j++;
        out+=`<rect x="${x(i).toFixed(1)}" y="${T}" width="${Math.max(1,x(j-1)-x(i)).toFixed(1)}" height="${H-T-Bm}" fill="${col}" opacity="${op}"/>`;
        i=j;} else i++;
    }
    return out;
  };
  o+=band(heat,'#e8734a',0.22);      // water heater
  o+=band(airOn,'#d29922',0.20);     // air heater / dry
  for(const g of [0,25,50,75,100]){
    if(g>hi) continue;
    o+=`<line x1="${L}" y1="${y(g).toFixed(1)}" x2="${W-R}" y2="${y(g).toFixed(1)}" stroke="#334" stroke-width="0.5"/>`;
    o+=`<text x="2" y="${(y(g)+3).toFixed(1)}" fill="#8a94a6" font-size="9">${g}</text>`;
  }
  if(cutout>0){
    o+=`<line x1="${L}" y1="${y(cutout).toFixed(1)}" x2="${W-R}" y2="${y(cutout).toFixed(1)}" stroke="#f5c518" stroke-width="1" stroke-dasharray="5 3"/>`;
    o+=`<text x="${W-R-2}" y="${(y(cutout)-3).toFixed(1)}" fill="#f5c518" font-size="9" text-anchor="end">cut-out ${cutout}</text>`;
  }
  o+='<polyline fill="none" stroke="#4ec9b0" stroke-width="1.6" points="'+
     temp.map((v,k)=>x(k).toFixed(1)+','+y(v).toFixed(1)).join(' ')+'"/>';
  const span = raw.length>=120 ? `-${Math.round(raw.length/60)} min` : `-${raw.length} s`;
  o+=`<text x="${L}" y="${H-3}" fill="#8a94a6" font-size="9">${span}</text>`;
  o+=`<text x="${W-R}" y="${H-3}" fill="#8a94a6" font-size="9" text-anchor="end">now</text>`;
  return o+'</svg>';
}

function svgFlow(f, W){
  const H=64,L=26,R=6,T=6,Bm=12;
  if(f.length<2) return '<div class=lbl>no samples yet</div>';
  const hi=Math.max(10, Math.max(...f)+2);
  const x=i=>L+(W-L-R)*(i/(f.length-1));
  const y=v=>T+(H-T-Bm)*(1-v/hi);
  let o=`<svg width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" style="display:block">`;
  for(const g of [0,Math.round(hi/2),hi]){
    o+=`<line x1="${L}" y1="${y(g).toFixed(1)}" x2="${W-R}" y2="${y(g).toFixed(1)}" stroke="#334" stroke-width="0.5"/>`;
    o+=`<text x="2" y="${(y(g)+3).toFixed(1)}" fill="#8a94a6" font-size="9">${g}</text>`;
  }
  o+='<polyline fill="none" stroke="#4a9ee8" stroke-width="1.6" points="'+
     f.map((v,k)=>x(k).toFixed(1)+','+y(v).toFixed(1)).join(' ')+'"/>';
  o+=`<text x="${L}" y="${H-2}" fill="#8a94a6" font-size="9">-60 s</text>`;
  o+=`<text x="${W-R}" y="${H-2}" fill="#8a94a6" font-size="9" text-anchor="end">now</text>`;
  return o+'</svg>';
}

window.addEventListener('resize',()=>{gLast=0});
let gLast=0;
async function graphs(){
  if(Date.now()-gLast < 5000) return;      // 30 min of hex is ~3.6 kB, no need to poll it fast
  gLast=Date.now();
  try{
    const g=await gj('/api/graph');
    const raw=hexBytes(g.temp), f=hexBytes(g.flow), air=hexBytes(g.air||'');
    const gw=Math.max(320,($('gtemp').clientWidth||900)-2);
    $('gtemp').innerHTML=svgTemp(raw,air,g.cutout,gw);
    const t=raw.length?(raw[raw.length-1]&0x7F):0;
    const h=raw.length?((raw[raw.length-1]&0x80)!==0):false;
    const a=air.length?(air[air.length-1]!==0):false;
    // Name which heater rather than just "heating" -- the two behave nothing
    // alike and the graph now colours them separately.
    $('gtnow').innerHTML=`now <b>${t}</b>`
      +(h?' <span style="color:#e8734a">&#9679; water heat</span>':'')
      +(a?' <span style="color:#d29922">&#9679; air heat</span>':'')
      +(g.cutout?` &middot; last cut-out ${g.cutout}`:'');
    $('gflow').innerHTML=svgFlow(f,Math.max(320,($('gflow').clientWidth||900)-2));
    $('gfnow').innerHTML = g.filling
      ? `count <b>${f.length?f[f.length-1]:0}</b> <span class=ok>&#9679; filling</span>`
      : '<span class=mut>idle &mdash; no pulses in the last 10 s</span>';
  }catch(e){}
}

// The graphs are 268 px of a page that has to fit a short viewport. Collapsing
// them is what gets everything onto one screen at 175% browser zoom.
// Shortest-column-first packing, recomputed on load, resize and any toggle that
// changes a box's height. Not on the 1 Hz poll -- moving a node would drop focus
// out of whatever field is being typed in.
let COLBOXES=null, COLN=0;
// force=true re-packs. Everything else keeps the existing column assignment: a
// box changing height must not make every other box jump to a different column,
// which is what happened when switching program rebuilt the stage table.
function layout(force){
  const c=$('cols'); if(!c) return;
  if(!COLBOXES) COLBOXES=[...c.children].filter(e=>e.classList.contains('box')||e.id==='gbox');
  // Never more columns than fit: a column narrower than it was designed for
  // crushes its tables into unreadable slivers, which is worse than scrolling.
  const w=c.clientWidth||document.body.clientWidth||360;
  const n=Math.max(1,Math.min(6,Math.floor((w+6)/336)));
  if(n===COLN && !force) return;
  COLN=n;
  const cols=[];
  c.innerHTML='';
  for(let i=0;i<n;i++){ const d=document.createElement('div'); d.className='colw'; c.appendChild(d); cols.push(d); }
  // measure by placing everything in column 0 first
  COLBOXES.forEach(b=>cols[0].appendChild(b));
  const hs=COLBOXES.map(b=>b.getBoundingClientRect().height);
  // Tallest first, each into the shortest column. Placing them in reading order
  // packs badly -- the two big boxes end up together and the page is as tall as
  // their sum. Tallest-first is the standard fix and gets close to optimal.
  // MACHINE stays top-left whatever the packing says -- it is the box you look at
  // first, and burying it in column five to save 20 px is a bad trade.
  const rest=COLBOXES.map((b,i)=>i).slice(1).sort((a,b)=>hs[b]-hs[a]);
  const order=[0,...rest];
  const tot=new Array(n).fill(0);
  cols[0].appendChild(COLBOXES[0]); tot[0]=hs[0]+6;
  order.slice(1).forEach(i=>{
    let k=0; for(let j=1;j<n;j++) if(tot[j]<tot[k]) k=j;
    cols[k].appendChild(COLBOXES[i]); tot[k]+=hs[i]+6;
  });
}
// A window resize event is not enough. The first layout can run before the
// container has its final width -- the page then keeps a column count computed
// from a width it never had, and nothing recomputes it because the guard only
// re-packs when the count CHANGES. Observed live: 6 columns of 72..363 px in a
// 1113 px container, when the correct answer was 3.
//
// A ResizeObserver on the container catches the real width whenever it lands,
// however late, and a re-pack after the first frame covers the load case.
addEventListener('resize',()=>{clearTimeout(window._lt);window._lt=setTimeout(()=>layout(false),120)});
if(window.ResizeObserver){
  new ResizeObserver(()=>{clearTimeout(window._ro);
                          window._ro=setTimeout(()=>layout(false),120)})
    .observe(document.getElementById('cols'));
}

function gtoggle(){
  const b=$('gbox'), on=b.style.display==='none';
  b.style.display=on?'contents':'none';
  $('gtog').innerHTML=(on?'&#9662;':'&#9656;')+' graphs';
  try{localStorage.d8graphs=on?'1':'0'}catch(e){}
  layout(true);          // boxes appear or vanish, so a re-pack is expected
  return false;
}
function diag(){
  const b=$('diagbox'), on=b.style.display==='none';
  b.style.display = on ? '' : 'none';
  $('diagtog').innerHTML = (on?'\u25be':'\u25b8')+' diagnostics';
  try{localStorage.d8diag = on?'1':'0'}catch(e){}
  return false;
}
// Become the controller. The panel is fed frames we synthesise; the real
// controller's are dropped and it is put in PROBE, so it keeps hearing an idle
// panel and cannot receive a command. Nothing the panel asks for reaches it.
function virt(){post('/api/virtual?v='+(CUR.virtual?'off':'on'))}
function vauto(){post('/api/virtual?auto='+(CUR.virt_auto?'off':'on'))}
function virtset(){post('/api/virtual?temp='+$('vt').value+'&flow='+$('vf').value
                        +'&st='+$('vs').value)}
// Two clicks. This drives 110 V heaters and a water pump with nothing above it.
let cycarm=0;
function cycgo(){
  if(!cycarm){cycarm=1;setTimeout(()=>{cycarm=0},4000);return false}
  cycarm=0; post('/api/cycle?run=start'); return false;
}
function cycsecs(i,v){post('/api/cycle?i='+i+'&secs='+v)}
// Switching program replaces the stage list, so the table has to be rebuilt.
function cycmode(m){cycBuilt=0;cycMode=-1;post('/api/cycle?mode='+m)}
// The editor reads the slots back from the device rather than keeping its own
// copy, so what you edit is always what is actually stored.
let CUS=null;
async function cusget(){
  CUS=await gj('/api/custom');
  const sel=$('cusl');
  sel.innerHTML=CUS.slots.map((s,i)=>
    `<option value="${i}">${i+1}. ${s.name||'(free)'}</option>`).join('');
  cusload();
}
function cusload(){
  if(!CUS) return;
  const k=+$('cusl').value||0;
  $('cusn').value=CUS.slots[k].name; $('cuss').value=CUS.slots[k].spec;
  $('cusmsg').textContent='';
}
function cusnew(modeIdx){
  const slot=modeIdx-(CUS?CUS.first:6);
  $('cusl').value=slot; $('cusn').value=''; $('cuss').value='';
  $('cusmsg').innerHTML='<span class=lbl>new program in slot '+(slot+1)+'</span>';
  $('cusn').focus();
}
async function cusdel(){
  const k=+$('cusl').value||0;
  if(!CUS||!CUS.slots[k].name){ $('cusmsg').textContent='slot is already empty'; return }
  if(delarm!==k){ delarm=k; $('cusmsg').innerHTML='<span class=warn>click delete again</span>';
                  setTimeout(()=>{delarm=-1},4000); return }
  delarm=-1;
  await req('/api/cycle?del='+k,'POST',1).catch(()=>{});
  cycMode=-1; await cusget(); kick();
}
let delarm=-1;
async function cussave(){
  const k=+$('cusl').value||0;
  const u='/api/custom?slot='+k+'&name='+encodeURIComponent($('cusn').value)
         +'&stages='+encodeURIComponent($('cuss').value);
  const j=await req(u,'POST',1,'json').catch(()=>({ok:false,err:'no answer'}));
  $('cusmsg').innerHTML = j.ok
    ? '<span class=ok>saved</span>'
    : '<span class=bad>'+j.err+'</span>';
  if(j.ok){ cycMode=-1; await cusget(); }
  kick();
}
function cyctemps(){cycMode=-1;post('/api/cycle?water='+$('wct').value+'&dry='+$('dct').value)}
let cycMode=-1;
function cycTabs(c){
  // Empty custom slots are not programs, so they are not tabs. "+ new" opens the
  // editor on the first free one.
  let h=c.modes.map((m,i)=> m.empty ? '' :
    `<span class="tab${i===c.mode?' on':''}" onclick="cycmode(${i})"`
    +` title="max water temperature ${m.c} °C — the runner aborts above it${m.custom?' · user program':''}">`
    +`${m.custom?'&#9998; ':''}${m.n}</span>`).join('');
  const free=c.modes.findIndex(m=>m.empty);
  if(free>=0) h+=`<span class=tab onclick="cusnew(${free})"`
    +` title="create a user program in the first free slot">+ new</span>`;
  return h;
}
// Stage times are saved to NVS on change, so a reboot keeps them. This puts the
// compiled-in defaults back.
function cycdef(){cycBuilt=0;post('/api/cycle?run=defaults')}
const CYCST=['idle','RUNNING','complete','ABORTED','PAUSED'];
// The table is BUILT ONCE. It used to be regenerated on every 1 Hz poll, which
// destroyed the <input> elements mid-keystroke -- the seconds fields could not be
// edited at all. Only the highlight and the elapsed counter update per tick.
let cycBuilt=0;
function cycTbl(c){
  let h='<table><tr><td class=lbl>#</td><td class=lbl>stage</td>'
       +'<td class=lbl>loads</td><td class=lbl>fill</td>'
       +'<td class=lbl>seconds</td></tr>';
  c.stages.forEach((s,i)=>{
    const lo=[0,1,2,3,4,5].filter(k=>s.loads>>k&1)
             .map(k=>B1NAME[k].split(' ')[0]).join('+')||'idle';
    const fill = s.tgt===255 ? 'flush'
               : (s.tgt ? Math.round(s.tgt/0.35)+' ct' : '&mdash;');
    h+=`<tr id=cr${i}><td class="mono nw"><span id=cm${i}></span>${i+1}</td>`
      +`<td class="lbl nw" id=cn${i}>${s.name}</td>`
      +`<td class="lbl nw">${lo}</td><td class="lbl nw">${fill}</td>`
      +`<td class=nw><input size=3 class=mono id=ci${i} value="${s.secs}"`
      +` onchange="cycsecs(${i},this.value)" title="${s.secs===0?'runs until the fill target lands':'seconds'}">`
      +` <span id=ce${i}></span></td></tr>`;
  });
  return h+'</table>';
}
function cycUpd(c){
  c.stages.forEach((s,i)=>{
    const tr=$('cr'+i); if(!tr) return;
    const on=(c.state==1&&c.stage==i);
    tr.style.background = on?'#1b2a1b':'';
    $('cm'+i).innerHTML = on?'&#9654;':'';
    $('cn'+i).className = on?'ok':'lbl';
    $('ce'+i).innerHTML = on?'<span class=ok>'+c.elapsed+'s</span>':'';
    // Never clobber a field the user is typing in.
    const inp=$('ci'+i);
    if(inp && document.activeElement!==inp && +inp.value!==s.secs) inp.value=s.secs;
  });
}
function e5f(m){post('/api/e5filter?mode='+m)}
function povr(c,v){post('/api/panel_ovr?clr='+c+'&set='+v)}
function modeo(a,b){post('/api/mode_ovr?b2='+a+'&b3='+b)}

// The fill targets the panel actually sends, named for the cycle stage each one
// was captured in. Byte 2 is not independent -- across 80,683 frames it has never
// varied apart from byte 3 -- so each button sends the real observed PAIR.
// Forcing a real byte 3 alongside byte2 = 0x00 would be a combination this
// machine has never produced.
//
// flush is the odd one: 0xFF is a sentinel, not a volume. The machine only ever
// sends it with the drain open, and the controller has no fill timeout, so a
// 0xFF fill with the drain shut runs the intake motor until something overflows.
const TGT=[['steam',0x2A,0x07,20],['rinse',0xAB,0x1C,80],['wash',0x40,0x20,90],
           ['clean',0xD6,0x23,100],['flush',0xFF,0xFF,0]];
function tgtBtns(s){
  const on=(s.mo2<0&&s.mo3<0);
  let h=`<button class="sm ${on?'on':''}" onclick="modeo('','')">pass</button>`;
  for(const [nm,b2,b3,n] of TGT){
    const sel=(s.mo2===b2&&s.mo3===b3);
    const t=n?`${nm} — ${h2(b2)}/${h2(b3)} → ${n} counts`
             :`${nm} — ${h2(b2)}/${h2(b3)} → NO TARGET, fills until stopped`;
    h+=`<button class="sm ${sel?'on':''}" title="${t}"`
      +` onclick="modeo('${b2.toString(16)}','${b3.toString(16)}')">${nm}</button>`;
  }
  return h;
}
const hx=v=>'0x'+(v||0).toString(16).padStart(2,'0').toUpperCase();
// b0 fires only in the ~1 s between the final flush's intake stopping and the
// flow counter resetting: 3 of 3 untargeted FF/FF flushes, 0 of 8 targeted
// fills. It is NOT "cycle running" -- that reading was falsified by a 91-minute
// wash cycle in which it was set for 13 s total.
// Short enough to fit an eighth of the card; the full meaning is the tooltip.
const BITSHORT={0:'flow? &#9888;',1:'lid reed',2:'E3',3:'E4',4:'E0',5:'E7',6:'E5',7:'lid micro'};
const BITFULL={0:'b0 — UNRESOLVED, flow-related. It was documented as NO FLOW; that was retracted. A flow counter frozen for 5.7 s under commanded intake did NOT set it, but a fill released short of its target blipped it for one frame. Two partial readings, neither sufficient. Do not rely on it, and do not use it as a dry-run alarm. See docs/protocol.md.',
               1:'b1 — lid reed/magnet sensor. Set = lid OFF.',
               2:'b2 — E3 Sensor Open Circuit',
               3:'b3 — E4 Sensor Short Circuit',
               4:'b4 — E0 Voltage Anomaly',
               5:'b5 — E7 Fan Failure (only shown while a cycle runs)',
               6:'b6 — E5 Communication Failure',
               7:'b7 — lid micro switch. Set = lid OFF. Trips before the reed.'};
const BITNAME=BITSHORT;
const B1NAME={0:'wash pump',1:'drain',2:'water heat',3:'air heat',
              4:'dry blower',5:'intake +tgt'};
let CUR={};
const h2=v=>'0x'+(v||0).toString(16).padStart(2,'0').toUpperCase();

// Name the phase from the load bitmap. The panel never says which cycle it is
// running, so this is inferred, not reported.
//
// "dry / storage" is deliberately one label: the 72-hour storage mode sends
// AA 18 00 00 B2 continuously, byte for byte the same frame as the dry phase.
// Storage is a panel-side concept that never reaches the controller, so nothing
// on this link can tell the two apart.
function phaseName(pb1){
  if(pb1&0x10) return 'DRY / STORAGE';
  if(pb1&0x20) return 'FILLING';
  if((pb1&0x04) && !(pb1&0x01)) return 'STEAM';   // heater with no circulation
  if(pb1&0x04) return 'WASH + HEAT';
  if(pb1&0x01) return 'WASH';
  if(pb1&0x02) return 'DRAIN';
  return 'ACTIVE';
}
function b1tbl(s){
 let h='<table><tr><td class=lbl>bit</td><td class=lbl>mask</td>'+
       '<td class="lbl c">panel</td><td class="lbl c">&rarr;ctrl</td>'+
       '<td class=lbl>override</td><td class=lbl>meaning</td></tr>';
 // b6 and b7 are omitted: never set in 99,285 checksum-valid frames across six
 // captures, so there is nothing to pass through or override. They stay
 // reachable through /api/panel_ovr, which takes any mask -- that is still the
 // only way left to hunt the unattributed 303.7 ohm sprayer valve.
 for(let k=5;k>=0;k--){
  const m=1<<k, pv=(s.pb1&m)?1:0;
  // The byte the controller was actually sent. Predicting it from the override
  // masks went wrong as soon as the flush cap could subtract a bit the masks
  // know nothing about -- the table claimed intake was being forwarded while it
  // was being held down.
  const fv=(s.pb1_fwd&m)?1:0;
  const capped=false;
  const st=(s.p1_set&m)?'on':((s.p1_clr&m)?'off':'pass');
  h+=`<tr><td class="mono nw">b${k}</td><td class="mono nw">${h2(m)}</td>`+
     `<td class="mono c">${pv}</td>`+
     `<td class="mono c"><b class=${fv?'ok':'mut'}>${fv}</b>`+
     (capped?'<span class=bad title="the flush cap is stripping this bit">*</span>':'')+`</td>`+
     `<td class=ov><button class="sm ${st=='pass'?'on':''}" onclick="b1(${k},0)">pass</button>`+
     `<button class="sm ${st=='on'?'on':''}" onclick="b1(${k},1)">ON</button>`+
     `<button class="sm ${st=='off'?'on':''}" onclick="b1(${k},2)">OFF</button></td>`+
     `<td class=lbl>${B1NAME[k]||'?'}</td></tr>`;
 }
 return h+'</table>';
}
// b5 is the intake motor, and on its own it does NOTHING. Byte 3 is the fill
// target, and zero means "fill zero counts" -- so the controller obeys exactly
// what it was asked and never starts the pump. Across 20,482 archived frames the
// panel has NEVER commanded b5 with a zero target; forcing the bit alone is why
// the pump looked like it only worked during a cycle.
//
// So forcing b5 ON also sends a target, and releasing it takes the target away
// again -- otherwise the next real fill inherits a stale one.
const B5_TGT=[0x2A,0x07];        // 20 counts, ~10 s of water, the smallest the machine uses
function b1(k,mode){
 const m=1<<k; let c=CUR.p1_clr|0, v=CUR.p1_set|0;
 if(mode==0){c&=~m; v&=~m} else if(mode==1){v|=m; c&=~m} else {c|=m; v&=~m}
 post('/api/panel_ovr?clr='+c.toString(16)+'&set='+v.toString(16));
 if(k===5){
   if(mode==1) modeo(B5_TGT[0].toString(16),B5_TGT[1].toString(16));
   else if(CUR.mo2===B5_TGT[0]&&CUR.mo3===B5_TGT[1]) modeo('','');
 }
}
async function tick(){
 try{
  const s=await gj('/api/status');
  $('hdr').textContent=`RELAY | up ${s.uptime_s}s | rssi ${s.rssi} | heap ${s.heap}`
    +(s.pressing?' | INJECTING':'');
  $('bflow').className=s.flow_hz>0?'on':'';
  for(let m=0;m<3;m++)$('lm'+m).className=(s.lid_mode==m)?'on':'';
  const st=s.st_fwd||0, re=s.st_real||0;
  // "Running" is read off the panel's load bitmap, not off a status bit. There
  // is no RUN flag on this link: byte 3 bit 0 was tried and falsified.
  // st is the byte the panel actually receives; re is what the controller
  // sent. Headline the panel's view so this card matches the appliance.
  $('state').innerHTML=(st&0x40)?'<span class=bad>E5</span>':(s.pb1)
    ?'<span class=ok>'+phaseName(s.pb1)+'</span>':'<span class=mut>IDLE</span>';
  $('state').title=(re!==st)
    ?'controller sent 0x'+re.toString(16)+', panel is being told 0x'+st.toString(16)
    :'';
  const mmss=t=>{t=Math.max(0,t|0);return ((t/60)|0)+':'+String(t%60).padStart(2,'0')};

  $('bits').innerHTML=[7,6,5,4,3,2,1,0].map(k=>
    `<div class=bitc><div class=bitn>b${k}</div>`
    +`<div class="bitb ${re>>k&1?'bset':'bclr'}" title="${BITFULL[k]||'unused'}">${re>>k&1}</div>`
    +`<div class=bitl>${BITSHORT[k]||''}</div></div>`).join('');
  const e5f=(s.st_set&0x40)!=0;
  $('errtbl').innerHTML=errTbl(s);
  $('bnop').className='sm'+(s.temp_ovr===255?' on':'');
  $('bnsh').className='sm'+(s.temp_ovr===0?' on':'');
  // In virtual mode the real controller is isolated, so an NTC fault is a pure
  // panel test with nothing at stake. Outside it, the real machine acts on what
  // it believes the temperature is.
  $('ntcst').innerHTML = s.temp_ovr<0
    ? (s.virtual?'<span class=ok>safe &mdash; controller isolated</span>'
               :'<span class=mut>machine is live</span>')
    : '<span class=bad>&#9679; byte1 forced to '+s.temp_ovr
      +' (real '+s.temp_real+')'+(s.virtual?'':' — MACHINE IS LIVE')+'</span>';
  const spoofing = e5f || s.flow_spoof || s.temp_ovr>=0;
  $('e5note').innerHTML = e5arm
    ? '<span class=warn>&#9679; E5 armed &mdash; click force again to confirm. It '
      +'LATCHES at the panel and needs a power cycle to clear.</span>'
    : spoofing
    ? '<span class=bad>&#9679; feeding the panel false data'
      + (e5f?' &middot; E5 latches, power-cycle to clear':'')
      + (s.temp_ovr>=0?' &middot; byte1 forced to '+s.temp_ovr+' (real '+s.temp_real+')':'')
      + (s.flow_spoof?' &middot; byte2 held at 0':'') + '</span>'
    : '<span class=mut title="E0/E3/E4/E5/E7 were each injected and the panel '
      +'displayed the matching code. E7 only shows while a cycle runs. E1 and E6 '
      +'have no bit. Every fault bit LATCHES: one frame holds until a power cycle, '
      +'and the panel shows the lowest set bit.">E0/E3/E4/E5/E7 confirmed &middot; '
      +'all latch &middot; hover</span>';
  for(var ei=0;ei<3;ei++){var eb=$('e5f'+ei); if(eb) eb.className='sm'+(s.e5f_mode==ei?' on':'');}
  $('e5fst').innerHTML = s.e5f_mode==0
    ? '<span class=mut>off &mdash; bit 6 relayed as sent</span>'
    : s.e5f_on
    ? '<span class=ok>&#9679; masking</span> <span class=mut>'+s.e5f_why
      +(s.e5f_n?' &middot; '+s.e5f_n+' frames':'')+'</span>'
    : '<span class=warn>&#9679; passing through</span> <span class=mut>'+s.e5f_why+'</span>';
  $('stshow').textContent='real '+hx(re)+' -> sent '+hx(st)
    +(s.st_clr||s.st_set?'   (clr '+hx(s.st_clr)+' set '+hx(s.st_set)+')':'');
  const P=v=>v?'<span class=warn>OFF</span>':'<span class=ok>ON</span>';
  // Two lid sensors, same polarity: SET means lid off on both. The machine only
  // calls the lid shut when b1 and b7 are BOTH clear, so lid_real/lid_fwd are
  // the combined verdict and the two rows below break it down.
  $('lidr').innerHTML=P(s.lid_real); $('lidf').innerHTML=P(s.lid_fwd);
  var wm=['OFF','FORCED ON','AUTO'][s.wsr_mode]||'?';
  $('wsrbig').innerHTML=s.wsr_on?'<span class=ok>PUMP ON</span>':'<span class=mut>pump off</span>';
  $('wsrst').innerHTML=s.wsr_on?'<b class=ok>closed</b>':'<span class=mut>open</span>';
  $('wsrwhy').innerHTML=(s.wsr_lock?'<b class=bad>CAPPED</b> - ':'')+(s.wsr_why||'');
  $('wsrms').textContent=s.wsr_on?(s.wsr_ms/1000).toFixed(1)+' s':'\u2014';
  $('wsrn').textContent=s.wsr_n;
  $('wsrpin').textContent='GPIO'+s.wsr_pin+', active-'+(s.wsr_low?'LOW':'HIGH');
  for(var wi=0;wi<3;wi++){var wb=$('wm'+wi); if(wb) wb.className=(s.wsr_mode==wi)?'on':'';}
  const sens=(v)=>v ? '<span class=warn>OFF</span>' : '<span class=ok>ON</span>';
  $('lidrd').innerHTML=sens(s.st_real&0x02);
  $('lidm').innerHTML=sens(s.st_real&0x80);
  // Show what the PANEL is being told. When that differs from what the
  // controller actually said, show both -- otherwise the box silently reports a
  // number nothing in the machine is acting on.
  const rt=s.fb?parseInt(s.fb.split(' ')[1],16):0;
  const rf=s.fb?parseInt(s.fb.split(' ')[2],16):0;
  const ft=s.virtual?s.virt_temp:(s.temp_ovr>=0?s.temp_ovr:rt);
  const ff=s.virtual?s.virt_flow:(s.flow_spoof?0:rf);
  const dt=(ft!==rt), df=(ff!==rf);
  $('t1').innerHTML=ft+' \u00B0C'+(dt?' <span class=mut>(real '+rt+')</span>':'');
  $('t2').innerHTML=ff+' pulses'+(df?' <span class=mut>(real '+rf+')</span>':'');
  $('srcnote').innerHTML = s.virtual
    ? '<span class=warn>&#9679; VIRTUAL CONTROLLER'
      +(s.virt_auto?' + AUTO model':' (fixed values)')
      +' &mdash; these are simulated. Graphs plot them too.</span>'
    : (s.temp_ovr>=0||s.flow_spoof
       ? '<span class=bad>&#9679; overriding what the panel sees</span>'
       : s.pc
       ? '<span class=ok>&#9654; panel cycle'+(s.pc_prog?' <b>'+s.pc_prog+'</b>':'')
         +'</span> &middot; '+mmss(s.pc_elapsed)+' elapsed'
         +(s.pc_prog?' &middot; ~'+mmss(s.pc_remain)+' left <span class=mut title="Estimated from the reference program tables. The wire carries no countdown; the panel&#39;s own timer is the authority.">(est)</span>':'')
       : '<span class=mut>live from the controller</span>');
  $('pb1').innerHTML=hx(s.pb1)+(s.pb1?' <span class=ok>&#9679;</span>':'');
  $('bprobe').className=s.probe?'on':'';
  $('bvirt').className=s.virtual?'on':'';
  $('virtset').style.display=s.virtual?'':'none';
  $('bvauto').className='sm'+(s.virt_auto?' on':'');
  $('virtnote').innerHTML=s.virtual
    ? '<span class=warn>&#9679; synthesising the controller frame &mdash; '
      +s.virt_n+' sent'+(s.virt_auto?', model running: '+s.virt_temp+' \u00B0C / '
      +s.virt_flow+' counts':'')
      +'. The real controller is isolated and receives nothing.</span>'
    : '';
  CUR=s; $('b1tbl').innerHTML=b1tbl(s);
  // A full cycle filled the machine 397 times with byte3 bit 0 clear, so nothing
  // here is gated. Every bit drives its load directly.
  // "Nothing is gated" was true for the pump, drain and heaters -- each verified
  // by forcing it. It is NOT true for b5.
  $('b1gate').innerHTML=(s.pb1&0x20)||(s.p1_set&0x20)
    ? '<span class=warn>&#9679; b5 needs a fill target in byte 3 &mdash; sent '
      + 'automatically with the bit</span>'
    : '<span class=mut>&#9679; b0-b4 drive their loads directly. b5 also needs a '
      + 'fill target.</span>';
  $('blast').innerHTML=s.btn_last?hx(s.btn_last):'<span class=mut>none</span>';
  $('bage').textContent=s.btn_last?((s.btn_age/1000).toFixed(1)+' s ago · '+s.btn_n+' distinct'):'';
  if(s.probe)$('hdr').textContent+=' | PROBE — presses blocked';
  $('pb2').textContent=hx(s.pb2);
  // byte3 = round(target_count * 0.35). 0xFF is a sentinel used only by the
  // final flush, which fills and drains at once and has no volume target.
  $('pb3').innerHTML = s.pb3===0 ? '<span class=mut>0x00 — no fill</span>'
    : s.pb3===0xFF ? hx(s.pb3)+' <span class=warn>sentinel — untargeted flush</span>'
    : hx(s.pb3)+' &rarr; <b class=ok>'+Math.round(s.pb3/0.35)+'</b> counts';
  // mo2/mo3 are -1 when the panel's own byte 2/3 ride through untouched.
  $('movr').innerHTML=(s.mo2<0&&s.mo3<0)
    ? '<span class=mut>pass-through</span>'
    : '<span class=warn>&#9679; forcing '+(s.mo2<0?'--':hx(s.mo2))+' / '+
      (s.mo3<0?'--':hx(s.mo3))+'</span>';
  $('tgtbtn').innerHTML=tgtBtns(s);
  // byte 2 has never varied independently of byte 3, so show the pairing the
  // machine actually uses rather than letting someone invent one.
  {const PAIR={0x07:0x2A,0x1C:0xAB,0x20:0x40,0x23:0xD6,0xFF:0xFF};
   const b3=parseInt($('m3').value,16);
   const want=PAIR[b3];
   $('pairhint').innerHTML = want===undefined ? ''
     : (parseInt($('m2').value,16)===want ? 'machine pairing'
        : '<span class=warn>machine pairs '+h2(want)+' with this byte3</span>');}
  $('tgtnote').innerHTML = (s.mo3===0xFF)
    ? '<span class=bad>&#9679; flush forced: 0xFF is not a volume. With the drain '
      +'shut the intake motor has nothing to stop it &mdash; watch the machine.</span>'
    : 'steam 20 &middot; rinse 80 &middot; wash 90 &middot; clean 100 counts &middot; '
      +'flush = no target';
  $('fb').innerHTML=frameHTML(s.fb,true);
  $('fp').innerHTML=frameHTML(s.fp,false);
  if(!$('fbleg').innerHTML){ $('fbleg').innerHTML=frameLegend(true);
                             $('fpleg').innerHTML=frameLegend(false); }
  try{
    const c=await gj('/api/cycle');
    if(cycMode!==c.mode){
      $('cyctabs').innerHTML=cycTabs(c); cycMode=c.mode; cycBuilt=0;
      // One sensor on this machine: byte 1, the sump NTC. water is a setpoint a
      // heat stage ends on; dry can only be a ceiling on the same reading.
      $('cyctemp').innerHTML='water target <input id=wct size=2 class=mono value="'
        +c.waterc+'" onchange="cyctemps()"> &deg;C &nbsp; dry ceiling <input id=dct '
        +'size=2 class=mono value="'+c.dryc+'" onchange="cyctemps()"> &deg;C'
        +' <span class=mut title="Byte 1 is the sump NTC and it is the only sensor '
        +'on this machine -- there is no air probe, which is why the manual lists no '
        +'max temperature for Drying. 0 disables. Capped at the program maximum ('
        +c.maxc+' \u00B0C).">0 = off &middot; max '+c.maxc+'&deg;</span>';
    }
    if(cycBuilt!==c.stages.length){ $('cyctbl').innerHTML=cycTbl(c); cycBuilt=c.stages.length; }
    cycUpd(c);
    $('cycst').innerHTML=(c.state==1)
      ? '<span class=ok>RUNNING &mdash; stage '+(c.stage+1)+'/'+c.stages.length+'</span>'
        +' <span class=lbl>max '+c.maxc+'&deg;C</span>'
      : (c.state==4?'<span class=warn>PAUSED &mdash; stage '+(c.stage+1)+'</span>'
        :(c.state==3?'<span class=bad>ABORTED</span>'
        :(c.state==2?'<span class=ok>complete</span>':'<span class=mut>idle</span>')));
    $('bcycgo').textContent=cycarm?'click again to confirm':'START CYCLE';
    $('bcycgo').className=(cycarm||c.state==1)?'on':'';
    $('bcycres').style.display=(c.state==4)?'':'none';
    $('bcycres').className=(c.state==4)?'on':'';
    $('cycwhy').innerHTML = c.why
    ? '<span class='+(c.state==4?'warn':'bad')+'>&#9679; '+c.why+'</span>'
    : (s.lid_mode===1
       ? '<span class=warn>&#9679; LID INTERLOCK OVERRIDDEN &mdash; the runner will '
         +'not stop for an open lid</span>' : '');
  }catch(e){}
  const diagOn = $('diagbox').style.display !== 'none';
  if(diagOn){
  const dt=await gj('/api/detect');
  const gp=v=>v<0?'<span class=mut>?</span>':'GPIO'+v;
  let dh=dt.running?`<span class=warn>&#9679; scanning, phase ${dt.phase}&hellip;</span>`
        :dt.done?(dt.applied?'<span class=ok>&#9679; resolved &amp; saved</span>'
                            :'<span class=warn>&#9679; finished</span>')
        :'<span class=mut>not run</span>';
  dh+='<br>edges: '+dt.cand.map((c,i)=>`GPIO${c}=${dt.edge[i]}`).join('  ');
  dh+=`<br>rx ctrl ${gp(dt.rx_ctrl)} &middot; rx panel ${gp(dt.rx_panel)}`
    + ` &middot; tx ctrl ${gp(dt.tx_ctrl)} &middot; tx panel ${gp(dt.tx_panel)}`;
  dh+=`<br>in use: rxB=GPIO${dt.now[0]} txB=GPIO${dt.now[1]} txP=GPIO${dt.now[2]} rxP=GPIO${dt.now[3]}`;
  if(dt.note)dh+='<br><span class=lbl>'+dt.note+'</span>';
  $('det').innerHTML=dh;
  $('thintxt').innerHTML=(s.thin_p==1&&s.thin_c==1)
    ? '<span class=mut>full rate</span>'
    : '<span class=warn>&#9679; thinned: panel 1-in-'+s.thin_p+', controller 1-in-'+s.thin_c+'</span>';
  $('lq').innerHTML=[['controller &rarr; ESP',s.ok_c,s.bad_c],
                     ['panel &rarr; ESP',s.ok_p,s.bad_p],
                     ['ESP &rarr; panel (emitted)',s.tx_bad_p?0:1,s.tx_bad_p],
                     ['ESP &rarr; controller (emitted)',s.tx_bad_c?0:1,s.tx_bad_c]].map(([n,o,b])=>{
    const t=o+b, pct=t?(100*b/t):0;
    return `${n}: <b>${o}</b> good, `+(b?`<span class=bad>${b} BAD (${pct.toFixed(2)}%)</span>`
                                        :`<span class=ok>0 bad</span>`);
  }).join(' &nbsp;|&nbsp; ');
  $('basetxt').textContent=s.base_c?('idle: '+s.base_c+'  |  '+s.base_p):'no baseline yet';
  const dd=await gj('/api/deltas');
  $('dl').textContent = dd.length
    ? dd.map(x=>`${x.dir.padEnd(6)} ${x.hex.padEnd(26)} x${String(x.n).padEnd(5)} first ${x.age}s ago`).join('\n')
    : (s.base_c?'nothing different from idle yet':'snapshot idle first');
  // /api/hist and /api/frames feed boxes inside this panel, so they belong in
  // here too. Polled unconditionally they were two of the five requests a tick
  // spent, every second, on four <pre> blocks nobody was looking at.
  const H=await gj('/api/hist');
  const fmt=a=>a.length?a.map(x=>
     (x.first/10).toFixed(1).padStart(7)+'s  '+x.hex.padEnd(26)+
     ' x'+String(x.n).padEnd(6)+(x.last<20?'\u25CF now':'last '+(x.last/10).toFixed(1)+'s')
   ).join('\n'):'(nothing yet)';
  $('hc').textContent=fmt(H.ctrl);    $('htp').textContent=fmt(H.to_panel);
  $('hp').textContent=fmt(H.panel);   $('htc').textContent=fmt(H.to_ctrl);
  if(!frozen)$('f').textContent=await gt('/api/frames?n=40');
  }  // end diagnostics: nothing in this panel is polled while it is collapsed
  if($('gbox').style.display!=='none') await graphs();
 }catch(e){ $('hdr').textContent='offline'; throw e }
}

// Chained, not intervalled: the next poll is scheduled when the last one has
// finished, so however slow the link gets there is never more than one in
// flight. Failures back off to 8 s, and a hidden tab (phone locked, app
// switched) drops to 5 s instead of building a queue nobody is reading.
let TICKT=null, TICKFAIL=0;
function sched(ms){ clearTimeout(TICKT); TICKT=setTimeout(run,ms) }
function kick(){ return run() }
async function run(){
  clearTimeout(TICKT);
  if(frozen){ sched(1000); return }
  try{ await tick(); TICKFAIL=0 }catch(e){ TICKFAIL++ }
  sched(TICKFAIL ? Math.min(8000,1000*TICKFAIL) : (document.hidden?5000:1000));
}
document.addEventListener('visibilitychange',()=>{ if(!document.hidden) sched(0) });
sched(0);
</script>
)HTML";

static String versionJson() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  String j = "{";
  j += "\"name\":\"" FW_NAME "\",\"version\":\"" FW_VERSION "\"";
  j += ",\"build\":\"" __DATE__ " " __TIME__ "\"";
  j += ",\"md5\":\"" + ESP.getSketchMD5() + "\"";
  j += ",\"partition\":\"" + String(run ? run->label : "?") + "\"";
  j += ",\"sketch_size\":" + String(ESP.getSketchSize());
  j += ",\"free_sketch_space\":" + String(ESP.getFreeSketchSpace());
  j += ",\"uptime_s\":" + String(millis() / 1000);
  j += ",\"heap\":" + String(ESP.getFreeHeap());
  j += ",\"boot_count\":" + String(app::bootCount());
  j += ",\"safe_mode\":" + String(app::safeMode() ? "true" : "false");
  j += ",\"image_marked_good\":" +
       String(app::imageMarkedGood() ? "true" : "false");
  j += ",\"reset_reason\":" + String((int)esp_reset_reason());
  j += ",\"ip\":\"" + net::ip() + "\",\"mac\":\"" + WiFi.macAddress() + "\"";
  j += "}";
  return j;
}

static String statusJson() {
  String j = "{";
  j += "\"uptime_s\":" + String(millis() / 1000);
  j += ",\"rssi\":" + String(net::rssi());
  j += ",\"heap\":" + String(ESP.getFreeHeap());
  j += ",\"wifi_down_ms\":" + String(net::downMs());
  j += ",\"baud\":" + String(cn2::baud());
  j += ",\"open\":" + String(cn2::open() ? "true" : "false");
  j += ",\"safe_mode\":" + String(app::safeMode() ? "true" : "false");
  j += ",\"board_bytes\":" + String(cn2::byteCount(cn2::FROM_BOARD));
  j += ",\"panel_bytes\":" + String(cn2::byteCount(cn2::FROM_PANEL));
  // -1 means "never seen a byte on this line", which is more legible than the
  // 0xFFFFFFFF sentinel and is the normal state before the tap is wired up.
  auto age = [](uint32_t a) {
    return a == 0xFFFFFFFFUL ? String("-1") : String(a);
  };
  j += ",\"board_age_ms\":" + age(cn2::lastByteAgeMs(cn2::FROM_BOARD));
  j += ",\"panel_age_ms\":" + age(cn2::lastByteAgeMs(cn2::FROM_PANEL));
  j += ",\"captured\":" + String(cn2::totalCaptured());
  j += ",\"ring\":" + String(cn2::ringSize());
  j += ",\"overflows\":" + String(cn2::overflows());
  j += ",\"spoof\":" + String(cn2::spoofOn() ? "true" : "false");
  j += ",\"spoof_sent\":" + String(cn2::spoofCount());
  j += ",\"spoof_frame\":\"" + cn2::spoofFrameHex() + "\"";
  j += ",\"wire\":" + String(cn2::wire() ? "true" : "false");
  j += ",\"pure\":" + String(cn2::pure() ? "true" : "false");
  j += ",\"locked_ms\":" + String(cn2::lockedForMs());
  j += ",\"edit_c\":" + String(cn2::editC());
  j += ",\"edit_p\":" + String(cn2::editP());
  j += ",\"worst_gap_us\":" + String(cn2::worstGapUs());
  j += ",\"worst_gap_at\":" + String(cn2::worstGapAtMs());
  j += ",\"wifi_delay\":" + String(cn2::wifiDelayMs());
  j += ",\"tx_to_board\":" + String(cn2::txCount(cn2::TO_BOARD));
  j += ",\"tx_to_panel\":" + String(cn2::txCount(cn2::TO_PANEL));
  j += ",\"flow_hz\":" + String(cn2::flowHz());
  j += ",\"flow_pulses\":" + String(cn2::flowPulses());
  j += ",\"sw2\":" + String(cn2::simGet() ? "true" : "false");
  j += ",\"lid_mode\":" + String(cn2::lidMode());
  j += ",\"lid_real\":" + String(cn2::lidReal() ? "true" : "false");
  j += ",\"lid_fwd\":" + String(cn2::lidFwd() ? "true" : "false");
  j += ",\"wsr_mode\":" + String(cn2::wsRelayMode());
  j += ",\"wsr_on\":" + String(cn2::wsRelayOn() ? "true" : "false");
  j += ",\"wsr_why\":\"" + String(cn2::wsRelayWhy()) + "\"";
  j += ",\"wsr_ms\":" + String(cn2::wsRelayOnMs());
  j += ",\"wsr_n\":" + String(cn2::wsRelayCloses());
  j += ",\"wsr_lock\":" + String(cn2::wsRelayLocked() ? "true" : "false");
  j += ",\"wsr_pin\":" + String(cn2::wsRelayPin());
  j += ",\"pc\":" + String(cn2::pcycleActive() ? "true" : "false");
  j += ",\"pc_elapsed\":" + String(cn2::pcycleElapsedS());
  j += ",\"pc_remain\":" + String(cn2::pcycleRemainS());
  const int8_t pg = cn2::pcycleGuess();
  j += ",\"pc_prog\":" + (pg >= 0
         ? "\"" + String(cn2::cycleModeName((uint8_t)pg)) + "\"" : String("null"));
  j += ",\"ota_gap_ms\":" + String(cn2::lastOtaGapMs());
  j += ",\"tx_bad_p\":" + String(cn2::txBadCount(0));
  j += ",\"tx_bad_c\":" + String(cn2::txBadCount(1));
  j += ",\"e5f_mode\":" + String(cn2::e5FilterMode());
  j += ",\"e5f_on\":" + String(cn2::e5FilterMasking() ? "true" : "false");
  j += ",\"e5f_n\":" + String(cn2::e5FilterFrames());
  j += ",\"e5f_leaks\":" + String(cn2::e5FilterLeaks());
  j += ",\"e5f_doubt\":" + String(cn2::e5FilterDoubt());
  j += ",\"e5f_why\":\"" + String(cn2::e5FilterWhy()) + "\"";
  j += ",\"kasa_ip\":\"" + String(kasa::plugIp()) + "\"";
  j += ",\"stuck_ms\":" + String(cn2::stuckDwellMs());
  j += ",\"stuck_c\":" + String(cn2::stuckHotC());
  j += ",\"stuck_off\":" + String(cn2::stuckOffS());
  j += ",\"stuck_armed\":" + String(cn2::stuckArmed() ? "true" : "false");
  j += ",\"stuck_fires\":" + String(cn2::stuckFires());
  j += ",\"hceil_c\":" + String(cn2::heatCeilingC());
  j += ",\"hceil_cut\":" + String(cn2::heatCeilingCut() ? "true" : "false");
  j += ",\"hceil_n\":" + String(cn2::heatCeilingCuts());
  j += ",\"fstall_ms\":" + String(cn2::fillStallMs());
  j += ",\"fstall_cut\":" + String(cn2::fillStallCut() ? "true" : "false");
  j += ",\"fstall_n\":" + String(cn2::fillStallCuts());
  j += ",\"wsr_low\":" + String(cn2::wsRelayActiveLow() ? "true" : "false");
  j += ",\"virtual\":" + String(cn2::virtualOn() ? "true" : "false");
  j += ",\"virt_n\":" + String(cn2::virtualCount());
  j += ",\"virt_auto\":" + String(cn2::virtualAuto() ? "true" : "false");
  j += ",\"virt_temp\":" + String(cn2::virtTemp());
  j += ",\"virt_flow\":" + String(cn2::virtFlow());
  j += ",\"virt_st\":" + String(cn2::virtStatus());
  j += ",\"temp_ovr\":" + String(cn2::tempOvr());
  j += ",\"temp_real\":" + String(cn2::tempReal());
  j += ",\"flow_spoof\":" + String(cn2::flowSpoof() ? "true" : "false");
  j += ",\"flow_real\":" + String(cn2::flowCountReal());
  j += ",\"flow_fwd\":" + String(cn2::flowCountFwd());
  j += ",\"st_clr\":" + String(cn2::statusClr());
  j += ",\"st_set\":" + String(cn2::statusSet());
  j += ",\"st_real\":" + String(cn2::statusReal());
  j += ",\"st_fwd\":" + String(cn2::statusFwd());
  j += ",\"pb1\":" + String(cn2::panelB1());
  j += ",\"pb1_fwd\":" + String(cn2::panelB1Fwd());
  j += ",\"pb2\":" + String(cn2::panelB2());
  j += ",\"pb3\":" + String(cn2::panelB3());
  j += ",\"mo2\":" + String(cn2::modeOvr2());
  j += ",\"mo3\":" + String(cn2::modeOvr3());
  j += ",\"ok_c\":"  + String(cn2::frameOk(0));
  j += ",\"bad_c\":" + String(cn2::frameBad(0));
  j += ",\"ok_p\":"  + String(cn2::frameOk(1));
  j += ",\"bad_p\":" + String(cn2::frameBad(1));
  j += ",\"thin_p\":" + String(cn2::thinPanel());
  j += ",\"thin_c\":" + String(cn2::thinCtrl());
  j += ",\"p1_clr\":" + String(cn2::panelClr());
  j += ",\"p1_set\":" + String(cn2::panelSet());
  j += ",\"pressing\":" + String(cn2::pressActive() ? "true" : "false");
  j += ",\"probe\":" + String(cn2::probeOn() ? "true" : "false");
  j += ",\"btn_last\":" + String(cn2::lastBtn());
  j += ",\"btn_age\":" + String((int32_t)cn2::lastBtnAge());
  j += ",\"btn_n\":" + String(cn2::btnCount());
  j += ",\"dn\":" + String(cn2::deltaCount());
  j += ",\"base_c\":\"" + cn2::baselineHex(cn2::FROM_BOARD) + "\"";
  j += ",\"base_p\":\"" + cn2::baselineHex(cn2::FROM_PANEL) + "\"";
  j += ",\"fb\":\"" + cn2::lastFrameHex(cn2::FROM_BOARD) + "\"";
  j += ",\"fp\":\"" + cn2::lastFrameHex(cn2::FROM_PANEL) + "\"";
  j += "}";
  return j;
}

void begin() {
  // "/" is the user app; the engineering page moved to /dev. Charset on both:
  // the source is UTF-8 and both pages use literal em dashes and degree signs.
  s_server.on("/", HTTP_GET,
              []() { s_server.send_P(200, "text/html; charset=utf-8", APP_HTML); });
  s_server.on("/dev", HTTP_GET,
              []() { s_server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });

  s_server.on("/api/version", HTTP_GET,
              []() { s_server.send(200, "application/json", versionJson()); });

  s_server.on("/api/status", HTTP_GET,
              []() { s_server.send(200, "application/json", statusJson()); });

  s_server.on("/api/frames", HTTP_GET, []() {
    uint16_t n = s_server.hasArg("n") ? s_server.arg("n").toInt() : 40;
    if (n == 0 || n > 256) n = 40;
    s_server.send(200, "text/plain", cn2::dumpFrames(n));
  });

  s_server.on("/api/baud", HTTP_POST, []() {
    if (!s_server.hasArg("v")) {
      s_server.send(400, "application/json", "{\"ok\":false,\"err\":\"need v\"}");
      return;
    }
    cn2::setBaud((uint32_t)s_server.arg("v").toInt());
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"baud\":" + String(cn2::baud()) + "}");
  });

  // Impersonate the panel toward the main board.
  //   POST /api/spoof?v=on          POST /api/spoof?v=off
  s_server.on("/api/spoof", HTTP_POST, []() {
    String v = s_server.arg("v"); v.toLowerCase();
    uint32_t ms = s_server.hasArg("ms") ? s_server.arg("ms").toInt() : 200;
    if (s_server.hasArg("hex")) {
      String h = s_server.arg("hex"); h.replace(" ", "");
      uint8_t buf[16]; size_t n = 0;
      for (size_t i = 0; i + 1 < h.length() && n < sizeof(buf); i += 2)
        buf[n++] = (uint8_t)strtoul(h.substring(i, i + 2).c_str(), nullptr, 16);
      cn2::setSpoofFrame(buf, n);
    }
    cn2::setSpoof(v == "on" || v == "1" || v == "true", ms);
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"spoof\":" +
                  String(cn2::spoofOn() ? "true" : "false") +
                  ",\"ms\":" + String(cn2::spoofPeriod()) + "}");
  });

  // Raw injection:  POST /api/send?to=board&hex=AA0000000AA
  s_server.on("/api/send", HTTP_POST, []() {
    String to = s_server.arg("to"); to.toLowerCase();
    String hex = s_server.arg("hex");
    hex.replace(" ", "");
    if (hex.length() < 2 || hex.length() % 2) {
      s_server.send(400, "application/json",
                    "{\"ok\":false,\"err\":\"hex must be even length\"}");
      return;
    }
    uint8_t buf[64];
    size_t n = 0;
    for (size_t i = 0; i + 1 < hex.length() && n < sizeof(buf); i += 2) {
      buf[n++] = (uint8_t)strtoul(hex.substring(i, i + 2).c_str(), nullptr, 16);
    }
    size_t w = cn2::sendTo(to == "panel" ? cn2::TO_PANEL : cn2::TO_BOARD, buf, n);
    s_server.send(w ? 200 : 409, "application/json",
                  "{\"ok\":" + String(w ? "true" : "false") +
                  ",\"sent\":" + String((unsigned)w) + "}");
  });

  //   POST /api/flow?hz=100     POST /api/flow?hz=0   (stop)
  s_server.on("/api/flow", HTTP_POST, []() {
    uint32_t hz = s_server.hasArg("hz") ? s_server.arg("hz").toInt() : 0;
    cn2::flowSet(hz);
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"hz\":" + String(cn2::flowHz()) + "}");
  });

  //   POST /api/sim?sw=2&v=on|off   — lid switch only; SW1 has no pin
  s_server.on("/api/sim", HTTP_POST, []() {
    int sw = s_server.arg("sw").toInt();
    String v = s_server.arg("v"); v.toLowerCase();
    bool on = (v == "on" || v == "1" || v == "true");
    if (sw != 2) {
      s_server.send(400, "application/json",
                    "{\"ok\":false,\"err\":\"only sw=2 (lid) — SW1 has no pin\"}");
      return;
    }
    cn2::simSet(on);
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"sw\":2,\"on\":" +
                  String(on ? "true" : "false") + "}");
  });

  //   POST /api/lidovr?v=on|off   — rewrites board->panel, display only
  //   POST /api/lid?m=0|1|2   0=pass real, 1=force LID ON, 2=force LID OFF
  s_server.on("/api/lid", HTTP_POST, []() {
    cn2::setLidMode((uint8_t)s_server.arg("m").toInt());
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"lid_mode\":" + String(cn2::lidMode()) + "}");
  });

  //   POST /api/wsrelay?mode=off|on|auto[&pol=low|high][&pin=N]
  //
  // The external wash-pump relay. AUTO follows b0 of the forwarded panel frame,
  // which is what makes the machine's own cycles drive the pump.
  s_server.on("/api/wsrelay", HTTP_POST, []() {
    if (s_server.hasArg("pin"))
      cn2::setWsRelayPin((int8_t)s_server.arg("pin").toInt());
    if (s_server.hasArg("pol"))
      cn2::setWsRelayPolarity(s_server.arg("pol") != "high");
    if (s_server.hasArg("mode")) {
      String m = s_server.arg("mode");
      cn2::setWsRelayMode(m == "on" ? cn2::WSR_ON
                        : m == "off" ? cn2::WSR_OFF : cn2::WSR_AUTO);
    }
    s_server.send(200, "application/json",
      "{\"ok\":true,\"mode\":" + String(cn2::wsRelayMode()) +
      ",\"on\":" + String(cn2::wsRelayOn() ? "true" : "false") +
      ",\"pin\":" + String(cn2::wsRelayPin()) +
      ",\"low\":" + String(cn2::wsRelayActiveLow() ? "true" : "false") +
      ",\"why\":\"" + String(cn2::wsRelayWhy()) + "\"}");
  });

  //   POST /api/kasa?ip=192.168.14.123        — set the plug
  //   POST /api/kasa?test=1                   — probe it
  //   POST /api/kasa?cycle=30                 — power-cycle NOW (refuses mid-cycle)
  s_server.on("/api/kasa", HTTP_POST, []() {
    if (s_server.hasArg("ip")) kasa::setPlug(s_server.arg("ip").c_str());
    String extra = "";
    if (s_server.hasArg("test"))
      extra = ",\"reachable\":" + String(kasa::reachable() ? "true" : "false");
    if (s_server.hasArg("cycle")) {
      // Never cut mains to a machine that is doing something.
      if (cn2::panelB1() != 0 || cn2::cycleState() == 1) {
        s_server.send(409, "application/json",
          "{\"ok\":false,\"err\":\"loads are commanded — refusing\"}");
        return;
      }
      extra += ",\"switched_off\":" +
               String(kasa::powerOff() ? "true" : "false");
    }
    s_server.send(200, "application/json",
      "{\"ok\":true,\"plug\":\"" + String(kasa::plugIp()) + "\"" + extra +
      ",\"err\":\"" + String(kasa::lastError()) + "\"}");
  });

  //   POST /api/stuckwatch?ms=180000&c=40&off=30   (ms=0 disables)
  s_server.on("/api/stuckwatch", HTTP_POST, []() {
    cn2::setStuckWatch(s_server.hasArg("ms") ? s_server.arg("ms").toInt()
                                             : cn2::stuckDwellMs(),
                       (uint8_t)(s_server.hasArg("c") ? s_server.arg("c").toInt() : 0),
                       (uint16_t)(s_server.hasArg("off") ? s_server.arg("off").toInt() : 0));
    s_server.send(200, "application/json",
      "{\"ok\":true,\"ms\":" + String(cn2::stuckDwellMs()) +
      ",\"c\":" + String(cn2::stuckHotC()) +
      ",\"off\":" + String(cn2::stuckOffS()) +
      ",\"fires\":" + String(cn2::stuckFires()) + "}");
  });

  //   POST /api/heatceiling?c=105      (0 disables)
  s_server.on("/api/heatceiling", HTTP_POST, []() {
    if (s_server.hasArg("c")) cn2::setHeatCeiling((uint8_t)s_server.arg("c").toInt());
    s_server.send(200, "application/json",
      "{\"ok\":true,\"c\":" + String(cn2::heatCeilingC()) +
      ",\"cut\":" + String(cn2::heatCeilingCut() ? "true" : "false") +
      ",\"cuts\":" + String(cn2::heatCeilingCuts()) + "}");
  });

  //   POST /api/fillstall?ms=120000    (0 disables)
  s_server.on("/api/fillstall", HTTP_POST, []() {
    if (s_server.hasArg("ms")) cn2::setFillStall(s_server.arg("ms").toInt());
    s_server.send(200, "application/json",
      "{\"ok\":true,\"ms\":" + String(cn2::fillStallMs()) +
      ",\"cut\":" + String(cn2::fillStallCut() ? "true" : "false") +
      ",\"cuts\":" + String(cn2::fillStallCuts()) + "}");
  });

  //
  // Bounds the untargeted cool-down flush. Nothing at either end of the link
  // times one out -- it ends when the water stops arriving, which is the tank
  // running dry. On a machine fed from an always-on supply that never happens.
  //   POST /api/wifidelay?ms=60000    (0 = normal)
  s_server.on("/api/wifidelay", HTTP_POST, []() {
    if (s_server.hasArg("ms")) cn2::setWifiDelayMs(s_server.arg("ms").toInt());
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"ms\":" + String(cn2::wifiDelayMs()) + "}");
  });

  //   POST /api/e5filter?mode=off|auto|force
  //
  // The controller on the newer board raises bit 6 two seconds after every
  // power-on while still warm, and again the instant a dry stage ends -- both
  // on a byte-exact link. AUTO refuses to relay that claim while it can be
  // disproved, and passes it straight through when it cannot.
  s_server.on("/api/e5filter", HTTP_POST, []() {
    if (s_server.hasArg("mode")) {
      String m = s_server.arg("mode");
      cn2::setE5Filter(m == "off" ? cn2::E5F_OFF
                     : m == "force" ? cn2::E5F_FORCE : cn2::E5F_AUTO);
    }
    s_server.send(200, "application/json",
      "{\"ok\":true,\"mode\":" + String(cn2::e5FilterMode()) +
      ",\"masking\":" + String(cn2::e5FilterMasking() ? "true" : "false") +
      ",\"frames\":" + String(cn2::e5FilterFrames()) +
      ",\"why\":\"" + String(cn2::e5FilterWhy()) + "\"}");
  });

  //   POST /api/pinprobe?pin=3    — transmit stubs only
  // Electrical proof of the wire bridge: enable the input stage on all four
  // CN2 pads and count level transitions for 250 ms. The two RX pads always
  // toggle (the devices transmit regardless); the two TX pads only toggle if
  // something is actually DRIVING them -- in wire mode that is the matrix
  // bridge, so tx edges ~= the opposite rx edges proves the bridge conducts,
  // and a silent tx pad proves it does not, no theory required.
  s_server.on("/api/wirecheck", HTTP_POST, []() {
    const int8_t pins[4] = { cn2::pinRxBoard(), cn2::pinTxBoard(),
                             cn2::pinRxPanel(), cn2::pinTxPanel() };
    const char *names[4] = { "rx_board", "tx_board", "rx_panel", "tx_panel" };
    for (int i = 0; i < 4; i++)
      if (pins[i] >= 0) gpio_ll_input_enable(&GPIO, (uint32_t)pins[i]);
    uint32_t edges[4] = {0,0,0,0};
    uint32_t prev = GPIO.in.data;
    const uint32_t t0 = millis();
    while (millis() - t0 < 250) {
      const uint32_t cur = GPIO.in.data;
      const uint32_t diff = cur ^ prev;
      for (int i = 0; i < 4; i++)
        if (pins[i] >= 0 && (diff >> pins[i]) & 1) edges[i]++;
      prev = cur;
    }
    String j = "{\"wire\":" + String(cn2::wire() ? "true" : "false");
    for (int i = 0; i < 4; i++)
      j += ",\"" + String(names[i]) + "\":" + String(edges[i]);
    j += "}";
    s_server.send(200, "application/json", j);
  });

  // WIRE mode: pad-to-pad bridge in the GPIO matrix. The default. Sniffing
  // continues; every rewrite feature goes dormant until this is turned off.
  // GET returns live state AND the stored boot preference -- added while
  // debugging a persist failure; cheap enough to keep.
  s_server.on("/api/wire", HTTP_GET, []() {
    Preferences p; p.begin("d8link", true);
    const bool stored = p.getBool("wire", true);
    p.end();
    s_server.send(200, "application/json",
      String("{\"wire\":") + (cn2::wire() ? "true" : "false") +
      ",\"stored\":" + (stored ? "true" : "false") + "}");
  });
  s_server.on("/api/wire", HTTP_POST, []() {
    bool ok = true;
    if (s_server.hasArg("on")) ok = cn2::wireSet(s_server.arg("on").toInt() != 0);
    s_server.send(200, "application/json",
      String("{\"ok\":") + (ok ? "true" : "false") +
      ",\"wire\":" + (cn2::wire() ? "true" : "false") + "}");
  });

  // Resume the NVS-persisted cycle after the lockout recovery's mains cut.
  // Called by the HA watchdog after a probe-verified unlock -- deliberately an
  // explicit external command, never something the board does on its own boot.
  s_server.on("/api/cycle_recover", HTTP_POST, []() {
    const bool ok = cn2::cycleRecover();
    s_server.send(200, "application/json",
      String("{\"recovered\":") + (ok ? "true" : "false") +
      ",\"state\":" + String(cn2::cycleState()) +
      ",\"stage\":" + String(cn2::cycleStage()) + "}");
  });

  // Relay-path stall profile. ?reset=1 zeroes it.
  s_server.on("/api/prof", HTTP_GET, []() {
    if (s_server.hasArg("reset")) cn2::profReset();
    static const char *BK[7] = {"lt2ms","2_5ms","5_10ms","10_20ms","20_50ms","50_100ms","gt100ms"};
    String j = "{\"passes\":" + String(cn2::profPasses());
    for (int b = 0; b < 7; b++)
      j += ",\"" + String(BK[b]) + "\":" + String(cn2::profHist(b));
    j += ",\"worst_us\":" + String(cn2::worstGapUs());
    j += ",\"rx_backlog_max\":" + String(cn2::profRxMax());
    static const char *CN[3] = {"http","nvs","misc"};
    for (uint8_t w = 0; w < 3; w++) {
      uint32_t n, mx, tot; cn2::profCause(w, n, mx, tot);
      j += ",\"" + String(CN[w]) + "_n\":" + String(n);
      j += ",\"" + String(CN[w]) + "_max_us\":" + String(mx);
      j += ",\"" + String(CN[w]) + "_total_ms\":" + String(tot);
    }
    j += "}";
    s_server.send(200, "application/json", j);
  });

  // Byte-perfect on demand. Turning this on strips every rewrite at the emit
  // point, so the E5 mask, the heat ceiling and the cycle runner all stop
  // acting -- it is a diagnostic and a fallback, not an operating mode.
  s_server.on("/api/pure", HTTP_POST, []() {
    if (s_server.hasArg("on")) cn2::setPure(s_server.arg("on").toInt() != 0);
    if (s_server.hasArg("reset")) cn2::resetEdits();
    String j = "{\"pure\":" + String(cn2::pure() ? "true" : "false");
    j += ",\"edit_c\":" + String(cn2::editC());
    j += ",\"edit_p\":" + String(cn2::editP()) + "}";
    s_server.send(200, "application/json", j);
  });

  s_server.on("/api/pinprobe", HTTP_POST, []() {
    cn2::PinProbe p;
    const bool ok = cn2::pinProbe((int8_t)s_server.arg("pin").toInt(), p);
    String j = "{\"ok\":" + String(ok ? "true" : "false");
    j += ",\"pin\":" + String(p.pin);
    j += ",\"pullup\":" + String(p.pullup);
    j += ",\"pulldown\":" + String(p.pulldown);
    j += ",\"drive_hi\":" + String(p.drive_hi);
    j += ",\"drive_lo\":" + String(p.drive_lo);
    j += ",\"toggles\":" + String(p.toggles);
    j += ",\"edges\":" + String(p.edges);
    j += ",\"verdict\":\"" + String(p.verdict) + "\"}";
    s_server.send(200, "application/json", j);
  });

  //   POST /api/status_ovr?clr=42&set=00     (hex masks on byte 3)
  s_server.on("/api/status_ovr", HTTP_POST, []() {
    uint8_t c = (uint8_t)strtoul(s_server.arg("clr").c_str(), nullptr, 16);
    uint8_t v = (uint8_t)strtoul(s_server.arg("set").c_str(), nullptr, 16);
    cn2::setStatusMask(c, v);
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"clr\":" + String(c) + ",\"set\":" + String(v) + "}");
  });

  //   POST /api/flowspoof?v=on|off
  // Holds the flow count the PANEL sees at zero, which is how E1 "no water" is
  // provoked: the panel raises it when the intake motor is commanded and the
  // counter never moves. Only the starve direction exists — see cn2.h.
  s_server.on("/api/flowspoof", HTTP_POST, []() {
    cn2::setFlowSpoof(s_server.arg("v") == "on");
    s_server.send(200, "application/json",
                  String("{\"ok\":true,\"flow_spoof\":") +
                      (cn2::flowSpoof() ? "true" : "false") + "}");
  });

  //   POST /api/virtual?v=on|off[&temp=30&flow=0&st=0&b5=11]
  // Become the controller: the panel is fed frames we synthesise and the real
  // controller's are dropped. PROBE is engaged at the same time so the real
  // controller keeps hearing an idle panel and never receives a command.
  s_server.on("/api/virtual", HTTP_POST, []() {
    if (s_server.hasArg("temp") || s_server.hasArg("flow") ||
        s_server.hasArg("st") || s_server.hasArg("b5")) {
      auto n = [](const String &a, uint8_t d) {
        return a.length() ? (uint8_t)strtoul(a.c_str(), nullptr, 0) : d;
      };
      cn2::setVirtualFrame(n(s_server.arg("temp"), cn2::virtTemp()),
                           n(s_server.arg("flow"), cn2::virtFlow()),
                           n(s_server.arg("st"),   cn2::virtStatus()),
                           n(s_server.arg("b5"),   cn2::virtB5()));
    }
    if (s_server.hasArg("auto")) cn2::setVirtualAuto(s_server.arg("auto") == "on");
    if (s_server.hasArg("v")) cn2::setVirtual(s_server.arg("v") == "on");
    s_server.send(200, "application/json",
                  String("{\"ok\":true,\"virtual\":") +
                      (cn2::virtualOn() ? "true" : "false") +
                      ",\"n\":" + String(cn2::virtualCount()) + "}");
  });

  //   GET /api/app — everything the user page draws, in one request.
  // Deliberately not /api/status: that is 40+ fields of engineering state, and
  // the app polls once a second.
  s_server.on("/api/app", HTTP_GET, []() {
    // What the PANEL is told, not what the controller said. Those differ
    // whenever the false-E5 filter is masking, and the app must agree with the
    // appliance in front of the user: reporting an error the machine is not
    // showing is worse than reporting none at all. The raw byte stays visible
    // on the engineering page, which is where a discrepancy belongs.
    const uint8_t st = cn2::statusFwd();
    const uint8_t s3 = cn2::cycleState();
    String j = "{\"state\":" + String(s3) +
               ",\"mode\":" + String(cn2::cycleMode()) +
               ",\"stage\":" + String(cn2::cycleStage()) +
               ",\"elapsed\":" + String(cn2::cycleElapsed()) +
               ",\"temp\":" + String(cn2::tempFwd()) +
               ",\"flow\":" + String(cn2::flowCountFwd()) +
               ",\"lid\":" + String(cn2core::lidClosed(st) ? "true" : "false") +
               ",\"e5\":" + String((st & 0x40) ? "true" : "false") +
               ",\"why\":\"" + String(cn2::cycleWhy()) + "\"";
    // A cycle started at the PANEL. The wire carries no countdown -- the panel
    // keeps its own timer -- so remain/total are estimates from the reference
    // program tables, and pc_prog is null until the stage sequence identifies
    // the program uniquely.
    j += ",\"pb1\":" + String(cn2::panelB1());
    j += ",\"pb1_fwd\":" + String(cn2::panelB1Fwd());
    j += ",\"p1_set\":" + String(cn2::panelSet());
    j += ",\"p1_clr\":" + String(cn2::panelClr());
    j += ",\"locked\":" + String((cn2::statusReal() & 0x40) ? "true" : "false");
    j += ",\"plug\":\"" + String(kasa::plugIp()) + "\"";
    j += ",\"wire\":" + String(cn2::wire() ? "true" : "false");
    j += ",\"locked_ms\":" + String(cn2::lockedForMs());
    j += ",\"link_err\":" + String(cn2::frameBad(0) + cn2::frameBad(1));
    j += ",\"uptime_s\":" + String(millis() / 1000);
    j += ",\"pc\":" + String(cn2::pcycleActive() ? "true" : "false");
    if (cn2::pcycleActive()) {
      j += ",\"pc_elapsed\":" + String(cn2::pcycleElapsedS());
      j += ",\"pc_phase\":\"" + String(cn2::pcyclePhaseName()) + "\"";
      j += ",\"pc_stage\":" + String(cn2::pcycleStageN());
      const int8_t g = cn2::pcycleGuess();
      j += ",\"pc_prog\":" + (g >= 0
             ? "\"" + String(cn2::cycleModeName((uint8_t)g)) + "\"" : String("null"));
      j += ",\"pc_remain\":" + String(cn2::pcycleRemainS());
      j += ",\"pc_total\":" + String(cn2::pcycleTotalS());
    }
    // The fault register, decoded to the code the panel would show. Lowest set
    // bit wins, which is what the panel does.
    const char *err = nullptr, *txt = "";
    if (st & 0x04)      { err = "E3"; txt = "Sensor open circuit"; }
    else if (st & 0x08) { err = "E4"; txt = "Sensor short circuit"; }
    else if (st & 0x10) { err = "E0"; txt = "Voltage anomaly"; }
    else if (st & 0x20) { err = "E7"; txt = "Fan failure"; }
    else if (st & 0x40) { err = "E5"; txt = "Communication failure"; }
    j += ",\"err\":" + (err ? "\"" + String(err) + "\"" : String("null"));
    j += ",\"errtxt\":\"" + String(txt) + "\",\"modes\":[";
    for (uint8_t m = 0; m < cn2::cycleModeCount(); m++) {
      if (m) j += ',';
      j += "{\"n\":\"" + String(cn2::cycleModeName(m)) +
           "\",\"empty\":" + String(cn2::cycleModeEmpty(m) ? "true" : "false") + "}";
    }
    j += "],\"stages\":[";
    for (uint8_t i = 0; i < cn2::cycleCount(); i++) {
      if (i) j += ',';
      j += "{\"name\":\"" + String(cn2::cycleName(i)) +
           "\",\"secs\":" + String(cn2::cycleSecs(i)) + "}";
    }
    s_server.send(200, "application/json", j + "]}");
  });

  //   POST /api/custom?slot=0&name=Triple+Wash&stages=02:00:20,20:20:0,...
  //   GET  /api/custom  — the current slots, as editable specs
  // Rejected lists come back 400 with the reason, so the editor can say why.
  s_server.on("/api/custom", HTTP_POST, []() {
    const char *err = cn2::cycleSetCustom(
        (uint8_t)s_server.arg("slot").toInt(),
        s_server.arg("name").c_str(), s_server.arg("stages").c_str());
    if (err) {
      s_server.send(400, "application/json",
                    String("{\"ok\":false,\"err\":\"") + err + "\"}");
      return;
    }
    s_server.send(200, "application/json", "{\"ok\":true}");
  });
  s_server.on("/api/custom", HTTP_GET, []() {
    const uint8_t first = cn2::cycleCustomFirst(), was = cn2::cycleMode();
    String j = "{\"first\":" + String(first) + ",\"slots\":[";
    for (uint8_t k = 0; k < cn2::cycleCustomSlots(); k++) {
      if (k) j += ',';
      if (cn2::cycleModeEmpty(first + k)) { j += "{\"name\":\"\",\"spec\":\"\"}"; continue; }
      cn2::cycleSetMode(first + k);          // read that program's stage list
      j += "{\"name\":\"" + String(cn2::cycleModeName(first + k)) + "\",\"spec\":\"";
      for (uint8_t i = 0; i < cn2::cycleCount(); i++) {
        if (i) j += ',';
        j += cn2::cycleStageSpec(i);
      }
      j += "\"}";
    }
    cn2::cycleSetMode(was);
    s_server.send(200, "application/json", j + "]}");
  });

  //   POST /api/cycle?run=start|stop        |  &i=<n>&secs=<n> to retime a stage
  //   GET  /api/cycle                        |  stage table + live state
  // The ESP32 drives the whole cycle; the panel is held idle. See cn2.h for the
  // interlocks, which exist because the machine has none.
  s_server.on("/api/cycle", HTTP_POST, []() {
    if (s_server.hasArg("mode")) cn2::cycleSetMode((uint8_t)s_server.arg("mode").toInt());
    if (s_server.hasArg("del")) cn2::cycleDelCustom((uint8_t)s_server.arg("del").toInt());
    if (s_server.hasArg("water") || s_server.hasArg("dry"))
      cn2::cycleSetTemps(s_server.hasArg("water") ? (int16_t)s_server.arg("water").toInt() : -1,
                         s_server.hasArg("dry")   ? (int16_t)s_server.arg("dry").toInt()   : -1);
    if (s_server.hasArg("i") && s_server.hasArg("secs"))
      cn2::cycleSetSecs((uint8_t)s_server.arg("i").toInt(),
                        (uint32_t)strtoul(s_server.arg("secs").c_str(), nullptr, 10));
    String r = s_server.arg("run");
    if (r == "start") cn2::cycleStart();
    else if (r == "stop") cn2::cycleStop();
    else if (r == "resume") cn2::cycleResume();
    else if (r == "defaults") cn2::cycleResetSecs();
    s_server.send(200, "application/json",
                  String("{\"ok\":true,\"state\":") + cn2::cycleState() + "}");
  });
  s_server.on("/api/cycle", HTTP_GET, []() {
    String j = "{\"state\":" + String(cn2::cycleState()) +
               ",\"mode\":" + String(cn2::cycleMode()) +
               ",\"maxc\":" + String(cn2::cycleModeMaxC(cn2::cycleMode())) +
               ",\"waterc\":" + String(cn2::cycleWaterC()) +
               ",\"dryc\":" + String(cn2::cycleDryC()) +
               ",\"modes\":[";
    for (uint8_t m = 0; m < cn2::cycleModeCount(); m++) {
      if (m) j += ',';
      j += "{\"n\":\"" + String(cn2::cycleModeName(m)) +
           "\",\"c\":" + String(cn2::cycleModeMaxC(m)) +
           ",\"empty\":" + String(cn2::cycleModeEmpty(m) ? "true" : "false") +
           ",\"custom\":" + String(m >= cn2::cycleCustomFirst() ? "true" : "false") + "}";
    }
    j += "],\"stage\":" + String(cn2::cycleStage()) +
               ",\"elapsed\":" + String(cn2::cycleElapsed()) +
               ",\"why\":\"" + String(cn2::cycleWhy()) + "\",\"stages\":[";
    for (uint8_t i = 0; i < cn2::cycleCount(); i++) {
      if (i) j += ',';
      j += "{\"name\":\"" + String(cn2::cycleName(i)) +
           "\",\"secs\":" + String(cn2::cycleSecs(i)) +
           ",\"loads\":" + String(cn2::cycleLoads(i)) +
           ",\"tgt\":" + String(cn2::cycleTgt(i)) + "}";
    }
    s_server.send(200, "application/json", j + "]}");
  });

  //   POST /api/tempovr?v=0..255   |   v=-1 (or empty) restores pass-through
  s_server.on("/api/tempovr", HTTP_POST, []() {
    String a = s_server.arg("v");
    cn2::setTempOvr(a.length() ? (int16_t)a.toInt() : -1);
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"temp_ovr\":" + String(cn2::tempOvr()) + "}");
  });

  //   POST /api/press?mask=20&ms=800
  s_server.on("/api/press", HTTP_POST, []() {
    uint8_t m = (uint8_t)strtoul(s_server.arg("mask").c_str(), nullptr, 16);
    uint32_t ms = s_server.hasArg("ms") ? s_server.arg("ms").toInt() : 800;
    cn2::pressButton(m, ms);
    s_server.send(200, "application/json", "{\"ok\":true,\"mask\":" + String(m) + "}");
  });

  //   POST /api/probe?v=on|off
  s_server.on("/api/probe", HTTP_POST, []() {
    String v = s_server.arg("v"); v.toLowerCase();
    cn2::setProbe(v == "on" || v == "1" || v == "true");
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"probe\":" +
                  String(cn2::probeOn() ? "true" : "false") + "}");
  });
  s_server.on("/api/btnclear", HTTP_POST, []() {
    cn2::clearBtn();
    s_server.send(200, "application/json", "{\"ok\":true}");
  });

  s_server.on("/api/baseline", HTTP_POST, []() {
    cn2::setBaseline();
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"ctrl\":\"" + cn2::baselineHex(cn2::FROM_BOARD) +
                  "\",\"panel\":\"" + cn2::baselineHex(cn2::FROM_PANEL) + "\"}");
  });
  s_server.on("/api/deltaclear", HTTP_POST, []() {
    cn2::clearDeltas();
    s_server.send(200, "application/json", "{\"ok\":true}");
  });
  s_server.on("/api/hist", HTTP_GET, []() {
    s_server.send(200, "application/json",
                  "{\"ctrl\":" + cn2::histJson(0) +
                  ",\"panel\":" + cn2::histJson(1) +
                  ",\"to_panel\":" + cn2::histJson(2) +
                  ",\"to_ctrl\":" + cn2::histJson(3) + "}");
  });
  //   POST /api/panel_ovr?clr=20&set=00   — byte 1, panel -> controller
  s_server.on("/api/panel_ovr", HTTP_POST, []() {
    uint8_t c = (uint8_t)strtoul(s_server.arg("clr").c_str(), nullptr, 16);
    uint8_t v = (uint8_t)strtoul(s_server.arg("set").c_str(), nullptr, 16);
    cn2::setPanelMask(c, v);
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"clr\":" + String(c) + ",\"set\":" + String(v) + "}");
  });

  //   POST /api/mode_ovr?b2=40&b3=20  — force the mode word; empty args = pass
  s_server.on("/api/mode_ovr", HTTP_POST, []() {
    String a = s_server.arg("b2"), b = s_server.arg("b3");
    int16_t v2 = a.length() ? (int16_t)strtoul(a.c_str(), nullptr, 16) : -1;
    int16_t v3 = b.length() ? (int16_t)strtoul(b.c_str(), nullptr, 16) : -1;
    cn2::setModeOvr(v2, v3);
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"b2\":" + String(v2) + ",\"b3\":" + String(v3) + "}");
  });

  //   POST /api/thin?panel=4&ctrl=1  — forward only 1 frame in N toward each end
  s_server.on("/api/thin", HTTP_POST, []() {
    uint16_t tp = (uint16_t)strtoul(s_server.arg("panel").c_str(), nullptr, 10);
    uint16_t tc = (uint16_t)strtoul(s_server.arg("ctrl").c_str(),  nullptr, 10);
    cn2::setThin(tp, tc);
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"panel\":" + String(cn2::thinPanel()) +
                  ",\"ctrl\":" + String(cn2::thinCtrl()) + "}");
  });

  //   POST /api/detect            passive pin scan only
  //   POST /api/detect?phase2=1    also resolve which TX stub is which
  s_server.on("/api/detect", HTTP_POST, []() {
    bool p2 = s_server.arg("phase2") == "1";
    bool ok = cn2::detectStart(p2);
    s_server.send(ok ? 200 : 409, "application/json",
                  ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"already running\"}");
  });

  //   POST /api/pinmap?rxb=5&txb=6&txp=3&rxp=4   — set it directly, persisted
  s_server.on("/api/pinmap", HTTP_POST, []() {
    auto g = [&](const char *k, int dflt) {
      String v = s_server.arg(k);
      return v.length() ? (int8_t)v.toInt() : (int8_t)dflt;
    };
    int8_t rxb, txb, txp, rxp;
    cn2::pinMapNow(rxb, txb, txp, rxp);
    cn2::setPinMap(g("rxb", rxb), g("txb", txb), g("txp", txp), g("rxp", rxp));
    cn2::pinMapNow(rxb, txb, txp, rxp);
    s_server.send(200, "application/json",
                  "{\"ok\":true,\"rxb\":" + String(rxb) + ",\"txb\":" + String(txb) +
                  ",\"txp\":" + String(txp) + ",\"rxp\":" + String(rxp) + "}");
  });

  s_server.on("/api/detect", HTTP_GET, []() {
    const cn2::Detect &d = cn2::detectResult();
    int8_t rxb, txb, txp, rxp;
    cn2::pinMapNow(rxb, txb, txp, rxp);
    String j = "{\"running\":" + String(d.running ? "true" : "false");
    j += ",\"done\":" + String(d.done ? "true" : "false");
    j += ",\"phase\":" + String(d.phase);
    j += ",\"applied\":" + String(d.applied ? "true" : "false");
    j += ",\"edge\":[";
    for (int i = 0; i < 4; i++) j += (i ? "," : "") + String(d.edge[i]);
    j += "],\"cand\":[";
    for (int i = 0; i < 4; i++) j += (i ? "," : "") + String(d.cand[i]);
    j += "],\"rx_ctrl\":"  + String(d.rx_ctrl);
    j += ",\"rx_panel\":"  + String(d.rx_panel);
    j += ",\"tx_ctrl\":"   + String(d.tx_ctrl);
    j += ",\"tx_panel\":"  + String(d.tx_panel);
    j += ",\"tx_a\":"      + String(d.tx_a);
    j += ",\"tx_b\":"      + String(d.tx_b);
    j += ",\"now\":[" + String(rxb) + "," + String(txb) + "," +
                          String(txp) + "," + String(rxp) + "]";
    j += ",\"note\":\"" + String(d.note) + "\"}";
    s_server.send(200, "application/json", j);
  });

  //   GET /api/graph — 30 min of temperature (bit 7 = WATER heater) plus a
  //   parallel air-heater lane, and 60 s of flow
  s_server.on("/api/graph", HTTP_GET, []() {
    String j = "{\"temp\":\"" + cn2::trendTempHex() + "\"";
    j += ",\"air\":\"" + cn2::trendAirHex() + "\"";
    j += ",\"flow\":\"" + cn2::trendFlowHex() + "\"";
    j += ",\"cutout\":" + String(cn2::trendCutout());
    j += ",\"filling\":" + String(cn2::trendFlowActive() ? "true" : "false") + "}";
    s_server.send(200, "application/json", j);
  });

  //   GET  /api/flowlog — every change of the flow count, [[ms,count],...]
  //   POST /api/flowlog?clear=1
  s_server.on("/api/flowlog", HTTP_GET, []() {
    s_server.send(200, "application/json",
                  "{\"n\":" + String(cn2::flowLogLen()) +
                  ",\"ev\":" + cn2::flowLogJson() + "}");
  });
  s_server.on("/api/flowlog", HTTP_POST, []() {
    cn2::flowLogClear();
    s_server.send(200, "application/json", "{\"ok\":true}");
  });

  s_server.on("/api/qclear", HTTP_POST, []() {
    cn2::qualityClear();
    cn2::resetGap();   // the worst-gap mark is part of link quality
    s_server.send(200, "application/json", "{\"ok\":true}");
  });

  s_server.on("/api/histclear", HTTP_POST, []() {
    cn2::histClear();
    s_server.send(200, "application/json", "{\"ok\":true}");
  });

  s_server.on("/api/deltas", HTTP_GET, []() {
    s_server.send(200, "application/json", cn2::deltaJson());
  });

  s_server.on("/api/clear", HTTP_POST, []() {
    cn2::clear();
    s_server.send(200, "application/json", "{\"ok\":true}");
  });

  s_server.on("/api/reboot", HTTP_POST, []() {
    s_server.send(200, "application/json", "{\"ok\":true}");
    delay(200);
    ESP.restart();
  });

  // ---- Second OTA path -----------------------------------------------------
  //   curl -f -F "firmware=@.pio/build/c3/firmware.bin" \
  //        "http://baby-washer.local/update?key=$OTA_PASSWORD"
  // Unlike espota this needs no reverse connection back to the uploading host,
  // so it survives the subnet boundary and anything that only allows outbound
  // TCP. Two independent ways in is the whole point.
  s_server.on(
      "/update", HTTP_POST,
      []() {
        bool ok = s_uploadAuthOk && !Update.hasError();
        s_server.sendHeader("Connection", "close");
        s_server.send(ok ? 200 : (s_uploadAuthOk ? 500 : 401),
                      "application/json",
                      ok ? "{\"ok\":true}"
                         : (s_uploadAuthOk ? "{\"ok\":false,\"err\":\"update\"}"
                                           : "{\"ok\":false,\"err\":\"auth\"}"));
        if (ok) {
          Serial.println("[http-ota] verified — rebooting");
          delay(300);
          ESP.restart();
        }
      },
      []() {
        HTTPUpload &up = s_server.upload();
        if (up.status == UPLOAD_FILE_START) {
          s_uploadAuthOk = (s_server.arg("key") == String(OTA_PASSWORD));
          if (!s_uploadAuthOk) {
            Serial.println("[http-ota] REJECTED — bad key");
            return;
          }
          // An update starves the panel for as long as it takes, and this panel
          // latches E5 over it -- a fault the owner then has to power-cycle the
          // appliance to clear. Never do that to a machine that is mid-cycle:
          // the loads are held by the panel, and the runner's interlocks are
          // not watching a cycle the panel owns.
          //
          // "Idle on the wire" is NOT the test. This controller runs its 72 h
          // storage autonomously with the panel commanding nothing, so pb1 == 0
          // proves only that the panel is quiet.
          if (!s_server.hasArg("force") &&
              (cn2::panelB1() != 0 || cn2::pcycleActive() ||
               cn2::cycleState() == 1 || (cn2::statusReal() & 0x40))) {
            s_uploadAuthOk = false;
            Serial.println("[http-ota] REFUSED — machine is active "
                           "(cycle, or storage flagged). Add &force=1 to override.");
            return;
          }
          Serial.printf("[http-ota] receiving %s\n", up.filename.c_str());
          cn2::markOtaStart();
          cn2::quiesce();
          esp_task_wdt_reset();
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
        } else if (up.status == UPLOAD_FILE_WRITE) {
          if (!s_uploadAuthOk) return;
          esp_task_wdt_reset();
          // Hand the UARTs everything pending BEFORE the write. Update.write()
          // disables the instruction cache, so relayTask cannot run through it
          // whatever its priority -- but the TX FIFO keeps clocking. 128 bytes
          // is ~133 ms of 9600-baud transmission, which is the buffer that
          // carries the far ends across each stall.
          cn2::serviceNow();
          if (Update.write(up.buf, up.currentSize) != up.currentSize)
            Update.printError(Serial);
          cn2::serviceNow();
        } else if (up.status == UPLOAD_FILE_END) {
          if (!s_uploadAuthOk) return;
          if (Update.end(true))
            Serial.printf("[http-ota] wrote %u bytes\n", up.totalSize);
          else
            Update.printError(Serial);
        }
      });

  s_server.onNotFound([]() { s_server.send(404, "text/plain", "not found\n"); });

  s_server.begin();
  Serial.println("[http ] server on :80");
}

void loop() {
  // Timed because handleClient() is suspect #1 for relay stalls: it serves a
  // whole request synchronously, including multi-KB JSON builds and slow
  // socket writes to weak-RF clients.
  const uint32_t t0 = micros();
  s_server.handleClient();
  const uint32_t us = micros() - t0;
  if (us > 300) cn2::profNote(0, us);   // ignore the no-client fast path
}

}  // namespace web
