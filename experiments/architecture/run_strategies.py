#!/usr/bin/env python3
import argparse,datetime,hashlib,itertools,json,os,random,subprocess,time
from pathlib import Path
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
def main():
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--repeats',type=int,default=5)
    a=p.parse_args();out=Path(a.out).resolve();out.mkdir(parents=True,exist_ok=False)
    # Full factorial on replayed sparse-book traffic; paced contrasts then test
    # topology independently for both the reference and combined optimized path.
    cells=[dict(topology=t,book=b,incremental=i,heap=h,profile='sparse',rate=0,count=200000)
           for t,b,i,h in itertools.product(['inline','pipeline','cached'],['dense','tree'],[0,1],[0,1])]
    for t in ['inline','pipeline','cached']:
        for b,i,h in [('tree',0,1),('dense',1,0)]:
            cells.append(dict(topology=t,book=b,incremental=i,heap=h,profile='sparse',rate=50000,count=25000))
    for b in ['tree','dense']:
        cells.append(dict(topology='inline',book=b,incremental=1,heap=0,profile='dense',rate=0,count=200000))
    files=[HERE/x for x in ['strategy_bench.cpp','queues.h','common.h','strategy_tests.py','run_strategies.py']]+[ROOT/'include/spsc_queue.h',ROOT/'build/strategy_lab']
    cfg={'cells':cells,'repeats':a.repeats,'sample_stride':257,'affinity':'unpinned; user-initiated QoS on macOS',
         'compile':'c++ -O2 -std=c++17 -pthread -Wall -Wextra -Iinclude experiments/architecture/strategy_bench.cpp -o build/strategy_lab',
         'sha256':{str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}}
    (out/'config.json').write_text(json.dumps(cfg,indent=2)+'\n')
    for repeat in range(a.repeats):
        order=list(cells);random.Random(157+repeat).shuffle(order)
        for cell in order:
            cmd=[str(ROOT/'build/strategy_lab'),'--seed',str(repeat+1)]
            for k,v in cell.items():cmd+=['--'+k,str(v)]
            stamp=datetime.datetime.now(datetime.timezone.utc).isoformat();load=os.getloadavg()
            proc=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
            if proc.returncode:raise RuntimeError(proc.stdout+proc.stderr)
            r=json.loads(proc.stdout);assert r['correct']
            row={'repeat':repeat,'cell':cell,'command':cmd,'started_utc':stamp,'load_before':load,'load_after':os.getloadavg(),'report':r}
            with(out/'raw.jsonl').open('a')as f:f.write(json.dumps(row)+'\n')
            print(repeat,cell,'correct',flush=True);time.sleep(.1)
if __name__=='__main__':main()
