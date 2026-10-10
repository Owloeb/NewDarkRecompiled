"""exe_stamp.py: warn when the SS2.exe being built is not the one symbols/manual.sym was verified against.

symbols/manual.sym names functions by address, so the names are only right for the exact exe they were checked against
(NewDark 2.48 retail; the GOG and Steam copies are byte-identical). The expected SHA-256 is the line
`# verified-against sha256 <hash> ...` in symbols/manual.sym. A different exe (a patched or modded one, another NewDark
version) still builds, but names may land on the wrong functions. This only prints a warning; it never stops the build.

Also usable by hand: python3 tools/exe_stamp.py <SS2.exe>"""
import hashlib, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def check(exe, sym=None):
    """Returns True when the exe matches the stamp (or no stamp exists); prints a warning when it does not."""
    sym = sym or os.path.join(ROOT, "symbols", "manual.sym")
    want = None
    try:
        for ln in open(sym, encoding="utf-8"):
            m = re.match(r"#\s*verified-against\s+sha256\s+([0-9a-fA-F]{64})", ln)
            if m: want = m.group(1).lower(); break
    except OSError:
        return True
    if not want: return True
    h = hashlib.sha256()
    with open(exe, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""): h.update(blk)
    got = h.hexdigest()
    if got == want: return True
    print(f"\nWARNING: this SS2.exe is not the one the hand-written names were verified against.\n"
          f"  yours:    sha256 {got}\n  expected: sha256 {want} (NewDark 2.48 retail, GOG and Steam)\n"
          f"  The build works, but names from symbols/manual.sym may point at the wrong functions.\n", flush=True)
    return False

if __name__ == "__main__":
    if len(sys.argv) != 2: sys.exit(__doc__)
    sys.exit(0 if check(sys.argv[1]) else 1)
