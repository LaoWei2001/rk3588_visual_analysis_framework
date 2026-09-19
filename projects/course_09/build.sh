#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# --engine overrides the environment, then the ignored local engine selection.
engine_dir="${RKVISION_ENGINE:-}"
args=("$@")
for ((i=0; i<${#args[@]}; i++)); do
    if [[ "${args[i]}" == --engine ]]; then
        engine_dir="${args[i+1]:?--engine requires a path}"
    fi
done
if [[ -z "$engine_dir" && -f "$project_dir/engine.local.cmake" ]]; then
    engine_dir="$(python3 -c 'import pathlib,re,sys; s=pathlib.Path(sys.argv[1]).read_text(); m=re.search(r"set\(RKVISION_ENGINE \[\[(.*?)\]\]\)",s); print(m.group(1) if m else "")' "$project_dir/engine.local.cmake")"
fi
if [[ -z "$engine_dir" ]]; then
    engine_dir="$(cd "$project_dir/../.." && pwd)"
fi
if [[ ! -f "$engine_dir/tools/project/build.sh" ]]; then
    echo "Set RKVISION_ENGINE or pass --engine /path/to/framework" >&2
    exit 1
fi
exec bash "$engine_dir/tools/project/build.sh" "$project_dir" "${args[@]}" --engine "$engine_dir"
