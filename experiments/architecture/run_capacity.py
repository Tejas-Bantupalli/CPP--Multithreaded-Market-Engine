#!/usr/bin/env python3
import argparse,datetime,hashlib,json,os,random,subprocess,time
from pathlib import Path
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
def main():
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--repeats',type=int,default=5)
    a=p.parse_args();out=Path(a.out).resolve();out.mkdir(parents=True,exist_ok=False)
    cells=[dict(capacity=c,first=f,second=256,topology='pipeline',count=100000)for c in [2,16,128,1024]for f in [0,256]]
    files=[HERE/x for x in ['pipeline_capacity.cpp','queues.h','common.h','run_capacity.py']]+[ROOT/'build/pipeline_capacity']
    cfg={'cells':cells,'repeats':a.repeats,'notes':'Storage capacity includes one reserved slot; no drops, full producers retry.',
         'sha256':{str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest()for f in files}}
    (out/'config.json').write_text(json.dumps(cfg,indent=2)+'\n')
    for repeat in range(a.repeats):
        order=list(cells);random.Random(6207+repeat).shuffle(order)
        for cell in order:
            cmd=[str(ROOT/'build/pipeline_capacity'),str(cell['capacity'])]
            for k,v in cell.items():
                if k!='capacity':cmd+=['--'+k,str(v)]
            stamp=datetime.datetime.now(datetime.timezone.utc).isoformat();load=os.getloadavg()
            proc=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
            if proc.returncode:raise RuntimeError(proc.stdout+proc.stderr)
            r=json.loads(proc.stdout);assert r['correct'] and r['usable_capacity']==cell['capacity']-1
            row={'repeat':repeat,'cell':cell,'command':cmd,'started_utc':stamp,'load_before':load,'load_after':os.getloadavg(),'report':r}
            with(out/'raw.jsonl').open('a')as f:f.write(json.dumps(row)+'\n')
            print(repeat,cell,'correct',flush=True);time.sleep(.1)
if __name__=='__main__':main()
