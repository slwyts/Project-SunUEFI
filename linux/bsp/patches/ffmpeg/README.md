# V4L2 codec patches for FFmpeg

The default rootfs builders install complete distribution packages with the
stateful V4L2 codec fixes:

| Target | Source and patch series | Package builder |
| --- | --- | --- |
| Debian GNOME / KDE | FFmpeg 9.0.2, `patches/ffmpeg/9.0.2/series.json` | `tools/build_ffmpeg_packages.py` |
| SteamOS | FFmpeg 8.1.2, `series-8.json` | `tools/build_arch_ffmpeg.py` |
| Nyarch / Arch Denial | FFmpeg 9.1, `patches/ffmpeg/9.1/series.json` | `tools/build_arch_ffmpeg.py` |

Arch packages preserve the installed codec configuration, dependencies,
epoch and library ABI. The pinned source version must match the installed
package. Source archives and patches are checksum-pinned and applied without fuzz.
Arch and SteamOS share the patch that preserves their distribution's
`av_stream_get_first_dts` export. FFmpeg 9.1 already counts each V4L2
buffer only once; the older series share the upstream backport.
Stage a copy with `tools/prepare_ffmpeg_source.py --output build/<new-directory>`.
The default series is FFmpeg 9. For SteamOS, pass
`--series linux/bsp/patches/ffmpeg/series-8.json`.

The series share the patch for the H.264 `-level:v` option. Use the integer
level value, such as `52` for Level 5.2. If omitted, the driver keeps its
default; if rejected, the encoder reports the error. For actual Iris
results and the remaining 4K60 checks, see the
[codec development notes](../../../../docs/devel/hardware/video-codec.md).
