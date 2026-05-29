# AOM IAMF Bridge SDK

This directory contains the source for the prebuilt IAMF bridge consumed by
`MR_ADM_ENABLE_IAMF=ON`. The bridge is built inside an AOM `iamf-tools` checkout
so Bazel owns protobuf/absl/codec dependencies, while MacinRender links only the
resulting C ABI library.

## Build

Copy this directory into an `iamf-tools` checkout as `iamf/cli/mr_bridge`, then
build the shared library:

```sh
cp -R /path/to/MacinRender-ADM-Core/tools/iamf_aom_bridge /path/to/iamf-tools/iamf/cli/mr_bridge
cd /path/to/iamf-tools
bazel build //iamf/cli/mr_bridge:mr_iamf_aom_bridge
```

Create an SDK layout for CMake:

```sh
mkdir -p /path/to/iamf-sdk/lib
cp bazel-bin/iamf/cli/mr_bridge/libmr_iamf_aom_bridge.* /path/to/iamf-sdk/lib/
```

On Windows, place the import library under `lib/` and the runtime DLL under
`bin/`:

```text
iamf-sdk/
  bin/mr_iamf_aom_bridge.dll
  lib/mr_iamf_aom_bridge.lib
```

Then configure MacinRender with:

```sh
cmake --preset release \
  -DMR_ADM_ENABLE_IAMF=ON \
  -DMR_ADM_IAMF_AOM_ROOT=/path/to/iamf-sdk
```

The bridge currently accepts rendered channel-based WAV input and writes raw
`.iamf` Opus output. Unsupported layouts fail explicitly; MacinRender never
falls back to the legacy hand-written IAMF writer.
