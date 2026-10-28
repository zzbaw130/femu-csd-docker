#!/usr/bin/env python3
"""Explain cold refill latency using Guest records and existing device traces.

Clock offset is bounded by DMA/ACK causality, independently for each trial.
No timestamps from different clocks are directly subtracted without that bound.
"""
import argparse
import csv
import json
import re
import statistics as stats
from collections import defaultdict
from pathlib import Path


def records(path, prefix):
    return [{k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}
            for line in path.read_text().splitlines() if line.startswith(prefix + ' ')]


def host_path(path):
    text = path.read_text()
    assert 'correct=yes' in text and 'cache_bytes_match=yes' in text, path
    wall = float(re.search(r'wall_ms=([\d.]+)', text)[1])
    run = records(path, 'CACHE_APP_RUN')[0]
    lanes = records(path, 'CACHE_LANE')
    lane = max(lanes, key=lambda r: r['end'])
    apps = records(path, 'CACHE_APP')
    blocks = sorted((r for r in apps if r['lane'] == lane['lane']), key=lambda r: r['block'])
    assert len(blocks) == 32 and len(apps) == 256
    ns = {
        'thread_start_join': lane['begin'] - run['begin'] + run['end'] - lane['end'],
        'allocate_setup': blocks[0]['configure'] - lane['begin'],
        'configure': sum(r['batch_begin'] - r['configure'] for r in blocks),
        'batch': sum(r['batch_end'] - r['batch_begin'] for r in blocks),
        'result_read': sum(r['copy_end'] - r['batch_end'] for r in blocks),
        'free': lane['end'] - blocks[-1]['copy_end'],
        'loop': sum(b['configure'] - a['copy_end'] for a, b in zip(blocks, blocks[1:])),
    }
    assert sum(ns.values()) == run['end'] - run['begin']
    return {'wall_ms': wall, 'critical_lane': lane['lane'],
            'path_ms': {k: v / 1e6 for k, v in ns.items()}}, apps, blocks


def overlap(a, b, c, d):
    return max(0, min(b, d) - max(a, c))


def refill_path(devices, hosts, critical, offset, parallel=False):
    segments = []
    seen_fetch = set()
    for token, p in hosts.items():
        d = devices[token]
        stages = [
            ('page_bio_prepare', p['begin'], p['dma_submit']),
            ('dma_request', p['dma_submit'], p['dma_done']),
            ('publish_pages', p['dma_done'], p['published']),
            *([('ack_request', p['ack_begin'], p['ack_end'])] if p['ack_begin'] else []),
        ]
        assert all(a <= b for _, a, b in stages)
        segments.extend((token, name, a, b) for name, a, b in stages)
        fetch = (p['fetch_begin'], p['fetch_end'])
        if fetch not in seen_fetch:
            segments.append((None, 'metadata_fetch', *fetch))
            seen_fetch.add(fetch)
    costs = defaultdict(float)
    wait = 0
    for token in critical:
        d = devices[token]
        a, b = d['cu'] - offset, d['release'] - offset
        if b <= a:
            continue
        wait += b - a
        for other, name, start, end in segments:
            # Concurrent service intervals overlap. Only the current block
            # has an unambiguous attribution; never add other workers twice.
            if parallel and other != token:
                continue
            key = name if other is None else ('own_' if token == other else 'other_') + name
            costs[key] += overlap(a, b, start, end)
    remainder = 'before_own_service' if parallel else 'notification_or_unrecorded_gap'
    costs[remainder] = wait - sum(costs.values())
    assert costs[remainder] >= -1, costs
    return wait / 1e6, {k: max(0, v) / 1e6 for k, v in costs.items()}


def analyze(root, round_number):
    base = root / f'round{round_number}-p1-cache1'
    result, apps, critical_apps = host_path(base.with_suffix('.app'))
    devices = {r['token']: r for r in records(base.with_suffix('.device'), 'CACHE_BLOCK')}
    hosts = {r['token']: r for r in records(base.with_suffix('.kernel'), 'CACHE_HOST')
             if r['token'] in devices}
    assert len(devices) == len(hosts) == 256
    by_lba = {r['lba']: r for r in hosts.values()}
    assert len(by_lba) == 256 and all(r['dma_calls'] == 1 for r in hosts.values())
    # D = G + offset. Each device operation occurs inside its Guest request.
    bounds = [(devices[t]['dma'] - p['dma_done'],
               devices[t]['dma_start'] - p['dma_submit']) for t, p in hosts.items()]
    bounds += [(devices[t]['cache_ack'] - p['ack_end'],
                devices[t]['cache_ack'] - p['ack_begin']) for t, p in hosts.items()
               if devices[t]['cache_ack'] and p['ack_begin']]
    low = max(a for a, _ in bounds)
    high = min(b for _, b in bounds)
    assert low <= high, f'clock bounds do not intersect: {low}, {high}'
    offset = (low + high) / 2
    critical = [by_lba[r['lba']]['token'] for r in critical_apps]
    for app in apps:
        d = devices[by_lba[app['lba']]['token']]
        assert app['batch_begin'] <= d['load'] - offset <= d['release'] - offset <= app['batch_end']
    parallel=any(p.get("parallel", 0) for p in hosts.values())
    wait, costs = refill_path(devices, hosts, critical, offset, parallel)
    _, low_costs = refill_path(devices, hosts, critical, low, parallel)
    _, high_costs = refill_path(devices, hosts, critical, high, parallel)
    result.update(round=round_number, parallel=parallel, clock_offset_ns=offset,
                  clock_bound_width_us=(high - low) / 1000,
                  critical_cache_wait_ms=wait, wait_cost_ms=costs,
                  wait_cost_alignment_samples_ms={k: [min(costs[k], low_costs[k], high_costs[k]),
                                           max(costs[k], low_costs[k], high_costs[k])]
                                       for k in costs})
    values = defaultdict(list)
    worker_wakes = set()
    fetches = set()
    for t, p in hosts.items():
        d = devices[t]
        values['metadata_to_service'].append(p['begin'] - p['fetch_end'])
        values['page_allocation'].append(p['pages_ns'])
        values['prepare_total'].append(p['dma_submit'] - p['begin'])
        values['dma_roundtrip'].append(p['dma_done'] - p['dma_submit'])
        values['dma_copy'].append(d['dma'] - d['dma_start'])
        values['dma_completion_return'].append(p['dma_done'] - (d['dma'] - offset))
        if p['ack_begin']:
            values['ack_roundtrip'].append(p['ack_end'] - p['ack_begin'])
        if d['cache_ack'] and p['ack_begin']:
            values['ack_completion_return'].append(p['ack_end'] - (d['cache_ack'] - offset))
        values['load_to_metadata'].append(p['fetch_end'] - (d['load'] - offset))
        worker_wakes.add((p['aer'], p['worker']))
        fetches.add((p['fetch_begin'], p['fetch_end']))
    values['aer_to_worker'] = [b - a for a, b in worker_wakes if a and b >= a]
    values['fetch_roundtrip'] = [b - a for a, b in fetches]
    service = defaultdict(float)
    for t, h in hosts.items():
        d = devices[t]
        service['page_allocation'] += h['pages_ns']
        service['prepare_other'] += h['dma_submit'] - h['begin'] - h['pages_ns']
        service['dma_delivery_queue'] += d['dma_start'] - offset - h['dma_submit']
        service['dma_copy'] += d['dma'] - d['dma_start']
        service['dma_completion_return'] += h['dma_done'] - (d['dma'] - offset)
        service['publish_pages'] += h['published'] - h['dma_done']
        if d['cache_ack'] and h['ack_begin']:
            service['ack_delivery'] += d['cache_ack'] - offset - h['ack_begin']
            service['ack_completion_return'] += h['ack_end'] - (d['cache_ack'] - offset)
    service['ack_delivery'] += 0
    service['ack_completion_return'] += 0
    service['metadata_fetch'] = sum(b - a for a, b in fetches)
    result['worker_service_ms'] = {k: v / 1e6 for k, v in service.items()}
    result['stage_mean_us'] = {k: stats.mean(v) / 1000 for k, v in values.items() if v}
    result['worker_invocations'] = len(worker_wakes)
    result['fetch_requests'] = len(fetches)
    return result


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('directory', type=Path)
p.add_argument('--plot', action='store_true', help='also export a matplotlib figure')
a = p.parse_args()
root = a.directory
rounds = sorted(int(re.search(r'round(\d+)', f.name)[1]) for f in root.glob('round*-p1-cache1.app'))
result = {'rounds': [analyze(root, r) for r in rounds], 'timing_ms': {}}
for profile in [0, 1]:
    for cache in [0, 1]:
        files = [root / f'round{r}-p{profile}-cache{cache}.app' for r in rounds]
        samples = [float(re.search(r'wall_ms=([\d.]+)', f.read_text())[1]) for f in files]
        result['timing_ms'][f'profile{profile}_cache{cache}'] = {
            'samples': samples, 'median': stats.median(samples)}
result['host_paths'] = {
    f'cache{cache}': [host_path(root / f'round{r}-p1-cache{cache}.app')[0] for r in rounds]
    for cache in [0, 1]}
(root / 'breakdown.json').write_text(json.dumps(result, indent=2) + '\n')
with (root / 'critical-waits.csv').open('w') as f:
    keys = list(result['rounds'][0]['wait_cost_ms'])
    w = csv.DictWriter(f, fieldnames=['round', 'wall_ms', 'critical_cache_wait_ms'] + keys)
    w.writeheader()
    for r in result['rounds']:
        w.writerow({**{k: r[k] for k in ['round', 'wall_ms', 'critical_cache_wait_ms']}, **r['wait_cost_ms']})
print(json.dumps(result, indent=2))

if a.plot:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    mean = stats.mean
    fig, axes = plt.subplots(1, 2, figsize=(13, 5), constrained_layout=True)
    for i, cache in enumerate([0, 1]):
        paths = result['host_paths'][f'cache{cache}']
        path = {k: mean(r['path_ms'][k] for r in paths) for k in paths[0]['path_ms']}
        wait = mean(r['critical_cache_wait_ms'] for r in result['rounds']) if cache else 0
        parts = [('Setup/config/free', sum(v for k, v in path.items() if k not in ['batch', 'result_read']), '#94a3b8'),
                 ('Batch excluding refill hold', path['batch'] - wait, '#2563eb'),
                 ('Refill hold after CU finished', wait, '#dc2626'),
                 ('Read compute result', path['result_read'], '#16a34a')]
        left = 0
        for label, width, color in parts:
            axes[0].barh(i, width, left=left, color=color, label=label if i == 0 else None)
            if width > 3:
                axes[0].text(left + width / 2, i, f'{width:.1f}', ha='center', va='center', color='white')
            left += width
        axes[0].text(left + .5, i, f'{left:.1f} ms', va='center')
    axes[0].set(yticks=[0, 1], yticklabels=['Refill OFF', 'Refill ON'], xlabel='Critical lane elapsed time (ms)',
                title='End-to-end critical path (mean of measured runs)')
    axes[0].margins(x=.2)
    axes[0].legend(loc='upper left', bbox_to_anchor=(0, -.17), fontsize=8)
    serial = {k: mean(r['worker_service_ms'][k] for r in result['rounds'])
              for k in result['rounds'][0]['worker_service_ms']}
    groups = [('DMA request roundtrip', sum(serial[k] for k in ['dma_delivery_queue', 'dma_copy', 'dma_completion_return'])),
              ('ACK request roundtrip', serial['ack_delivery'] + serial['ack_completion_return']),
              ('Cache page / BIO preparation', serial['page_allocation'] + serial['prepare_other']),
              ('FETCH metadata roundtrip', serial['metadata_fetch']), ('Publish/unlock pages', serial['publish_pages'])]
    total = sum(v for _, v in groups)
    for i, (label, value) in enumerate(groups):
        axes[1].barh(i, value, color=['#2563eb', '#d97706', '#7c3aed', '#0891b2', '#16a34a'][i])
        axes[1].text(value + .3, i, f'{value:.1f} ms ({value / total:.1%})', va='center', fontsize=9)
    axes[1].set(yticks=range(len(groups)), yticklabels=[n for n, _ in groups], xlabel='Sum of per-block service time (ms)',
                title=(f'Overlapping service summed: {total:.1f} ms'
                       if any(r['parallel'] for r in result['rounds']) else
                       f'Why the single worker takes {total:.1f} ms'))
    axes[1].invert_yaxis()
    axes[1].margins(x=.45)
    for ax in axes:
        ax.grid(axis='x', alpha=.2)
    fig.suptitle('Light Filter: 16 MiB, 256 blocks, 8 CU / 256 MiB workmem / 64 MiB FDM', fontsize=11)
    fig.savefig(root / 'breakdown.png', dpi=160)
    fig.savefig(root / 'breakdown.pdf')
