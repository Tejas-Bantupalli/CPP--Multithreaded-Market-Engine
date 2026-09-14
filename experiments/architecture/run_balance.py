#!/usr/bin/env python3
import argparse,datetime,hashlib,json,os,random,subprocess,time
from pathlib import Path
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
def main():
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--repeats',type=int,default=5)
    a=p.parse_args();out=Path(a.out).resolve();out.mkdir(parents=True,exist_ok=False)
    cells=[dict(first=f,second=s,topology=t,count=100000)for f,s in [(0,0),(8,8),(64,64),(256,256),(1024,1024),(0,256),(256,0)]for t in ['inline','pipeline']]
    files=[HERE/x for x in ['pipeline_balance.cpp','queues.h','common.h','run_balance.py']]+[ROOT/'build/pipeline_balance']
    cfg={'cells':cells,'repeats':a.repeats,'notes':'Synthetic dependent hash work, not a protocol parser or trading algorithm',
         'sha256':{str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest()for f in files}}
    (out/'config.json').write_text(json.dumps(cfg,indent=2)+'\n')
    for repeat in range(a.repeats):
        order=list(cells);random.Random(4407+repeat).shuffle(order)
        for cell in order:
            cmd=[str(ROOT/'build/pipeline_balance')]
            for k,v in cell.items():cmd+=['--'+k,str(v)]
            stamp=datetime.datetime.now(datetime.timezone.utc).isoformat();load=os.getloadavg()
            proc=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
            if proc.returncode:raise RuntimeError(proc.stdout+proc.stderr)
            r=json.loads(proc.stdout);assert r['correct']
            row={'repeat':repeat,'cell':cell,'command':cmd,'started_utc':stamp,'load_before':load,'load_after':os.getloadavg(),'report':r}
            with(out/'raw.jsonl').open('a')as f:f.write(json.dumps(row)+'\n')
            print(repeat,cell,'correct',flush=True);time.sleep(.1)
if __name__=='__main__':main()
