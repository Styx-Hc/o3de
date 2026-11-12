/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <AzCore/IO/Path/Path.h>
#include <AzCore/IO/SystemFile.h>
#include <AzCore/UnitTest/UnitTest.h>
#include <AzCore/Utils/Utils.h>
#include <AzTest/AzTest.h>

#include "../Source/Tools/Contexts/UniversalImportContextProvider.h"
#include "../Source/Tools/Scene/UniversalSceneWrapper.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    std::string GetTestDirectory()
    {
        const fs::path directory = fs::temp_directory_path() / "UniversalSceneImporterTests";
        std::error_code error;
        fs::create_directories(directory, error);
        return directory.string();
    }

    //! Finds the engine root by walking up from the test executable. AZ::Utils::GetEnginePath()
    //! depends on the settings registry being populated, which is not guaranteed for test runners.
    AZ::IO::Path FindEngineRoot()
    {
        AZ::IO::Path directory(AZ::Utils::GetExecutableDirectory().c_str());
        for (int depth = 0; depth < 12; ++depth)
        {
            if (AZ::IO::SystemFile::Exists((directory / "engine.json").c_str()))
            {
                return directory;
            }
            const AZStd::string parentString = directory.ParentPath().String();
            if (parentString.empty())
            {
                break;
            }
            AZ::IO::Path parent(parentString.c_str());
            if (parent == directory)
            {
                break;
            }
            directory = parent;
        }
        return AZ::IO::Path();
    }

    std::string WriteTestFile(const std::string& name, const void* data, size_t size)
    {
        const fs::path path = fs::path(GetTestDirectory()) / name;
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        stream.close();
        return path.string();
    }

    std::string WriteTestFile(const std::string& name, const std::string& contents)
    {
        return WriteTestFile(name, contents.data(), contents.size());
    }

    std::string Base64Encode(const std::vector<AZ::u8>& data)
    {
        static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        result.reserve(((data.size() + 2) / 3) * 4);
        size_t index = 0;
        while (index + 2 < data.size())
        {
            const AZ::u32 value = (data[index] << 16) | (data[index + 1] << 8) | data[index + 2];
            result.push_back(alphabet[(value >> 18) & 0x3F]);
            result.push_back(alphabet[(value >> 12) & 0x3F]);
            result.push_back(alphabet[(value >> 6) & 0x3F]);
            result.push_back(alphabet[value & 0x3F]);
            index += 3;
        }
        if (index + 1 == data.size())
        {
            const AZ::u32 value = data[index] << 16;
            result.push_back(alphabet[(value >> 18) & 0x3F]);
            result.push_back(alphabet[(value >> 12) & 0x3F]);
            result.push_back('=');
            result.push_back('=');
        }
        else if (index + 2 == data.size())
        {
            const AZ::u32 value = (data[index] << 16) | (data[index + 1] << 8);
            result.push_back(alphabet[(value >> 18) & 0x3F]);
            result.push_back(alphabet[(value >> 12) & 0x3F]);
            result.push_back(alphabet[(value >> 6) & 0x3F]);
            result.push_back('=');
        }
        return result;
    }

    void AppendFloat(std::vector<AZ::u8>& bytes, float value)
    {
        AZ::u8 raw[4];
        std::memcpy(raw, &value, sizeof(value));
        bytes.insert(bytes.end(), raw, raw + 4);
    }

    void AppendUint16(std::vector<AZ::u8>& bytes, AZ::u16 value)
    {
        bytes.push_back(static_cast<AZ::u8>(value & 0xFF));
        bytes.push_back(static_cast<AZ::u8>((value >> 8) & 0xFF));
    }

    void AppendUint32(std::vector<AZ::u8>& bytes, AZ::u32 value)
    {
        bytes.push_back(static_cast<AZ::u8>(value & 0xFF));
        bytes.push_back(static_cast<AZ::u8>((value >> 8) & 0xFF));
        bytes.push_back(static_cast<AZ::u8>((value >> 16) & 0xFF));
        bytes.push_back(static_cast<AZ::u8>((value >> 24) & 0xFF));
    }

    std::vector<AZ::u8> BuildTriangleBuffer()
    {
        std::vector<AZ::u8> buffer;
        // Positions: (0,0,0), (1,0,0), (0,1,0)
        AppendFloat(buffer, 0.0f);
        AppendFloat(buffer, 0.0f);
        AppendFloat(buffer, 0.0f);
        AppendFloat(buffer, 1.0f);
        AppendFloat(buffer, 0.0f);
        AppendFloat(buffer, 0.0f);
        AppendFloat(buffer, 0.0f);
        AppendFloat(buffer, 1.0f);
        AppendFloat(buffer, 0.0f);
        // Indices: 0, 1, 2
        AppendUint16(buffer, 0);
        AppendUint16(buffer, 1);
        AppendUint16(buffer, 2);
        return buffer;
    }

    std::string BuildTriangleGltf(bool useEmbeddedBuffer)
    {
        const std::vector<AZ::u8> buffer = BuildTriangleBuffer();
        const std::string bufferUri = useEmbeddedBuffer
            ? "data:application/octet-stream;base64," + Base64Encode(buffer)
            : "triangle.bin";
        if (!useEmbeddedBuffer)
        {
            WriteTestFile("triangle.bin", buffer.data(), buffer.size());
        }

        return R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [ { "nodes": [ 0 ] } ],
            "nodes": [ { "name": "Triangle", "mesh": 0 } ],
            "meshes": [ {
                "name": "TriangleMesh",
                "primitives": [ {
                    "attributes": { "POSITION": 0 },
                    "indices": 1,
                    "material": 0
                } ]
            } ],
            "materials": [ {
                "name": "TriangleMaterial",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [ 1.0, 0.5, 0.25, 1.0 ],
                    "metallicFactor": 0.25,
                    "roughnessFactor": 0.75
                }
            } ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                  "min": [ 0, 0, 0 ], "max": [ 1, 1, 0 ] },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "buffers": [ { "byteLength": 42, "uri": ")" + bufferUri + R"(" } ]
        })";
    }

    std::vector<AZ::u8> BuildGlb(const std::string& json, const std::vector<AZ::u8>& binary)
    {
        std::string paddedJson = json;
        while ((paddedJson.size() % 4) != 0)
        {
            paddedJson.push_back(' ');
        }
        std::vector<AZ::u8> paddedBinary = binary;
        while ((paddedBinary.size() % 4) != 0)
        {
            paddedBinary.push_back(0);
        }

        const AZ::u32 jsonChunkLength = static_cast<AZ::u32>(paddedJson.size());
        const AZ::u32 binaryChunkLength = static_cast<AZ::u32>(paddedBinary.size());
        const AZ::u32 totalLength = 12 + 8 + jsonChunkLength + (binaryChunkLength > 0 ? 8 + binaryChunkLength : 0);

        std::vector<AZ::u8> glb;
        AppendUint32(glb, 0x46546C67); // "glTF"
        AppendUint32(glb, 2);
        AppendUint32(glb, totalLength);
        AppendUint32(glb, jsonChunkLength);
        AppendUint32(glb, 0x4E4F534A); // "JSON"
        glb.insert(glb.end(), paddedJson.begin(), paddedJson.end());
        if (binaryChunkLength > 0)
        {
            AppendUint32(glb, binaryChunkLength);
            AppendUint32(glb, 0x004E4942); // "BIN"
            glb.insert(glb.end(), paddedBinary.begin(), paddedBinary.end());
        }
        return glb;
    }

} // namespace

//////////////////////////////////////////////////////////////////////////
// Extension and format detection
//////////////////////////////////////////////////////////////////////////

TEST(UniversalSceneImporterTests, NormalizeExtension_HandlesCommonSpellings)
{
    using UniversalSceneImporter::UniversalImportContextProvider;

    EXPECT_EQ(UniversalImportContextProvider::NormalizeExtension(".fbx"), ".fbx");
    EXPECT_EQ(UniversalImportContextProvider::NormalizeExtension(".FBX"), ".fbx");
    EXPECT_EQ(UniversalImportContextProvider::NormalizeExtension("fbx"), ".fbx");
    EXPECT_EQ(UniversalImportContextProvider::NormalizeExtension("*.GLTF"), ".gltf");
    EXPECT_EQ(UniversalImportContextProvider::NormalizeExtension("  .Glb  "), ".glb");
    EXPECT_TRUE(UniversalImportContextProvider::NormalizeExtension("").empty());
    EXPECT_TRUE(UniversalImportContextProvider::NormalizeExtension(".").empty());
    EXPECT_TRUE(UniversalImportContextProvider::NormalizeExtension("*").empty());
}

TEST(UniversalSceneImporterTests, Provider_ClaimsSupportedExtensions)
{
    UniversalSceneImporter::UniversalImportContextProvider provider;

    EXPECT_TRUE(provider.CanHandleExtension(".fbx"));
    EXPECT_TRUE(provider.CanHandleExtension(".FBX"));
    EXPECT_TRUE(provider.CanHandleExtension("fbx"));
    EXPECT_TRUE(provider.CanHandleExtension(".gltf"));
    EXPECT_TRUE(provider.CanHandleExtension(".glb"));
    EXPECT_FALSE(provider.CanHandleExtension(".obj"));
    EXPECT_FALSE(provider.CanHandleExtension(".usd"));
    EXPECT_FALSE(provider.CanHandleExtension(""));
    EXPECT_EQ(provider.GetImporterName(), "UniversalSceneImporter");
}

TEST(UniversalSceneImporterTests, DetectFormat_UsesExtensionAndContent)
{
    using UniversalSceneImporter::SourceFormat;
    using UniversalSceneImporter::UniversalSceneWrapper;

    const std::string jsonPath = WriteTestFile("detect.gltf", BuildTriangleGltf(true));
    EXPECT_EQ(UniversalSceneWrapper::DetectFormat(jsonPath.c_str()), SourceFormat::Gltf);

    const std::string fbxPath = WriteTestFile(
        "detect.fbx",
        "Kaydara FBX Binary  \x00\x1a\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00");
    EXPECT_EQ(UniversalSceneWrapper::DetectFormat(fbxPath.c_str()), SourceFormat::Fbx);

    // Content wins over a misleading extension.
    const std::string mislabeledPath = WriteTestFile("mislabeled.fbx", BuildTriangleGltf(true));
    EXPECT_EQ(UniversalSceneWrapper::DetectFormat(mislabeledPath.c_str()), SourceFormat::Gltf);

    const std::string unknownPath = WriteTestFile("unknown.txt", "not a scene");
    EXPECT_EQ(UniversalSceneWrapper::DetectFormat(unknownPath.c_str()), SourceFormat::Unknown);
}

TEST(UniversalSceneImporterTests, Wrapper_RejectsUnsupportedFiles)
{
    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    const std::string path = WriteTestFile("unsupported.txt", "hello");

    // The wrapper reports an error for unsupported files; suppress the expected trace so the test
    // runner does not treat it as a failure.
    AZ_TEST_START_TRACE_SUPPRESSION;
    EXPECT_FALSE(wrapper.LoadSceneFromFile(path.c_str(), settings));
    AZ_TEST_STOP_TRACE_SUPPRESSION_NO_COUNT;
}

TEST(UniversalSceneImporterTests, Wrapper_RejectsMalformedGltf)
{
    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    const std::string path = WriteTestFile("malformed.gltf", "{ this is not json ");

    AZ_TEST_START_TRACE_SUPPRESSION;
    EXPECT_FALSE(wrapper.LoadSceneFromFile(path.c_str(), settings));
    AZ_TEST_STOP_TRACE_SUPPRESSION_NO_COUNT;
}

//////////////////////////////////////////////////////////////////////////
// glTF loading through the SceneWrapper
//////////////////////////////////////////////////////////////////////////

TEST(UniversalSceneImporterTests, Gltf_EmbeddedTriangle)
{
    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    const std::string path = WriteTestFile("triangle_embedded.gltf", BuildTriangleGltf(true));

    ASSERT_TRUE(wrapper.LoadSceneFromFile(path.c_str(), settings));

    const auto& scene = wrapper.GetScene();
    EXPECT_EQ(scene.GetFormat(), UniversalSceneImporter::SourceFormat::Gltf);
    EXPECT_EQ(scene.GetNodes().size(), 1u);
    EXPECT_EQ(scene.GetMeshes().size(), 1u);
    EXPECT_EQ(scene.GetMaterials().size(), 1u);
    EXPECT_EQ(scene.GetVertexCount(), 3u);
    EXPECT_FALSE(scene.GetMeshes()[0].m_primitives.empty());
    EXPECT_EQ(scene.GetMeshes()[0].m_primitives[0].m_indices.size(), 3u);

    // glTF is Y-up: (0,0,0), (1,0,0), (0,1,0) becomes (0,0,0), (-1,0,0), (0,0,1).
    const AZ::Aabb& aabb = scene.GetAabb();
    EXPECT_TRUE(aabb.IsValid());
    EXPECT_NEAR(aabb.GetMin().GetX(), -1.0f, 0.001f);
    EXPECT_NEAR(aabb.GetMin().GetY(), 0.0f, 0.001f);
    EXPECT_NEAR(aabb.GetMin().GetZ(), 0.0f, 0.001f);
    EXPECT_NEAR(aabb.GetMax().GetX(), 0.0f, 0.001f);
    EXPECT_NEAR(aabb.GetMax().GetY(), 0.0f, 0.001f);
    EXPECT_NEAR(aabb.GetMax().GetZ(), 1.0f, 0.001f);

    const UniversalSceneImporter::Material& material = scene.GetMaterials()[0];
    EXPECT_EQ(material.m_name, "TriangleMaterial");
    EXPECT_NEAR(material.m_metallic, 0.25f, 0.001f);
    EXPECT_NEAR(material.m_roughness, 0.75f, 0.001f);
    EXPECT_NEAR(material.m_baseColor.GetY(), 0.5f, 0.001f);
    EXPECT_NE(material.m_uniqueId, 0u);
}

TEST(UniversalSceneImporterTests, Gltf_ExternalBuffer)
{
    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    const std::string path = WriteTestFile("triangle_external.gltf", BuildTriangleGltf(false));

    ASSERT_TRUE(wrapper.LoadSceneFromFile(path.c_str(), settings));
    EXPECT_EQ(wrapper.GetScene().GetVertexCount(), 3u);
}

TEST(UniversalSceneImporterTests, Gltf_MissingExternalBufferFailsGracefully)
{
    std::string json = BuildTriangleGltf(false);
    const std::string marker = "triangle.bin";
    const size_t position = json.find(marker);
    ASSERT_NE(position, std::string::npos);
    json.replace(position, marker.size(), "missing_buffer.bin");

    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    const std::string path = WriteTestFile("triangle_missing_buffer.gltf", json);

    AZ_TEST_START_TRACE_SUPPRESSION;
    EXPECT_FALSE(wrapper.LoadSceneFromFile(path.c_str(), settings));
    AZ_TEST_STOP_TRACE_SUPPRESSION_NO_COUNT;
}

TEST(UniversalSceneImporterTests, Glb_BinaryTriangle)
{
    const std::string json = R"({
        "asset": { "version": "2.0" },
        "scene": 0,
        "scenes": [ { "nodes": [ 0 ] } ],
        "nodes": [ { "name": "Triangle", "mesh": 0 } ],
        "meshes": [ { "name": "Mesh", "primitives": [ {
            "attributes": { "POSITION": 0 }, "indices": 1 } ] } ],
        "accessors": [
            { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
              "min": [ 0, 0, 0 ], "max": [ 1, 1, 0 ] },
            { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
        ],
        "bufferViews": [
            { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
            { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
        ],
        "buffers": [ { "byteLength": 42 } ]
    })";

    const std::vector<AZ::u8> glb = BuildGlb(json, BuildTriangleBuffer());
    const std::string path = WriteTestFile("triangle.glb", glb.data(), glb.size());

    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    ASSERT_TRUE(wrapper.LoadSceneFromFile(path.c_str(), settings));
    EXPECT_EQ(wrapper.GetScene().GetVertexCount(), 3u);
    EXPECT_EQ(wrapper.GetScene().GetMeshes().size(), 1u);
}

TEST(UniversalSceneImporterTests, Gltf_TriangleStripIsConvertedToTriangleList)
{
    std::vector<AZ::u8> buffer;
    for (int index = 0; index < 4; ++index)
    {
        AppendFloat(buffer, static_cast<float>(index));
        AppendFloat(buffer, 0.0f);
        AppendFloat(buffer, 0.0f);
    }
    AppendUint16(buffer, 0);
    AppendUint16(buffer, 1);
    AppendUint16(buffer, 2);
    AppendUint16(buffer, 3);

    const std::string json = R"({
        "asset": { "version": "2.0" },
        "scene": 0,
        "scenes": [ { "nodes": [ 0 ] } ],
        "nodes": [ { "mesh": 0 } ],
        "meshes": [ { "primitives": [ {
            "attributes": { "POSITION": 0 }, "indices": 1, "mode": 5 } ] } ],
        "accessors": [
            { "bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3" },
            { "bufferView": 1, "componentType": 5123, "count": 4, "type": "SCALAR" }
        ],
        "bufferViews": [
            { "buffer": 0, "byteOffset": 0, "byteLength": 48 },
            { "buffer": 0, "byteOffset": 48, "byteLength": 8 }
        ],
        "buffers": [ { "byteLength": 56, "uri": "data:application/octet-stream;base64,)" + Base64Encode(buffer) + R"(" } ]
    })";

    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    const std::string path = WriteTestFile("strip.gltf", json);
    ASSERT_TRUE(wrapper.LoadSceneFromFile(path.c_str(), settings));
    ASSERT_EQ(wrapper.GetScene().GetMeshes().size(), 1u);
    EXPECT_EQ(wrapper.GetScene().GetMeshes()[0].m_primitives[0].m_indices.size(), 6u);
    EXPECT_EQ(wrapper.GetScene().GetVertexCount(), 4u);
}

TEST(UniversalSceneImporterTests, Gltf_NoSceneFallsBackToRootNodes)
{
    const std::string json = R"({
        "asset": { "version": "2.0" },
        "nodes": [ { "name": "LooseNode" } ]
    })";

    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    const std::string path = WriteTestFile("no_scene.gltf", json);
    ASSERT_TRUE(wrapper.LoadSceneFromFile(path.c_str(), settings));
    EXPECT_EQ(wrapper.GetScene().GetNodes().size(), 1u);
    EXPECT_EQ(wrapper.GetScene().GetVertexCount(), 0u);
    EXPECT_TRUE(wrapper.GetScene().GetNodes()[0].m_isRoot);
}

TEST(UniversalSceneImporterTests, Gltf_EmbeddedTextureDataUri)
{
    std::vector<AZ::u8> buffer = BuildTriangleBuffer();
    // Minimal 1x1 PNG header appended to the binary buffer (only the header is needed to detect
    // the format; the material importer copies the bytes verbatim).
    static const AZ::u8 pngBytes[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    buffer.insert(buffer.end(), std::begin(pngBytes), std::end(pngBytes));

    const std::string json = R"({
        "asset": { "version": "2.0" },
        "scene": 0,
        "scenes": [ { "nodes": [ 0 ] } ],
        "nodes": [ { "mesh": 0 } ],
        "meshes": [ { "primitives": [ {
            "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 } ] } ],
        "materials": [ {
            "name": "Textured",
            "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } }
        } ],
        "textures": [ { "source": 0 } ],
        "images": [ { "uri": "data:image/png;base64,)" + Base64Encode(std::vector<AZ::u8>(std::begin(pngBytes), std::end(pngBytes))) + R"(" } ],
        "accessors": [
            { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
            { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
        ],
        "bufferViews": [
            { "buffer": 0, "byteOffset": 0, "byteLength": 36 },
            { "buffer": 0, "byteOffset": 36, "byteLength": 6 }
        ],
        "buffers": [ { "byteLength": 42, "uri": "data:application/octet-stream;base64,)" + Base64Encode(buffer) + R"(" } ]
    })";

    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    const std::string path = WriteTestFile("textured.gltf", json);
    ASSERT_TRUE(wrapper.LoadSceneFromFile(path.c_str(), settings));

    ASSERT_EQ(wrapper.GetScene().GetMaterials().size(), 1u);
    const auto& texture = wrapper.GetScene().GetMaterials()[0].GetTexture(UniversalSceneImporter::MaterialTexture::BaseColor);
    EXPECT_TRUE(texture.IsEmbedded());
    EXPECT_EQ(texture.m_extension, "png");
    EXPECT_EQ(texture.m_embeddedData.size(), sizeof(pngBytes));
}

TEST(UniversalSceneImporterTests, Gltf_SparseAccessorIsResolved)
{
    // Positions live in bufferView 0 with a dummy third vertex. A sparse accessor overrides
    // index 2 with (2, 2, 0).
    std::vector<AZ::u8> buffer;
    AppendFloat(buffer, 0.0f);
    AppendFloat(buffer, 0.0f);
    AppendFloat(buffer, 0.0f);
    AppendFloat(buffer, 1.0f);
    AppendFloat(buffer, 0.0f);
    AppendFloat(buffer, 0.0f);
    AppendFloat(buffer, 0.0f); // dummy third vertex, replaced by the sparse value
    AppendFloat(buffer, 0.0f);
    AppendFloat(buffer, 0.0f);
    // Sparse index: 2 (uint16) plus padding so the values stay 4 byte aligned.
    AppendUint16(buffer, 2);
    AppendUint16(buffer, 0);
    // Sparse value: (2, 2, 0)
    AppendFloat(buffer, 2.0f);
    AppendFloat(buffer, 2.0f);
    AppendFloat(buffer, 0.0f);
    // Indices
    AppendUint16(buffer, 0);
    AppendUint16(buffer, 1);
    AppendUint16(buffer, 2);

    const std::string json = R"({
        "asset": { "version": "2.0" },
        "scene": 0,
        "scenes": [ { "nodes": [ 0 ] } ],
        "nodes": [ { "mesh": 0 } ],
        "meshes": [ { "primitives": [ {
            "attributes": { "POSITION": 0 }, "indices": 1 } ] } ],
        "accessors": [
            { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
              "sparse": {
                "count": 1,
                "indices": { "bufferView": 1, "componentType": 5123 },
                "values": { "bufferView": 2 }
              } },
            { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
        ],
        "bufferViews": [
            { "buffer": 0, "byteOffset": 0, "byteLength": 36 },
            { "buffer": 0, "byteOffset": 36, "byteLength": 4 },
            { "buffer": 0, "byteOffset": 40, "byteLength": 12 },
            { "buffer": 0, "byteOffset": 52, "byteLength": 6 }
        ],
        "buffers": [ { "byteLength": 58, "uri": "data:application/octet-stream;base64,)" + Base64Encode(buffer) + R"(" } ]
    })";

    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    const std::string path = WriteTestFile("sparse.gltf", json);
    ASSERT_TRUE(wrapper.LoadSceneFromFile(path.c_str(), settings));
    ASSERT_EQ(wrapper.GetScene().GetMeshes().size(), 1u);
    const auto& positions = wrapper.GetScene().GetMeshes()[0].m_primitives[0].m_positions;
    ASSERT_EQ(positions.size(), 3u);
    // Third position is (2, 2, 0) in glTF space -> (-2, 0, 2) in O3DE space.
    EXPECT_NEAR(positions[2].GetX(), -2.0f, 0.001f);
    EXPECT_NEAR(positions[2].GetY(), 0.0f, 0.001f);
    EXPECT_NEAR(positions[2].GetZ(), 2.0f, 0.001f);
}

//////////////////////////////////////////////////////////////////////////
// FBX loading (skipped when the engine test asset is not available)
//////////////////////////////////////////////////////////////////////////

TEST(UniversalSceneImporterTests, Fbx_CubeLoads)
{
    const AZ::IO::Path enginePath = FindEngineRoot();
    if (enginePath.empty())
    {
        GTEST_SKIP() << "Engine path is not available.";
    }

    const AZ::IO::Path cubePath = enginePath / "AutomatedTesting" / "Objects" / "cube.fbx";
    if (!AZ::IO::SystemFile::Exists(cubePath.c_str()))
    {
        GTEST_SKIP() << "cube.fbx is not available.";
    }

    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    ASSERT_TRUE(wrapper.LoadSceneFromFile(cubePath.c_str(), settings));

    const auto& scene = wrapper.GetScene();
    EXPECT_EQ(scene.GetFormat(), UniversalSceneImporter::SourceFormat::Fbx);
    EXPECT_GT(scene.GetNodes().size(), 1u);
    EXPECT_GE(scene.GetMeshes().size(), 1u);
    EXPECT_GT(scene.GetVertexCount(), 0u);
    EXPECT_TRUE(scene.GetAabb().IsValid());

    auto root = wrapper.GetRootNode();
    ASSERT_TRUE(root != nullptr);
    EXPECT_GE(root->GetChildCount(), 1);
}

TEST(UniversalSceneImporterTests, Fbx_SkinnedCharacterLoads)
{
    const AZ::IO::Path enginePath = FindEngineRoot();
    if (enginePath.empty())
    {
        GTEST_SKIP() << "Engine path is not available.";
    }

    const AZ::IO::Path characterPath = enginePath / "AutomatedTesting" / "Objects" / "Characters" / "Jack" / "Jack.fbx";
    if (!AZ::IO::SystemFile::Exists(characterPath.c_str()))
    {
        GTEST_SKIP() << "Jack.fbx is not available.";
    }

    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    ASSERT_TRUE(wrapper.LoadSceneFromFile(characterPath.c_str(), settings));

    const auto& scene = wrapper.GetScene();
    EXPECT_EQ(scene.GetFormat(), UniversalSceneImporter::SourceFormat::Fbx);

    size_t skinnedMeshCount = 0;
    for (const auto& mesh : scene.GetMeshes())
    {
        if (mesh.IsSkinned())
        {
            ++skinnedMeshCount;
        }
    }
    EXPECT_GT(skinnedMeshCount, 0u);

    size_t boneCount = 0;
    for (const auto& node : scene.GetNodes())
    {
        if (node.m_isBone)
        {
            ++boneCount;
        }
    }
    EXPECT_GT(boneCount, 0u);
}

TEST(UniversalSceneImporterTests, Fbx_MotionLoadsAnimations)
{
    const AZ::IO::Path enginePath = FindEngineRoot();
    if (enginePath.empty())
    {
        GTEST_SKIP() << "Engine path is not available.";
    }

    const AZ::IO::Path motionPath =
        enginePath / "AutomatedTesting" / "Animations" / "Motions" / "Jack_Idle_Aim_ZUp.fbx";
    if (!AZ::IO::SystemFile::Exists(motionPath.c_str()))
    {
        GTEST_SKIP() << "Jack_Idle_Aim_ZUp.fbx is not available.";
    }

    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    ASSERT_TRUE(wrapper.LoadSceneFromFile(motionPath.c_str(), settings));

    const auto& scene = wrapper.GetScene();
    EXPECT_GT(scene.GetAnimations().size(), 0u);
    ASSERT_FALSE(scene.GetAnimations().empty());

    const auto& animation = scene.GetAnimations()[0];
    EXPECT_FALSE(animation.m_name.empty());
    EXPECT_GT(animation.m_timeStep, 0.0f);
    EXPECT_FALSE(animation.m_nodeAnimations.empty());

    bool hasKeyFrames = false;
    for (const auto& nodeAnimation : animation.m_nodeAnimations)
    {
        if (!nodeAnimation.m_keyFrames.empty())
        {
            hasKeyFrames = true;
            break;
        }
    }
    EXPECT_TRUE(hasKeyFrames);
}

TEST(UniversalSceneImporterTests, Gltf_SkinnedTriangleImportsSkinWeightsAndTangents)
{
    const AZ::IO::Path enginePath = FindEngineRoot();
    if (enginePath.empty())
    {
        GTEST_SKIP() << "Engine path is not available.";
    }
    const AZ::IO::Path path =
        enginePath / "AutomatedTesting" / "Assets" / "UniversalSceneImporterTest" / "skinned_triangle.gltf";
    if (!AZ::IO::SystemFile::Exists(path.c_str()))
    {
        GTEST_SKIP() << "skinned_triangle.gltf is not available.";
    }

    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    ASSERT_TRUE(wrapper.LoadSceneFromFile(path.c_str(), settings));

    const auto& scene = wrapper.GetScene();
    ASSERT_EQ(scene.GetSkins().size(), 1u);
    EXPECT_EQ(scene.GetSkins()[0].m_joints.size(), 2u);
    ASSERT_EQ(scene.GetMeshes().size(), 1u);

    const auto& primitive = scene.GetMeshes()[0].m_primitives[0];
    EXPECT_TRUE(primitive.IsSkinned());
    ASSERT_EQ(primitive.m_skinInfluences.size(), primitive.m_positions.size());
    for (const auto& influences : primitive.m_skinInfluences)
    {
        float totalWeight = 0.0f;
        for (const auto& influence : influences)
        {
            totalWeight += influence.m_weight;
        }
        EXPECT_NEAR(totalWeight, 1.0f, 0.001f);
    }

    // Skinned meshes need tangents. The converter generates a fallback frame when the source has
    // no UVs.
    EXPECT_TRUE(primitive.HasTangents());
    EXPECT_TRUE(primitive.HasBitangents());

    size_t boneCount = 0;
    bool foundSkinnedNode = false;
    for (const auto& node : scene.GetNodes())
    {
        if (node.m_isBone)
        {
            ++boneCount;
        }
        if (node.m_name == "SkinnedMesh")
        {
            foundSkinnedNode = true;
            // glTF ignores the transform of the node that holds a skinned mesh.
            EXPECT_TRUE(node.m_localTransform.GetTranslation().IsZero());
        }
    }
    EXPECT_GE(boneCount, 2u);
    EXPECT_TRUE(foundSkinnedNode);
}

TEST(UniversalSceneImporterTests, Gltf_UvSetIsImported)
{
    const AZ::IO::Path enginePath = FindEngineRoot();
    if (enginePath.empty())
    {
        GTEST_SKIP() << "Engine path is not available.";
    }
    const AZ::IO::Path path =
        enginePath / "AutomatedTesting" / "Assets" / "UniversalSceneImporterTest" / "triangle_uv.gltf";
    if (!AZ::IO::SystemFile::Exists(path.c_str()))
    {
        GTEST_SKIP() << "triangle_uv.gltf is not available.";
    }

    UniversalSceneImporter::UniversalSceneWrapper wrapper;
    AZ::SceneAPI::SceneImportSettings settings;
    ASSERT_TRUE(wrapper.LoadSceneFromFile(path.c_str(), settings));

    const auto& scene = wrapper.GetScene();
    ASSERT_EQ(scene.GetMeshes().size(), 1u);
    const auto& primitive = scene.GetMeshes()[0].m_primitives[0];
    ASSERT_EQ(primitive.m_uvSets[0].size(), 3u);
    EXPECT_NEAR(primitive.m_uvSets[0][1].GetX(), 1.0f, 0.001f);
    EXPECT_NEAR(primitive.m_uvSets[0][1].GetY(), 0.0f, 0.001f);
    EXPECT_NEAR(primitive.m_uvSets[0][2].GetX(), 0.0f, 0.001f);
    EXPECT_NEAR(primitive.m_uvSets[0][2].GetY(), 1.0f, 0.001f);
}

TEST(UniversalSceneImporterTests, Fbx_AuthoredBindIsIndependentOfDefaultPose)
{
    UniversalSceneImporter::Scene scene;
    scene.SetFormat(UniversalSceneImporter::SourceFormat::Fbx);
    scene.GetNodes().resize(2);
    scene.GetNodes()[0].m_localTransform = AZ::Matrix3x4::CreateTranslation(AZ::Vector3(5.0f, 0.0f, 0.0f));
    scene.GetNodes()[1].m_skin = 0;
    scene.GetSkins().resize(1);
    scene.GetSkins()[0].m_joints = { 0 };
    scene.GetSkins()[0].m_inverseBindMatrices = {
        AZ::Matrix3x4::CreateTranslation(AZ::Vector3(-2.0f, 0.0f, 0.0f)) };
    EXPECT_NEAR(scene.GetNodeBindWorldTransform(0).GetTranslation().GetX(), 2.0f, 0.0001f);
    EXPECT_NEAR(scene.GetNodeWorldTransform(0).GetTranslation().GetX(), 5.0f, 0.0001f);
}

TEST(UniversalSceneImporterTests, Gltf_BindIgnoresSkinnedMeshNodeTransformAndConvertsAxes)
{
    UniversalSceneImporter::Scene scene;
    scene.SetFormat(UniversalSceneImporter::SourceFormat::Gltf);
    scene.GetNodes().resize(2);
    scene.GetNodes()[1].m_skin = 0;
    scene.GetNodes()[1].m_localTransform = AZ::Matrix3x4::CreateScale(AZ::Vector3(2.0f));
    scene.GetSkins().resize(1);
    scene.GetSkins()[0].m_joints = { 0 };
    scene.GetSkins()[0].m_inverseBindMatrices = {
        AZ::Matrix3x4::CreateTranslation(AZ::Vector3(0.0f, -3.0f, 0.0f)) };
    const auto bind = scene.GetNodeBindWorldTransform(0);
    EXPECT_NEAR(bind.GetTranslation().GetZ(), 3.0f, 0.0001f);
    EXPECT_NEAR(bind.GetBasisX().GetLength(), 1.0f, 0.0001f);
}

TEST(UniversalSceneImporterTests, Finalize_BonesCarryAuthoredBindPoseInsteadOfDefaultPose)
{
    UniversalSceneImporter::Scene scene;
    scene.SetFormat(UniversalSceneImporter::SourceFormat::Fbx);
    scene.GetNodes().resize(4);
    scene.GetNodes()[0].m_localTransform = AZ::Matrix3x4::CreateTranslation(AZ::Vector3(1.0f, 0.0f, 0.0f));
    scene.GetNodes()[1].m_parent = 0;
    scene.GetNodes()[1].m_isBone = true;
    scene.GetNodes()[1].m_localTransform = AZ::Matrix3x4::CreateTranslation(AZ::Vector3(5.0f, 0.0f, 0.0f));
    scene.GetNodes()[2].m_parent = 1;
    scene.GetNodes()[2].m_isBone = true;
    scene.GetNodes()[2].m_localTransform = AZ::Matrix3x4::CreateTranslation(AZ::Vector3(0.0f, 7.0f, 0.0f));
    scene.GetNodes()[3].m_parent = 0;
    scene.GetNodes()[3].m_skin = 0;
    scene.GetSkins().resize(1);
    scene.GetSkins()[0].m_joints = { 1, 2 };
    scene.GetSkins()[0].m_inverseBindMatrices = {
        AZ::Matrix3x4::CreateTranslation(AZ::Vector3(-2.0f, 0.0f, 0.0f)),
        AZ::Matrix3x4::CreateTranslation(AZ::Vector3(-2.0f, -3.0f, 0.0f)) };
    ASSERT_TRUE(scene.Finalize());

    // The default pose is untouched; animation tracks stay relative to it.
    EXPECT_NEAR(scene.GetNodeWorldTransform(1).GetTranslation().GetX(), 6.0f, 0.0001f);
    // The graph carries the bind pose, placed below the skeleton's parent, so the chain of bone
    // locals EMotionFX accumulates from the root bone equals the inverse of the inverse binds.
    EXPECT_TRUE(scene.GetNodes()[1].m_hasLocalOverride);
    EXPECT_NEAR(scene.GetNodes()[1].m_localOverride.GetTranslation().GetX(), 2.0f, 0.0001f);
    EXPECT_NEAR(scene.GetNodes()[2].m_localOverride.GetTranslation().GetX(), 0.0f, 0.0001f);
    EXPECT_NEAR(scene.GetNodes()[2].m_localOverride.GetTranslation().GetY(), 3.0f, 0.0001f);
    EXPECT_NEAR(scene.GetNodeGraphWorldTransform(2).GetTranslation().GetX(), 3.0f, 0.0001f);
}

TEST(UniversalSceneImporterTests, Gltf_SmallInvertibleBindIsNotDiscarded)
{
    UniversalSceneImporter::Scene scene;
    scene.SetFormat(UniversalSceneImporter::SourceFormat::Gltf);
    scene.GetNodes().resize(2);
    scene.GetNodes()[1].m_skin = 0;
    scene.GetSkins().resize(1);
    scene.GetSkins()[0].m_joints = { 0 };
    scene.GetSkins()[0].m_inverseBindMatrices = { AZ::Matrix3x4::CreateScale(AZ::Vector3(0.0001f)) };
    const auto bind = scene.GetNodeBindWorldTransform(0);
    EXPECT_NEAR(bind.GetBasisX().GetLength(), 10000.0f, 0.01f);
    EXPECT_NEAR(bind.GetBasisY().GetLength(), 10000.0f, 0.01f);
    EXPECT_NEAR(bind.GetBasisZ().GetLength(), 10000.0f, 0.01f);
}

TEST(UniversalSceneImporterTests, MissingBindFallsBackToDefaultWorldTransform)
{
    UniversalSceneImporter::Scene scene;
    scene.GetNodes().resize(2);
    scene.GetNodes()[0].m_localTransform = AZ::Matrix3x4::CreateTranslation(AZ::Vector3(2.0f, 0.0f, 0.0f));
    scene.GetNodes()[1].m_parent = 0;
    scene.GetNodes()[1].m_localTransform = AZ::Matrix3x4::CreateTranslation(AZ::Vector3(3.0f, 0.0f, 0.0f));
    EXPECT_NEAR(scene.GetNodeBindWorldTransform(1).GetTranslation().GetX(), 5.0f, 0.0001f);
}

AZ_UNIT_TEST_HOOK(DEFAULT_UNIT_TEST_ENV);
