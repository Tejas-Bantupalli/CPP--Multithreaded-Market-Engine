import itertools,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
binary=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/'build/strategy_lab'
seen={}
for topology,book,inc,heap,profile in itertools.product(['inline','pipeline','cached'],['dense','tree'],[0,1],[0,1],['dense','sparse']):
    cmd=[str(binary),'--count','5000','--topology',topology,'--book',book,'--incremental',str(inc),'--heap',str(heap),'--profile',profile]
    r=json.loads(subprocess.check_output(cmd,text=True,timeout=20));assert r['correct']
    key=profile;value=(r['checksum'],r['orders'])
    if key in seen:assert seen[key]==value
    seen[key]=value
for topology in ['inline','pipeline','cached']:
    r=json.loads(subprocess.check_output([str(binary),'--topology',topology,'--count','5000','--rate','50000'],text=True,timeout=20))
    assert r['correct']
print('48 combinations and 3 paced paths match reference; identical decisions across architecture variants')
