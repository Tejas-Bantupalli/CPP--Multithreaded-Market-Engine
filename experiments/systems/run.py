#!/usr/bin/env python3
"""Run systems comparisons serially; save every session and machine-load observation."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import sweep

def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def matrix():
    cells = []
    for n in (1, 4, 12):
        for transport, wait in [('spsc','yield'), ('mutex','yield'), ('mutex_cv','yield'), ('mutex_cv','block')]:
            cells.append(dict(id=f'queue-{transport}-{wait}-{n}', family='queue', mode='pair', n=n,
                              transport=transport, wait=wait))
        for wait in ('spin','sleep'):
            cells.append(dict(id=f'wait-{wait}-{n}', family='waiting', mode='pair', n=n, wait=wait))
        cells.append(dict(id=f'padding-unpadded-{n}', family='padding', mode='pair', n=n, transport='spsc_unpadded',wait='yield'))
        for fanout in ('spsc','multicast'):
            cells.append(dict(id=f'fanout-{fanout}-{n}', family='fanout', mode='fanout', n=n, fanout=fanout))
    for n in (4,12):
        for batch in (1,16,256):
            for order in (0,1):
                cells.append(dict(id=f'orders-{n}-batch{batch}-sort{order}', family='batching', mode='orders',
                                  n=n,batch=batch,sort=order,burst=32))
    for n in (4,12):
        for transport,fanout,wait in [('spsc','multicast','yield'), ('spsc','multicast','spin'),
                                       ('spsc','spsc','yield'), ('mutex','spsc','yield'), ('mutex_cv','spsc','yield')]:
            cells.append(dict(id=f'engine-{transport}-{fanout}-{wait}-{n}',family='engine',
                              mode='engine',n=n,transport=transport,fanout=fanout,wait=wait))
    return cells

def checksum(count, producer):
    total=0
    for seq in range(count):
        total ^= ((seq*0x9e3779b97f4a7c15)&((1<<64)-1)) ^ (producer+0xabc123)
    return total

def inspect_micro(r,cell,count):
    reasons=[]
    if r['errors'] or not r['accounting_ok']: reasons.append('corrupt/out-of-order/accounting failure')
    if r['drops'] or r['lapped']: reasons.append('message loss')
    expected=count*cell['n']
    if r['received']!=expected: reasons.append('incomplete delivery')
    if not reasons:
        for c in r['consumers']:
            if cell['mode']=='orders':
                wanted=0
                for p in range(cell['n']): wanted ^= checksum(count,p)
            else: wanted=checksum(count,0 if cell['mode']=='fanout' else c['id'])
            if wanted!=c['checksum']: reasons.append('checksum mismatch')
    return reasons

def summarize(rows):
    result=[]
    for name in sorted({r['cell']['id'] for r in rows}):
        rr=[r for r in rows if r['cell']['id']==name]
        cell=rr[0]['cell']; engine=cell['mode']=='engine'
        def vals(key):
            if not engine: return [r['report'][key] for r in rr]
        entry={'id':name,'cell':cell,'sessions':len(rr),'valid_sessions':sum(r['valid'] for r in rr)}
        if not engine:
            for metric in ('scheduled_to_complete_ns','attempt_to_complete_ns','attempt_to_pop_ns','batch_wait_ns','producer_lateness_ns'):
                for percentile in ('p50','p99','p999'):
                    numbers=[v[percentile] for v in vals(metric)]
                    entry[f'{metric}_{percentile}']={'median':statistics.median(numbers),'min':min(numbers),'max':max(numbers)}
            entry['consumer_cpu_seconds_median']=statistics.median(v/1e9 for v in vals('consumer_cpu_ns'))
            entry['producer_cpu_seconds_median']=statistics.median(v/1e9 for v in vals('producer_cpu_ns'))
            entry['deliveries_per_second_median']=statistics.median(r['report']['received']/(r['report']['elapsed_ns']/1e9) for r in rr)
        else:
            for metric in ('match','submit_to_pop'):
                for percentile in ('p50','p99','p999'):
                    numbers=[r['report']['latency'][metric][percentile] for r in rr]
                    entry[f'{metric}_{percentile}']={'median':statistics.median(numbers),'min':min(numbers),'max':max(numbers)}
            numbers=[r['report']['agents'][0]['latency']['delivery']['p99'] for r in rr]
            entry['probe_delivery_p99']={'median':statistics.median(numbers),'min':min(numbers),'max':max(numbers)}
            entry['commands_per_second_median']=statistics.median(r['report']['commands_per_sec'] for r in rr)
        result.append(entry)
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',required=True)
    p.add_argument('--repeats',type=int,default=3)
    p.add_argument('--count',type=int,default=10000)
    p.add_argument('--rate',type=int,default=10000)
    p.add_argument('--engine-seconds',type=float,default=2)
    p.add_argument('--only',choices=['micro','engine'])
    args=p.parse_args()
    if args.repeats<1 or args.count<1 or args.rate<1 or args.engine_seconds<=0: p.error('positive durations/counts required')
    out=Path(args.out).resolve(); out.mkdir(parents=True,exist_ok=False)
    binary=ROOT/'build/systems_bench'; engine=ROOT/'build/market_engine'
    cells=matrix()
    if args.only: cells=[c for c in cells if (c['mode']=='engine')==(args.only=='engine')]
    paths=list((ROOT/'include').glob('*.h'))+list((ROOT/'src').glob('*.cpp'))+list(HERE.glob('*'))+[binary,engine]
    config={'arguments':vars(args),'cells':cells,'platform':platform.platform(),'machine':platform.machine(),
            'cpu_count':os.cpu_count(),'created_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'source_and_binary_sha256':{str(f.relative_to(ROOT)):digest(f) for f in paths if f.is_file()},
            'compiler':subprocess.check_output(['c++','--version'],text=True),
            'micro_qos':'user_initiated on macOS; default scheduler elsewhere','engine_qos':'initiated',
            'notes':'Serial randomized order per round; identical offered schedule; no AI; percentile summaries include invalid runs explicitly.'}
    write(out/'config.json',config)
    rows=[]
    sentinel={'id':'sentinel','family':'sentinel','mode':'pair','n':1,'transport':'spsc','wait':'yield'}
    for repeat in range(args.repeats):
        ordered=list(cells); random.Random(9081+repeat).shuffle(ordered)
        ordered=[sentinel]+ordered+[sentinel]
        for index,cell in enumerate(ordered):
            started=datetime.datetime.now(datetime.timezone.utc).isoformat(); load=os.getloadavg()
            if cell['mode']=='engine':
                specs=['mm:interval_us=50']+['noise']*(cell['n']-1)
                extra=['--transport',cell['transport'],'--fanout',cell['fanout'],'--idle',cell['wait'],'--qos','initiated']
                report=sweep.run_one(engine,repeat+1,args.engine_seconds,specs,extra=extra)
                reasons=[]
                if report['events_dropped'] or any(a['queue_full'] for a in report['agents']): reasons.append('events or orders lost')
                if report['latency']['match']['count']<100: reasons.append('insufficient samples')
                if report['fanout']!=cell['fanout'] or any(a['transport']!=cell['transport'] for a in report['agents']): reasons.append('configuration mismatch')
                command={'specs':specs,'extra':extra,'seed':repeat+1,'seconds':args.engine_seconds}
            else:
                command=[str(binary),'--count',str(args.count),'--rate',str(args.rate)]
                for key in ('mode','n','transport','wait','fanout','batch','sort','burst'):
                    if key in cell: command += ['--'+key,str(cell[key])]
                proc=subprocess.run(command,capture_output=True,text=True,timeout=max(30,args.count/args.rate+20))
                if proc.returncode:
                    (out/'failure.log').write_text(proc.stdout+proc.stderr)
                    raise RuntimeError(f'benchmark failure: {command}; see failure.log')
                report=json.loads(proc.stdout)
                reasons=inspect_micro(report,cell,args.count)
            row={'cell':cell,'repeat':repeat,'position':index,'started_utc':started,'load_before':load,
                 'load_after':os.getloadavg(),'command':command,'valid':not reasons,'invalid_reasons':reasons,'report':report}
            with (out/'raw.jsonl').open('a') as f: f.write(json.dumps(row)+'\n')
            rows.append(row)
            print(f"round {repeat+1} {index+1}/{len(ordered)} {cell['id']}: {'valid' if not reasons else reasons}",flush=True)
            time.sleep(.1)
    write(out/'summary.json',summarize(rows))
    print(f'Saved {len(rows)} sessions in {out}',flush=True)

if __name__=='__main__': main()
