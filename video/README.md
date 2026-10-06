# video/: the built-in cutscene decoder

System Shock 2's cutscenes are AVI files with Indeo 5 video and 16-bit PCM audio. NewDark plays them with `lgvid.dll`
(Looking Glass's player, recompiled like the rest of the game), which in turn loads a 2011 build of `ffmpeg.dll`
(Lavc 52.114 / Lavf 52.103) and calls 39 of its functions. That DLL is x86 Windows code, so it would tie the cutscenes to
32-bit Windows. This directory replaces it with portable C.

## How it fits in

When the recompiled `lgvid.dll` calls `LoadLibrary("ffmpeg.dll")`, the host returns a pseudo module, and `GetProcAddress`
on it returns the functions in `lavshim.c`. Each is a host C function called the way the original export was (cdecl,
arguments on the guest stack, structs in guest memory). `lgvid.dll` itself runs unchanged: it still threads, buffers,
synchronises audio and video and draws into the engine's movie surface exactly as before. Only the decoding underneath
changed.

`lavshim.c` implements the 29 functions `lgvid.dll` actually calls (the other 10 it looks up are inert stand-ins), using
FFmpeg 0.7's 32-bit structure layout, which is ABI-compatible with the version the game shipped. The offsets were taken
by compiling FFmpeg 0.7.1's public headers for a 32-bit Windows target and checked against every field access in
`lgvid.dll`'s disassembly (the list is at the top of `lavshim.c`).

| Part | What it does |
| --- | --- |
| AVI reader | `RIFF AVI` (and OpenDML `AVIX`) headers and chunks, read sequentially through `lgvid.dll`'s own file callbacks (it never seeks) |
| Video | FFmpeg's Indeo 5 decoder (`ffmpeg/libavcodec`, unmodified), output YUV 4:1:0 |
| Audio | PCM 16-bit and 8-bit, handed out as 16-bit samples |
| Scaler | YUV to the six display formats `lgvid.dll` can ask for (32-bit BGRA/RGBA/ARGB/ABGR, 16-bit RGB565/BGR565), BT.601 video range like swscale's default, point/bilinear/bicubic per the game's `movie_sw_scale_quality` |

The host side is a handful of functions declared in `lavshim.h` (guest memory allocation, a call into guest code, a clock,
a lock and a log); `host/win_host.c` has the Windows versions. A port to another platform provides those and nothing else.

Anything other than Indeo 5 / PCM in an AVI is reported to `lgvid.dll` as missing, so the movie is skipped (or plays
without sound) instead of crashing; `darkrecomp_native_ffmpeg.txt` next to the exe brings back the original `ffmpeg.dll`.

## Files and licences

| Path | Licence |
| --- | --- |
| `lavshim.c`, `lavshim.h`, `test/` | MIT, like the rest of the tools |
| `ffmpeg/libavcodec/` | LGPL 2.1 or later: files copied unmodified from FFmpeg (commit in `ffmpeg/FFMPEG_COMMIT`) |
| `ffmpeg/compat/` | LGPL 2.1 or later: the minimal FFmpeg support code those files need (memory, frame buffer, tables) |

`ffmpeg/COPYING.LGPLv2.1` is the licence text. The LGPL code is built from source as part of your build, so you can always
modify and rebuild it.

## Test

`test/lavshim_test.c` drives the shim exactly as `lgvid.dll` does (same calls, order and arguments) on a movie file,
and writes a few scaled frames (PPM) and the decoded audio (WAV). Build it as a 32-bit program:

```
Z="python -m ziglang cc -target x86-linux-musl -O2 -fwrapv -fno-strict-aliasing -DRT_IDENTITY -I../runtime -I. -Iffmpeg/compat -Iffmpeg/libavcodec"
$Z lavshim.c test/lavshim_test.c ffmpeg/compat/ffcompat.c ffmpeg/libavcodec/{indeo5,ivi,ivi_dsp,vlc}.c -o lavshim_test -lm
./lavshim_test /path/to/Intro.avi out
```

On the game's `Intro.avi`: 241 frames at 15 fps (235 pictures plus 6 "repeat the previous frame" entries) and 16.07 s of
22050 Hz stereo audio, matching the file's own length.
