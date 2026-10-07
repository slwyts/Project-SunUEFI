# Pinned BSP patch inputs

These three configuration files are unmodified blobs from
`blu-sharky/debian-piano` commit `25babfe3ff5d8ddee98b1e0ea88152d69a0c01b1`.
Their paths and SHA256 values are checked against `linux/bsp/manifest.json`.
They let the portable patch tests run without initializing an upstream checkout.

They retain the upstream MIT license, included in
[`linux/bsp/licenses/debian-piano.MIT`](../../../linux/bsp/licenses/debian-piano.MIT).
They are test inputs and are never installed by the product builders.
