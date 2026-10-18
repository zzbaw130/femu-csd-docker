#!/usr/bin/env python3
"""Verify the actual ELF dependency, including operators with no allocator calls."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(sys.argv[1])
operators = sorted(p for p in root.glob('*.so') if p.name != 'libcs-workmem.so')
if not operators:
    sys.exit('No native CSF libraries found')
errors = []
for operator in operators:
    dynamic = subprocess.check_output(['readelf', '-d', str(operator)], text=True)
    needed = re.findall(r'\(NEEDED\).*?\[([^]]+)\]', dynamic)
    paths = re.findall(r'\((?:RUNPATH|RPATH)\).*?\[([^]]+)\]', dynamic)
    if 'libcs-workmem.so' not in needed:
        errors.append(f'{operator.name}: missing libcs-workmem.so dependency')
    if not any('$ORIGIN' in path.split(':') for path in paths):
        errors.append(f'{operator.name}: missing $ORIGIN library search path')
if errors:
    sys.exit('\n'.join(errors))
print(f'PASS: {len(operators)} native CSF libraries depend on shared workmem with $ORIGIN')
