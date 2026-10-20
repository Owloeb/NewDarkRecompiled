#!/usr/bin/env python3
"""hotfiles.py <profile> <generated dir> [--cover 0.95] [-o hot.txt]: which generated C files hold the hot guest code.

<profile> is ss2port_prof.txt from a PORT_PROF build (every function sampled since start, "count name" lines) or, failing
that, an ss2port.log with PROF tables (only the top 30 of each 20 s window, so coarser). <generated dir> is the lifter
output (out/port: nd/ ao/ sq/ lv/ fm/). Prints, or writes with -o, the base names of the fewest generated files that
together hold --cover (default 95%) of the samples spent in recompiled code; imports and host functions are left out.

The list is for cmake -DPORT_GAME_HOT=hot.txt: listed files are compiled with PORT_GAME_HOT_OPT (default -O2), the rest
with PORT_GAME_OPT (default -Os there), so the code that rarely runs takes less room in the instruction cache. Profile
with the same lift (same options and game files) as the build the list is for; a relift with other options moves functions
between files. Reads only the generated files you made yourself; prints file names and percentages, nothing else."""
import argparse, collections, os, re, sys

ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
ap.add_argument("profile"); ap.add_argument("gendir")
ap.add_argument("--cover", type=float, default=0.95, help="fraction of the recompiled-code samples the listed files must hold")
ap.add_argument("-o", "--out", help="write the list here (default: stdout)")
a = ap.parse_args()

NAME = re.compile(r"^(nd|ao|sq|lv|fm)_[0-9a-f]{8}$")
samples = collections.Counter()
text = open(a.profile, errors="replace").read()
if "PROF" in text and "top guest functions" in text:                      # an ss2port.log: average its tables
    tabs, cur = [], None
    for l in text.splitlines():
        if "top guest functions" in l: cur = {}; tabs.append(cur); continue
        m = re.search(r"PROF\s+([\d.]+)% (\S+)", l)
        if m and cur is not None: cur[m.group(2)] = float(m.group(1))
    for t in tabs:
        for k, v in t.items(): samples[k] += v / len(tabs)
    print(f"(PROF tables from a log: {len(tabs)} windows, top 30 each; ss2port_prof.txt is more complete)", file=sys.stderr)
else:
    for l in text.splitlines():
        p = l.split()
        if len(p) == 2 and not l.startswith("#") and p[0].isdigit(): samples[p[1]] += int(p[0])

where = {}
DEF = re.compile(r"^void ((?:nd|ao|sq|lv|fm)_[0-9a-f]{8})\(CPU \*c\) \{", re.M)
for root, _, files in os.walk(a.gendir):
    for fn in files:
        if fn.endswith(".c"):
            for m in DEF.finditer(open(os.path.join(root, fn), errors="replace").read()): where[m.group(1)] = fn
if not where: sys.exit(f"no recompiled functions found under {a.gendir} (expected the lifter output, e.g. out/port)")

guest = {k: v for k, v in samples.items() if NAME.match(k)}
total = sum(guest.values())
if not total: sys.exit("no samples of recompiled functions in the profile (was it a PORT_PROF build?)")
per_file = collections.Counter(); missing = 0.0
for k, v in guest.items():
    if k in where: per_file[where[k]] += v
    else: missing += v
if missing / total > 0.05:
    print(f"warning: {100 * missing / total:.0f}% of the samples are in functions not found in {a.gendir}: profile and lift differ?", file=sys.stderr)
hot, acc = [], 0.0
for fn, v in per_file.most_common():
    if acc >= a.cover * total: break
    hot.append(fn); acc += v
nfiles = sum(1 for r, _, fs in os.walk(a.gendir) for f in fs if f.endswith(".c"))
out = [f"# hot generated files: {len(hot)} of {nfiles}, {100 * acc / total:.1f}% of {total:.0f} samples in recompiled code "
       f"({100 * total / max(sum(samples.values()), 1):.0f}% of all samples)"] + hot
if a.out: open(a.out, "w").write("\n".join(out) + "\n"); print(out[0], file=sys.stderr)
else: print("\n".join(out))
