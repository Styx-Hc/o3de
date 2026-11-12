# UniversalSceneImporter

`UniversalSceneImporter` is an O3DE **tool** gem that imports **FBX**, **glTF** (`.gltf`) and **GLB**
scene files into the SceneAPI Scene Builder pipeline. It is a self-contained alternative to the
AssImp based importer for projects that want direct control over how FBX and glTF data is
translated into O3DE scenes.

The gem is based on the standalone `ufbxImporter` and `cgltfImporter` gems from
[ImporterGems](https://codeberg.org/system_v/ImporterGems), merged into a single gem with a shared,
format-neutral scene representation.

## Highlights

* **One gem, three formats** - `.fbx` (ufbx), `.gltf` and `.glb` (cgltf). No third party package is
  downloaded; both libraries are vendored under `Code/Source/External`.
* **No paks, no runtime module** - the gem is a tool-only gem. It is loaded by the Asset Processor,
  the Asset Builder and the Editor, and adds nothing to a shipped game.
* **Mutually exclusive with AssImp** - both importer gems declare the same `provided_unique_service`
  (`SceneImporter`) in their `gem.json`, so a project can only enable one of them. Enabling the
  second gem is rejected by `o3de enable-gem` and the Project Manager compatibility check, and CMake
  fails configuration with a clear error if both end up enabled anyway.
* **Configurable extensions** - the advertised formats come from
  `/O3DE/SceneAPI/ImportProviderFileTypeExtensions/UniversalSceneImporter` and are shared by the
  Scene Builder job patterns and the Asset Processor `.assetinfo` metadata registration.
* **Format independent edge case handling** - both converters normalize their data into one
  representation, so limits, validation and error reporting behave the same for every format.

## What is imported

| Data | FBX (ufbx) | glTF / GLB (cgltf) |
| --- | --- | --- |
| Node hierarchy and transforms | yes | yes |
| Static triangle meshes | yes (triangulated) | yes (triangles, strips, fans, non indexed) |
| Skinned meshes and skeleton | yes | yes |
| Bone hierarchy | yes | yes |
| UV sets (up to 4) | yes | yes |
| Vertex color sets (up to 4) | yes | yes |
| Tangents / bitangents | yes | yes, generated when missing and UVs are available |
| PBR materials and textures | yes | yes |
| Embedded textures | extracted on demand | data URIs and GLB buffer views |
| Bone/node animation | yes (30 fps sampling) | yes (linear, step and cubic spline sampling) |
| Morph targets | yes | yes |
| Morph (blend shape) animation | yes | yes |

## Coordinate system and units

All source data is converted to O3DE's **Z-up, right-handed** space and to **meters before** it
reaches the SceneAPI importers:

* FBX is loaded with ufbx's `right_handed_z_up` target axes and `target_unit_meters = 1`.
  Geometry transforms, pivots and inherit modes are baked into the scene graph.
* glTF is Y-up; the converter applies the same axis conversion the SceneAPI uses for Y-up sources
  (`(x, y, z) -> (-x, z, y)`) to positions, directions and local transforms.

Because of this, the import context provider reports `Z` up / unit size `1` and does not rely on the
SceneAPI's root transform adjustment.

## Edge cases covered

The converters and importers explicitly handle, log and degrade gracefully for:

* **Detection** - extension and content based detection, including files with misleading extensions
  (the content wins). Unsupported files fail with an actionable message.
* **File level** - missing/unreadable/empty files, malformed JSON, unsupported glTF versions,
  invalid GLB chunks, missing external `.bin` buffers, missing textures (logged and skipped, not
  failures).
* **Geometry** - non-indexed primitives, triangle strips/fans (converted to lists), degenerate
  triangles (dropped), out of range indices (dropped), missing normals (generated), missing UVs
  (zero-filled), missing colors (white), missing tangents (generated when possible, otherwise
  stable fallbacks), primitives with no positions or no material.
* **Compression** - Draco and `EXT_meshopt_compression` are detected and rejected with a clear
  message instead of producing corrupt geometry. `KHR_texture_basisu` and `EXT_texture_webp`
  images are referenced using their extension images.
* **Accessors** - sparse accessors are resolved, normalized integer attributes are converted,
  out-of-bounds counts are rejected, glTF validation failures are reported as warnings and the read
  paths remain bounds checked.
* **Scenes** - missing default scene (scene 0 is used), scenes without any scene object (all root
  nodes are imported), multiple scenes (the default is imported with a warning), disconnected or
  cyclic node hierarchies (broken cycles are reported and detached).
* **Names** - empty names, duplicate names, names containing the scene graph separator and names
  with `%` are sanitized or renamed; skin weight bone names are resolved against the final scene
  graph names during finalization.
* **Skinning** - duplicate joints are merged, weights are normalized, more than eight influences are
  reduced to the strongest eight, joints missing from the imported scene are dropped with a warning,
  and inverse bind matrices are read for diagnostics.
* **Animation** - zero/negative durations are skipped, missing channels fall back to the bind
  transform, times outside the key range are clamped, cubic spline tangents are evaluated, animation
  frame counts are bounded, and non-bone object animation is skipped (O3DE motions are skeletal).
* **Materials** - external, absolute, embedded and GLB buffer view textures; texture paths are
  resolved to scan folder relative source assets, stale or non-portable paths fall back to a file
  next to the scene, and textures that cannot be resolved are logged and skipped instead of being
  emitted as broken material references; embedded textures are written next to the source scene only
  when the asset's *Extract embedded textures* setting is on; all texture slots (base color, diffuse,
  specular, normal, bump, metallic, roughness, AO, emissive) are populated when the source provides
  them.

## Configuration

Enable the gem for a project:

```sh
o3de enable-gem -gn UniversalSceneImporter -pp <project-path>
```

The gem publishes its extensions in `Registry/sceneassetimporter.setreg`:

```json
{
    "O3DE": {
        "SceneAPI": {
            "ImportProviderFileTypeExtensions": {
                "UniversalSceneImporter": [ ".fbx", ".gltf", ".glb" ]
            }
        }
    }
}
```

Remove an entry (for example `".fbx"`) to stop importing that format through this gem. If the
object or the array is missing entirely the gem falls back to all three formats.

### Embedded textures

For GLB files and FBX files with embedded textures, enable *Extract embedded textures* on the source
asset (`<file>.assetinfo` -> import settings) to have the images written next to the scene file so
the material converter can reference them.
Projects can also set `/O3DE/Preferences/SceneAPI/ImportSettings/ExtractEmbeddedTextures` to enable
extraction by default for scenes without an asset-specific import-settings group.

### Importer conflicts

This gem and the AssImp gem are mutually exclusive. Both declare
`"provided_unique_service": "SceneImporter"` in their `gem.json`, which makes the engine treat them
as alternative providers of the same unique service:

* `o3de enable-gem` refuses to enable one when the other is already enabled in the project.
* The Project Manager reports the conflict through the standard gem compatibility check.
* CMake configuration fails with `conflicting gems that provide the same service 'SceneImporter'`
  if both are listed in `gem_names` anyway (for example after editing `project.json` by hand).

| AssImp | UniversalSceneImporter | Result |
| --- | --- | --- |
| Enabled | Enabled | Not allowed: rejected at enable time; CMake configure fails if forced. |
| Enabled | Disabled | AssImp handles the formats registered to it. |
| Disabled | Enabled | UniversalSceneImporter handles its formats independently. |

To switch a project from one importer to the other, disable the active gem first, then enable the
other one.

## Building and testing

The gem uses the standard O3DE gem build:

```sh
cmake -B build/windows -S . -G "Ninja Multi-Config" -DLY_3RDPARTY_PATH=<packages>
cmake --build build/windows --target UniversalSceneImporter.Editor --config profile
cmake --build build/windows --target UniversalSceneImporter.Editor.Tests --config profile
```

Run the unit tests with the O3DE test runner:

```sh
bin/profile/AzTestRunner.exe UniversalSceneImporter.Editor.Tests.dll AzRunUnitTests
```

The tests generate their own glTF/GLB fixtures, exercise the converters and wrapper directly, and
(where the engine test assets are available) import real FBX files such as `AutomatedTesting/Objects/cube.fbx`,
the skinned `Jack.fbx` character and an animation-only motion file.

## Limitations

* O3DE only supports triangle meshes. Points, lines and line loops are skipped with a warning.
* Draco and meshopt compressed glTF files must be re-exported without compression.
* Only bone (skeletal) node animation is imported. Object animation tracks are ignored because O3DE
  motions are skeletal.
* Motion extraction and root motion baking are not performed by this gem; use the standard O3DE
  motion pipeline on the imported animation data.
* NURBS, subdivision surfaces, cameras and lights are not imported.

## Requirements

The SceneAPI `ImportContextRegistry` is a Scene System component in `SceneBuilder`. It must be
registered by `Code/Tools/SceneAPI/SceneBuilder/DllMain.cpp`; without that registration no import
provider gem (including AssImp) can register itself. This branch contains the registration; when
porting the gem to another engine revision, make sure the same one line is present:

```cpp
g_componentDescriptors.push_back(ImportContextRegistryComponent::CreateDescriptor());
```

The registry implementation (`ImportContextRegistryManager`) must also live in the `SceneBuilder`
module only. `SceneProcessing` must not statically link `SceneBuilder.Static` or register the
registry component itself, otherwise it embeds a second copy of the manager and providers
registered through one copy are invisible to the other. This branch updates the `SceneBuilder`
module to link its static library privately and removes the duplicate component registration from
`SceneProcessing`; the `SceneProcessing` module still declares a runtime dependency on
`SceneBuilder`.

## Validation

The gem was enabled in the `AutomatedTesting` project and exercised by that project's asset
pipeline. The Asset Processor run that processed the project's scene library used this gem for
all 97 scene compilation jobs with zero failed assets, including skinned characters, motion only
FBX files, morph targets and the fixtures in `AutomatedTesting/Assets/UniversalSceneImporterTest`
(see `AutomatedTesting/UniversalSceneImporterTest.md`). AutomatedTesting now enables AssImp again
because the project also contains thousands of scene assets in formats only AssImp covers
(`.obj`, `.usd`, `.stl`, `.dae`, `.blend`, `.3ds`); the two gems can no longer be enabled at the
same time.

The SceneBuilder registry tests cover provider registration order precedence, extension unions and
removal fallback. The importer unit tests cover extension normalization, provider selection, format
detection (including misleading extensions), embedded and external glTF buffers, GLB chunks,
triangle strips, sparse accessors, embedded textures, malformed files, missing buffers, skins/UVs
and real FBX files (static cube, skinned character and animation only motion).

## Third party

See [Code/Source/External/README.md](Code/Source/External/README.md) for the vendored ufbx and cgltf
versions and licenses.
