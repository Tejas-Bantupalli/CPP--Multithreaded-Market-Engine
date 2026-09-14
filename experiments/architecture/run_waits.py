#!/usr/bin/env python3
import argparse,datetime,hashlib,itertools,json,os,random,subprocess,time
from pathlib import Path
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
def main():
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--repeats',type=int,default=5)
    a=p.parse_args();out=Path(a.out).resolve();out.mkdir(parents=True,exist_ok=False)
    cells=[dict(mode=m,n=n,burst=b,count=5000,rate=10000)for m,n,b in itertools.product(['spin','yield','atomic','hybrid','cv'],[1,4,12],[1,32])]
    paths=[HERE/x for x in ['wait_bench.cpp','common.h','queues.h','run_waits.py']]+[ROOT/'include/spsc_queue.h',ROOT/'include/transport.h',ROOT/'build/wait_lab']
    cfg={'cells':cells,'repeats':a.repeats,'cxx_standard':'C++20','platform_wait':'std::atomic<uint32_t>::wait/notify_one, not raw futex',
         'compile':'c++ -O2 -std=c++20 -pthread -Wall -Wextra -Iinclude experiments/architecture/wait_bench.cpp -o build/wait_lab',
         'sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()for p in paths}}
    (out/'config.json').write_text(json.dumps(cfg,indent=2)+'\n')
    for repeat in range(a.repeats):
        order=list(cells);random.Random(8107+repeat).shuffle(order)
        for cell in order:
            cmd=[str(ROOT/'build/wait_lab')]
            for k,v in cell.items():cmd+=['--'+k,str(v)]
            stamp=datetime.datetime.now(datetime.timezone.utc).isoformat();load=os.getloadavg()
            proc=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
            if proc.returncode:raise RuntimeError(proc.stdout+proc.stderr)
            r=json.loads(proc.stdout);assert r['errors']==0 and r['received']==cell['n']*cell['count']
            row={'repeat':repeat,'cell':cell,'command':cmd,'started_utc':stamp,'load_before':load,'load_after':os.getloadavg(),'report':r}
            with(out/'raw.jsonl').open('a')as f:f.write(json.dumps(row)+'\n')
            print(repeat,cell,'valid',flush=True);time.sleep(.1)
if __name__=='__main__':main()
