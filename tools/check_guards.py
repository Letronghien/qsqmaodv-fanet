#!/usr/bin/env python3
"""
check_guards.py — detect include-guard collisions between ns-3 modules.

Modules cloned from one another (aodv -> aomdv -> pmaodv/qmaodv -> qs2maodv) often keep
the original '#ifndef XXX_H' guards. A program that includes two such modules
(qsq-compare includes aodv, pmaodv, qmaodv and qs2maodv) then silently skips one header
and fails with "RoutingTableEntry does not name a type".

usage: python3 tools/check_guards.py [--fix=mod1,mod2] <ns-3 tree> <module> [<module> ...]
  --fix  rename colliding guards ONLY inside the listed (project) modules to
         <MODULE>_<FILE>_H. Renaming a guard does not change any behaviour.
exit 1 if collisions remain.
"""
import os, re, sys
from collections import defaultdict

args = sys.argv[1:]
fixable = set()
if args and args[0].startswith("--fix="):
    fixable = set(args.pop(0).split("=", 1)[1].split(","))
ns3, mods = args[0], args[1:]
paths = {}
guard_re = re.compile(r'^\s*#\s*ifndef\s+(\w+)\s*\n\s*#\s*define\s+\1\b', re.M)
owners = defaultdict(set)
for m in mods:
    root = os.path.join(ns3, "src", m)
    for dirpath, _, files in os.walk(root, followlinks=True):
        if "/test" in dirpath or "/examples" in dirpath:
            continue
        for f in files:
            if f.endswith(".h"):
                txt = open(os.path.join(dirpath, f), errors="ignore").read()
                g = guard_re.search(txt)
                if g:
                    key = f"{m}/{os.path.relpath(os.path.join(dirpath, f), root)}"
                    owners[g.group(1)].add(key)
                    paths[key] = os.path.join(dirpath, f)
bad = {g: sorted(o) for g, o in owners.items() if len({x.split('/')[0] for x in o}) > 1}
if not bad:
    print(f"include guards OK ({sum(len(o) for o in owners.values())} headers in {len(mods)} modules)")
    sys.exit(0)
print("INCLUDE-GUARD COLLISIONS (same guard in different modules):")
for g, o in sorted(bad.items()):
    print(f"  {g}: " + ", ".join(o))
if fixable:
    changed = []
    for g, o in bad.items():
        for key in o:
            mod = key.split("/")[0]
            if mod not in fixable:
                continue
            M = re.sub(r"[^A-Z0-9]", "_", mod.upper())
            new = re.sub(r"[^A-Z0-9]", "_", os.path.basename(key).upper())
            if not new.startswith(M + "_"):
                new = M + "_" + new
            p = paths[key]
            txt = open(p).read()
            open(p, "w").write(re.sub(r"\b%s\b" % re.escape(g), new, txt))
            changed.append(f"{key}: {g} -> {new}")
    if changed:
        print("FIXED (guard renamed, no functional change):")
        for x in changed: print("  " + x)
        log = os.environ.get("GUARD_LOG")
        if log:
            with open(log, "a") as fh:
                fh.write("\n## Include-guard renames (tools/check_guards.py --fix)\n")
                fh.writelines(f"- {x}\n" for x in changed)
        os.execv(sys.executable, [sys.executable, __file__, ns3] + mods)   # re-check
print("Fix: give every module its own guard prefix, e.g. PMAODV_RTABLE_H instead of AODV_RTABLE_H.")
sys.exit(1)
