#!/usr/bin/env bash
# check_env.sh — collect the machine/ns-3 information needed for the setup.
# Usage: bash tools/check_env.sh [path-to-your-ns-3.48]   and paste the output.
NS3_SRC=${1:-}
echo "=== OS ===";        (lsb_release -ds 2>/dev/null || cat /etc/os-release | head -2); uname -r
echo "=== CPU/RAM/DISK ==="; echo "cores: $(nproc)"; free -h | awk 'NR<=2'; df -h "$HOME" | tail -1
echo "=== toolchain ==="
for t in g++ clang++ cmake ninja make python3 git; do printf "%-8s " "$t"; ($t --version 2>/dev/null | head -1) || echo "NOT FOUND"; done
python3 - <<'PY'
import importlib
for m in ("numpy","pandas","scipy","matplotlib"):
    try: print(f"python {m:10s}", importlib.import_module(m).__version__)
    except Exception: print(f"python {m:10s} NOT INSTALLED")
PY
echo "=== ns-3 ==="
if [[ -z "$NS3_SRC" ]]; then
  echo "candidates:"; find "$HOME" -maxdepth 4 -name ns3 -type f -path "*ns-3*" 2>/dev/null | head
else
  echo "path: $NS3_SRC"; cat "$NS3_SRC/VERSION" 2>/dev/null
  (cd "$NS3_SRC" && git describe --tags 2>/dev/null)
  ls "$NS3_SRC/contrib" 2>/dev/null | sed 's/^/contrib: /'
  ls -d "$NS3_SRC"/cmake-cache "$NS3_SRC"/build 2>/dev/null
  grep -E "CMAKE_BUILD_TYPE|NS3_ENABLED_MODULES" "$NS3_SRC/cmake-cache/CMakeCache.txt" 2>/dev/null | head -3
fi
echo "=== baseline modules (pmaodv / qmaodv) anywhere in \$HOME? ==="
find "$HOME" -maxdepth 6 -type d \( -name pmaodv -o -name qmaodv \) 2>/dev/null | head
