#!/usr/bin/env python3
import argparse,datetime,hashlib,json,os,platform,random,subprocess,time
from pathlib import Path
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
def hardware():
    def query(name):
        try:return subprocess.check_output(['sysctl','-n',name],text=True,stderr=subprocess.DEVNULL).strip()
        except (OSError,subprocess.CalledProcessError):return None
    line=query('hw.cachelinesize') if platform.system()=='Darwin' else None
    physical=query('hw.physicalcpu') if platform.system()=='Darwin' else None
    model=query('hw.model') if platform.system()=='Darwin' else platform.machine()
    if line is None and platform.system()=='Linux':
        try:line=Path('/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size').read_text().strip()
        except OSError:pass
    return {'cache_line_bytes':int(line) if line else None,'physical_cores':int(physical) if physical else None,
            'logical_cpus':os.cpu_count(),'model':model,'source':'runtime hardware query; null means unavailable'}
def main():
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--repeats',type=int,default=5)
    a=p.parse_args();out=Path(a.out).resolve();out.mkdir(parents=True,exist_ok=False)
    paths=[HERE/x for x in ['queue_bench.cpp','queues.h','common.h','queue_tests.cpp','run_queues.py']]+[ROOT/'include/spsc_queue.h',ROOT/'build/queue_lab']
    cfg={'repeats':a.repeats,'hardware_observation':hardware(),'affinity':'unpinned; user-initiated QoS on macOS','sha256':{str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in paths},'compiler':subprocess.check_output(['c++','--version'],text=True)}
    (out/'config.json').write_text(json.dumps(cfg,indent=2)+'\n')
    cells=[(align,cached,rate)for align in [0,8,64,128]for cached in [0,1]for rate in [0,50000]if not(align==0 and cached)]
    for repeat in range(a.repeats):
        order=list(cells);random.Random(1307+repeat).shuffle(order)
        for align,cached,rate in order:
            count=500000 if rate==0 else 50000
            cmd=[str(ROOT/'build/queue_lab'),'--align',str(align),'--cached',str(cached),'--rate',str(rate),'--count',str(count)]
            stamp=datetime.datetime.now(datetime.timezone.utc).isoformat();load=os.getloadavg()
            proc=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
            if proc.returncode:raise RuntimeError(proc.stdout+proc.stderr)
            report=json.loads(proc.stdout);assert report['errors']==0
            row={'repeat':repeat,'started_utc':stamp,'load_before':load,'load_after':os.getloadavg(),'command':cmd,'report':report}
            with(out/'raw.jsonl').open('a')as f:f.write(json.dumps(row)+'\n')
            print(repeat,align,cached,rate,'valid',flush=True);time.sleep(.1)
if __name__=='__main__':main()
