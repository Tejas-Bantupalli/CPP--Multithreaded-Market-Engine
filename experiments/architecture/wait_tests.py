import itertools,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
binary=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/'build/wait_lab'
for mode,n,burst in itertools.product(['spin','yield','atomic','hybrid','cv'],[1,4,12],[1,32]):
    cmd=[str(binary),'--mode',mode,'--n',str(n),'--burst',str(burst),'--count','1000','--rate','20000']
    r=json.loads(subprocess.check_output(cmd,text=True,timeout=20));assert r['errors']==0 and r['received']==n*1000
for mode in ['spin','yield','atomic','hybrid','cv']:
    r=json.loads(subprocess.check_output([str(binary),'--mode',mode,'--n','4','--count','100000','--rate','0'],text=True,timeout=30))
    assert r['errors']==0 and r['received']==400000
print('30 paced/burst configurations and 5 saturation/wraparound runs completed with exact FIFO delivery')
