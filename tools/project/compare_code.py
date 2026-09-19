#!/usr/bin/env python3
"""Compare executable ELF sections of original/new object files, ignoring paths/debug data."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def code_sections(path):
    data=path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError(f'Expected little-endian ELF64: {path}')
    shoff=struct.unpack_from('<Q',data,40)[0]
    entsize,count,names_index=struct.unpack_from('<HHH',data,58)
    headers=[struct.unpack_from('<IIQQQQIIQQ',data,shoff+i*entsize) for i in range(count)]
    names_header=headers[names_index]
    names=data[names_header[4]:names_header[4]+names_header[5]]
    result={}
    for header in headers:
        if header[2]&4:
            name=names[header[0]:].split(b'\0',1)[0].decode()
            result[name]=hashlib.sha256(data[header[4]:header[4]+header[5]]).hexdigest()
    return result


def compare(before,after):
    old={str(p.relative_to(before/'CMakeFiles/vision_analysis.dir')):p for p in (before/'CMakeFiles/vision_analysis.dir/src').rglob('*.o')}
    if not old:
        raise ValueError(f'No baseline object files found in {before}')
    roots = [after] if isinstance(after, Path) else after
    current = [p for root in roots for p in root.rglob('*.o')]
    matches=[]; differences=[]; missing=[]
    for name,p in old.items():
        if '/logic/modules/' in name or '/logic/global_modules/' in name:
            suffix='/logic/'+name.split('/logic/',1)[1]
            candidates=[q for q in current if str(q).endswith(suffix)]
        else:
            candidates=[q for q in current if str(q).endswith('/'+name)]
        if not candidates:
            missing.append(name); continue
        if all(code_sections(p)==code_sections(candidate) for candidate in candidates): matches.append(name)
        else: differences.append(name)
    return {'identical_executable_sections':matches,'different_executable_sections':differences,'missing_or_ambiguous':missing}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before',type=Path); parser.add_argument('after',type=Path,nargs='+')
    args=parser.parse_args()
    result=compare(args.before,args.after)
    print(json.dumps(result,indent=2))
    raise SystemExit(bool(result["different_executable_sections"] or result["missing_or_ambiguous"]))
