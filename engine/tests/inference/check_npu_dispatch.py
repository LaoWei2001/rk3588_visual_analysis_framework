"""Reject engine submissions that bypass the shared RKNN execution layer."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]
allowed = root / 'src/vision/inference/npu_execution.h'
calls = re.compile(r'\brknn_(?:run|inputs_set|outputs_get|outputs_release)\s*\(')
comments_strings = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
errors = []
for tree in (root / 'src',):
    for path in tree.rglob('*'):
        if path.suffix not in {'.h', '.hpp', '.c', '.cc', '.cpp'} or path == allowed:
            continue
        code = comments_strings.sub(lambda m: '\n' * m[0].count('\n'), path.read_text(errors='replace'))
        for match in calls.finditer(code):
            errors.append(f'{path.relative_to(root)}:{code[:match.start()].count(chr(10)) + 1}: {match[0]}')
if errors:
    sys.exit('RKNN calls must use vision/inference/npu_execution.h:\n' + '\n'.join(errors))
print('All engine RKNN submissions use the shared execution layer.')
