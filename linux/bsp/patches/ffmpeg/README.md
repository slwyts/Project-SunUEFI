# FFmpeg stateful decoder BSP adaptation

The default rootfs builders install complete distribution packages with the
stateful V4L2 decoder fixes:

| Target | Source and patch series | Package builder |
| --- | --- | --- |
| Debian GNOME / KDE | FFmpeg 7.1.5, `series.json` | `tools/build_ffmpeg_packages.py` |
| SteamOS | FFmpeg 8.1.1, `series-8.json` | `tools/build_arch_ffmpeg.py` |
| Nyarch | FFmpeg 9.0.2, `patches/ffmpeg/9.0.2/series.json` | `tools/build_arch_ffmpeg.py` |

Arch packages preserve the installed codec configuration, dependencies,
epoch and library ABI. A different upstream ABI needs its own patch port.
Source archives and patches are checksum-pinned and applied without fuzz.
Stage a copy with `tools/prepare_ffmpeg_source.py --output build/<new-directory>`;
add `--series patches/ffmpeg/9.0.2/series.json` for FFmpeg 9.

Real Iris hardware has decoded and encoded the 12-frame H.264 sample with
both versions. The full FFmpeg 9 package was built and installed on Nyarch.
Both default patch series now include capture-pool lifetime handling. With
the default Iris kernel fix, FFmpeg 7 and 9 both decode all 36 frames across
640x480 -> 1280x720 -> 640x480 and encode 12 H.264 frames. Other codecs and
player-specific hardware selection need separate checks. See
[the codec development notes](../../../../docs/devel/hardware/video-codec.md).
