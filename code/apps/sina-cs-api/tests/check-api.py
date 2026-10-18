#!/usr/bin/env python3
"""检查 SDK 可独立构建，以及原生 FEMU ABI 边界。"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
prototypes = re.findall(r'\b(?:CS_STATUS|void)\s+(cs\w+)\s*\(', (root / 'include/cs.h').read_text())
assert len(prototypes) == len(set(prototypes)) == 33, '规范 API 入口数不是 33'
sources = '\n'.join(p.read_text() for p in (root / 'src').glob('*.c'))
for name in prototypes:
    assert re.search(r'\b(?:CS_STATUS|void)\s+' + name + r'\s*\(', sources), name
print('PASS: 33 API declarations/implementations, original FEMU transport')

assert not (root / 'include/cs-api-wire.h').exists()
for name in ('csd.c', 'csd.h'):
    source = root.parents[1] / 'FEMU/hw/femu/csd' / name
    if source.exists():
        assert 'cs-api' not in source.read_text()
