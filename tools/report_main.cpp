// report_main.cpp — Interactive HTML report with P&L, multi-model race, and review.
#include "lob/order_book.hpp"
#include "lob/rdtsc.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <string>
#include <vector>
using namespace lob;
namespace {
constexpr size_t kTicks=10000,kPool=200000,kSeed=10000,kWarmup=50000,kOps=300000;
constexpr Price kMid=5000;
struct OpS{std::vector<uint64_t>s;uint64_t p50=0,p99=0,p999=0,mx=0;
void res(size_t n){s.reserve(n);}void rec(uint64_t c){s.push_back(c);}
void fin(const TscCalibration&cal){std::sort(s.begin(),s.end());
auto p=[&](double v)->uint64_t{if(s.empty())return 0;return s[size_t(v/100.0*double(s.size()-1))];};
p50=cal.cycles_to_ns(p(50));p99=cal.cycles_to_ns(p(99));p999=cal.cycles_to_ns(p(99.9));mx=s.empty()?0:cal.cycles_to_ns(s.back());}};
struct Dat{OpS all,sr,sm,sc,smod;std::vector<std::pair<const char*,size_t>>hist;double throughput;size_t total_ops,total_trades;};
Dat collect(const TscCalibration&cal){
Dat d{};d.all.res(kOps);d.sr.res(kOps);d.sm.res(kOps);d.sc.res(kOps);d.smod.res(kOps);
OrderBook book(kTicks,kPool);std::mt19937_64 rng(42);std::uniform_real_distribution<double>ud(0.0,1.0);
std::vector<OrderId>live;live.reserve(kPool);OrderId nid=1;d.total_trades=0;
book.set_trade_sink([&](const Trade&){++d.total_trades;});
for(size_t i=0;i<kSeed;++i){book.submit(nid,Side::Buy,OrderType::Limit,kMid-1-Price(rng()%100),1+rng()%20);live.push_back(nid++);
book.submit(nid,Side::Sell,OrderType::Limit,kMid+1+Price(rng()%100),1+rng()%20);live.push_back(nid++);}
auto t0=std::chrono::steady_clock::now();
for(size_t i=0;i<kWarmup+kOps;++i){double r=ud(rng);bool m=(i>=kWarmup);unsigned c0,c1;uint64_t s,e;
if(r<0.25||live.size()<100){Side si=(rng()&1)?Side::Buy:Side::Sell;Price p=si==Side::Buy?kMid-1-Price(rng()%100):kMid+1+Price(rng()%100);
OrderId id=nid++;s=read_tsc(c0);auto res=book.submit(id,si,OrderType::Limit,p,1+rng()%20);e=read_tsc(c1);if(res.resting>0)live.push_back(id);
if(m&&c0==c1){d.sr.rec(e-s);d.all.rec(e-s);}}
else if(r<0.45){Side si=(rng()&1)?Side::Buy:Side::Sell;OrderId id=nid++;s=read_tsc(c0);book.submit(id,si,OrderType::Market,0,1+rng()%3);e=read_tsc(c1);
if(m&&c0==c1){d.sm.rec(e-s);d.all.rec(e-s);}Side rs=(rng()&1)?Side::Buy:Side::Sell;
book.submit(nid++,rs,OrderType::Limit,rs==Side::Buy?kMid-1-Price(rng()%100):kMid+1+Price(rng()%100),1+rng()%10);}
else if(r<0.80&&!live.empty()){size_t idx=rng()%live.size();OrderId cid=live[idx];s=read_tsc(c0);book.cancel(cid);e=read_tsc(c1);
live[idx]=live.back();live.pop_back();if(m&&c0==c1){d.sc.rec(e-s);d.all.rec(e-s);}}
else if(!live.empty()){size_t idx=rng()%live.size();Price np=(rng()&1)?kMid-1-Price(rng()%100):kMid+1+Price(rng()%100);
s=read_tsc(c0);auto res=book.modify(live[idx],np,1+rng()%20);e=read_tsc(c1);
if(!res.accepted||res.resting==0){live[idx]=live.back();live.pop_back();}if(m&&c0==c1){d.smod.rec(e-s);d.all.rec(e-s);}}}
auto t1=std::chrono::steady_clock::now();auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count();
d.throughput=double(kWarmup+kOps)/(double(ms)/1000.0);d.total_ops=kOps;
d.all.fin(cal);d.sr.fin(cal);d.sm.fin(cal);d.sc.fin(cal);d.smod.fin(cal);
struct B{const char*l;uint64_t lo,hi;size_t c;};
B bins[]={{"0-25",0,25,0},{"25-50",25,50,0},{"50-75",50,75,0},{"75-100",75,100,0},{"100-150",100,150,0},{"150-200",150,200,0},{"200-300",200,300,0},{"300-500",300,500,0},{"500-1k",500,1000,0},{"1-2k",1000,2000,0},{"2-5k",2000,5000,0},{"5k+",5000,UINT64_MAX,0}};
for(auto cy:d.all.s){uint64_t ns=cal.cycles_to_ns(cy);for(auto&b:bins)if(ns>=b.lo&&ns<b.hi){++b.c;break;}}
for(auto&b:bins)d.hist.push_back({b.l,b.c});return d;}
void w(FILE*f,const char*s){std::fputs(s,f);}

void write_report(const char*path,const Dat&d){
FILE*f=std::fopen(path,"w");if(!f)return;
auto t=std::time(nullptr);char date[64];std::strftime(date,sizeof(date),"%Y-%m-%d %H:%M",std::localtime(&t));
std::string hl,hc;
for(size_t i=0;i<d.hist.size();++i){if(i){hl+=",";hc+=",";}hl+="\""+std::string(d.hist[i].first)+"\"";hc+=std::to_string(d.hist[i].second);}

w(f,R"==(<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LOB Engine — Interactive Report</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
:root{--bg:#0d1117;--card:#161b22;--card2:#1c2333;--border:#30363d;--text:#e6edf3;--dim:#8b949e;--green:#3fb950;--red:#f85149;--blue:#58a6ff;--yellow:#d29922;--purple:#bc8cff}
*{margin:0;padding:0;box-sizing:border-box}body{background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}
.wrap{max-width:1440px;margin:0 auto;padding:16px}
header{display:flex;align-items:center;justify-content:space-between;padding:12px 0;border-bottom:1px solid var(--border);margin-bottom:20px}
header h1{font-size:20px;font-weight:600}
.tabs{display:flex;gap:4px;background:var(--card);border-radius:8px;padding:4px;margin-bottom:20px;flex-wrap:wrap}
.tab-btn{padding:7px 16px;border-radius:6px;border:none;background:transparent;color:var(--dim);cursor:pointer;font-size:13px;font-weight:500;transition:all .2s}
.tab-btn:hover{color:var(--text)}.tab-btn.active{background:var(--blue);color:#fff}
.tab-content{display:none}.tab-content.active{display:block}
.metrics{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px;margin-bottom:20px}
.metric{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:14px;text-align:center}
.metric .v{font-size:24px;font-weight:700}.metric .l{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.5px;margin-top:3px}
.g{color:var(--green)}.r{color:var(--red)}.b{color:var(--blue)}.y{color:var(--yellow)}.p{color:var(--purple)}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:14px;margin-bottom:20px}
.grid3{display:grid;grid-template-columns:5fr 3fr;gap:14px;margin-bottom:20px}
.grid4{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin-bottom:20px}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:16px;margin-bottom:14px}
.card h2{font-size:13px;font-weight:600;color:var(--dim);text-transform:uppercase;letter-spacing:.5px;margin-bottom:12px}
table{width:100%;border-collapse:collapse;font-size:12px}
th{text-align:left;color:var(--dim);font-weight:500;padding:5px 8px;border-bottom:1px solid var(--border)}
td{padding:5px 8px;border-bottom:1px solid #21262d}.rt{text-align:right}
.ctrl{display:flex;align-items:center;gap:8px;margin-bottom:14px;flex-wrap:wrap}
.ctrl button{padding:6px 14px;border-radius:6px;border:1px solid var(--border);background:var(--card2);color:var(--text);cursor:pointer;font-size:12px;font-weight:500;transition:all .15s}
.ctrl button:hover{border-color:var(--blue)}.ctrl button.on{background:var(--green);color:#000;border-color:var(--green)}
.ctrl select,.ctrl input[type=range]{background:var(--card2);border:1px solid var(--border);color:var(--text);border-radius:6px;padding:5px 8px;font-size:12px}
.sg{display:flex;flex-direction:column;gap:1px}.sg span{font-size:10px;color:var(--dim)}.sg label{font-size:11px;color:var(--dim)}
.mono{font-family:'SF Mono',Consolas,monospace}
#ladder,.mladder{font-family:'SF Mono',Consolas,monospace;font-size:12px;line-height:1.5}
.lrow{display:flex;align-items:center;height:20px}.lrow:hover{background:#ffffff06}
.bid-bar{height:14px;background:var(--green);border-radius:2px;transition:width .12s;opacity:.7}
.ask-bar{height:14px;background:var(--red);border-radius:2px;transition:width .12s;opacity:.7}
.bid-qty{color:var(--green);width:44px;text-align:right;padding-right:4px;font-size:11px}
.ask-qty{color:var(--red);width:44px;padding-left:4px;font-size:11px}
.price-col{width:50px;text-align:center;color:var(--dim);font-size:11px}.price-col.bb{color:var(--green);font-weight:700}.price-col.ba{color:var(--red);font-weight:700}
.bar-cell{width:80px;display:flex;align-items:center}.bar-cell.bid{justify-content:flex-end}
.tape{font-family:'SF Mono',Consolas,monospace;font-size:11px;max-height:220px;overflow-y:auto}
.trade-row{padding:1px 0;opacity:0;animation:fadeIn .2s forwards}
@keyframes fadeIn{to{opacity:1}}.trade-buy{color:var(--green)}.trade-sell{color:var(--red)}
.live-m{display:grid;grid-template-columns:repeat(auto-fit,minmax(100px,1fr));gap:6px;margin-bottom:10px}
.live-m .lm{background:var(--card2);border-radius:6px;padding:8px;text-align:center}
.live-m .lm .v{font-size:18px;font-weight:700}.live-m .lm .l{font-size:9px;color:var(--dim);text-transform:uppercase}
.model-tag{display:inline-block;padding:2px 8px;border-radius:4px;font-size:10px;font-weight:600}
.pnl-pos{color:var(--green)}.pnl-neg{color:var(--red)}
.sub-tabs{display:flex;gap:4px;margin-bottom:16px}
.sub-tab{padding:6px 14px;border-radius:6px;border:1px solid var(--border);background:transparent;color:var(--dim);cursor:pointer;font-size:12px}
.sub-tab.active{background:var(--purple);color:#fff;border-color:var(--purple)}
.tag{display:inline-block;background:#1f6feb22;color:var(--blue);border:1px solid #1f6feb44;border-radius:20px;padding:2px 10px;font-size:11px;margin:2px}
.arch{display:flex;align-items:center;justify-content:center;gap:0;flex-wrap:wrap;padding:14px 0}
.arch-box{background:#21262d;border:1px solid var(--border);border-radius:8px;padding:8px 16px;text-align:center;font-size:12px;font-weight:600}
.arch-arrow{color:#484f58;font-size:20px;padding:0 5px}
.arch-label{font-size:9px;color:var(--dim);font-weight:400;display:block;margin-top:2px}
.mcard{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:12px;text-align:center}
.mcard h3{font-size:11px;color:var(--dim);margin-bottom:8px;text-transform:uppercase}
.mcard .mv{font-size:16px;font-weight:700;margin:2px 0}
.mcard .ml{font-size:10px;color:var(--dim)}
.info{display:inline-flex;align-items:center;justify-content:center;width:13px;height:13px;border-radius:50%;background:#30363d;color:var(--dim);font-size:9px;font-style:italic;font-family:serif;cursor:help;position:relative;margin-left:3px;vertical-align:middle}
.info:hover .tip{display:block}
.tip{display:none;position:absolute;bottom:20px;left:50%;transform:translateX(-50%);background:#1c2333;border:1px solid var(--border);border-radius:8px;padding:10px 12px;width:230px;font-size:11px;line-height:1.5;color:var(--text);z-index:100;box-shadow:0 4px 16px rgba(0,0,0,.5);pointer-events:none;font-style:normal;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;text-transform:none;letter-spacing:0;font-weight:400}
.tip b{color:var(--blue);display:block;margin-bottom:4px}.tip em{color:var(--dim);font-style:normal;display:block;margin-top:4px;font-size:10px}
.feed-btn{padding:6px 16px;border-radius:20px;border:1px solid var(--border);background:transparent;color:var(--dim);cursor:pointer;font-size:12px;font-weight:600;transition:all .15s}
.feed-btn:hover{border-color:var(--blue);color:var(--text)}.feed-btn.on{background:var(--green);color:#000;border-color:var(--green)}
.st-yes{color:var(--green);font-weight:600}.st-part{color:var(--yellow);font-weight:600}.st-no{color:var(--red);font-weight:600}
.gap-item{display:flex;gap:12px;padding:10px 0;border-bottom:1px solid #21262d;align-items:flex-start}
.gap-item:last-child{border-bottom:none}.gap-icon{font-size:18px;flex-shrink:0;width:24px;text-align:center;padding-top:1px}
.gap-body{flex:1}.gap-body h3{font-size:13px;font-weight:600;margin-bottom:2px}.gap-body p{color:var(--dim);font-size:12px;line-height:1.5;margin:0}
.gap-why{color:var(--yellow);font-size:11px;margin-top:3px;display:block}
.gap-sec{margin-bottom:20px}
.codeblock{background:#0d1117;border:1px solid var(--border);border-radius:8px;overflow-x:auto;overflow-y:auto;max-height:320px;margin:8px 0;font-family:'SF Mono',Consolas,monospace;font-size:11px;line-height:1.6;counter-reset:line}
.codeblock .cl{display:flex;padding:0 12px;min-height:20px}.codeblock .cl:hover{background:#161b22}
.codeblock .ln{color:#484f58;width:32px;text-align:right;padding-right:12px;flex-shrink:0;user-select:none}
.codeblock .cc{flex:1;white-space:pre;color:#e6edf3}
.codeblock .ann{background:#1f6feb15;border-left:3px solid var(--blue)}
.ck{color:#ff7b72}.cv{color:#79c0ff}.cs{color:#a5d6ff}.cf{color:#d2a8ff}.cc2{color:#8b949e}.cn{color:#79c0ff}
.mdl-grid{display:grid;grid-template-columns:1fr 1fr;gap:14px;margin-bottom:14px}
.str-w{display:inline-block;padding:2px 8px;border-radius:4px;font-size:10px;font-weight:600;margin:2px}
.str-pro{background:#3fb95022;color:var(--green)}.str-con{background:#f8514922;color:var(--red)}.str-mid{background:#d2992222;color:var(--yellow)}.gap-sec>h2{font-size:14px;font-weight:600;margin-bottom:10px;display:flex;align-items:center;gap:8px}
.gap-sec>h2 .badge{font-size:10px;padding:2px 8px;border-radius:10px;font-weight:500}
.acard{margin-bottom:10px}.acard h2{cursor:pointer;user-select:none;display:flex;align-items:center;gap:6px}
.acard h2 .arr{transition:transform .2s;display:inline-block}.acard h2 .arr.shut{transform:rotate(-90deg)}
.apanel{overflow:hidden;transition:max-height .3s ease,opacity .2s;max-height:300px;opacity:1}.apanel.shut{max-height:0;opacity:0;padding:0}
@media(max-width:900px){.grid2,.grid3,.grid4{grid-template-columns:1fr}}
</style></head><body><div class="wrap">
)==");

std::fprintf(f,R"==(<header><h1>LOB Matching Engine</h1><span style="color:var(--dim);font-size:12px">%s</span></header>)==",date);

w(f,R"==(<div class="tabs">
<button class="tab-btn active" onclick="showTab('sim')" data-tab="sim">Solo Simulator</button>
<button class="tab-btn" onclick="showTab('multi')" data-tab="multi">Multi Race</button>
<button class="tab-btn" onclick="showTab('perf')" data-tab="perf">Performance Review</button>
<button class="tab-btn" onclick="showTab('ref')" data-tab="ref">C++ Reference</button>
<button class="tab-btn" onclick="showTab('explore')" data-tab="explore">Data Explorer</button>
<button class="tab-btn" onclick="showTab('feeds')" data-tab="feeds">Live Feeds</button>
<button class="tab-btn" onclick="showTab('gap')" data-tab="gap">Production Gap</button>
<button class="tab-btn" onclick="showTab('models')" data-tab="models">Model Deep Dive</button>
</div>)==");

// === SOLO SIMULATOR ===
w(f,R"==(<div class="tab-content active" id="sim">
<div class="ctrl">
<button id="playBtn" onclick="toggleSim()">&#9654; Start</button>
<button onclick="resetSim()">&#8635; Reset</button>
<button onclick="snapshotSolo()" style="border-color:var(--purple);color:var(--purple)">Snapshot</button>
<div class="sg"><label>Model <span class="info">i<span class="tip"><b>Engine implementation</b>Each model uses a different data structure for the order book. Compare them side by side in Multi Race.<em>Optimized = O(1) array + pool + BBO cache. Tree = std::map levels. No Cache = linear BBO scan. Naive = all three penalties combined.</em></span></span></label><select id="modelSel" onchange="switchModel(this.value)">
<option value="optimized">Optimized</option><option value="tree">Tree Levels</option>
<option value="nocache">No BBO Cache</option><option value="naive">Naive</option></select></div>
<div class="sg"><label>Data <span class="info">i<span class="tip"><b>Data source</b>Synthetic generates random orders locally. Crypto options connect to the Binance public API and stream real-time order book depth into the engine.<em>Crypto needs network access. Works best served via HTTP.</em></span></span></label><select id="scenSel" onchange="setDataset(this.value,'solo')">
<option value="steady">Synthetic: Steady</option><option value="sweep">Synthetic: Sweep</option>
<option value="stress">Synthetic: Stress</option>
<option value="crypto:bitcoin">BTC (Bitcoin)</option><option value="crypto:ethereum">ETH (Ethereum)</option>
<option value="crypto:solana">SOL (Solana)</option><option value="crypto:dogecoin">DOGE (Dogecoin)</option>
<option value="crypto:ripple">XRP (Ripple)</option></select></div>
<div class="sg"><label>Speed <span class="info">i<span class="tip"><b>Operations per frame</b>Number of order events processed each animation frame (~60 fps). Controls how fast the book evolves.<em>Ex: 80 = ~4,800 ops/sec</em></span></span></label><input type="range" id="spdS" min="1" max="500" value="80" oninput="document.getElementById('spdV').textContent=this.value"><span id="spdV">80</span></div>
<div class="sg"><label>Aggr% <span class="info">i<span class="tip"><b>Aggression rate</b>Percentage of orders that cross the spread and match immediately. Higher = more fills, tighter book, more trades on the tape.<em>Ex: 20% = 1 in 5 orders is marketable</em></span></span></label><input type="range" id="aggS" min="5" max="60" value="20" oninput="document.getElementById('aggV').textContent=this.value+'%'"><span id="aggV">20%</span></div>
<div class="sg"><label>Cancel% <span class="info">i<span class="tip"><b>Cancel rate</b>Percentage of operations that cancel a resting order instead of submitting a new one. Higher = faster turnover, thinner book depth.<em>Ex: 35% = roughly 1 in 3 ops is a cancel</em></span></span></label><input type="range" id="canS" min="10" max="70" value="35" oninput="document.getElementById('canV').textContent=this.value+'%'"><span id="canV">35%</span></div>
</div>
<div class="live-m">
<div class="lm"><div class="v g" id="sp50">--</div><div class="l">p50</div></div>
<div class="lm"><div class="v y" id="sp99">--</div><div class="l">p99</div></div>
<div class="lm"><div class="v b" id="sthru">--</div><div class="l">ops/s</div></div>
<div class="lm"><div class="v" id="spnl">$0</div><div class="l">Paper P&L</div></div>
<div class="lm"><div class="v" id="svol">0</div><div class="l">Volume</div></div>
</div>
<div class="grid3">
<div class="card"><h2>Book <span id="mTag" class="model-tag" style="background:var(--green);color:#000">Optimized</span></h2><div id="ladder"></div></div>
<div><div class="card"><h2>Trades</h2><div id="tape" class="tape"></div></div>
<div class="card"><h2>Stats</h2><div id="sStats" style="font-size:12px;line-height:1.8"></div></div>
<div class="card"><h2>P&amp;L Over Time</h2><canvas id="soloPnlChart"></canvas></div></div>
</div>
<div style="margin-top:14px">
<div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:10px"><h2 style="font-size:13px;font-weight:600;color:var(--dim);text-transform:uppercase;letter-spacing:.5px;margin:0">Live Analytics</h2>
<button onclick="document.querySelectorAll('.apanel').forEach(p=>p.classList.toggle('shut'));document.querySelectorAll('.arr').forEach(a=>a.classList.toggle('shut'))" style="background:transparent;border:1px solid var(--border);color:var(--dim);border-radius:6px;padding:3px 10px;cursor:pointer;font-size:11px">Toggle All</button></div>
<div class="grid2">
<div class="card acard"><h2 onclick="this.nextElementSibling.classList.toggle('shut');this.querySelector('.arr').classList.toggle('shut')"><span class="arr">&#9662;</span> Mid Price</h2><div class="apanel"><canvas id="aMidC"></canvas></div></div>
<div class="card acard"><h2 onclick="this.nextElementSibling.classList.toggle('shut');this.querySelector('.arr').classList.toggle('shut')"><span class="arr">&#9662;</span> Bid-Ask Spread</h2><div class="apanel"><canvas id="aSpreadC"></canvas></div></div>
<div class="card acard"><h2 onclick="this.nextElementSibling.classList.toggle('shut');this.querySelector('.arr').classList.toggle('shut')"><span class="arr">&#9662;</span> Book Imbalance</h2><div class="apanel"><canvas id="aImbalC"></canvas></div></div>
<div class="card acard"><h2 onclick="this.nextElementSibling.classList.toggle('shut');this.querySelector('.arr').classList.toggle('shut')"><span class="arr">&#9662;</span> Trade Volume</h2><div class="apanel"><canvas id="aVolC"></canvas></div></div>
</div>
<div class="card acard"><h2 onclick="this.nextElementSibling.classList.toggle('shut');this.querySelector('.arr').classList.toggle('shut')"><span class="arr">&#9662;</span> Event Throughput</h2><div class="apanel"><canvas id="aThruC"></canvas></div></div>
</div>
</div>)==");

// === MULTI SIMULATOR ===
w(f,R"==(<div class="tab-content" id="multi">
<div class="ctrl">
<button id="mPlayBtn" onclick="toggleMulti()">&#9654; Start Race</button>
<button onclick="resetMulti()">&#8635; Reset</button>
<button onclick="snapshotMulti()" style="border-color:var(--purple);color:var(--purple)">Snapshot</button>
<div class="sg"><label>Data <span class="info">i<span class="tip"><b>Data source</b>Synthetic runs the same random workload on all 4 models. Crypto streams real Binance depth through each model simultaneously.<em>Compare how each engine handles real market structure.</em></span></span></label><select id="mScenSel" onchange="setMultiData(this.value)">
<option value="steady">Steady</option><option value="sweep">Sweep</option>
<option value="stress">Stress</option><option value="wide">Wide</option>
<option value="crypto:bitcoin">BTC (Bitcoin)</option><option value="crypto:ethereum">ETH (Ethereum)</option>
<option value="crypto:solana">SOL (Solana)</option><option value="crypto:dogecoin">DOGE (Dogecoin)</option>
<option value="crypto:ripple">XRP (Ripple)</option></select></div>
<div class="sg"><label>Speed</label><input type="range" id="mSpdS" min="1" max="300" value="60" oninput="document.getElementById('mSpdV').textContent=this.value"><span id="mSpdV">60</span></div>
</div>
<div id="multiStats"></div>
<div class="grid2" id="multiChartArea">
<div class="card"><h2>p50 Latency</h2><canvas id="mP50Chart"></canvas></div>
<div class="card"><h2>p99 Latency</h2><canvas id="mP99Chart"></canvas></div>
<div class="card"><h2>Throughput (k ops/s)</h2><canvas id="mThruChart"></canvas></div>
<div class="card"><h2>Paper P&amp;L ($)</h2><canvas id="mPnlChart"></canvas></div>
</div></div>)==");

// === PERFORMANCE REVIEW ===
w(f,R"==(<div class="tab-content" id="perf">
<div class="sub-tabs">
<button class="sub-tab active" onclick="showPerfView('solo')" id="perfSoloBtn">Solo Runs</button>
<button class="sub-tab" onclick="showPerfView('multi')" id="perfMultiBtn">Multi Race</button>
<button onclick="clearHistory()" style="margin-left:auto;padding:6px 14px;border-radius:6px;border:1px solid var(--red);background:transparent;color:var(--red);cursor:pointer;font-size:12px">Clear History</button>
</div>
<div id="perfSolo">
<div id="perfSoloEmpty" style="text-align:center;padding:40px;color:var(--dim)"><p>Run the Solo Simulator and click Snapshot.</p></div>
<div id="perfSoloContent" style="display:none">
<div class="metrics" id="psMet"></div>
<div class="grid2"><div class="card"><h2>Latency</h2><canvas id="psHist"></canvas></div>
<div class="card"><h2>By Operation</h2><canvas id="psOps"></canvas></div></div>
<div class="card"><h2>Solo Run History</h2><div id="psHistory"></div></div>
</div></div>
<div id="perfMulti" style="display:none">
<div id="perfMultiEmpty" style="text-align:center;padding:40px;color:var(--dim)"><p>Run the Multi Race and click Snapshot.</p></div>
<div id="perfMultiContent" style="display:none">
<div class="card"><h2>Model Comparison</h2><div id="pmTable"></div></div>
<div class="grid2"><div class="card"><h2>p50 Comparison</h2><canvas id="pmP50"></canvas></div>
<div class="card"><h2>Throughput Comparison</h2><canvas id="pmThru"></canvas></div></div>
<div class="card"><h2>Multi Race History</h2><div id="pmHistory"></div></div>
</div></div>
</div>)==");

// === C++ REF ===
w(f,R"==(<div class="tab-content" id="ref">)==");
std::fprintf(f,R"==(<div class="metrics">
<div class="metric"><div class="v g">%llu ns</div><div class="l">C++ p50</div></div>
<div class="metric"><div class="v y">%llu ns</div><div class="l">C++ p99</div></div>
<div class="metric"><div class="v r">%llu ns</div><div class="l">C++ p99.9</div></div>
<div class="metric"><div class="v b">%.1fM</div><div class="l">C++ ops/s</div></div></div>)==",
(unsigned long long)d.all.p50,(unsigned long long)d.all.p99,(unsigned long long)d.all.p999,d.throughput/1e6);
w(f,R"==(<div class="card"><h2>Architecture</h2><div class="arch">
<div class="arch-box">Market Data<span class="arch-label">in</span></div><div class="arch-arrow">&rarr;</div>
<div class="arch-box" style="border-color:var(--blue)">SPSC Queue<span class="arch-label">lock-free</span></div><div class="arch-arrow">&rarr;</div>
<div class="arch-box" style="border-color:var(--green)">Engine<span class="arch-label">1 thread</span></div><div class="arch-arrow">&rarr;</div>
<div class="arch-box">Trades<span class="arch-label">out</span></div></div>
<div style="text-align:center;margin-top:8px"><span class="tag">Array O(1)</span><span class="tag">Pool alloc</span><span class="tag">Intrusive FIFO</span><span class="tag">rdtscp</span><span class="tag">Int ticks</span></div></div>)==");
std::fprintf(f,R"==(<div class="card"><h2>C++ Latency</h2><canvas id="refH"></canvas></div>)==");
w(f,"</div>");

// === DATA EXPLORER ===
w(f,R"==(<div class="tab-content" id="explore">
<div class="ctrl">
<select id="expSrc" onchange="expSource=this.value"><option value="solo">Solo Simulator</option><option value="multi">Multi Race</option></select>
<button id="expPauseBtn" onclick="expPaused=!expPaused;this.classList.toggle('on',!expPaused);this.textContent=expPaused?'Paused':'Logging'" class="on">Logging</button>
<button onclick="expEvents=[];expRaw=[];expTrades=[]">Clear</button>
<label style="color:var(--dim);font-size:12px"><input type="checkbox" id="expAutoScroll" checked> Auto-scroll</label>
<div class="sg"><label>Filter</label><select id="expFilterSel" onchange="expFilter=this.value"><option value="all">All</option><option value="add">Adds</option><option value="cancel">Cancels</option><option value="fill">Fills</option></select></div>
</div>
<div class="live-m">
<div class="lm"><div class="v g" id="xEps">--</div><div class="l">events/s</div></div>
<div class="lm"><div class="v y" id="xFps">--</div><div class="l">fills/s</div></div>
<div class="lm"><div class="v b" id="xOpen">--</div><div class="l">open orders</div></div>
<div class="lm"><div class="v" id="xQueue">--</div><div class="l">queue depth</div></div>
<div class="lm"><div class="v" id="xConn">--</div><div class="l">connection</div></div>
</div>
<div class="grid3">
<div>
<div class="card" style="min-height:420px"><h2>Event Stream</h2>
<div id="xLog" style="height:370px;overflow-y:auto;font-family:'SF Mono',Consolas,monospace;font-size:11px;line-height:1.6"></div></div>
<div class="card" id="xLiveCard" style="display:none;min-height:200px"><h2>Live Feed <span id="xLiveStatus" style="float:right;font-size:11px"></span></h2>
<div id="xLiveLog" style="height:150px;overflow-y:auto;font-family:'SF Mono',Consolas,monospace;font-size:10px;line-height:1.5;color:var(--dim)"></div></div>
</div>
<div>
<div class="card"><h2>Depth Map</h2><canvas id="xDepthChart" height="180"></canvas></div>
<div class="card" style="min-height:200px"><h2>Trade Log <span id="xTradeCount" style="float:right;color:var(--dim);font-size:11px"></span></h2>
<div id="xTradeLog" style="height:160px;overflow-y:auto;font-family:'SF Mono',Consolas,monospace;font-size:11px;line-height:1.6"></div></div>
</div>
</div>
</div>)==");

// === LIVE FEEDS ===
w(f,R"==(<div class="tab-content" id="feeds">
<div class="ctrl">
<button class="feed-btn" data-pair="BTCUSDT" onclick="toggleFeed(this)">BTC</button>
<button class="feed-btn" data-pair="ETHUSDT" onclick="toggleFeed(this)">ETH</button>
<button class="feed-btn" data-pair="SOLUSDT" onclick="toggleFeed(this)">SOL</button>
<button class="feed-btn" data-pair="DOGEUSDT" onclick="toggleFeed(this)">DOGE</button>
<button class="feed-btn" data-pair="XRPUSDT" onclick="toggleFeed(this)">XRP</button>
<button onclick="connectAllFeeds()" style="margin-left:8px;border-color:var(--green);color:var(--green)">Connect All</button>
<button onclick="stopAllFeeds()" style="border-color:var(--red);color:var(--red)">Stop All</button>
</div>
<div id="feedGrid" style="display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:14px;margin-bottom:14px"></div>
<div class="card" id="feedAgg" style="text-align:center;padding:10px;font-size:12px;color:var(--dim)"></div>
</div>)==");

// === PRODUCTION GAP ===
w(f,R"==(<div class="tab-content" id="gap">
<div style="max-width:960px;margin:0 auto">
<div style="margin-bottom:24px"><h1 style="font-size:20px;font-weight:600;margin-bottom:6px">This Engine vs. Production</h1>
<p style="color:var(--dim);font-size:13px;line-height:1.6;margin:0">An honest assessment of what this matching engine shares with production exchange infrastructure and what a real system requires on top. The core matching logic is production-grade; the surrounding infrastructure is where the gap lives.</p></div>

<div class="gap-sec"><h2><span style="color:var(--green)">&#10003;</span> What This Engine Gets Right <span class="badge" style="background:#3fb95022;color:var(--green)">Shared with production</span></h2>
<div class="card">
<div class="gap-item"><div class="gap-icon st-yes">&#10003;</div><div class="gap-body"><h3>Direct-Indexed O(1) Price Levels</h3><p>Array indexed by tick, not a tree or hash map. O(1) access, contiguous memory, cache-friendly inward walk during matching. This is the data structure choice real exchanges make for single-instrument books.</p></div></div>
<div class="gap-item"><div class="gap-icon st-yes">&#10003;</div><div class="gap-body"><h3>Pool Allocator &mdash; Zero Malloc on Hot Path</h3><p>All order slots pre-allocated. Intrusive free list hands out slots by index in O(1). A general-purpose allocator can lock, page-fault, or fragment &mdash; exactly the tail-latency spikes the design exists to eliminate.</p></div></div>
<div class="gap-item"><div class="gap-icon st-yes">&#10003;</div><div class="gap-body"><h3>Intrusive Doubly-Linked FIFO per Level</h3><p>Links live inside the Order record in the pool. O(1) append (time priority), O(1) pop on fill, O(1) cancel given a handle. No separate list-node allocation.</p></div></div>
<div class="gap-item"><div class="gap-icon st-yes">&#10003;</div><div class="gap-body"><h3>BBO Caching with Inward Reprice</h3><p>Best bid/ask cached and updated on every operation. Only scans inward when the top level empties. Avoids O(n) scan on every query.</p></div></div>
<div class="gap-item"><div class="gap-icon st-yes">&#10003;</div><div class="gap-body"><h3>Price-Time Priority Matching</h3><p>Standard exchange priority: best price first, then FIFO at each level. Partial fills on both sides, multi-level sweeps, limit-price cap, price improvement (executes at resting price).</p></div></div>
<div class="gap-item"><div class="gap-icon st-yes">&#10003;</div><div class="gap-body"><h3>SPSC Lock-Free Ring Buffer</h3><p>Single-producer/single-consumer queue connects ingest to the matching core without a mutex. Acquire/release atomics only &mdash; not even sequential consistency. Power-of-2 capacity for bitwise modular indexing.</p></div></div>
<div class="gap-item"><div class="gap-icon st-yes">&#10003;</div><div class="gap-body"><h3>rdtscp Cycle-Accurate Timing</h3><p>Not std::chrono (can alias to system_clock, non-monotonic). rdtscp returns core ID so cross-core migration samples are detected and discarded. TSC&rarr;ns calibrated once at startup.</p></div></div>
<div class="gap-item"><div class="gap-icon st-yes">&#10003;</div><div class="gap-body"><h3>Integer Tick Prices</h3><p>No floating point. Floats can't exactly represent decimal money and break the equality comparisons matching depends on. Price is a signed tick index; Quantity is unsigned.</p></div></div>
</div></div>)==");

w(f,R"==(<div class="gap-sec"><h2><span style="color:var(--yellow)">&#9673;</span> Partially Addressed <span class="badge" style="background:#d2992222;color:var(--yellow)">Demo-level implementation</span></h2>
<div class="card">
<div class="gap-item"><div class="gap-icon st-part">&#9673;</div><div class="gap-body"><h3>Self-Trade Prevention</h3><p>Cancel-incoming (cancel-aggressor) policy is implemented with OwnerId. Production systems typically support multiple STP policies (cancel-oldest, cancel-both, decrement-and-cancel) configurable per participant.</p><span class="gap-why">Why it matters: Firms use different STP strategies; exchanges must support all of them.</span></div></div>
<div class="gap-item"><div class="gap-icon st-part">&#9673;</div><div class="gap-body"><h3>Order Types</h3><p>Limit, Market, IOC, FOK, and Modify are implemented. Missing: Stop/Stop-Limit, Iceberg/Reserve, Post-Only, Pegged orders, and auction order types (MOO/MOC/LOO/LOC).</p><span class="gap-why">Why it matters: Each order type has specific exchange rules and edge cases that affect matching semantics.</span></div></div>
<div class="gap-item"><div class="gap-icon st-part">&#9673;</div><div class="gap-body"><h3>Benchmarking</h3><p>Per-operation percentile distributions with warmup discarded. But numbers are only meaningful on isolated bare-metal cores (isolcpus, taskset, governor=performance). VM/cloud numbers measure the hypervisor, not the engine.</p><span class="gap-why">Why it matters: The README honestly says "tbd" &mdash; fake numbers are worse than no numbers.</span></div></div>
<div class="gap-item"><div class="gap-icon st-part">&#9673;</div><div class="gap-body"><h3>Reference-Model Cross-Check</h3><p>A deliberately slow std::map/deque book runs alongside the fast one on 100M+ random operations. Compares every observable. Has a shrinker. But this is fuzz testing, not formal verification.</p><span class="gap-why">Why it matters: Exchanges use formal methods and multi-day soak tests; fuzz is a good start, not sufficient.</span></div></div>
</div></div>)==");

w(f,R"==(<div class="gap-sec"><h2><span style="color:var(--red)">&#10007;</span> The Production Gap <span class="badge" style="background:#f8514922;color:var(--red)">Required for a real exchange</span></h2>

<div class="card" style="margin-bottom:12px"><h2 style="font-size:12px">Network &amp; I/O</h2>
<div class="gap-item"><div class="gap-icon st-no">&#10007;</div><div class="gap-body"><h3>Kernel Bypass (DPDK / Solarflare ef_vi / Mellanox VMA)</h3><p>Production systems bypass the OS network stack entirely. The kernel adds 5&ndash;50&micro;s of latency per packet &mdash; often larger than the matching itself. ef_vi/DPDK deliver packets directly to userspace via DMA.</p></div></div>
<div class="gap-item"><div class="gap-icon st-no">&#10007;</div><div class="gap-body"><h3>Hardware Timestamping &amp; PTP Sync</h3><p>NIC-level nanosecond timestamps via PTP (IEEE 1588). Software timestamps have &micro;s-level jitter from kernel scheduling. Regulatory audit trails require NIC timestamps.</p></div></div>
<div class="gap-item"><div class="gap-icon st-no">&#10007;</div><div class="gap-body"><h3>Binary Wire Protocols (FIX/ITCH/OUCH)</h3><p>No text parsing on the hot path. ITCH (market data out) and OUCH (order entry in) are fixed-width binary. FIX is tag-value but exchanges use pre-parsed binary variants (FIXT/SBE).</p></div></div>
</div>

<div class="card" style="margin-bottom:12px"><h2 style="font-size:12px">Persistence &amp; Recovery</h2>
<div class="gap-item"><div class="gap-icon st-no">&#10007;</div><div class="gap-body"><h3>Write-Ahead Log (WAL)</h3><p>Every accepted order/cancel must be durably logged BEFORE the ack is sent. On crash, replay the WAL to reconstruct exact book state. Without this, orders are lost on restart.</p></div></div>
<div class="gap-item"><div class="gap-icon st-no">&#10007;</div><div class="gap-body"><h3>Deterministic Replay &amp; Hot Standby</h3><p>A secondary engine replays the same message stream to maintain identical state. On primary failure, the standby takes over with zero message loss. Requires bit-exact determinism.</p></div></div>
</div>)==");

w(f,R"==(<div class="card" style="margin-bottom:12px"><h2 style="font-size:12px">Risk &amp; Safety</h2>
<div class="gap-item"><div class="gap-icon st-no">&#10007;</div><div class="gap-body"><h3>Pre-Trade Risk Checks</h3><p>Fat-finger protection (price/size reasonability), position limits, credit checks, rate limiting. These run BEFORE matching and must not add more than ~100ns. Often in FPGA.</p></div></div>
<div class="gap-item"><div class="gap-icon st-no">&#10007;</div><div class="gap-body"><h3>Circuit Breakers &amp; Kill Switches</h3><p>Automatic halt when price moves exceed thresholds (LULD bands). Per-firm and market-wide kill switches. Regulatory requirement after the 2010 Flash Crash.</p></div></div>
</div>

<div class="card" style="margin-bottom:12px"><h2 style="font-size:12px">Regulatory &amp; Compliance</h2>
<div class="gap-item"><div class="gap-icon st-no">&#10007;</div><div class="gap-body"><h3>Audit Trail (CAT / MiFID II)</h3><p>Every order lifecycle event timestamped to nanosecond precision, stored immutably, reported to regulators. CAT (US) requires reporting within 8AM next day. MiFID II requires 1&micro;s timestamp granularity.</p></div></div>
<div class="gap-item"><div class="gap-icon st-no">&#10007;</div><div class="gap-body"><h3>Best Execution &amp; Reg NMS</h3><p>Trade-through protection: cannot execute at a price worse than the NBBO (national best bid/offer) across all venues. Requires real-time consolidated feed from all exchanges.</p></div></div>
</div>

<div class="card" style="margin-bottom:12px"><h2 style="font-size:12px">Hardware &amp; OS Tuning</h2>
<div class="gap-item"><div class="gap-icon st-no">&#10007;</div><div class="gap-body"><h3>NUMA-Aware Memory &amp; Huge Pages</h3><p>Allocate all hot data on the same NUMA node as the core. 2MB/1GB huge pages eliminate TLB misses on the pool array. Default 4KB pages cause measurable tail spikes on large books.</p></div></div>
<div class="gap-item"><div class="gap-icon st-no">&#10007;</div><div class="gap-body"><h3>Core Isolation &amp; IRQ Affinity</h3><p>isolcpus + taskset pins the matching thread. IRQ affinity steers all interrupts to other cores. nohz_full disables timer ticks. Without this, the kernel preempts the engine every 4ms.</p></div></div>
</div>

<div class="card"><h2 style="font-size:12px">Multi-Instrument &amp; Scale</h2>
<div class="gap-item"><div class="gap-icon st-no">&#10007;</div><div class="gap-body"><h3>Multiple Order Books</h3><p>Real exchanges run thousands of instruments. Each book may live on a different core. Needs instrument lifecycle (IPO, halt, delist), symbol directory, and cross-book risk.</p></div></div>
<div class="gap-item"><div class="gap-icon st-no">&#10007;</div><div class="gap-body"><h3>Session Management &amp; Client Gateway</h3><p>TCP session handling, authentication, per-client rate limiting, sequence number tracking, gap recovery. The gateway is often the bottleneck, not the matching engine.</p></div></div>
</div>
</div>

<div class="card" style="background:#21262d;text-align:center;padding:20px">
<p style="font-size:13px;color:var(--text);margin:0 0 8px"><b>Bottom line:</b> the matching core &mdash; the data structures, the allocation strategy, the priority logic &mdash; is the same approach used in production. The gap is the infrastructure around it: network I/O, persistence, risk, regulation, and operational tooling.</p>
<p style="font-size:11px;color:var(--dim);margin:0">This is intentional. The core is where the computer science lives. The infrastructure is engineering plumbing that varies by exchange and jurisdiction.</p>
</div>
</div>
</div>)==");

// === MODEL DEEP DIVE ===
w(f,R"==(<div class="tab-content" id="models">
<div style="max-width:1100px;margin:0 auto">
<div style="margin-bottom:20px"><h1 style="font-size:20px;font-weight:600;margin-bottom:6px">Engine Model Deep Dive</h1>
<p style="color:var(--dim);font-size:13px;line-height:1.6;margin:0">Technical analysis of each order book implementation. All four models implement the same interface &mdash; the only difference is the data structure backing price levels, order allocation, or BBO lookup. Annotated lines are highlighted in blue.</p></div>

<div class="card" style="border-top:3px solid #3fb950;margin-bottom:16px">
<h2 style="display:flex;align-items:center;gap:8px"><span style="color:#3fb950">&#9632;</span> Optimized Book <span class="str-w str-pro">BASELINE</span></h2>
<p style="color:var(--dim);font-size:12px;line-height:1.6;margin:0 0 12px">The production-grade implementation. Uses a hash map for O(1) level access, cached BBO, and avoids any tree rebalancing. This is what a real exchange's matching core looks like at the data-structure level.</p>
<div class="mdl-grid">
<div><h3 style="font-size:12px;color:var(--dim);margin-bottom:6px">Strengths</h3>
<span class="str-w str-pro">O(1) submit</span><span class="str-w str-pro">O(1) cancel</span><span class="str-w str-pro">O(1) BBO lookup</span><span class="str-w str-pro">Cache-friendly</span><span class="str-w str-pro">No rebalancing</span></div>
<div><h3 style="font-size:12px;color:var(--dim);margin-bottom:6px">Weaknesses</h3>
<span class="str-w str-mid">Memory &prop; tick range</span><span class="str-w str-mid">Single instrument</span><span class="str-w str-con">BBO reprice O(k) on empty level</span></div></div>
<h3 style="font-size:12px;color:var(--dim);margin:12px 0 6px">Complexity Table</h3>
<table><tr><th>Operation</th><th>Time</th><th>Why</th></tr>
<tr><td>Submit (resting)</td><td class="rt g">O(1)</td><td>Hash lookup + list append</td></tr>
<tr><td>Submit (matching)</td><td class="rt g">O(k)</td><td>k = fills across levels, sequential walk</td></tr>
<tr><td>Cancel</td><td class="rt g">O(1)</td><td>ID → handle index, unlink from doubly-linked list</td></tr>
<tr><td>Best bid/ask</td><td class="rt g">O(1)</td><td>Cached, updated on every mutation</td></tr>
<tr><td>BBO reprice</td><td class="rt y">O(k)</td><td>Inward scan when top level empties (k = empty levels skipped)</td></tr></table>)==");

w(f,R"==(<h3 style="font-size:12px;color:var(--dim);margin:12px 0 6px">Core Implementation</h3>
<div class="codeblock">
<div class="cl ann"><span class="ln">1</span><span class="cc"><span class="ck">class</span> <span class="cf">OptimizedBook</span> {</span></div>
<div class="cl"><span class="ln">2</span><span class="cc">  <span class="ck">constructor</span>() {</span></div>
<div class="cl ann"><span class="ln">3</span><span class="cc">    <span class="ck">this</span>.bids = {};  <span class="cc2">// hash map: price → [orders]  O(1) access</span></span></div>
<div class="cl ann"><span class="ln">4</span><span class="cc">    <span class="ck">this</span>.asks = {};  <span class="cc2">// no tree rebalancing, no O(log n) penalty</span></span></div>
<div class="cl"><span class="ln">5</span><span class="cc">    <span class="ck">this</span>.orders = {}; <span class="cc2">// id → order for O(1) cancel</span></span></div>
<div class="cl ann"><span class="ln">6</span><span class="cc">    <span class="ck">this</span>._bb = <span class="ck">null</span>; <span class="cc2">// cached best bid — never scans</span></span></div>
<div class="cl ann"><span class="ln">7</span><span class="cc">    <span class="ck">this</span>._ba = <span class="ck">null</span>; <span class="cc2">// cached best ask — updated on every op</span></span></div>
<div class="cl"><span class="ln">8</span><span class="cc">  }</span></div>
<div class="cl"><span class="ln">9</span><span class="cc"></span></div>
<div class="cl"><span class="ln">10</span><span class="cc">  <span class="cf">submit</span>(s, p, q) {</span></div>
<div class="cl"><span class="ln">11</span><span class="cc">    <span class="ck">const</span> id = <span class="ck">this</span>.nid++;</span></div>
<div class="cl ann"><span class="ln">12</span><span class="cc">    <span class="cc2">// Match against opposite side — walks inward from BBO</span></span></div>
<div class="cl"><span class="ln">13</span><span class="cc">    <span class="ck">if</span> (s === <span class="cs">'buy'</span>) {</span></div>
<div class="cl ann"><span class="ln">14</span><span class="cc">      <span class="ck">while</span> (rem > <span class="cn">0</span>) {</span></div>
<div class="cl ann"><span class="ln">15</span><span class="cc">        <span class="ck">if</span> (<span class="ck">this</span>._ba === <span class="ck">null</span> || <span class="ck">this</span>._ba > p) <span class="ck">break</span>; <span class="cc2">// O(1) BBO check</span></span></div>
<div class="cl"><span class="ln">16</span><span class="cc">        rem = <span class="ck">this</span>._fill(<span class="ck">this</span>._ba, rem, <span class="cs">'buy'</span>);</span></div>
<div class="cl"><span class="ln">17</span><span class="cc">      }</span></div>
<div class="cl"><span class="ln">18</span><span class="cc">    }</span></div>
<div class="cl ann"><span class="ln">19</span><span class="cc">    <span class="cc2">// Rest remainder — O(1) hash insert + list append</span></span></div>
<div class="cl"><span class="ln">20</span><span class="cc">    <span class="ck">if</span> (rem > <span class="cn">0</span>) {</span></div>
<div class="cl ann"><span class="ln">21</span><span class="cc">      <span class="ck">if</span> (!L[p]) L[p] = []; <span class="cc2">// create level if needed</span></span></div>
<div class="cl"><span class="ln">22</span><span class="cc">      L[p].push({ id, remaining: rem });</span></div>
<div class="cl ann"><span class="ln">23</span><span class="cc">      <span class="ck">if</span> (s===<span class="cs">'buy'</span> &amp;&amp; (bb===<span class="ck">null</span>||p>bb)) <span class="ck">this</span>._bb=p; <span class="cc2">// update BBO cache</span></span></div>
<div class="cl"><span class="ln">24</span><span class="cc">    }</span></div>
<div class="cl"><span class="ln">25</span><span class="cc">  }</span></div>
<div class="cl"><span class="ln">26</span><span class="cc">}</span></div>
</div></div>)==");

w(f,R"==(<div class="card" style="border-top:3px solid #58a6ff;margin-bottom:16px">
<h2 style="display:flex;align-items:center;gap:8px"><span style="color:#58a6ff">&#9632;</span> Tree Book <span class="str-w str-mid">ABLATION: LEVELS</span></h2>
<p style="color:var(--dim);font-size:12px;line-height:1.6;margin:0 0 12px">Replaces the hash map with a sorted array (binary-search tree simulation). Levels are kept in sorted order so BBO is always the last bid / first ask element. The cost: every insert requires finding the correct position via binary search &mdash; O(log n) instead of O(1).</p>
<div class="mdl-grid">
<div><h3 style="font-size:12px;color:var(--dim);margin-bottom:6px">Strengths</h3>
<span class="str-w str-pro">Sorted iteration</span><span class="str-w str-pro">BBO is O(1) — end of array</span><span class="str-w str-pro">Memory &prop; active levels only</span></div>
<div><h3 style="font-size:12px;color:var(--dim);margin-bottom:6px">Weaknesses</h3>
<span class="str-w str-con">O(log n) insert — binary search</span><span class="str-w str-con">O(n) splice on insert/delete</span><span class="str-w str-con">Cache-unfriendly for wide books</span></div></div>
<h3 style="font-size:12px;color:var(--dim);margin:12px 0 6px">Complexity Table</h3>
<table><tr><th>Operation</th><th>Time</th><th>Why</th></tr>
<tr><td>Submit (resting)</td><td class="rt y">O(log n)</td><td>Binary search to find insertion point, then splice</td></tr>
<tr><td>Cancel</td><td class="rt g">O(1)</td><td>ID lookup + array splice at known index</td></tr>
<tr><td>Best bid/ask</td><td class="rt g">O(1)</td><td>Last/first element of sorted array</td></tr></table>
<h3 style="font-size:12px;color:var(--dim);margin:12px 0 6px">Key Difference</h3>
<div class="codeblock">
<div class="cl"><span class="ln">1</span><span class="cc"><span class="cc2">// Binary search to find correct sorted position</span></span></div>
<div class="cl ann"><span class="ln">2</span><span class="cc"><span class="cf">_fi</span>(L, p) {</span></div>
<div class="cl ann"><span class="ln">3</span><span class="cc">  <span class="ck">let</span> lo=<span class="cn">0</span>, hi=L.length;</span></div>
<div class="cl ann"><span class="ln">4</span><span class="cc">  <span class="ck">while</span> (lo &lt; hi) {            <span class="cc2">// O(log n) — this is the cost</span></span></div>
<div class="cl ann"><span class="ln">5</span><span class="cc">    <span class="ck">const</span> m = (lo+hi) >> <span class="cn">1</span>;   <span class="cc2">// vs O(1) hash lookup in Optimized</span></span></div>
<div class="cl"><span class="ln">6</span><span class="cc">    <span class="ck">if</span> (L[m].price &lt; p) lo=m+<span class="cn">1</span>; <span class="ck">else</span> hi=m;</span></div>
<div class="cl"><span class="ln">7</span><span class="cc">  }</span></div>
<div class="cl"><span class="ln">8</span><span class="cc">  <span class="ck">return</span> lo;</span></div>
<div class="cl"><span class="ln">9</span><span class="cc">}</span></div>
<div class="cl"><span class="ln">10</span><span class="cc"></span></div>
<div class="cl ann"><span class="ln">11</span><span class="cc"><span class="cc2">// Insert requires splice — shifts all elements right: O(n)</span></span></div>
<div class="cl ann"><span class="ln">12</span><span class="cc">L.<span class="cf">splice</span>(i, <span class="cn">0</span>, lv);  <span class="cc2">// this is where Tree pays the penalty</span></span></div>
</div></div>)==");

w(f,R"==(<div class="card" style="border-top:3px solid #d29922;margin-bottom:16px">
<h2 style="display:flex;align-items:center;gap:8px"><span style="color:#d29922">&#9632;</span> No Cache Book <span class="str-w str-mid">ABLATION: BBO</span></h2>
<p style="color:var(--dim);font-size:12px;line-height:1.6;margin:0 0 12px">Same hash-map levels as Optimized, but removes the BBO cache. Every best_bid/best_ask call scans ALL keys to find the max/min. This isolates the exact cost of the BBO caching optimization.</p>
<div class="mdl-grid">
<div><h3 style="font-size:12px;color:var(--dim);margin-bottom:6px">Strengths</h3>
<span class="str-w str-pro">Simple — no cache invalidation logic</span><span class="str-w str-pro">Always correct — no stale BBO bugs</span></div>
<div><h3 style="font-size:12px;color:var(--dim);margin-bottom:6px">Weaknesses</h3>
<span class="str-w str-con">O(n) BBO lookup — scans every active level</span><span class="str-w str-con">Called on EVERY match check</span><span class="str-w str-con">Dominates latency at scale</span></div></div>
<h3 style="font-size:12px;color:var(--dim);margin:12px 0 6px">Complexity Table</h3>
<table><tr><th>Operation</th><th>Time</th><th>Why</th></tr>
<tr><td>Submit (resting)</td><td class="rt g">O(1)</td><td>Same hash map as Optimized</td></tr>
<tr><td>Best bid/ask</td><td class="rt r">O(n)</td><td>Math.max/min over all keys every call</td></tr>
<tr><td>Submit (matching)</td><td class="rt r">O(k &times; n)</td><td>Each fill step calls bestAsk() which is O(n)</td></tr></table>
<h3 style="font-size:12px;color:var(--dim);margin:12px 0 6px">The Expensive Path</h3>
<div class="codeblock">
<div class="cl"><span class="ln">1</span><span class="cc"><span class="cc2">// Every call scans ALL active price levels</span></span></div>
<div class="cl ann"><span class="ln">2</span><span class="cc"><span class="cf">bestBid</span>() {</span></div>
<div class="cl ann"><span class="ln">3</span><span class="cc">  <span class="ck">const</span> k = Object.keys(<span class="ck">this</span>.bids).map(Number);</span></div>
<div class="cl ann"><span class="ln">4</span><span class="cc">  <span class="ck">return</span> k.length ? Math.<span class="cf">max</span>(...k) : <span class="ck">null</span>; <span class="cc2">// O(n) — spreads ALL keys</span></span></div>
<div class="cl"><span class="ln">5</span><span class="cc">}</span></div>
<div class="cl"><span class="ln">6</span><span class="cc"></span></div>
<div class="cl"><span class="ln">7</span><span class="cc"><span class="cc2">// Compare to Optimized:</span></span></div>
<div class="cl ann"><span class="ln">8</span><span class="cc"><span class="cf">bestBid</span>() { <span class="ck">return this</span>._bb; } <span class="cc2">// O(1) — just return the cached value</span></span></div>
</div></div>)==");

w(f,R"==(<div class="card" style="border-top:3px solid #f85149;margin-bottom:16px">
<h2 style="display:flex;align-items:center;gap:8px"><span style="color:#f85149">&#9632;</span> Naive Book <span class="str-w str-con">ALL PENALTIES</span></h2>
<p style="color:var(--dim);font-size:12px;line-height:1.6;margin:0 0 12px">Combines the Tree Book's O(log n) level lookup with the No Cache Book's O(n) BBO scan, plus a linear cancel search. This represents the "textbook" implementation you'd write if you weren't thinking about latency &mdash; correct but slow.</p>
<div class="mdl-grid">
<div><h3 style="font-size:12px;color:var(--dim);margin-bottom:6px">Strengths</h3>
<span class="str-w str-pro">Easy to understand</span><span class="str-w str-pro">Easy to verify correct</span></div>
<div><h3 style="font-size:12px;color:var(--dim);margin-bottom:6px">Weaknesses</h3>
<span class="str-w str-con">O(log n) insert</span><span class="str-w str-con">O(n) BBO scan</span><span class="str-w str-con">O(n) cancel — linear search</span><span class="str-w str-con">Every optimization missing</span></div></div>
<h3 style="font-size:12px;color:var(--dim);margin:12px 0 6px">Complexity Table</h3>
<table><tr><th>Operation</th><th>Time</th><th>Why</th></tr>
<tr><td>Submit (resting)</td><td class="rt y">O(log n)</td><td>Inherited from Tree: binary search + splice</td></tr>
<tr><td>Best bid/ask</td><td class="rt r">O(n)</td><td>Linear scan through all levels</td></tr>
<tr><td>Cancel</td><td class="rt r">O(n)</td><td>Linear search through all levels to find the order</td></tr>
<tr><td>Submit (matching)</td><td class="rt r">O(k &times; n)</td><td>Each fill = BBO scan + level search</td></tr></table>
<h3 style="font-size:12px;color:var(--dim);margin:12px 0 6px">The Linear Cancel</h3>
<div class="codeblock">
<div class="cl"><span class="ln">1</span><span class="cc"><span class="cc2">// Naive cancel: search EVERY level for the order</span></span></div>
<div class="cl ann"><span class="ln">2</span><span class="cc"><span class="cf">cancel</span>(id) {</span></div>
<div class="cl"><span class="ln">3</span><span class="cc">  <span class="ck">const</span> o = <span class="ck">this</span>.orders[id];</span></div>
<div class="cl ann"><span class="ln">4</span><span class="cc">  <span class="ck">for</span> (<span class="ck">const</span> lv <span class="ck">of</span> L) {       <span class="cc2">// walk ALL levels — O(n)</span></span></div>
<div class="cl ann"><span class="ln">5</span><span class="cc">    <span class="ck">const</span> i = lv.orders.<span class="cf">findIndex</span>(x => x.id===id); <span class="cc2">// O(m) per level</span></span></div>
<div class="cl"><span class="ln">6</span><span class="cc">    <span class="ck">if</span> (i >= <span class="cn">0</span>) {</span></div>
<div class="cl"><span class="ln">7</span><span class="cc">      lv.orders.<span class="cf">splice</span>(i, <span class="cn">1</span>);</span></div>
<div class="cl"><span class="ln">8</span><span class="cc">      <span class="ck">break</span>;</span></div>
<div class="cl"><span class="ln">9</span><span class="cc">    }</span></div>
<div class="cl"><span class="ln">10</span><span class="cc">  }</span></div>
<div class="cl"><span class="ln">11</span><span class="cc">}</span></div>
<div class="cl"><span class="ln">12</span><span class="cc"></span></div>
<div class="cl"><span class="ln">13</span><span class="cc"><span class="cc2">// Compare to Optimized:</span></span></div>
<div class="cl ann"><span class="ln">14</span><span class="cc"><span class="cf">cancel</span>(id) {</span></div>
<div class="cl ann"><span class="ln">15</span><span class="cc">  <span class="ck">const</span> o = <span class="ck">this</span>.orders[id]; <span class="cc2">// O(1) hash lookup</span></span></div>
<div class="cl ann"><span class="ln">16</span><span class="cc">  lv.<span class="cf">splice</span>(i, <span class="cn">1</span>);          <span class="cc2">// known index — no search</span></span></div>
<div class="cl"><span class="ln">17</span><span class="cc">}</span></div>
</div></div>)==");

w(f,R"==(<div class="card" style="background:#21262d;margin-bottom:16px">
<h2 style="font-size:14px;margin-bottom:12px">Side-by-Side Comparison</h2>
<table style="font-size:12px">
<tr><th></th><th style="color:#3fb950">Optimized</th><th style="color:#58a6ff">Tree</th><th style="color:#d29922">No Cache</th><th style="color:#f85149">Naive</th></tr>
<tr><td>Level access</td><td class="rt g">O(1) hash</td><td class="rt y">O(log n) bsearch</td><td class="rt g">O(1) hash</td><td class="rt y">O(log n) bsearch</td></tr>
<tr><td>BBO lookup</td><td class="rt g">O(1) cached</td><td class="rt g">O(1) array end</td><td class="rt r">O(n) scan keys</td><td class="rt r">O(n) scan levels</td></tr>
<tr><td>Cancel</td><td class="rt g">O(1) indexed</td><td class="rt g">O(1) indexed</td><td class="rt g">O(1) indexed</td><td class="rt r">O(n) linear search</td></tr>
<tr><td>Memory</td><td class="rt">prop; tick range</td><td class="rt g">prop; active levels</td><td class="rt">prop; tick range</td><td class="rt g">prop; active levels</td></tr>
<tr><td>Cache behavior</td><td class="rt g">Sequential scan</td><td class="rt y">Pointer chasing</td><td class="rt g">Sequential scan</td><td class="rt y">Pointer chasing</td></tr>
<tr><td>Ablation variable</td><td class="rt">&mdash;</td><td class="rt">Level structure</td><td class="rt">BBO strategy</td><td class="rt">All three</td></tr>
</table></div>

<div class="card" style="text-align:center;padding:20px;background:#21262d">
<p style="font-size:13px;margin:0 0 8px"><b>The ablation insight:</b> each model changes exactly ONE variable from the baseline. Run them on the same data in Multi Race to see the isolated cost of each design choice.</p>
<p style="font-size:11px;color:var(--dim);margin:0">Tree measures the price of O(log n) level access. No Cache measures the price of O(n) BBO scanning. Naive measures the combined penalty. The difference from Optimized is the value of each optimization.</p>
</div>
</div>
</div>)==");

// ======= JS =======
std::fprintf(f,R"==(<script>
Chart.defaults.color='#8b949e';Chart.defaults.borderColor='#21262d';
new Chart(document.getElementById('refH'),{type:'bar',data:{labels:[%s],datasets:[{data:[%s],backgroundColor:'#1f6feb',borderRadius:3}]},options:{responsive:true,plugins:{legend:{display:false}},scales:{y:{grid:{color:'#21262d'}},x:{grid:{display:false}}}}});
)==",hl.c_str(),hc.c_str());

w(f,R"==(
function showTab(id){document.querySelectorAll('.tab-content').forEach(e=>e.classList.remove('active'));
document.querySelectorAll('.tab-btn').forEach(e=>e.classList.remove('active'));
document.getElementById(id).classList.add('active');document.querySelector('[data-tab="'+id+'"]').classList.add('active');}
function showPerfView(v){document.getElementById('perfSolo').style.display=v==='solo'?'block':'none';
document.getElementById('perfMulti').style.display=v==='multi'?'block':'none';
document.getElementById('perfSoloBtn').classList.toggle('active',v==='solo');
document.getElementById('perfMultiBtn').classList.toggle('active',v==='multi');}

// ========= ENGINE MODELS =========
class OptimizedBook{
constructor(){this.bids={};this.asks={};this.orders={};this.nid=1;this.trades=[];this.mc=0;this._bb=null;this._ba=null;}
submit(s,p,q){const id=this.nid++;let rem=q;
if(s==='buy'){while(rem>0){if(this._ba===null||this._ba>p)break;rem=this._fill(this._ba,rem,'buy');}}
else{while(rem>0){if(this._bb===null||this._bb<p)break;rem=this._fill(this._bb,rem,'sell');}}
if(rem>0){const L=s==='buy'?this.bids:this.asks;if(!L[p])L[p]=[];L[p].push({id,remaining:rem});this.orders[id]={side:s,price:p,remaining:rem};
if(s==='buy'&&(this._bb===null||p>this._bb))this._bb=p;if(s==='sell'&&(this._ba===null||p<this._ba))this._ba=p;}return id;}
cancel(id){const o=this.orders[id];if(!o)return;const L=o.side==='buy'?this.bids:this.asks;const lv=L[o.price];
if(lv){const i=lv.findIndex(x=>x.id===id);if(i>=0){lv.splice(i,1);if(!lv.length){delete L[o.price];this._rp(o.side);}}}delete this.orders[id];}
bestBid(){return this._bb;}bestAsk(){return this._ba;}
sizeAt(s,p){const lv=(s==='buy'?this.bids:this.asks)[p];return lv?lv.reduce((a,o)=>a+o.remaining,0):0;}
orderCount(){return Object.keys(this.orders).length;}
_rp(side){if(side==='buy'){const k=Object.keys(this.bids).map(Number);this._bb=k.length?Math.max(...k):null;}
else{const k=Object.keys(this.asks).map(Number);this._ba=k.length?Math.min(...k):null;}}
_fill(price,rem,agg){const L=agg==='buy'?this.asks:this.bids;const lv=L[price];if(!lv||!lv.length)return rem;
while(rem>0&&lv.length>0){const r=lv[0];const t=Math.min(rem,r.remaining);rem-=t;r.remaining-=t;
this.trades.push({price,qty:t,side:agg});this.mc++;if(r.remaining===0){delete this.orders[r.id];lv.shift();}}
if(!lv.length){delete L[price];if(agg==='buy')this._rp('sell');else this._rp('buy');}return rem;}}

class TreeBook{
constructor(){this.bl=[];this.al=[];this.orders={};this.nid=1;this.trades=[];this.mc=0;}
_fi(L,p){let lo=0,hi=L.length;while(lo<hi){const m=(lo+hi)>>1;if(L[m].price<p)lo=m+1;else hi=m;}return lo;}
_goc(L,p){const i=this._fi(L,p);if(i<L.length&&L[i].price===p)return L[i];const lv={price:p,orders:[]};L.splice(i,0,lv);return lv;}
_gl(L,p){const i=this._fi(L,p);return(i<L.length&&L[i].price===p)?L[i]:null;}
submit(s,p,q){const id=this.nid++;let rem=q;
if(s==='buy'){while(rem>0){const ba=this.bestAsk();if(ba===null||ba>p)break;rem=this._fill(this.al,ba,rem,'buy');}}
else{while(rem>0){const bb=this.bestBid();if(bb===null||bb<p)break;rem=this._fill(this.bl,bb,rem,'sell');}}
if(rem>0){const lv=this._goc(s==='buy'?this.bl:this.al,p);lv.orders.push({id,remaining:rem});this.orders[id]={side:s,price:p,remaining:rem};}return id;}
cancel(id){const o=this.orders[id];if(!o)return;const L=o.side==='buy'?this.bl:this.al;const lv=this._gl(L,o.price);
if(lv){const i=lv.orders.findIndex(x=>x.id===id);if(i>=0){lv.orders.splice(i,1);if(!lv.orders.length){const li=this._fi(L,o.price);L.splice(li,1);}}}delete this.orders[id];}
bestBid(){return this.bl.length?this.bl[this.bl.length-1].price:null;}
bestAsk(){return this.al.length?this.al[0].price:null;}
sizeAt(s,p){const lv=this._gl(s==='buy'?this.bl:this.al,p);return lv?lv.orders.reduce((a,o)=>a+o.remaining,0):0;}
orderCount(){return Object.keys(this.orders).length;}
_fill(L,price,rem,agg){const lv=this._gl(L,price);if(!lv||!lv.orders.length)return rem;
while(rem>0&&lv.orders.length>0){const r=lv.orders[0];const t=Math.min(rem,r.remaining);rem-=t;r.remaining-=t;
this.trades.push({price,qty:t,side:agg});this.mc++;if(r.remaining===0){delete this.orders[r.id];lv.orders.shift();}}
if(!lv.orders.length){const i=this._fi(L,price);L.splice(i,1);}return rem;}}

class NoCacheBook{
constructor(){this.bids={};this.asks={};this.orders={};this.nid=1;this.trades=[];this.mc=0;}
submit(s,p,q){const id=this.nid++;let rem=q;
if(s==='buy'){while(rem>0){const ba=this.bestAsk();if(ba===null||ba>p)break;rem=this._fill(ba,rem,'buy');}}
else{while(rem>0){const bb=this.bestBid();if(bb===null||bb<p)break;rem=this._fill(bb,rem,'sell');}}
if(rem>0){const L=s==='buy'?this.bids:this.asks;if(!L[p])L[p]=[];L[p].push({id,remaining:rem});this.orders[id]={side:s,price:p,remaining:rem};}return id;}
cancel(id){const o=this.orders[id];if(!o)return;const L=o.side==='buy'?this.bids:this.asks;const lv=L[o.price];
if(lv){const i=lv.findIndex(x=>x.id===id);if(i>=0){lv.splice(i,1);if(!lv.length)delete L[o.price];}}delete this.orders[id];}
bestBid(){const k=Object.keys(this.bids).map(Number);return k.length?Math.max(...k):null;}
bestAsk(){const k=Object.keys(this.asks).map(Number);return k.length?Math.min(...k):null;}
sizeAt(s,p){const lv=(s==='buy'?this.bids:this.asks)[p];return lv?lv.reduce((a,o)=>a+o.remaining,0):0;}
orderCount(){return Object.keys(this.orders).length;}
_fill(price,rem,agg){const L=agg==='buy'?this.asks:this.bids;const lv=L[price];if(!lv||!lv.length)return rem;
while(rem>0&&lv.length>0){const r=lv[0];const t=Math.min(rem,r.remaining);rem-=t;r.remaining-=t;
this.trades.push({price,qty:t,side:agg});this.mc++;if(r.remaining===0){delete this.orders[r.id];lv.shift();}}
if(!lv.length)delete L[price];return rem;}}

class NaiveBook extends TreeBook{
bestBid(){for(let i=this.bl.length-1;i>=0;i--)if(this.bl[i].orders.length)return this.bl[i].price;return null;}
bestAsk(){for(let i=0;i<this.al.length;i++)if(this.al[i].orders.length)return this.al[i].price;return null;}
cancel(id){const o=this.orders[id];if(!o)return;const L=o.side==='buy'?this.bl:this.al;
for(const lv of L){const i=lv.orders.findIndex(x=>x.id===id);if(i>=0){lv.orders.splice(i,1);if(!lv.orders.length){const li=L.indexOf(lv);L.splice(li,1);}break;}}delete this.orders[id];}}

const MODELS={optimized:OptimizedBook,tree:TreeBook,nocache:NoCacheBook,naive:NaiveBook};
const MC={optimized:'#3fb950',tree:'#58a6ff',nocache:'#d29922',naive:'#f85149'};
const MN={optimized:'Optimized',tree:'Tree',nocache:'No Cache',naive:'Naive'};
const MK=Object.keys(MODELS);

// ========= P&L TRACKER =========
class PnL{constructor(){this.cash=0;this.pos=0;this.vol=0;this.trades=0;}
onTrade(p,q,aggSide){if(aggSide==='buy'){this.cash+=p*q;this.pos-=q;}else{this.cash-=p*q;this.pos+=q;}this.vol+=q;this.trades++;}
total(mid){return this.cash+this.pos*(mid||1000);}
fmt(mid){const v=this.total(mid);return(v>=0?'+':'')+v.toLocaleString(undefined,{maximumFractionDigits:0});}}

// ========= PERF TRACKER =========
class PT{constructor(){this.s=[];this.ops={add:[],cancel:[],match:[]};this.t0=performance.now();this.n=0;}
rec(us,type){this.s.push(us);if(this.s.length>20000)this.s=this.s.slice(-10000);if(type&&this.ops[type]){this.ops[type].push(us);if(this.ops[type].length>10000)this.ops[type]=this.ops[type].slice(-5000);}this.n++;}
pct(p,a){const arr=a||this.s;if(!arr.length)return 0;const sorted=[...arr].sort((a,b)=>a-b);return sorted[Math.floor(p/100*(sorted.length-1))];}
thru(){const e=(performance.now()-this.t0)/1000;return e>0?this.n/e:0;}
hist(){const b=[0,5,10,20,50,100,200,500,1000,2000,5000];const c=new Array(b.length).fill(0);
for(const s of this.s){for(let i=0;i<b.length-1;i++){if(s>=b[i]&&s<b[i+1]){c[i]++;break;}}if(s>=b[b.length-1])c[b.length-1]++;}
return{labels:b.map((v,i)=>i<b.length-1?v+'-'+b[i+1]:v+'+'),counts:c};}
snap(model,scen,pnl,mid){return{model,scenario:scen,ops:this.n,p50:this.pct(50),p99:this.pct(99),p999:this.pct(99.9),
max:this.s.length?Math.max(...this.s):0,thru:this.thru(),addP50:this.pct(50,this.ops.add),cancelP50:this.pct(50,this.ops.cancel),
matchP50:this.pct(50,this.ops.match),hist:this.hist(),time:new Date().toLocaleTimeString(),pnl:pnl?pnl.total(mid):0,vol:pnl?pnl.vol:0};}}

// ========= CRYPTO FEED (WebSocket only — tries Binance, falls back to Coinbase) =========
class CryptoFeed{constructor(){this.ws=null;this.updates=[];this.basePrice=0;this.tickSize=0.01;this.qtyScale=1;this.connected=false;this.error=null;this.pair='';this.realMid=0;this.initialized=false;this.prev={};this.provider='';}
connect(pair){this.stop();this.updates=[];this.error=null;this.pair=pair;this.initialized=false;this.prev={};this.provider='';
return this._tryBinance(pair).catch(()=>this._tryCoinbase(pair));}
_cal(bids,asks){const bB=parseFloat(bids[0][0]),bA=parseFloat(asks[0][0]);
this.basePrice=(bB+bA)/2;this.realMid=this.basePrice;
const wB=parseFloat(bids[bids.length-1][0]),wA=parseFloat(asks[asks.length-1][0]);
const range=wA-wB;this.tickSize=range>0?range/200:0.01;
const qs=[...bids,...asks].map(x=>parseFloat(x[1])).sort((a,b)=>a-b);
const med=qs[Math.floor(qs.length/2)];this.qtyScale=med>0?20/med:1;this.initialized=true;}
_snap(bids,asks){const cur={};let chg=0;
for(const[p,q]of bids){const t=this.toTick(parseFloat(p)),v=this.normQ(parseFloat(q));if(!isNaN(t))cur['b:'+t]=v;}
for(const[p,q]of asks){const t=this.toTick(parseFloat(p)),v=this.normQ(parseFloat(q));if(!isNaN(t))cur['a:'+t]=v;}
for(const k in this.prev){if(!(k in cur)){const[sd,tk]=k.split(':');this.updates.push({side:sd==='b'?'buy':'sell',tick:+tk,qty:0});chg++;}}
for(const k in cur){if(cur[k]!==(this.prev[k]||0)){const[sd,tk]=k.split(':');this.updates.push({side:sd==='b'?'buy':'sell',tick:+tk,qty:cur[k]});chg++;}}
this.prev=cur;if(chg>0){let s='';if(bids[0])s=' | $'+bids[0][0]+' → tick '+this.toTick(parseFloat(bids[0][0]));logExpRaw(this.provider+': '+chg+' changes'+s);}}
_diff(changes){let chg=0;for(const[side,p,q]of changes){const sd=side==='buy'?'b':'a';
const t=this.toTick(parseFloat(p)),v=this.normQ(parseFloat(q));if(isNaN(t))continue;
const key=sd+':'+t;if(v===0)delete this.prev[key];else this.prev[key]=v;
this.updates.push({side:side,tick:t,qty:v});chg++;}
if(chg>0)logExpRaw(this.provider+': '+chg+' diffs');}
_tryBinance(pair){return new Promise((resolve,reject)=>{
const tm=setTimeout(()=>{this.stop();reject(new Error('timeout'));},4000);
this.ws=new WebSocket('wss://stream.binance.com:9443/ws/'+pair.toLowerCase()+'@depth20@100ms');
this.ws.onopen=()=>{this.connected=true;};this.ws.onclose=()=>{this.connected=false;};
this.ws.onerror=()=>{clearTimeout(tm);this.stop();reject(new Error('Binance blocked'));};
this.ws.onmessage=e=>{const d=JSON.parse(e.data);if(!d.bids||!d.bids.length)return;
if(!this.initialized){this._cal(d.bids,d.asks);this.provider='Binance';clearTimeout(tm);resolve();}
this._snap(d.bids,d.asks);};});}
_tryCoinbase(pair){const cb=pair.replace(/USDT$/,'').toUpperCase()+'-USD';
return new Promise((resolve,reject)=>{
const tm=setTimeout(()=>{this.stop();reject(new Error('Coinbase timeout'));},8000);
this.ws=new WebSocket('wss://ws-feed.exchange.coinbase.com');
this.ws.onopen=()=>{this.connected=true;this.ws.send(JSON.stringify({type:'subscribe',product_ids:[cb],channels:['level2_batch']}));};
this.ws.onclose=()=>{this.connected=false;};
this.ws.onerror=()=>{clearTimeout(tm);this.stop();reject(new Error('Coinbase failed'));};
this.ws.onmessage=e=>{const m=JSON.parse(e.data);
if(m.type==='snapshot'&&m.bids&&m.bids.length){if(!this.initialized){this._cal(m.bids,m.asks);this.provider='Coinbase';clearTimeout(tm);resolve();}this._snap(m.bids,m.asks);}
else if(m.type==='l2update'&&this.initialized&&m.changes){this._diff(m.changes);}};});}
toTick(p){return Math.round((p-this.basePrice)/this.tickSize)+1000;}
toPrice(t){return this.basePrice+(t-1000)*this.tickSize;}
normQ(q){const v=Math.round(q*this.qtyScale);return q>0?Math.max(1,v):0;}
drain(n){return this.updates.splice(0,n);}
stop(){if(this.ws){this.ws.close();this.ws=null;}this.connected=false;this.initialized=false;}}

)=="); w(f,R"==(
// ========= DATA EXPLORER LOG =========
let expEvents=[],expRaw=[],expTrades=[],expPaused=false,expFilter='all',expSource='solo';
let expEpsCount=0,expFpsCount=0,expEpsLast=performance.now(),expDepthChart=null;
function logExp(type,detail){if(expPaused)return;expEvents.push({type,detail,t:performance.now()});if(expEvents.length>5000)expEvents=expEvents.slice(-3000);expEpsCount++;}
function logExpRaw(msg){if(expPaused)return;expRaw.push({msg,t:performance.now()});if(expRaw.length>1000)expRaw=expRaw.slice(-500);}
function logExpTrade(tr){if(expPaused)return;expTrades.push({price:tr.price,qty:tr.qty,side:tr.side,t:performance.now()});if(expTrades.length>5000)expTrades=expTrades.slice(-3000);expFpsCount++;}

// ========= SOLO SIMULATOR =========
let curModel='optimized',simBook=new OptimizedBook(),perf=new PT(),pnl=new PnL();
let simOn=false,simScen='steady',simFrame=0,live=[],simMid=1000,opsT=0;
let soloHist=[],multiHist=[];let psHC=null,psOC=null;
let cryptoFeed=null,cryptoLevelMap={};
const CRYPTO_PAIRS={bitcoin:'BTCUSDT',ethereum:'ETHUSDT',solana:'SOLUSDT',dogecoin:'DOGEUSDT',ripple:'XRPUSDT'};
let aData={mid:[],spread:[],imbal:[],vol:[],eps:[],fps:[]},aVolAcc=0,aEpsAcc=0,aFpsAcc=0,aLastSample=performance.now();
let aMidCh=null,aSpreadCh=null,aImbalCh=null,aVolCh=null,aThruCh=null;
const A_MAX=300;

function switchModel(m){curModel=m;resetSim();document.getElementById('mTag').textContent=MN[m];document.getElementById('mTag').style.background=MC[m];}
function toggleSim(){simOn=!simOn;const b=document.getElementById('playBtn');b.innerHTML=simOn?'&#9646;&#9646; Pause':'&#9654; Start';b.classList.toggle('on',simOn);}
function resetSim(){simOn=false;if(cryptoFeed){cryptoFeed.stop();cryptoFeed=null;}cryptoLevelMap={};expEvents=[];expRaw=[];expTrades=[];if(expDepthChart){expDepthChart.destroy();expDepthChart=null;}simBook=new(MODELS[curModel])();perf=new PT();pnl=new PnL();live=[];simMid=1000;opsT=0;simFrame=0;
soloPnlData=[];if(soloPnlChart){soloPnlChart.destroy();soloPnlChart=null;}
aData={mid:[],spread:[],imbal:[],vol:[],eps:[],fps:[]};aVolAcc=0;aEpsAcc=0;aFpsAcc=0;aLastSample=performance.now();
for(const c of[aMidCh,aSpreadCh,aImbalCh,aVolCh,aThruCh]){if(c)c.destroy();}aMidCh=aSpreadCh=aImbalCh=aVolCh=aThruCh=null;
document.getElementById('playBtn').innerHTML='&#9654; Start';document.getElementById('playBtn').classList.remove('on');renderSolo();}

function setDataset(v,target){if(v.startsWith('crypto:')){startCryptoFeed(v.split(':')[1]);}else{if(cryptoFeed){cryptoFeed.stop();cryptoFeed=null;}cryptoLevelMap={};if(target==='solo')simScen=v;else multiScen=v;}}
async function startCryptoFeed(coin){resetSim();cryptoFeed=new CryptoFeed();
try{await cryptoFeed.connect(CRYPTO_PAIRS[coin]);toggleSim();}catch(e){cryptoFeed=null;
document.getElementById('sStats').innerHTML='<span style="color:var(--red)">'+e.message+'</span><br><span style="color:var(--dim);font-size:10px">Try: python -m http.server</span>';}}

function genOp(mid,aggr,canc,liveArr,book,scen,frame){
const r=Math.random();const burst=scen==='sweep'&&(frame%200)>180;const wide=scen==='wide'?30:8;
const stress=scen==='stress';
// Trend bias: price drifts in waves so P&L isn't monotonically positive
const trend=Math.sin(frame/250)*0.35;
const buySide=Math.random()<(0.5+trend)?'buy':'sell';
if(burst){const bdir=Math.sin(frame/400)>0?'buy':'sell';return{type:'match',side:bdir,price:bdir==='buy'?mid+50:mid-50,qty:2+Math.floor(Math.random()*8)};}
if(r<0.05+aggr*0.5){return{type:'match',side:buySide,price:buySide==='buy'?mid+50:mid-50,qty:1+Math.floor(Math.random()*5)};}
if(r<0.30||liveArr.length<30){return{type:'add',side:buySide,price:buySide==='buy'?mid-1-Math.floor(Math.random()*wide):mid+1+Math.floor(Math.random()*wide),qty:1+Math.floor(Math.random()*20)};}
if(r<0.30+(stress?0.60:canc)&&liveArr.length>0){const idx=Math.floor(Math.random()*liveArr.length);return{type:'cancel',idx};}
return{type:'add',side:buySide,price:buySide==='buy'?mid-1-Math.floor(Math.random()*wide):mid+1+Math.floor(Math.random()*wide),qty:1+Math.floor(Math.random()*20)};}

function applyOp(book,op,liveArr,perfT,pnlT,doLog){
let t0,t1;const tb=book.trades.length;
if(op.type==='match'||op.type==='add'){
t0=performance.now();const id=book.submit(op.side,op.price,op.qty);t1=performance.now();
if(op.type==='add'&&book.orders[id])liveArr.push(id);
perfT.rec((t1-t0)*1000,op.type==='match'?'match':'add');
if(doLog!==false){logExp('add',{side:op.side,price:op.price,qty:op.qty});aEpsAcc++;}
}else if(op.type==='cancel'){
const cid=liveArr[op.idx];t0=performance.now();book.cancel(cid);t1=performance.now();
liveArr[op.idx]=liveArr[liveArr.length-1];liveArr.pop();perfT.rec((t1-t0)*1000,'cancel');
if(doLog!==false){logExp('cancel',{id:cid});aEpsAcc++;}}
for(let j=tb;j<book.trades.length;j++){const t=book.trades[j];if(pnlT)pnlT.onTrade(t.price,t.qty,t.side);if(doLog!==false){logExpTrade(t);aFpsAcc++;aVolAcc+=t.qty;}}}

function soloStep(){
if(cryptoFeed){const ups=cryptoFeed.drain(300);
for(const u of ups){if(isNaN(u.tick))continue;const key=u.side+':'+u.tick;const tb=simBook.trades.length;let t0,t1;
const old=cryptoLevelMap[key];if(old!==undefined){t0=performance.now();simBook.cancel(old);t1=performance.now();perf.rec((t1-t0)*1000,'cancel');
logExp('cancel',{id:old,side:u.side,tick:u.tick});
const ix=live.indexOf(old);if(ix>=0){live[ix]=live[live.length-1];live.pop();}delete cryptoLevelMap[key];opsT++;}
if(u.qty>0){t0=performance.now();const bid=simBook.submit(u.side,u.tick,u.qty);t1=performance.now();perf.rec((t1-t0)*1000,'add');
logExp('add',{side:u.side,price:u.tick,qty:u.qty,realPrice:cryptoFeed.toPrice(u.tick)});aEpsAcc++;
if(simBook.orders[bid]){live.push(bid);cryptoLevelMap[key]=bid;}opsT++;}
for(let j=tb;j<simBook.trades.length;j++){const tr=simBook.trades[j];pnl.onTrade(tr.price,tr.qty,tr.side);logExpTrade(tr);aFpsAcc++;aVolAcc+=tr.qty;}}}
else{const spd=+document.getElementById('spdS').value;const aggr=+document.getElementById('aggS').value/100;const canc=+document.getElementById('canS').value/100;
for(let i=0;i<spd;i++){const op=genOp(simMid,aggr,canc,live,simBook,simScen,simFrame);applyOp(simBook,op,live,perf,pnl);opsT++;}}
const bb=simBook.bestBid(),ba=simBook.bestAsk();if(bb!==null&&ba!==null)simMid=Math.floor((bb+ba)/2);
if(simFrame%15===0){soloPnlData.push(pnl.total(simMid));if(soloPnlData.length>300)soloPnlData.shift();}simFrame++;}

function renderLadder(book,mid,el){
const bb=book.bestBid()||mid,ba=book.bestAsk()||mid;const c=Math.floor(((bb||mid)+(ba||mid))/2);const R=14;
let mq=1;for(let p=c-R/2;p<=c+R/2;p++){const bq=book.sizeAt('buy',p),aq=book.sizeAt('sell',p);if(bq>mq)mq=bq;if(aq>mq)mq=aq;}
let h='';for(let p=c+R/2;p>=c-R/2;p--){const bq=book.sizeAt('buy',p),aq=book.sizeAt('sell',p);
const bw=Math.round(bq/mq*100),aw=Math.round(aq/mq*100);const pc=p==bb?'price-col bb':p==ba?'price-col ba':'price-col';
h+='<div class="lrow"><div class="bar-cell bid"><div class="bid-bar" style="width:'+bw+'%"></div></div><div class="bid-qty">'+(bq||'')+'</div><div class="'+pc+'">'+p+'</div><div class="ask-qty">'+(aq||'')+'</div><div class="bar-cell"><div class="ask-bar" style="width:'+aw+'%"></div></div></div>';}
el.innerHTML=h;}

function renderSolo(){
renderLadder(simBook,simMid,document.getElementById('ladder'));
const last=simBook.trades.slice(-14).reverse();let tp='';
for(const t of last)tp+='<div class="trade-row '+(t.side==='buy'?'trade-buy':'trade-sell')+'">'+(t.side==='buy'?'BUY ':'SELL')+' '+t.qty+' @ '+t.price+'</div>';
document.getElementById('tape').innerHTML=tp;
const spr=(simBook.bestBid()!==null&&simBook.bestAsk()!==null)?simBook.bestAsk()-simBook.bestBid():'--';
let st='Orders: <b>'+simBook.orderCount()+'</b><br>Trades: <b>'+simBook.mc+'</b><br>Spread: <b>'+spr+'</b><br>Ops: <b>'+opsT.toLocaleString()+'</b>';
if(cryptoFeed&&cryptoFeed.connected)st+='<br><span style="color:var(--green)">&#9679; LIVE</span> <span style="color:var(--dim)">'+cryptoFeed.pair+'</span>';
else if(cryptoFeed)st+='<br><span style="color:var(--yellow)">&#9679; Connecting...</span>';
document.getElementById('sStats').innerHTML=st;
document.getElementById('sp50').textContent=perf.pct(50).toFixed(1)+' µs';
document.getElementById('sp99').textContent=perf.pct(99).toFixed(1)+' µs';
document.getElementById('sthru').textContent=(perf.thru()/1000).toFixed(1)+'k';
const pv=pnl.total(simMid);document.getElementById('spnl').textContent='$'+pnl.fmt(simMid);
document.getElementById('spnl').className='v '+(pv>=0?'pnl-pos':'pnl-neg');
document.getElementById('svol').textContent=pnl.vol.toLocaleString();
// Solo P&L chart
if(soloPnlData.length>1&&simFrame%15===0){
if(!soloPnlChart){soloPnlChart=new Chart(document.getElementById('soloPnlChart'),{type:'line',
data:{labels:soloPnlData.map((_,i)=>i),datasets:[{label:'P&L',data:soloPnlData,borderColor:soloPnlData[soloPnlData.length-1]>=0?'#3fb950':'#f85149',
backgroundColor:'rgba(63,185,80,0.1)',fill:true,tension:.3,pointRadius:0,borderWidth:2}]},
options:{responsive:true,animation:false,plugins:{legend:{display:false}},scales:{y:{grid:{color:'#21262d'},title:{display:true,text:'$'}},x:{display:false}}}});}
else{soloPnlChart.data.labels=soloPnlData.map((_,i)=>i);soloPnlChart.data.datasets[0].data=soloPnlData;
const c=soloPnlData[soloPnlData.length-1]>=0?'#3fb950':'#f85149';soloPnlChart.data.datasets[0].borderColor=c;
soloPnlChart.data.datasets[0].backgroundColor=c+'22';soloPnlChart.update('none');}}}

)=="); w(f,R"==(
// ========= MULTI SIMULATOR =========
let mBooks={},mPerfs={},mPnls={},mLive={},mMid=1000,mOps=0,mOn=false,multiScen='steady',mFrame=0;
let mCharts={},mData={p50:{},p99:{},thru:{},pnl:{}};
let soloPnlChart=null,soloPnlData=[];

function makeMultiChart(id,title,yLabel){
const ds=MK.map(k=>({label:MN[k],data:[],borderColor:MC[k],tension:.3,pointRadius:0,borderWidth:2}));
return new Chart(document.getElementById(id),{type:'line',data:{labels:[],datasets:ds},
options:{responsive:true,animation:false,plugins:{legend:{position:'top',labels:{usePointStyle:true,boxWidth:8}}},
scales:{y:{title:{display:true,text:yLabel},grid:{color:'#21262d'}},x:{display:false}}}});}
let mCryptoFeed=null,mCryptoLevels={};
function resetMulti(){mOn=false;mOps=0;mMid=1000;mFrame=0;
if(mCryptoFeed){mCryptoFeed.stop();mCryptoFeed=null;}mCryptoLevels={};
for(const k of MK){mBooks[k]=new(MODELS[k])();mPerfs[k]=new PT();mPnls[k]=new PnL();mLive[k]=[];mCryptoLevels[k]={};}
document.getElementById('mPlayBtn').innerHTML='&#9654; Start Race';document.getElementById('mPlayBtn').classList.remove('on');
for(const m of['p50','p99','thru','pnl'])for(const k of MK)mData[m][k]=[];
for(const c of Object.values(mCharts))c.destroy();
mCharts.p50=makeMultiChart('mP50Chart','p50','µs');mCharts.p99=makeMultiChart('mP99Chart','p99','µs');
mCharts.thru=makeMultiChart('mThruChart','Throughput','k ops/s');mCharts.pnl=makeMultiChart('mPnlChart','P&L','$');
renderMulti();}
function setMultiData(v){if(v.startsWith('crypto:')){startMultiCrypto(v.split(':')[1]);}else{if(mCryptoFeed){mCryptoFeed.stop();mCryptoFeed=null;}multiScen=v;}}
async function startMultiCrypto(coin){resetMulti();mCryptoFeed=new CryptoFeed();
try{await mCryptoFeed.connect(CRYPTO_PAIRS[coin]);for(const k of MK)mCryptoLevels[k]={};if(!mOn)toggleMulti();}catch(e){mCryptoFeed=null;}}
resetMulti();

function toggleMulti(){mOn=!mOn;const b=document.getElementById('mPlayBtn');b.innerHTML=mOn?'&#9646;&#9646; Pause':'&#9654; Start Race';b.classList.toggle('on',mOn);}

function multiStep(){
if(mCryptoFeed){const ups=mCryptoFeed.drain(200);
for(const u of ups){if(isNaN(u.tick))continue;
for(const k of MK){const key=u.side+':'+u.tick;const lm=mCryptoLevels[k]||{};const tb=mBooks[k].trades.length;
const old=lm[key];if(old!==undefined){const t0=performance.now();mBooks[k].cancel(old);const t1=performance.now();mPerfs[k].rec((t1-t0)*1000,'cancel');delete lm[key];}
if(u.qty>0){const t0=performance.now();const bid=mBooks[k].submit(u.side,u.tick,u.qty);const t1=performance.now();mPerfs[k].rec((t1-t0)*1000,'add');
if(mBooks[k].orders[bid]){mLive[k].push(bid);lm[key]=bid;}}mCryptoLevels[k]=lm;
for(let j=tb;j<mBooks[k].trades.length;j++){mPnls[k].onTrade(mBooks[k].trades[j].price,mBooks[k].trades[j].qty,mBooks[k].trades[j].side);}
if(k==='optimized'&&u.qty>0)logExp('add',{side:u.side,price:u.tick,qty:u.qty});}mOps++;}}
else{const spd=+document.getElementById('mSpdS').value;
for(let i=0;i<spd;i++){
const op=genOp(mMid,0.20,0.35,mLive.optimized,mBooks.optimized,multiScen,mFrame);
for(const k of MK){const opCopy=op.type==='cancel'?{type:'cancel',idx:Math.min(op.idx,mLive[k].length-1)}:op;
if(op.type==='cancel'&&mLive[k].length===0)continue;applyOp(mBooks[k],opCopy,mLive[k],mPerfs[k],mPnls[k],k==='optimized');}mOps++;}}
const bb=mBooks.optimized.bestBid(),ba=mBooks.optimized.bestAsk();if(bb!==null&&ba!==null)mMid=Math.floor((bb+ba)/2);
if(mFrame%20===0){for(const k of MK){mData.p50[k].push(mPerfs[k].pct(50));mData.p99[k].push(mPerfs[k].pct(99));
mData.thru[k].push(mPerfs[k].thru()/1000);mData.pnl[k].push(mPnls[k].total(mMid));
for(const m of['p50','p99','thru','pnl'])if(mData[m][k].length>200)mData[m][k].shift();}}mFrame++;}

function renderMulti(){
let h='<div class="grid4">';
for(const k of MK){const p=mPerfs[k];const pn=mPnls[k];const pv=pn.total(mMid);
h+='<div class="mcard" style="border-top:3px solid '+MC[k]+'"><h3>'+MN[k]+'</h3>';
h+='<div class="mv g">'+p.pct(50).toFixed(1)+' <small>µs</small></div><div class="ml">p50</div>';
h+='<div class="mv y">'+p.pct(99).toFixed(1)+'</div><div class="ml">p99</div>';
h+='<div class="mv b">'+(p.thru()/1000).toFixed(1)+'k</div><div class="ml">ops/s</div>';
h+='<div class="mv '+(pv>=0?'pnl-pos':'pnl-neg')+'">$'+pn.fmt(mMid)+'</div><div class="ml">P&L</div>';
h+='<div class="mv">'+mBooks[k].orderCount()+'</div><div class="ml">Orders</div>';
h+='<div class="mv">'+mBooks[k].mc+'</div><div class="ml">Trades</div></div>';}
h+='</div>';document.getElementById('multiStats').innerHTML=h;
if(mFrame%20===0&&mData.p50.optimized.length>1){const labels=Array.from({length:mData.p50.optimized.length},(_,i)=>i);
for(const[m,ch]of Object.entries(mCharts)){ch.data.labels=labels;
for(const k of MK)ch.data.datasets.find(d=>d.label===MN[k]).data=mData[m][k];ch.update('none');}}}

)=="); w(f,R"==(
// ========= SNAPSHOTS =========
function snapshotSolo(){const s=perf.snap(curModel,simScen,pnl,simMid);soloHist.push(s);renderSoloPerf();showTab('perf');showPerfView('solo');}
function snapshotMulti(){const s={};for(const k of MK)s[k]=mPerfs[k].snap(k,multiScen,mPnls[k],mMid);multiHist.push({time:new Date().toLocaleTimeString(),scenario:multiScen,models:s});renderMultiPerf();showTab('perf');showPerfView('multi');}
function clearHistory(){soloHist=[];multiHist=[];renderSoloPerf();renderMultiPerf();}

function renderSoloPerf(){
if(!soloHist.length){document.getElementById('perfSoloEmpty').style.display='block';document.getElementById('perfSoloContent').style.display='none';return;}
document.getElementById('perfSoloEmpty').style.display='none';document.getElementById('perfSoloContent').style.display='block';
const s=soloHist[soloHist.length-1];
document.getElementById('psMet').innerHTML='<div class="metric"><div class="v g">'+s.p50.toFixed(1)+' µs</div><div class="l">p50</div></div><div class="metric"><div class="v y">'+s.p99.toFixed(1)+' µs</div><div class="l">p99</div></div><div class="metric"><div class="v r">'+s.p999.toFixed(1)+' µs</div><div class="l">p99.9</div></div><div class="metric"><div class="v b">'+(s.thru/1000).toFixed(1)+'k</div><div class="l">ops/s</div></div><div class="metric"><div class="v '+(s.pnl>=0?'pnl-pos':'pnl-neg')+'">$'+s.pnl.toLocaleString(undefined,{maximumFractionDigits:0})+'</div><div class="l">P&L</div></div>';
const h=s.hist;if(psHC)psHC.destroy();
psHC=new Chart(document.getElementById('psHist'),{type:'bar',data:{labels:h.labels.map(l=>l+' µs'),datasets:[{data:h.counts,backgroundColor:MC[s.model],borderRadius:3}]},options:{responsive:true,plugins:{legend:{display:false}},scales:{y:{grid:{color:'#21262d'}},x:{grid:{display:false}}}}});
if(psOC)psOC.destroy();
psOC=new Chart(document.getElementById('psOps'),{type:'bar',data:{labels:['Add','Cancel','Match'],datasets:[{data:[s.addP50,s.cancelP50,s.matchP50],backgroundColor:['#3fb950','#58a6ff','#f85149'],borderRadius:3}]},options:{responsive:true,plugins:{legend:{display:false}},scales:{y:{grid:{color:'#21262d'}},x:{grid:{display:false}}}}});
let th='<table><tr><th>Time</th><th>Model</th><th>Data</th><th class="rt">Ops</th><th class="rt">p50</th><th class="rt">p99</th><th class="rt">Thru</th><th class="rt">P&L</th><th class="rt">Vol</th></tr>';
for(const r of soloHist)th+='<tr><td>'+r.time+'</td><td style="color:'+MC[r.model]+'">'+MN[r.model]+'</td><td>'+r.scenario+'</td><td class="rt">'+r.ops.toLocaleString()+'</td><td class="rt g">'+r.p50.toFixed(1)+'</td><td class="rt y">'+r.p99.toFixed(1)+'</td><td class="rt">'+(r.thru/1000).toFixed(1)+'k</td><td class="rt '+(r.pnl>=0?'pnl-pos':'pnl-neg')+'">$'+r.pnl.toLocaleString(undefined,{maximumFractionDigits:0})+'</td><td class="rt">'+r.vol.toLocaleString()+'</td></tr>';
th+='</table>';document.getElementById('psHistory').innerHTML=th;}

let pmP50C=null,pmThC=null;
function renderMultiPerf(){
if(!multiHist.length){document.getElementById('perfMultiEmpty').style.display='block';document.getElementById('perfMultiContent').style.display='none';return;}
document.getElementById('perfMultiEmpty').style.display='none';document.getElementById('perfMultiContent').style.display='block';
const last=multiHist[multiHist.length-1];
let tb='<table><tr><th>Model</th><th class="rt">p50</th><th class="rt">p99</th><th class="rt">Throughput</th><th class="rt">P&L</th><th class="rt">Trades</th><th class="rt">Volume</th></tr>';
for(const k of MK){const m=last.models[k];tb+='<tr><td style="color:'+MC[k]+';font-weight:600">'+MN[k]+'</td><td class="rt g">'+m.p50.toFixed(1)+' µs</td><td class="rt y">'+m.p99.toFixed(1)+'</td><td class="rt">'+(m.thru/1000).toFixed(1)+'k/s</td><td class="rt '+(m.pnl>=0?'pnl-pos':'pnl-neg')+'">$'+m.pnl.toLocaleString(undefined,{maximumFractionDigits:0})+'</td><td class="rt">'+m.ops.toLocaleString()+'</td><td class="rt">'+m.vol.toLocaleString()+'</td></tr>';}
tb+='</table>';document.getElementById('pmTable').innerHTML=tb;
if(pmP50C)pmP50C.destroy();if(pmThC)pmThC.destroy();
pmP50C=new Chart(document.getElementById('pmP50'),{type:'bar',data:{labels:MK.map(k=>MN[k]),datasets:[{data:MK.map(k=>last.models[k].p50),backgroundColor:MK.map(k=>MC[k]),borderRadius:4}]},options:{responsive:true,plugins:{legend:{display:false}},scales:{y:{title:{display:true,text:'µs'},grid:{color:'#21262d'}},x:{grid:{display:false}}}}});
pmThC=new Chart(document.getElementById('pmThru'),{type:'bar',data:{labels:MK.map(k=>MN[k]),datasets:[{data:MK.map(k=>last.models[k].thru/1000),backgroundColor:MK.map(k=>MC[k]),borderRadius:4}]},options:{responsive:true,plugins:{legend:{display:false}},scales:{y:{title:{display:true,text:'k ops/s'},grid:{color:'#21262d'}},x:{grid:{display:false}}}}});
let hh='';for(const r of multiHist){hh+='<div style="padding:8px 0;border-bottom:1px solid var(--border);font-size:12px"><b>'+r.time+'</b> &mdash; '+r.scenario+' &mdash; ';
for(const k of MK)hh+='<span style="color:'+MC[k]+'">'+MN[k]+': '+r.models[k].p50.toFixed(1)+'µs</span> &nbsp;';hh+='</div>';}
document.getElementById('pmHistory').innerHTML=hh;}

// ========= DATA EXPLORER RENDER =========
function renderExplorer(){
if(!document.getElementById('explore').classList.contains('active'))return;
const book=expSource==='solo'?simBook:(mBooks.optimized||null);const now=performance.now();
if(now-expEpsLast>1000){const dt=(now-expEpsLast)/1000;
document.getElementById('xEps').textContent=(expEpsCount/dt).toFixed(0);
document.getElementById('xFps').textContent=(expFpsCount/dt).toFixed(0);
expEpsCount=0;expFpsCount=0;expEpsLast=now;}
document.getElementById('xOpen').textContent=book?book.orderCount():'--';
document.getElementById('xQueue').textContent=cryptoFeed?cryptoFeed.updates.length:'--';
if(cryptoFeed){document.getElementById('xLiveCard').style.display='block';
const cs=cryptoFeed.connected?'<span style="color:var(--green)">&#9679; '+cryptoFeed.pair+'</span>':'<span style="color:var(--yellow)">&#9679; Connecting</span>';
document.getElementById('xConn').innerHTML=cs;document.getElementById('xLiveStatus').innerHTML=cs;}
else{document.getElementById('xLiveCard').style.display='none';document.getElementById('xConn').textContent='Synthetic';}
const filtered=expFilter==='all'?expEvents:expEvents.filter(e=>e.type===expFilter);
const logEl=document.getElementById('xLog');const recent=filtered.slice(-200);let html='';
for(const e of recent){const ts=new Date(e.t).toLocaleTimeString(undefined,{hour12:false});
if(e.type==='add'){const c=e.detail.side==='buy'?'var(--green)':'var(--red)';
html+='<div><span style="color:var(--dim)">'+ts+'</span> <span style="color:'+c+'">ADD '+e.detail.side.toUpperCase()+'</span> '+e.detail.qty+' @ '+e.detail.price;
if(e.detail.realPrice)html+=' <span style="color:var(--dim)">($'+e.detail.realPrice.toFixed(2)+')</span>';html+='</div>';}
else if(e.type==='cancel'){html+='<div><span style="color:var(--dim)">'+ts+'</span> <span style="color:var(--purple)">CXL</span> id:'+e.detail.id+'</div>';}
else if(e.type==='fill'){html+='<div><span style="color:var(--dim)">'+ts+'</span> <span style="color:var(--yellow)">FILL</span> '+(e.detail.side==='buy'?'<span style="color:var(--green)">BUY</span>':'<span style="color:var(--red)">SELL</span>')+' '+e.detail.qty+' @ '+e.detail.price+'</div>';}}
logEl.innerHTML=html;if(document.getElementById('expAutoScroll').checked)logEl.scrollTop=logEl.scrollHeight;
if(cryptoFeed){const liveEl=document.getElementById('xLiveLog');let lh='';
for(const r of expRaw.slice(-50)){const rts=new Date(r.t).toLocaleTimeString(undefined,{hour12:false});lh+='<div><span style="color:var(--dim)">'+rts+'</span> '+r.msg+'</div>';}
liveEl.innerHTML=lh;if(document.getElementById('expAutoScroll').checked)liveEl.scrollTop=liveEl.scrollHeight;}
const tradeEl=document.getElementById('xTradeLog');let th='';
for(const t of expTrades.slice(-100)){const tts=new Date(t.t).toLocaleTimeString(undefined,{hour12:false});
const c=t.side==='buy'?'var(--green)':'var(--red)';
th+='<div><span style="color:var(--dim)">'+tts+'</span> <span style="color:'+c+'">'+(t.side==='buy'?'BUY':'SELL')+'</span> '+t.qty+' @ '+t.price+'</div>';}
tradeEl.innerHTML=th;document.getElementById('xTradeCount').textContent=expTrades.length+' trades';
if(document.getElementById('expAutoScroll').checked)tradeEl.scrollTop=tradeEl.scrollHeight;
renderExpDepth(book);}
function renderExpDepth(book){if(!book)return;
const bb=book.bestBid(),ba=book.bestAsk();if(bb===null&&ba===null)return;
const mid=bb!==null&&ba!==null?Math.floor((bb+ba)/2):(bb||ba);const R=25;
const prices=[],bids=[],asks=[];
for(let p=mid-R;p<=mid+R;p++){prices.push(p);bids.push(book.sizeAt('buy',p));asks.push(book.sizeAt('sell',p));}
if(!expDepthChart){expDepthChart=new Chart(document.getElementById('xDepthChart'),{type:'bar',
data:{labels:prices,datasets:[{label:'Bids',data:bids,backgroundColor:'rgba(63,185,80,0.6)',borderRadius:1,barPercentage:1,categoryPercentage:1},
{label:'Asks',data:asks,backgroundColor:'rgba(248,81,73,0.6)',borderRadius:1,barPercentage:1,categoryPercentage:1}]},
options:{responsive:true,animation:false,plugins:{legend:{position:'top',labels:{usePointStyle:true,boxWidth:8}}},
scales:{y:{grid:{color:'#21262d'}},x:{display:false}}}});}
else{expDepthChart.data.labels=prices;expDepthChart.data.datasets[0].data=bids;expDepthChart.data.datasets[1].data=asks;expDepthChart.update('none');}}

)=="); w(f,R"==(
// ========= ANALYTICS =========
function sampleAnalytics(){
const now=performance.now();if(now-aLastSample<500)return;const dt=(now-aLastSample)/1000;
const bb=simBook.bestBid(),ba=simBook.bestAsk();
if(bb!==null&&ba!==null){
let mid=Math.floor((bb+ba)/2);if(cryptoFeed&&cryptoFeed.initialized)mid=cryptoFeed.toPrice(mid);
aData.mid.push(mid);if(aData.mid.length>A_MAX)aData.mid.shift();
const spr=ba-bb;aData.spread.push(spr);if(aData.spread.length>A_MAX)aData.spread.shift();
let bq=0,aq=0;const c=Math.floor((bb+ba)/2);for(let p=c-30;p<=c+30;p++){bq+=simBook.sizeAt('buy',p);aq+=simBook.sizeAt('sell',p);}
const imb=(bq+aq)>0?(bq-aq)/(bq+aq):0;aData.imbal.push(+(imb.toFixed(3)));if(aData.imbal.length>A_MAX)aData.imbal.shift();}
aData.vol.push(aVolAcc);aVolAcc=0;if(aData.vol.length>A_MAX)aData.vol.shift();
aData.eps.push(+(aEpsAcc/dt).toFixed(1));aEpsAcc=0;if(aData.eps.length>A_MAX)aData.eps.shift();
aData.fps.push(+(aFpsAcc/dt).toFixed(1));aFpsAcc=0;if(aData.fps.length>A_MAX)aData.fps.shift();
aLastSample=now;}
function mkAChart(id,label,color,fill){const el=document.getElementById(id);if(!el)return null;
return new Chart(el,{type:'line',data:{labels:[],datasets:[{label,data:[],borderColor:color,backgroundColor:fill||'transparent',
fill:!!fill,tension:.3,pointRadius:0,borderWidth:2}]},
options:{responsive:true,animation:false,plugins:{legend:{display:false}},scales:{y:{grid:{color:'#21262d'}},x:{display:false}}}});}
function renderAnalytics(){
if(!document.getElementById('sim').classList.contains('active'))return;
if(aData.mid.length<2)return;
const labels=Array.from({length:aData.mid.length},(_,i)=>i);
if(!aMidCh){aMidCh=mkAChart('aMidC','Mid Price','#58a6ff');aSpreadCh=mkAChart('aSpreadC','Spread','#d29922','rgba(210,153,34,0.15)');
const el3=document.getElementById('aImbalC');if(el3)aImbalCh=new Chart(el3,{type:'line',data:{labels,datasets:[{label:'Imbalance',data:aData.imbal,
borderColor:'#3fb950',backgroundColor:ctx=>{const c=ctx.chart.ctx;const g=c.createLinearGradient(0,0,0,ctx.chart.height);g.addColorStop(0,'rgba(63,185,80,0.3)');g.addColorStop(0.5,'transparent');g.addColorStop(1,'rgba(248,81,73,0.3)');return g;},
fill:true,tension:.3,pointRadius:0,borderWidth:2}]},options:{responsive:true,animation:false,plugins:{legend:{display:false}},
scales:{y:{min:-1,max:1,grid:{color:'#21262d'},ticks:{callback:v=>v>0?'+'+v:v}},x:{display:false}}}});
aVolCh=mkAChart('aVolC','Volume','#bc8cff','rgba(188,140,255,0.15)');
const el5=document.getElementById('aThruC');if(el5)aThruCh=new Chart(el5,{type:'line',data:{labels,datasets:[
{label:'Events/s',data:aData.eps,borderColor:'#3fb950',tension:.3,pointRadius:0,borderWidth:2},
{label:'Fills/s',data:aData.fps,borderColor:'#f85149',tension:.3,pointRadius:0,borderWidth:2}]},
options:{responsive:true,animation:false,plugins:{legend:{position:'top',labels:{usePointStyle:true,boxWidth:8}}},
scales:{y:{grid:{color:'#21262d'}},x:{display:false}}}});}
if(!aMidCh)return;
aMidCh.data.labels=labels;aMidCh.data.datasets[0].data=aData.mid;aMidCh.update('none');
aSpreadCh.data.labels=labels;aSpreadCh.data.datasets[0].data=aData.spread;aSpreadCh.update('none');
if(aImbalCh){aImbalCh.data.labels=labels;aImbalCh.data.datasets[0].data=aData.imbal;aImbalCh.update('none');}
aVolCh.data.labels=labels;aVolCh.data.datasets[0].data=aData.vol;aVolCh.update('none');
if(aThruCh){aThruCh.data.labels=labels;aThruCh.data.datasets[0].data=aData.eps;aThruCh.data.datasets[1].data=aData.fps;aThruCh.update('none');}}

// ========= LIVE FEEDS =========
const FEED_COINS=[{sym:'BTCUSDT',label:'BTC',color:'#f7931a'},{sym:'ETHUSDT',label:'ETH',color:'#627eea'},{sym:'SOLUSDT',label:'SOL',color:'#00ffa3'},{sym:'DOGEUSDT',label:'DOGE',color:'#c2a633'},{sym:'XRPUSDT',label:'XRP',color:'#00aae4'}];
const liveFeeds={};
async function toggleFeed(btn){const sym=btn.dataset.pair;if(liveFeeds[sym]){liveFeeds[sym].feed.stop();delete liveFeeds[sym];btn.classList.remove('on');buildFeedGrid();}
else{btn.classList.add('on');const feed=new CryptoFeed();const book=new OptimizedBook();
liveFeeds[sym]={feed,book,lm:{},evts:[],tds:[],ec:0,tc:0,el:performance.now(),eps:0,tps:0,te:0,tt:0};
try{await feed.connect(sym);buildFeedGrid();}catch(e){delete liveFeeds[sym];btn.classList.remove('on');buildFeedGrid();
const ag=document.getElementById('feedAgg');if(ag)ag.innerHTML='<span style="color:var(--red)">Failed to connect '+sym+': '+e.message+'</span><br><span style="color:var(--dim)">Serve via HTTP: <b>python -m http.server 8080</b> then open <b>localhost:8080/report.html</b></span>';}}}
function connectAllFeeds(){document.querySelectorAll('.feed-btn').forEach(b=>{if(!liveFeeds[b.dataset.pair])toggleFeed(b);});}
function stopAllFeeds(){for(const sym of Object.keys(liveFeeds)){liveFeeds[sym].feed.stop();delete liveFeeds[sym];}document.querySelectorAll('.feed-btn').forEach(b=>b.classList.remove('on'));buildFeedGrid();}
function stepFeeds(){for(const[sym,s]of Object.entries(liveFeeds)){if(!s.feed.connected)continue;const ups=s.feed.drain(200);
for(const u of ups){if(isNaN(u.tick))continue;const key=u.side+':'+u.tick;const tb=s.book.trades.length;
const old=s.lm[key];if(old!==undefined){s.book.cancel(old);delete s.lm[key];}
if(u.qty>0){const bid=s.book.submit(u.side,u.tick,u.qty);if(s.book.orders[bid])s.lm[key]=bid;}
s.ec++;s.te++;s.evts.push({side:u.side,tick:u.tick,qty:u.qty,t:performance.now()});if(s.evts.length>200)s.evts=s.evts.slice(-100);
for(let j=tb;j<s.book.trades.length;j++){s.tds.push(s.book.trades[j]);s.tc++;s.tt++;if(s.tds.length>100)s.tds=s.tds.slice(-50);}}
const now=performance.now();if(now-s.el>1000){const dt=(now-s.el)/1000;s.eps=s.ec/dt;s.tps=s.tc/dt;s.ec=0;s.tc=0;s.el=now;}}}
function buildFeedGrid(){const g=document.getElementById('feedGrid');if(!g)return;let h='';
for(const c of FEED_COINS){const s=liveFeeds[c.sym];
h+='<div class="card" style="border-top:3px solid '+c.color+';min-height:320px">';
h+='<h2 style="display:flex;align-items:center;gap:6px"><span style="color:'+c.color+'">'+c.label+'</span>/USDT ';
h+='<span id="fcs_'+c.sym+'" style="font-size:10px;margin-left:auto">'+(s?'':'<span style="color:var(--dim)">Off</span>')+'</span></h2>';
if(s){h+='<div id="fmet_'+c.sym+'" style="font-size:11px;line-height:1.8;margin-bottom:6px"></div>';
h+='<div id="flad_'+c.sym+'" style="font-size:11px;line-height:1.4;margin-bottom:6px"></div>';
h+='<div style="font-size:10px;color:var(--dim);margin-bottom:2px">Event Log</div>';
h+='<div id="flog_'+c.sym+'" style="height:90px;overflow-y:auto;font-family:\'SF Mono\',Consolas,monospace;font-size:10px;line-height:1.5"></div>';}
else{h+='<div style="text-align:center;padding:60px 0;color:var(--dim)">Click <b>'+c.label+'</b> to connect</div>';}
h+='</div>';}g.innerHTML=h;}
function renderLiveFeeds(){if(!document.getElementById('feeds').classList.contains('active'))return;
let tE=0,tT=0,ac=0;
for(const c of FEED_COINS){const s=liveFeeds[c.sym];if(!s)continue;ac++;tE+=s.eps;tT+=s.tps;
const st=document.getElementById('fcs_'+c.sym);if(st)st.innerHTML=s.feed.connected?'<span style="color:var(--green)">&#9679; Live</span>':'<span style="color:var(--yellow)">&#9679;...</span>';
const me=document.getElementById('fmet_'+c.sym);if(me){const bb=s.book.bestBid(),ba=s.book.bestAsk();
const spr=bb!==null&&ba!==null?ba-bb:'--';const mid=bb!==null&&ba!==null?(bb+ba)/2:null;
const rp=mid!==null?s.feed.toPrice(Math.floor(mid)):null;
let mh='<span style="color:var(--green)">'+s.eps.toFixed(0)+'</span> evt/s &nbsp;<span style="color:var(--yellow)">'+s.tps.toFixed(0)+'</span> fill/s &nbsp;Spread: <b>'+spr+'</b> &nbsp;Orders: '+s.book.orderCount();
if(rp!==null)mh+='<br>Mid: <span style="color:var(--blue)">$'+rp.toLocaleString(undefined,{minimumFractionDigits:2,maximumFractionDigits:2})+'</span> &nbsp;Trades: '+s.tt;
me.innerHTML=mh;}
const la=document.getElementById('flad_'+c.sym);if(la){const bb=s.book.bestBid(),ba=s.book.bestAsk();
const mid=bb!==null&&ba!==null?Math.floor((bb+ba)/2):(bb||ba||1000);const R=5;let mq=1;
for(let p=mid-R;p<=mid+R;p++){const bq=s.book.sizeAt('buy',p),aq=s.book.sizeAt('sell',p);if(bq>mq)mq=bq;if(aq>mq)mq=aq;}
let lh='';for(let p=mid+R;p>=mid-R;p--){const bq=s.book.sizeAt('buy',p),aq=s.book.sizeAt('sell',p);
const bw=Math.round(bq/mq*100),aw=Math.round(aq/mq*100);const pc=p==bb?'price-col bb':p==ba?'price-col ba':'price-col';
lh+='<div class="lrow"><div class="bar-cell bid"><div class="bid-bar" style="width:'+bw+'%"></div></div><div class="bid-qty">'+(bq||'')+'</div><div class="'+pc+'">'+p+'</div><div class="ask-qty">'+(aq||'')+'</div><div class="bar-cell"><div class="ask-bar" style="width:'+aw+'%"></div></div></div>';}
la.innerHTML=lh;}
const lo=document.getElementById('flog_'+c.sym);if(lo){let lh='';for(const e of s.evts.slice(-15)){
const cl=e.side==='buy'?'var(--green)':'var(--red)';
if(e.qty>0)lh+='<div><span style="color:'+cl+'">'+e.side.toUpperCase()+'</span> '+e.qty+' @ '+e.tick+'</div>';
else lh+='<div style="color:var(--dim)">CXL @ '+e.tick+'</div>';}
for(const t of s.tds.slice(-3)){lh+='<div><span style="color:var(--yellow)">FILL</span> '+t.qty+' @ '+t.price+'</div>';}
lo.innerHTML=lh;lo.scrollTop=lo.scrollHeight;}}
const ag=document.getElementById('feedAgg');if(ag)ag.innerHTML='<span style="color:var(--green)">'+tE.toFixed(0)+'</span> total events/s &nbsp;|&nbsp; <span style="color:var(--yellow)">'+tT.toFixed(0)+'</span> total fills/s &nbsp;|&nbsp; '+ac+'/'+FEED_COINS.length+' connected';}

// ========= ANIMATION =========
function animate(){if(simOn){soloStep();sampleAnalytics();}renderSolo();if(mOn){multiStep();renderMulti();}stepFeeds();renderExplorer();renderLiveFeeds();renderAnalytics();requestAnimationFrame(animate);}
animate();buildFeedGrid();
</script></div></body></html>
)==");
std::fclose(f);}
} // namespace
int main(int argc,char*argv[]){
const char*out="report.html";if(argc>1)out=argv[1];
std::printf("\n  Generating report...\n  C++ benchmark (%zuk ops)...",kOps/1000);std::fflush(stdout);
const auto cal=TscCalibration::measure();auto d=collect(cal);
std::printf(" done\n");write_report(out,d);
std::printf("  Written to: %s\n  Open in browser.\n\n",out);}
