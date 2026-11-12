# UniversalSceneImporter test assets

These files are Asset Processor integration fixtures for the `UniversalSceneImporter` gem. They are
small, self contained scenes that exercise each source format through the Scene Builder pipeline.

| File | Purpose |
| --- | --- |
| `triangle.gltf` | glTF with an embedded base64 buffer and a PBR material. |
| `triangle.glb` | The same scene packed as a binary GLB. |
| `triangle_uv.gltf` | glTF with `TEXCOORD_0`, used to verify the UV stream import. |
| `skinned_triangle.gltf` | glTF with a two joint skin, joint indices/weights and inverse bind matrices. |
| `cube.fbx` | Copy of `AutomatedTesting/Objects/cube.fbx`; binary FBX with a textured material. |

Process them with:

```sh
AssetProcessorBatch.exe --project-path <engine>/AutomatedTesting --platform pc \
    --reprocessFileList <engine>/AutomatedTesting/build/usc_reprocess.txt
```
