"""deps.py: third-party pieces the Windows cross-build needs, fetched once into build/deps and checked by SHA-256."""
import hashlib, os, tarfile, urllib.request

SDL2_VERSION = "2.30.9"
SDL2_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL2_VERSION}/SDL2-devel-{SDL2_VERSION}-mingw.tar.gz"
SDL2_SHA256 = "b188d165dc4a0372042bff3d0c18e9bb90772581e92a7b8b47b521b47714f75c"

def sdl2_mingw(deps_dir, arch="x86_64"):
    """Returns SDL2_DIR (the CMake package folder) of SDL2's MinGW development package, downloading it if needed."""
    root = os.path.join(deps_dir, f"SDL2-{SDL2_VERSION}")
    cfg = os.path.join(root, f"{arch}-w64-mingw32", "lib", "cmake", "SDL2")
    if os.path.isfile(os.path.join(cfg, "SDL2Config.cmake")): return cfg
    os.makedirs(deps_dir, exist_ok=True)
    tgz = os.path.join(deps_dir, os.path.basename(SDL2_URL))
    if not os.path.isfile(tgz):
        print(f"  downloading SDL2 {SDL2_VERSION} for Windows", flush=True)
        urllib.request.urlretrieve(SDL2_URL, tgz + ".part"); os.replace(tgz + ".part", tgz)
    if hashlib.sha256(open(tgz, "rb").read()).hexdigest() != SDL2_SHA256:
        os.remove(tgz); raise SystemExit(f"{os.path.basename(tgz)}: checksum mismatch (download damaged or replaced); run again")
    with tarfile.open(tgz) as t:
        for m in t.getmembers():                        # refuse paths that would leave deps_dir
            if m.name.startswith("/") or ".." in m.name.split("/"): raise SystemExit(f"unexpected path in {tgz}: {m.name}")
        t.extractall(deps_dir)
    return cfg
