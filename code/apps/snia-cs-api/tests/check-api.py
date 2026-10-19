#!/usr/bin/env python3
"""检查 SDK 可独立构建，以及原生 FEMU ABI 边界。"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
prototypes = re.findall(r'\b(?:CS_STATUS|void)\s+(cs\w+)\s*\(', (root / 'include/cs.h').read_text())
assert len(prototypes) == len(set(prototypes)) == 33, '规范 API 入口数不是 33'
sources = '\n'.join(p.read_text() for p in (root / 'src').glob('cs_*.cpp'))
for name in prototypes:
    assert re.search(r'\b(?:CS_STATUS|void)\s+' + name + r'\s*\(', sources), name
print('PASS: 33 API declarations/implementations, native FEMU argument ABI')

assert not (root / 'include/cs-api-wire.h').exists()
for name in ('csd.c', 'csd.h'):
    source = root.parents[1] / 'FEMU/hw/femu/csd' / name
    if source.exists():
        assert 'cs-api' not in source.read_text()

assert sorted(p.name for p in (root / 'include').iterdir()) == ['cs.h']
for p in (root / 'src').rglob('*'):
    if p.suffix in ('.c', '.h', '.cpp', '.hpp'):
        text = p.read_text()
        assert 'nvme_ioctl' not in text and 'femu_csd_args' not in text, p
        assert not re.search(r'#include.*(?:femu|cs_csf)', text), p
for p in (root / 'plugins').rglob('*'):
    if p.suffix in ('.c', '.h', '.cpp', '.hpp'):
        assert 'cs_internal.hpp' not in p.read_text(), p
print('PASS: single public header, device-independent API implementation, isolated plugins')

backend = (root / 'src/cs_backend.h').read_text()
methods = set(re.findall(r'^    (?:CS_STATUS|void|u32) \(\*(\w+)\)', backend, re.M))
internal_hooks = {'getCSFResourceId', 'validateRequest', 'executeBatchEntry', 'finishBatchExecution'}
assert methods - internal_hooks <= {name[2:] if name[2:4] == 'CS' else name[2].lower() + name[3:] for name in prototypes}
assert not (root / 'src/core').exists() and not (root / 'src/facade').exists()
assert (root / 'src/cs_handle_registry.cpp').exists() and (root / 'src/cs_request_execution.cpp').exists()
print('PASS: backend names match SNIA APIs; shared handle/request mechanisms only')
