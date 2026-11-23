# CPAL 0.18.2

Imported from the crates.io release, SHA-256
`6f02e8d0327b42d3e2e4ab2119af397344eb9fc54a34bf0ddeaa1277af8681f1`,
upstream revision `e1612d5d98152f8dc2a62e1b51ef7cbf4f7f26b7` (Apache-2.0).

The macOS output AudioUnit uses the requested client stream format and the system's
sample rate converter. Opening a monitor therefore preserves the device's physical
format and nominal rate. MacinRender requests the default device buffer size.

macOS capture uses input devices, including multichannel virtual input devices. The
Core Audio process-tap module is excluded to support the macOS 13 deployment target.
Windows capture uses the upstream WASAPI endpoint loopback.
