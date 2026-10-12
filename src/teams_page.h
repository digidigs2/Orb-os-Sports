#pragma once
// The Scores screen's /teams web page, served by the device (main.cpp) and by the desktop
// simulator's small web server (sim_main.cpp), so both show exactly the same page.
#ifndef PROGMEM
#define PROGMEM
#endif

// The Scores screen's teams, picked by name. The Orb fetches each league's team list from
// ESPN when the page asks (/teamlist), so the browser only ever talks to the Orb.
static const char TEAMS_PAGE[] PROGMEM = R"HTML(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>Orb teams</title><style>
*{box-sizing:border-box}
body{font:16px system-ui;margin:0;padding:22px 18px 60px;background:#101418;color:#e8edf2;max-width:640px;margin:auto}
h1{font-size:20px;margin:0 0 4px}p{color:#98a2ad;margin:4px 0 14px;line-height:1.5}
a{color:#8fc3ff}
.bar{display:flex;flex-wrap:wrap;gap:8px;margin:10px 0 14px;align-items:center}
.btn,select,input{padding:9px 12px;border-radius:10px;background:#22303a;color:#e8edf2;border:1px solid #33414c;font:inherit}
.btn{cursor:pointer}.btn.go{background:#2c5a2a;border-color:#3c7a38}
.btn:disabled{opacity:.45;cursor:default}
select{max-width:100%}
.row{display:flex;align-items:center;gap:10px;padding:8px 12px;border:1px solid #29323b;border-radius:10px;margin-bottom:6px;background:#161c22}
.row img{width:32px;height:32px;object-fit:contain}
.nm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.lg{font-size:12px;color:#7d8893;margin-left:6px}
.mv{background:none;border:0;color:#cfd8e0;cursor:pointer;font-size:18px;padding:2px 6px}
.x{background:none;border:0;color:#ff8a6a;cursor:pointer;font-size:16px;padding:2px 6px}
#prev{width:40px;height:40px;object-fit:contain;display:none}
#msg{margin-top:12px;font:14px ui-monospace,monospace;white-space:pre-wrap;color:#98a2ad}
.ok{color:#7ddb8a}.bad{color:#ff8a6a}
details{margin-top:18px;color:#98a2ad}
</style>
<h1>Teams on the Scores screen</h1>
<p>Pick up to six teams. They show on the Orb in this order. Save, and the Orb fetches their
games straight away. <a href="/">Back</a></p>
<div id=list></div>
<div class=bar>
 <select id=lg>
  <option value="">League...</option>
  <option value="nfl">NFL</option><option value="nhl">NHL</option><option value="nba">NBA</option>
  <option value="mlb">MLB</option><option value="wnba">WNBA</option><option value="mls">MLS</option>
  <option value="epl">Premier League</option><option value="laliga">La Liga</option><option value="bundes">Bundesliga</option>
  <option value="seriea">Serie A</option><option value="ligue1">Ligue 1</option><option value="ucl">Champions League</option>
 </select>
 <select id=tm disabled><option>Team...</option></select>
 <img id=prev alt="">
 <button class=btn id=add disabled>Add</button>
</div>
<div class=bar><button class="btn go" id=save>Save</button></div>
<div id=msg></div>
<h1 style="font-size:16px;margin-top:20px">College teams, or any team by its address</h1>
<p>Open the team on espn.com and paste its address here, for example
<code>https://www.espn.com/college-football/team/_/id/151/east-carolina-pirates</code>.
College football, men's and women's basketball and baseball all work, and so does any pro
team's address. For a football (soccer) club, also pick its league in the list above first:
its address does not say which league it plays in.</p>
<div class=bar><input id=murl style="flex:1;min-width:0" placeholder="https://www.espn.com/.../team/_/..."><button class=btn id=madd>Add</button></div>
<script>
const PATH={nfl:'football/nfl',nhl:'hockey/nhl',nba:'basketball/nba',mlb:'baseball/mlb',wnba:'basketball/wnba',mls:'soccer/usa.1',
  epl:'soccer/eng.1',laliga:'soccer/esp.1',bundes:'soccer/ger.1',seriea:'soccer/ita.1',ligue1:'soccer/fra.1',ucl:'soccer/uefa.champions'};
const MAX=6;
let teams=[], cache={};
const $=id=>document.getElementById(id);
function say(t,c){$('msg').innerHTML='<span class="'+(c||'')+'">'+t+'</span>';}
const COLLEGE=l=>l.startsWith('ncaa');
const SOCCER=l=>(PATH[l]||'').startsWith('soccer/');
// college and soccer teams are saved with ESPN's team number, which is how they are found
const NUMBERED=l=>COLLEGE(l)||SOCCER(l);
const LNAME={nfl:'NFL',nhl:'NHL',nba:'NBA',mlb:'MLB',wnba:'WNBA',mls:'MLS',ncaaf:'NCAA FB',ncaam:'NCAA MBB',ncaaw:'NCAA WBB',ncaabb:'NCAA BASE',
  epl:'EPL',laliga:'LA LIGA',bundes:'BUNDESLIGA',seriea:'SERIE A',ligue1:'LIGUE 1',ucl:'UCL'};
// a team is found by its number when it has one (college), else by its code
function find(list,a,id){return list.find(x=>(id&&x.id)?x.id==id:x.abbr.toLowerCase()==a.toLowerCase());}
function logo(l,a,id){const c=cache[l];if(!c)return '';const t=find(c,a,id);return t?t.logo:'';}
function draw(){
  const L=$('list');L.innerHTML='';
  if(!teams.length)L.innerHTML='<p>No teams yet. Without any, the Orb shows the Hurricanes and the Devils.</p>';
  teams.forEach((t,i)=>{
    const r=document.createElement('div');r.className='row';
    const src=logo(t.league,t.abbr,t.id);
    r.innerHTML=(src?'<img src="'+src+'">':'')+'<span class=nm>'+(t.name||t.abbr.toUpperCase())+'<span class=lg>'+(LNAME[t.league]||t.league.toUpperCase())+' '+t.abbr.toUpperCase()+'</span></span>'+
      '<button class=mv title="Up">&#9650;</button><button class=mv title="Down">&#9660;</button><button class=x title="Remove">&#10005;</button>';
    const b=r.querySelectorAll('button');
    b[0].onclick=()=>{if(i>0){[teams[i-1],teams[i]]=[teams[i],teams[i-1]];draw();}};
    b[1].onclick=()=>{if(i<teams.length-1){[teams[i+1],teams[i]]=[teams[i],teams[i+1]];draw();}};
    b[2].onclick=()=>{teams.splice(i,1);draw();};
    L.appendChild(r);
  });
  $('add').disabled=!$('tm').value||teams.length>=MAX;
}
async function league(l,quiet){
  if(cache[l])return cache[l];
  // the Orb fetches the list in the background: 202 means ask again shortly
  for(let i=0;i<20;i++){
    const r=await fetch('/teamlist?league='+l);
    if(r.status==200){cache[l]=(await r.json()).sort((a,b)=>a.name.localeCompare(b.name));return cache[l];}
    if(r.status!=202)throw new Error('HTTP '+r.status);
    if(i==1&&!quiet)say('Getting the '+l.toUpperCase()+' teams...');
    await new Promise(z=>setTimeout(z,1000));
  }
  throw new Error('the Orb could not reach ESPN');
}
$('lg').onchange=async()=>{
  const l=$('lg').value,s=$('tm');s.disabled=true;s.innerHTML='<option value="">Team...</option>';$('prev').style.display='none';
  if(!l){draw();return;}
  try{const list=await league(l);
    list.forEach((t,k)=>{const o=document.createElement('option');o.value=k;o.textContent=t.name;s.appendChild(o);});
    s.disabled=false;say('');draw();
  }catch(e){say('Could not load the '+l.toUpperCase()+' teams ('+e.message+'). Try again, or use "Team not in the list?" below.','bad');}
};
$('tm').onchange=()=>{
  const l=$('lg').value,t=(cache[l]||[])[$('tm').value],src=t?t.logo:'',p=$('prev');
  if(src){p.src=src;p.style.display='inline';}else p.style.display='none';draw();
};
function push(l,a,name,id){
  a=a.trim().toLowerCase();id=(id||'').trim();
  if(!/^[a-z0-9]{1,7}$/.test(a)){say('A team code is letters and numbers, up to seven.','bad');return;}
  if(id&&!/^[0-9]{1,7}$/.test(id)){say('A team number is digits only.','bad');return;}
  if(NUMBERED(l)&&!id){say('This team needs its ESPN team number: paste its espn.com address.','bad');return;}
  if(teams.length>=MAX){say('Six teams at most.','bad');return;}
  if(teams.some(t=>t.league==l&&(id&&t.id?t.id==id:t.abbr.toLowerCase()==a))){say('Already on the list.','bad');return;}
  teams.push({league:l,abbr:a,id:id,name:name||''});say('');draw();
}
$('add').onclick=()=>{const l=$('lg').value,t=(cache[l]||[])[$('tm').value];if(t)push(l,t.abbr,t.name,NUMBERED(l)?t.id:'');};
// espn.com sport folder -> the Orb's league code
const SPORT={nfl:'nfl',nhl:'nhl',nba:'nba',mlb:'mlb',wnba:'wnba','college-football':'ncaaf',
  'mens-college-basketball':'ncaam','womens-college-basketball':'ncaaw','college-baseball':'ncaabb'};
// ".../college-football/team/_/id/151/east-carolina-pirates" or ".../nfl/team/_/name/dal/dallas-cowboys"
function fromUrl(u){
  const m=u.match(/espn\.com\/([a-z-]+)\/team\/_\/(name|id)\/([a-z0-9]+)(?:\/([a-z0-9-]+))?/i);
  if(!m)return null;
  let l=SPORT[m[1].toLowerCase()];
  // ".../soccer/team/_/id/359/arsenal": the league comes from the list above
  if(m[1].toLowerCase()=='soccer'){l=$('lg').value;if(!SOCCER(l))return {needLeague:true};}
  if(!l)return null;
  const slug=(m[4]||'').split('-').filter(Boolean);
  const name=slug.map(w=>w[0].toUpperCase()+w.slice(1)).join(' ')||('Team '+m[3]);
  if(m[2].toLowerCase()=='name')return {l,a:m[3],id:'',name};
  // college addresses carry only the number: a stand-in code from the name's initials, which
  // the Orb replaces with ESPN's own ("ECU") the first time it reads the team
  const code=(slug.map(w=>w[0]).join('')||'t').slice(0,4);
  return {l,a:code,id:m[3],name};
}
$('madd').onclick=()=>{
  const t=fromUrl($('murl').value.trim());
  if(t&&t.needLeague){say('Pick the club\'s league in the list above first (Premier League, La Liga, ...), then Add again.','bad');return;}
  if(!t){say('That does not look like an espn.com team address (".../team/_/id/151/..." or ".../team/_/name/dal/...").','bad');return;}
  push(t.l,t.a,t.name,t.id);
  if(!$('msg').textContent)$('murl').value='';
};
$('save').onclick=async()=>{
  const body=teams.map(t=>t.league+' '+t.abbr.toLowerCase()+(t.id&&NUMBERED(t.league)?' '+t.id:'')).join('\n')+'\n';
  try{const r=await fetch('/teamsave',{method:'POST',headers:{'Content-Type':'text/plain'},body});
    const t=await r.text();
    if(r.ok)say('Saved. The Orb is fetching the games now.','ok');else say('Not saved: '+t,'bad');
  }catch(e){say('Not saved: '+e,'bad');}
};
(async()=>{
  try{const r=await fetch('/teams.json');teams=await r.json();}catch(e){teams=[];}
  draw();
  // logos and full names for what is already chosen
  for(const l of [...new Set(teams.map(t=>t.league))].filter(l=>!COLLEGE(l))){
    try{const list=await league(l,true);teams.forEach(t=>{if(t.league==l){const m=find(list,t.abbr,t.id);if(m)t.name=m.name;}});}catch(e){}
  }
  draw();
})();
</script>
)HTML";
