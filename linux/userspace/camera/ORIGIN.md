# Camera source

`piano-camerad.c` originates from `camera/piano-camerad.c` in [debian-piano](https://github.com/blu-sharky/debian-piano). Its imported revision is recorded by `debian-piano-current` in the repository’s `sources.lock.json`. The original MIT copyright and license are retained in `LICENSE-MIT`. The camera changes previously applied by the runtime builder are maintained directly in this directory.

The release runtime builder and the Nix package build this same source through `Makefile`. The camera owns its existing control socket and V4L2 capture path; it is not run alongside another sensor controller.

The `cam_cfgs[].ccm` constants are described in the source as averages of stock tuning matrices. Their separate source and redistribution permission are not recorded. The MIT license for this program is not evidence that external OEM tuning may be copied into a libcamera IPA tuning file. New tuning requires a documented, compatible public source or independent measurements.
