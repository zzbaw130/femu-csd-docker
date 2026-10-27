#!/usr/bin/env python3
"""Parse measured FEMU CACHE_BLOCK records and export CSV, summary and figures."""
import argparse
import csv
import json
import re
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p=argparse.ArgumentParser()
p.add_argument('log',type=Path)
p.add_argument('output',type=Path)
a=p.parse_args()
rows=[]
for line in a.log.read_text(errors='replace').splitlines():
    if not line.startswith('CACHE_BLOCK '):
        continue
    row={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}
    if row['dma'] and row['cache_ack']:
        if not (row['load'] <= row['cu'] <= row['release'] and row['load'] <= row['dma'] <= row['cache_ack'] <= row['release']):
            raise SystemExit('invalid timestamp ordering: '+str(row))
    rows.append(row)
if not rows:
    raise SystemExit('No measured cache blocks found')
rows.sort(key=lambda r:r['load'])
a.output.mkdir(parents=True,exist_ok=True)
with (a.output/'blocks.csv').open('w') as f:
    w=csv.DictWriter(f,fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
valid=[r for r in rows if r['cache_ack'] and r['dma']]
missing=len(rows)-len(valid)
if not valid:
    raise SystemExit('No successfully DMA-refilled blocks')
def mean(values): return sum(values)/len(values)
summary={'blocks':len(rows),'incomplete':missing,'bytes':sum(r['bytes'] for r in valid),
         'dma_after_cu':sum(r['dma']>r['cu'] for r in valid),
         'cache_confirm_after_cu':sum(r['cache_ack']>r['cu'] for r in valid),
         'mean_cu_from_load_ms':mean([(r['cu']-r['load'])/1e6 for r in valid]),
         'mean_dma_copy_ms':mean([(r['dma']-r['dma_start'])/1e6 for r in valid]),
         'mean_dma_queue_ms':mean([(r['dma_start']-r['load'])/1e6 for r in valid]),
         'mean_dma_from_load_ms':mean([(r['dma']-r['load'])/1e6 for r in valid]),
         'mean_cache_confirm_from_load_ms':mean([(r['cache_ack']-r['load'])/1e6 for r in valid]),
         'mean_extra_hold_ms':mean([max(0,r['cache_ack']-r['cu'])/1e6 for r in valid]),
         'max_extra_hold_ms':max(max(0,r['cache_ack']-r['cu'])/1e6 for r in valid)}
(a.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
fig,axes=plt.subplots(3,1,figsize=(12,10),constrained_layout=True)
colors={'cu':'#2563eb','dma':'#ea580c','cache_ack':'#16a34a'}
labels={'cu':'CU consumption finished','dma':'DMA data transfer finished','cache_ack':'Host cache valid (ACK upper bound)'}
x=list(range(len(valid)))
for field in colors:
    axes[0].plot(x,[(r[field]-r['load'])/1e6 for r in valid],'.-',lw=.6,ms=2,color=colors[field],label=labels[field])
axes[0].set(xlabel='Actual LOAD block (arrival order)',ylabel='Time since LOAD ready (ms)',title='Every block: completion order')
axes[0].legend(loc='upper right',fontsize=8)
axes[1].bar(x,[max(0,r['cache_ack']-r['cu'])/1e6 for r in valid],width=1,color='#dc2626')
axes[1].set(xlabel='Actual LOAD block (arrival order)',ylabel='Additional FDM hold (ms)',title='Backpressure after CU finished: max(cache confirmation - CU, 0)')
first=valid[0]['load']
for i,r in enumerate(valid[:24]):
    start=(r['load']-first)/1e6
    axes[2].barh(2*i,(r['cu']-r['load'])/1e6,left=start,height=.7,color=colors['cu'])
    axes[2].barh(2*i+1,(r['cache_ack']-r['load'])/1e6,left=start,height=.7,color=colors['cache_ack'])
    axes[2].scatter((r['dma']-first)/1e6,2*i+1,s=15,color=colors['dma'],marker='|')
axes[2].invert_yaxis()
axes[2].set(yticks=[2*i+.5 for i in range(min(24,len(valid)))],yticklabels=[str(i) for i in range(min(24,len(valid)))],xlabel='Common FEMU monotonic timeline (ms)',ylabel='Block',title='First 24 blocks: CU (blue), cache (green), DMA end (orange tick)')
for ax in axes:
    ax.grid(axis='x',alpha=.15)
fig.suptitle(f'{len(valid)} measured blocks; DMA later than CU: {summary["dma_after_cu"]}; cache confirmation later: {summary["cache_confirm_after_cu"]}; incomplete: {missing}',fontsize=11)
fig.savefig(a.output/'timeline.png',dpi=160)
fig.savefig(a.output/'timeline.pdf')
print(json.dumps(summary,indent=2))
