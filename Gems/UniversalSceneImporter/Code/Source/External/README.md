# Vendored libraries

## ufbx

* Files: `ufbx.h`, `ufbx.c`
* Upstream: https://github.com/ufbx/ufbx
* License: Public Domain (Unlicense) or MIT, at your option. See the license header at the top of
  `ufbx.h` and the notice at the end of `ufbx.c`.
* Used for: FBX parsing and animation sampling.

## cgltf

* Files: `cgltf.h`, `cgltf.cc`
* Upstream: https://github.com/jkuhlmann/cgltf
* Version: 1.15
* License: MIT. See the notice at the end of `cgltf.h`.
* Used for: glTF and GLB parsing.

Both libraries are compiled into the gem as static libraries
(`UniversalSceneImporter.External.ufbx` and `UniversalSceneImporter.External.cgltf`) and are not
exposed through the gem's public interface.
