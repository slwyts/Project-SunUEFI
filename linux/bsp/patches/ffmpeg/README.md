# FFmpeg stateful decoder BSP adaptation

`series.json` fixes FFmpeg 7.1.5 and the distro-neutral patch. Stage a fresh
copy with `tools/prepare_ffmpeg_source.py --output build/<new-directory>`.
Do not edit fixed upstream source or apply this patch to a different FFmpeg
version with fuzz.

The ARM64 minimal CLI is a prototype for real device decoding, not the
release ffmpeg package. See [the codec development notes](../../../../docs/devel/piano-video-codec.md)
for the captured failure, state handling, build command, known reference
lifetime limitation and Debian/Arch package integration route.

The sunuefi3 series was verified on the real g086 kernel with a synthetic
12-frame 640x480 H.264 decode and a short encoder comparison. The encoder
bitstream matched the system baseline byte for byte. DRC/reallocation,
playback timing and the complete distro packages still need validation.
