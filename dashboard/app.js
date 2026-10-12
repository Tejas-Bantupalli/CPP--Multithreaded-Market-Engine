'use strict';
const $ = s => document.querySelector(s);
const escapeHTML = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const money = n => Number.isFinite(n) ? `${n < 0 ? '−' : ''}$${Math.abs(n).toLocaleString(undefined,{minimumFractionDigits:2,maximumFractionDigits:2})}` : '—';
const count = n => Number(n || 0).toLocaleString();
const COLORS = ['#207e6b','#719cbc','#c49f5d','#9b82b0','#c87870','#88a05d','#5fa6ac','#a69b88'];
const DESCRIPTIONS = {exp1_info:'Does access to market and rival feedback change how agents adapt?',exp2_persona:'Different trading instincts. The same simulated market.',exp3_code:'Agents move beyond parameters and write their own strategies.',exp4_model:'Different models, a shared strategy catalogue and evaluation.',exp5_freecode:'Unconstrained strategy design, tested against a fixed market.',exp6_latency:'A short pilot connecting strategy decisions to execution latency.',exp7_hft:'Agents take control of their transport and event loops.',exp8_hft_scratch:'Building the full trading decision path from scratch.',exp9_field_a:'Trading performance and self-forecasting in an all-Haiku field.',exp9_field_b:'Trading performance and self-forecasting in an all-Sonnet field.',exp9_field_c:'A mixed-model field with recorded forecasts and outcomes.'};
let config, runs = [], filter = 'all', selected = null, detailCache = null, apiKey = '', model = '', loading = false;
let strategies = [
 {name:'cautious',mode:'ai',instruction:'Prefer consistency, conservative inventory limits and wider spreads.',builtin:'mm',source:''},
 {name:'adaptive',mode:'ai',instruction:'Adapt spreads to recent volatility and adverse selection.',builtin:'mm',source:''},
 {name:'inventory',mode:'ai',instruction:'Skew quotes to actively manage inventory and reduce directional exposure.',builtin:'mm',source:''},
 {name:'aggressive',mode:'ai',instruction:'Compete for fills with tight quotes, but enforce an explicit position limit.',builtin:'mm',source:''}
];
async function api(path, data) {
 const response = await fetch(path, data === undefined ? {} : {method:'POST',headers:{'Content-Type':'application/json','X-Dashboard-Token':config.token},body:JSON.stringify(data)});
 const result = await response.json();
 if (!response.ok) throw new Error(result.error || 'The request failed.');
 return result;
}
function toast(message) { $('#toast').textContent=message; $('#toast').hidden=false; clearTimeout(toast.timer);toast.timer=setTimeout(()=>$('#toast').hidden=true,5500); }
function badge(status) { return `<span class="badge ${escapeHTML(status)}">${status==='running'?'● ':''}${escapeHTML(status === 'completed' ? 'Complete' : status.charAt(0).toUpperCase()+status.slice(1))}</span>`; }
function metric(label,value,note,icon='↗') { return `<div class="metric"><span class="metric-icon">${icon}</span><p class="metric-label">${label}</p><p class="metric-value">${value}</p><p class="metric-note">${note}</p></div>`; }
function spark(values,color='#388c75') {
 if (!values?.length) return '<div class="spark empty">Results will appear after the first generation.</div>';
 let lo=Math.min(...values), hi=Math.max(...values); const span=hi-lo || 1; lo-=span*.15;hi+=span*.15;
 const points=values.map((v,i)=>`${10+i*280/Math.max(1,values.length-1)},${55-(v-lo)/(hi-lo)*44}`);
 return `<svg class="spark" viewBox="0 0 300 65" preserveAspectRatio="none" aria-label="Recorded mean PnL by generation" role="img"><path d="M0 60H300" stroke="#eef2f1" fill="none"/><path d="M${points.join(' L')}" stroke="${color}" stroke-width="1.7" fill="none" vector-effect="non-scaling-stroke"/>${points.map(p=>`<circle cx="${p.split(',')[0]}" cy="${p.split(',')[1]}" r="2.3" fill="${color}"/>`).join('')}</svg>`;
}
function show(view) {
 for (const name of ['library','create','detail']) $(`#${name}-view`).hidden=name!==view;
 $('#breadcrumb').textContent=view==='library'?'Experiments':view==='create'?'New experiment':'Experiment results';
 $('#nav-experiments').classList.toggle('active',view!=='create');$('#nav-new').classList.toggle('active',view==='create');
}
function renderLibrary() {
 $('#run-count').textContent=runs.length;
 const generations=runs.reduce((n,r)=>n+r.evaluated,0), live=runs.filter(r=>['queued','running'].includes(r.status)).length;
 $('#overview').innerHTML=metric('Saved experiments',count(runs.length),'Your local research library','◫')+metric('Evaluated generations',count(generations),'Recorded, across all experiments','≋')+metric('Active experiments',count(live),live?'Live progress available':'Ready for your next hypothesis','◷')+metric('Execution environment','C++17','Native engine · macOS','⌁');
 const query=$('#search').value.toLowerCase();
 const visible=runs.filter(r=>(filter==='all'||(filter==='control')===(r.kind==='Control'))&&`${r.title} ${r.id}`.toLowerCase().includes(query));
 $('#experiment-grid').innerHTML=visible.map(r=>`<button class="experiment-card" data-run="${r.id}"><div class="card-top"><span class="card-id">${r.local?'LOCAL RUN':escapeHTML(r.id.replaceAll('_',' / '))}</span>${badge(r.status)}</div><h3>${escapeHTML(r.title)}</h3><p class="card-description">${escapeHTML(DESCRIPTIONS[r.id] || (r.kind==='Control'?'A non-model baseline for interpreting adaptive strategies.':`${r.subjects} competing strategies. ${r.generations} generations of evidence.`))}</p>${spark(r.curve,r.kind==='Control'?'#94a5ae':'#388c75')}<div class="card-bottom"><span><strong>${r.evaluated}</strong> generations · <strong>${r.subjects}</strong> strategies</span><span class="arrow">↗</span></div><p class="field-note" style="margin-top:8px">${r.leader?'Curve: '+escapeHTML(r.leader)+' · highest final mean PnL':'Awaiting evaluation'}</p></button>`).join('') || '<p class="empty">No experiments match this view.</p>';
}
function renderStrategies() {
 $('#strategy-list').innerHTML=strategies.map((s,i)=>`<div class="strategy-row" data-index="${i}"><div class="row-head"><span class="strategy-number">${String(i+1).padStart(2,'0')}</span><input class="name" aria-label="Strategy ${i+1} name" value="${escapeHTML(s.name)}" maxlength="32" required><select class="mode" aria-label="Strategy ${i+1} type">${[['ai','✧ AI-generated'],['custom','Custom C++'],['builtin','Built-in control']].map(([v,l])=>`<option value="${v}" ${s.mode===v?'selected':''}>${l}</option>`).join('')}</select><button type="button" class="icon-button remove" aria-label="Remove ${escapeHTML(s.name)}">×</button></div>${s.mode==='ai'?`<textarea class="instruction" rows="2" aria-label="Mandate for ${escapeHTML(s.name)}" placeholder="Give this agent its own approach…">${escapeHTML(s.instruction)}</textarea>`:s.mode==='builtin'?`<select class="builtin" aria-label="Built-in strategy">${[['mm','Market maker'],['noise','Noise trader'],['momentum','Momentum'],['meanrev','Mean reversion']].map(([v,l])=>`<option value="${v}" ${s.builtin===v?'selected':''}>${l}</option>`).join('')}</select>`:`<textarea class="code-editor" spellcheck="false" aria-label="C++ source for ${escapeHTML(s.name)}" placeholder='#include "plugin.h"\n// Define your Strategy and end with MARKET_PLUGIN(ClassName)'>${escapeHTML(s.source)}</textarea><label class="upload-label">Upload a .cpp file<input type="file" class="upload" accept=".cpp,.cc,.cxx"></label><button class="text-button example" type="button" style="margin-top:10px">Use example breakout strategy</button>`}</div>`).join('');
 $('#add-strategy').disabled=strategies.length>=8;
}
function updateConnection() {
 const ready=!!(apiKey || config?.key_configured);
 $('#connection-dot').classList.toggle('connected',ready);
 $('#model-summary').textContent=ready?`${model} · key configured. Connection is checked on the first request.`:'No API key configured. Built-in and custom C++ strategies work without a model.';
 $('#credential-note').textContent=config?.key_configured?'ANTHROPIC_API_KEY is available on the server. An entered key overrides it for this run only.':'Keys stay in memory for this page and active run. They are never saved in experiment files.';
}
function openConnection() { $('#model-id').value=model;$('#api-key').value='';$('#connection-dialog').showModal(); }
function newExperiment() { selected=null; location.hash='new';show('create');renderStrategies(); }
function chart(rows) {
 if (!rows.length) return '<p class="empty">The first curve appears when a generation completes. Compilation and per-seed progress are shown above.</p>';
 const names=[...new Set(rows.map(r=>r.subject))], values=rows.map(r=>r.score), maxGen=Math.max(...rows.map(r=>r.generation));
 let low=Math.min(0,...values),high=Math.max(0,...values),range=high-low||1;low-=range*.12;high+=range*.12;
 const width=700,height=250,left=64,right=20,top=17,bottom=30;
 const x=g=>left+g*(width-left-right)/Math.max(1,maxGen), y=v=>top+(high-v)*(height-top-bottom)/(high-low);
 let svg=`<svg class="chart" viewBox="0 0 ${width} ${height}" role="img" aria-label="Mean PnL by generation, one line per strategy">`;
 for(let i=0;i<5;i++){const v=low+(high-low)*i/4;svg+=`<path d="M${left} ${y(v)}H${width-right}" stroke="#edf1f2"/><text x="${left-10}" y="${y(v)+3}" text-anchor="end" fill="#96a2a7" font-size="9" font-family="monospace">${Math.round(v).toLocaleString()}</text>`;}
 for(let g=0;g<=maxGen;g++){svg+=`<text x="${x(g)}" y="${height-7}" text-anchor="middle" fill="#96a2a7" font-size="9" font-family="monospace">G${g+1}</text>`;}
 svg+=`<path d="M${left} ${y(0)}H${width-right}" stroke="#cbd9d4" stroke-dasharray="3 4"/>`;
 names.forEach((name,i)=>{const points=rows.filter(r=>r.subject===name).sort((a,b)=>a.generation-b.generation);const color=COLORS[i%COLORS.length];svg+=`<path d="${points.map((r,j)=>`${j?'L':'M'}${x(r.generation)} ${y(r.score)}`).join(' ')}" stroke="${color}" stroke-width="2" fill="none"/>`;svg+=points.map(r=>`<circle cx="${x(r.generation)}" cy="${y(r.score)}" r="3" fill="${color}"><title>${escapeHTML(name)} · G${r.generation+1}: ${money(r.score)}${r.ok===false?' (fallback)':''}</title></circle>`).join('');});
 return svg+'</svg><div class="legend">'+names.map((n,i)=>`<span><i style="background:${COLORS[i%COLORS.length]}"></i>${escapeHTML(n)}</span>`).join('')+'</div>';
}
function renderDetail(d) {
 const state=d.state||{}, active=['queued','running'].includes(d.status), lastGen=Math.max(-1,...d.rows.map(r=>r.generation));
 const latest=d.rows.filter(r=>r.generation===lastGen), engine=d.latest.summary?.engine, names=[...new Set(d.rows.map(r=>r.subject))];
 const best=latest.length?Math.max(...latest.map(r=>r.score)):null;
 const total=d.rows.length, failed=d.rows.filter(r=>r.ok===false).length;
 const pct=state.sessions_total?Math.round(100*state.sessions_done/state.sessions_total):0;
 const download=kind=>`/api/runs/${d.id}/download?kind=${kind}`;
 const sourceFiles=d.files.filter(f=>f.endsWith('.cpp')&&!f.includes('attempt_'));
 $('#detail-content').innerHTML=`<div class="page-heading"><div><p class="eyebrow">${d.local?'LOCAL EXPERIMENT':'RECORDED RESEARCH'} / ${escapeHTML(d.id)}</p><h1 class="detail-title">${escapeHTML(d.title)}</h1><p class="subtitle">${d.subjects} strategies · ${d.seeds} seeds per generation · ${d.seconds}s sessions &nbsp; ${badge(d.status)}</p></div><div class="detail-actions"><button class="secondary" data-back>← Library</button><a class="primary" href="${download('all')}">↓ Download run</a></div></div>
 ${active?`<div class="progress-panel"><div class="progress-head"><span>${escapeHTML(state.phase||'Starting')} · ${state.sessions_done||0}/${state.sessions_total||0} sessions</span><button class="text-button" id="cancel-run">Stop run</button></div><div class="progress-track"><div class="progress-fill" style="width:${pct}%"></div></div></div>`:''}
 ${state.error?`<p class="error" style="margin-bottom:22px">${escapeHTML(state.error)}</p>`:''}
 <div class="overview-grid">${metric('Best final mean PnL',money(best),'Among subject strategies','↗')}${metric('Evaluated generations',`${d.evaluated}<span style="font-size:16px;color:#b2bcc0"> / ${d.generations}</span>`,'Each measured across several seeds','≋')}${metric('Matching p99',engine?`${(engine.match_p99_ns/1000).toFixed(1)}<span style="font-size:13px"> µs</span>`:'—','Latest generation · median across seeds','⌁')}${metric('Evaluated submissions',count(total),`${failed} recorded failure${failed===1?'':'s'} / fallback${failed===1?'':'s'}`,'◫')}</div>
 <div class="detail-grid"><div><section class="panel"><div class="panel-title"><h2>How strategies evolved</h2><span class="tag">MEAN PNL / USD</span></div>${chart(d.rows)}<p class="field-note" style="margin-top:16px">Per-generation outcomes, not cumulative returns. Compare the spread and hit rate as well as the mean.</p></section>
 <section class="panel"><div class="panel-title"><h2>Latest generation</h2><span class="tag">${lastGen>=0?'GENERATION '+(lastGen+1):'AWAITING RESULTS'}</span></div><div class="table-scroll"><table><thead><tr><th>Strategy</th><th>Mean PnL</th><th>Std dev</th><th>Hit rate</th><th>Fills</th><th>Result</th></tr></thead><tbody>${latest.map(r=>`<tr><td><i class="row-dot" style="background:${COLORS[names.indexOf(r.subject)%COLORS.length]}"></i>${escapeHTML(r.subject)}</td><td class="numeric ${r.score>=0?'positive':'negative'}">${money(r.score)}</td><td class="numeric">${money(r.std)}</td><td class="numeric">${(r.hit_rate*100).toFixed(0)}%</td><td class="numeric">${count(Math.round(r.fills))}</td><td>${r.ok===false?'<span class="badge failed">Fallback</span>':'<span class="badge">Evaluated</span>'}</td></tr>`).join('')}</tbody></table></div>${!latest.length?'<p class="empty">Waiting for the first completed generation.</p>':''}</section>
 <section class="panel"><div class="panel-title"><h2>Activity log</h2><span class="tag">${active?'LIVE':'RECORDED'}</span></div><pre class="log-output">${escapeHTML(d.log)}</pre></section>
 </div><aside class="detail-aside"><div><section class="panel"><div class="panel-title"><h2>Experiment setup</h2></div><dl class="detail-facts"><div><dt>Feedback</dt><dd>${escapeHTML(d.config.info||d.config.subjects?.[0]?.info||'Per agent')}</dd></div><div><dt>Background</dt><dd>${d.config.background?.length||4} traders</dd></div><div><dt>Model</dt><dd>${escapeHTML(d.config.model||d.config.subjects?.[0]?.model||'Per agent')}</dd></div><div><dt>Evaluation</dt><dd>${d.local?'Serial seeds':'Recorded run'}</dd></div></dl>${d.config.prompt?`<p class="brief-display" style="margin-top:18px">${escapeHTML(d.config.prompt)}</p>`:''}</section>
 <section class="panel"><div class="panel-title"><h2>Export evidence</h2></div><a class="download-link" href="${download('results')}">Results & configuration <span>↓ ZIP</span></a><a class="download-link" href="${download('sources')}">Strategies & briefs <span>↓ ZIP</span></a>${d.files.some(f=>f.endsWith('.csv')||f.endsWith('.log'))?`<a class="download-link" href="${download('logs')}">Logs & CSV files <span>↓ ZIP</span></a>`:'<p class="field-note" style="margin-top:15px">No raw logs were captured for this run.</p>'}</section></div>
 <section class="panel"><div class="panel-title"><h2>Strategy sources</h2><span class="tag">${sourceFiles.length} FILES</span></div><div class="file-list">${sourceFiles.map(f=>`<button class="file-button" data-file="${escapeHTML(f)}">${escapeHTML(f.replace('/strategy.cpp',''))} ↗</button>`).join('')||'<p class="field-note">This run uses catalogue parameters, or source generation is still in progress.</p>'}</div><button class="text-button" data-file="config.json" style="margin-top:17px">View configuration ↗</button></section></aside></div>`;
}
async function openRun(id) {
 selected=id;show('detail');$('#detail-content').innerHTML='<p class="empty">Opening recorded evidence…</p>';
 try {const d=await api(`/api/runs/${id}`);if(selected!==id)return;detailCache=JSON.stringify(d);renderDetail(d);}catch(e){$('#detail-content').textContent=e.message;}
}
async function refresh() {
 if (loading) return;loading=true;
 try{runs=await api('/api/runs');renderLibrary();if(selected){const id=selected,d=await api(`/api/runs/${id}`),value=JSON.stringify(d);if(selected===id&&value!==detailCache){detailCache=value;renderDetail(d);}}}catch(e){toast(e.message);}finally{loading=false;}
}
function route() {const hash=decodeURIComponent(location.hash.slice(1));if(hash==='new'){selected=null;show('create');renderStrategies();}else if(hash.startsWith('run/'))openRun(hash.slice(4));else{selected=null;show('library');renderLibrary();}}
$('#new-experiment').onclick=newExperiment;$('#nav-new').onclick=newExperiment;
$('#nav-experiments').onclick=$('#back-library').onclick=()=>{location.hash='';selected=null;show('library');};
$('#nav-connection').onclick=$('#configure-model').onclick=openConnection;
$('#close-connection').onclick=()=>$('#connection-dialog').close();$('#close-source').onclick=()=>$('#source-dialog').close();
$('#connection-form').onsubmit=e=>{e.preventDefault();model=$('#model-id').value.trim();if($('#api-key').value.trim())apiKey=$('#api-key').value.trim();$('#api-key').value='';updateConnection();$('#connection-dialog').close();toast('Connection settings held in memory.');};
$('#library-tabs').onclick=e=>{const b=e.target.closest('[data-filter]');if(!b)return;filter=b.dataset.filter;for(const tab of $('#library-tabs').children)tab.classList.toggle('selected',tab===b);renderLibrary();};
$('#search').oninput=renderLibrary;
$('#experiment-grid').onclick=e=>{const card=e.target.closest('[data-run]');if(card)location.hash=`run/${card.dataset.run}`;};
$('#add-strategy').onclick=()=>{if(strategies.length<8){let i=1;while(strategies.some(s=>s.name===`agent_${i}`))i++;strategies.push({name:`agent_${i}`,mode:'ai',instruction:'',builtin:'mm',source:''});renderStrategies();}};
$('#strategy-list').addEventListener('input',e=>{const row=e.target.closest('[data-index]');if(!row)return;const s=strategies[+row.dataset.index];if(e.target.matches('.name'))s.name=e.target.value;if(e.target.matches('.instruction'))s.instruction=e.target.value;if(e.target.matches('.code-editor'))s.source=e.target.value;});
$('#strategy-list').addEventListener('change',async e=>{const row=e.target.closest('[data-index]');if(!row)return;const s=strategies[+row.dataset.index];if(e.target.matches('.mode')){s.mode=e.target.value;renderStrategies();}if(e.target.matches('.builtin'))s.builtin=e.target.value;if(e.target.matches('.upload')){const f=e.target.files[0];if(!f)return;if(f.size>100000){toast('Source files must be under 100 KB.');return;}s.source=await f.text();renderStrategies();}});
$('#strategy-list').onclick=e=>{const row=e.target.closest('[data-index]');if(!row)return;const i=+row.dataset.index;if(e.target.closest('.remove')){if(strategies.length===1){toast('Keep at least one strategy.');return;}strategies.splice(i,1);renderStrategies();}if(e.target.closest('.example')){strategies[i].source=config.example;renderStrategies();}};
for(const name of ['generations','seeds'])$(`#${name}`).oninput=()=>$('#session-estimate').textContent=Number($('#generations').value)*Number($('#seeds').value);
$('#experiment-form').onsubmit=async e=>{e.preventDefault();$('#form-error').hidden=true;$('#launch').disabled=true;try{const result=await api('/api/runs',{title:$('#experiment-title').value,prompt:$('#prompt').value,model,api_key:apiKey,generations:Number($('#generations').value),seeds:Number($('#seeds').value),seconds:Number($('#seconds').value),info:$('#feedback').value,logs:$('#capture-logs').checked,subjects:strategies});location.hash=`run/${result.id}`;await refresh();}catch(err){$('#form-error').textContent=err.message;$('#form-error').hidden=false;}finally{$('#launch').disabled=false;}};
$('#detail-content').onclick=async e=>{if(e.target.closest('[data-back]'))location.hash='';const file=e.target.closest('[data-file]');if(file){try{const result=await api(`/api/runs/${selected}/file?path=${encodeURIComponent(file.dataset.file)}`);$('#source-title').textContent=result.path;$('#source-code').textContent=result.text;$('#source-dialog').showModal();}catch(err){toast(err.message);}}if(e.target.closest('#cancel-run')){try{await api(`/api/runs/${selected}/cancel`,{});toast('Stopping. An in-flight model request may take a moment to finish.');}catch(err){toast(err.message);}}};
window.addEventListener('hashchange',route);
(async()=>{try{config=await api('/api/config');model=config.model;updateConnection();await refresh();route();setInterval(refresh,2500);}catch(e){$('#experiment-grid').textContent=e.message;}})();
