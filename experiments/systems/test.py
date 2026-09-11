#!/usr/bin/env python3
"""Correctness checks, including saturation/wraparound and explicit message loss."""
import json
from pathlib import Path
import subprocess
import sys
import run

binary=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else run.ROOT/'build/systems_bench'
cases=run.matrix()
for cell in cases:
    if cell['mode']=='engine': continue
    cmd=[str(binary),'--count','128','--rate','10000']
    for k in ('mode','n','transport','wait','fanout','batch','sort','burst'):
        if k in cell: cmd += ['--'+k,str(cell[k])]
    result=json.loads(subprocess.check_output(cmd,text=True,timeout=20))
    assert not run.inspect_micro(result,cell,128), cell
# Force more than one ring wrap and verify strict ordering/accounting even if
# the producer overruns capacity. Loss must remain explicit, never a fast "pass".
for transport in ('spsc','spsc_unpadded','mutex','mutex_cv'):
    cell={'mode':'pair','n':1}
    cmd=[str(binary),'--mode','pair','--transport',transport,'--count','100000','--rate','0']
    r=json.loads(subprocess.check_output(cmd,text=True,timeout=20))
    assert r['accounting_ok'] and r['errors']==0
    assert r['accepted']+r['drops']==100000
    if r['drops']: assert 'message loss' in run.inspect_micro(r,cell,100000)
cmd=[str(binary),'--mode','fanout','--fanout','multicast','--n','4','--count','100000','--rate','0']
r=json.loads(subprocess.check_output(cmd,text=True,timeout=20))
assert r['accounting_ok'] and r['errors']==0 and r['received']+r['lapped']==400000
assert run.inspect_micro(dict(r,errors=1),{'mode':'fanout','n':4},100000)
assert run.inspect_micro(dict(r,accounting_ok=False),{'mode':'fanout','n':4},100000)
print('All micro configurations, wraparound, loss-accounting and validation rejection checks passed')
