/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "UniversalSceneWrapper.h"

#include <AzCore/IO/Path/Path.h>
#include <AzCore/IO/SystemFile.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/containers/unordered_set.h>
#include <AzCore/std/string/conversions.h>
#include <SceneAPI/SceneCore/Utilities/Reporting.h>

#include "../Converters/FbxSceneConverter.h"
#include "../Converters/GltfSceneConverter.h"

namespace UniversalSceneImporter
{
    namespace
    {
        constexpr size_t FormatSniffBytes = 64;

        //! Reads the first bytes of a file to identify its content, independently of the extension.
        SourceFormat SniffFormat(const char* fileName)
        {
            AZ::IO::SystemFile file;
            if (!file.Open(fileName, AZ::IO::SystemFile::SF_OPEN_READ_ONLY))
            {
                return SourceFormat::Unknown;
            }

            char buffer[FormatSniffBytes] = {};
            const size_t bytesRead = file.Read(FormatSniffBytes, buffer);
            file.Close();
            if (bytesRead == 0)
            {
                return SourceFormat::Unknown;
            }

            AZStd::string_view header(buffer, bytesRead);
            // Skip a UTF-8 BOM and leading whitespace for JSON based formats.
            if (header.size() >= 3 && static_cast<AZ::u8>(header[0]) == 0xEF && static_cast<AZ::u8>(header[1]) == 0xBB &&
                static_cast<AZ::u8>(header[2]) == 0xBF)
            {
                header.remove_prefix(3);
            }
            size_t firstNonWhitespace = 0;
            while (firstNonWhitespace < header.size() &&
                   (header[firstNonWhitespace] == ' ' || header[firstNonWhitespace] == '\t' || header[firstNonWhitespace] == '\r' ||
                    header[firstNonWhitespace] == '\n'))
            {
                ++firstNonWhitespace;
            }
            const AZStd::string_view trimmed = header.substr(firstNonWhitespace);

            if (trimmed.starts_with("Kaydara FBX Binary") || trimmed.starts_with("; FBX") || trimmed.starts_with(";FBX"))
            {
                return SourceFormat::Fbx;
            }
            if (trimmed.starts_with("glTF"))
            {
                return SourceFormat::Gltf;
            }
            if (trimmed.starts_with("{"))
            {
                return SourceFormat::Gltf;
            }
            return SourceFormat::Unknown;
        }

        SourceFormat FormatFromExtension(AZStd::string_view extension)
        {
            AZStd::string lower(extension);
            AZStd::to_lower(lower.begin(), lower.end());

            if (lower == ".fbx")
            {
                return SourceFormat::Fbx;
            }
            if (lower == ".gltf" || lower == ".glb")
            {
                return SourceFormat::Gltf;
            }
            return SourceFormat::Unknown;
        }
    } // namespace

    UniversalSceneWrapper::UniversalSceneWrapper() = default;

    UniversalSceneWrapper::~UniversalSceneWrapper() = default;

    SourceFormat UniversalSceneWrapper::DetectFormat(const char* fileName)
    {
        if (!fileName || fileName[0] == '\0')
        {
            return SourceFormat::Unknown;
        }

        const AZStd::string extension = AZ::IO::Path(fileName).Extension().String();
        const SourceFormat extensionFormat = FormatFromExtension(extension);
        const SourceFormat contentFormat = SniffFormat(fileName);

        if (extensionFormat != SourceFormat::Unknown && contentFormat != SourceFormat::Unknown && extensionFormat != contentFormat)
        {
            AZ_Warning(
                "UniversalSceneImporter", false,
                "File '%s' has a '%s' extension but its content looks like %s. The content format will be used.",
                fileName, extension.c_str(), ToString(contentFormat));
            return contentFormat;
        }

        if (contentFormat != SourceFormat::Unknown)
        {
            return contentFormat;
        }

        return extensionFormat;
    }

    bool UniversalSceneWrapper::LoadScene(
        const char* fileName, SourceFormat format, const AZ::SceneAPI::SceneImportSettings& importSettings)
    {
        if (!fileName || fileName[0] == '\0')
        {
            AZ_Error("UniversalSceneImporter", false, "Cannot import a scene with an empty file name.");
            return false;
        }

        Scene loadedScene;
        loadedScene.SetFormat(format);
        loadedScene.SetSourcePath(fileName);
        loadedScene.SetExtractEmbeddedTextures(importSettings.m_extractEmbeddedTextures);

        AZStd::string error;
        bool converted = false;
        switch (format)
        {
        case SourceFormat::Fbx:
            converted = ConvertFbxScene(fileName, loadedScene, error);
            break;
        case SourceFormat::Gltf:
            converted = ConvertGltfScene(fileName, loadedScene, error);
            break;
        default:
            error = "Unsupported scene format.";
            break;
        }

        if (!converted)
        {
            AZ_Error(
                "UniversalSceneImporter", false, "Failed to import %s scene '%s': %s", ToString(format), fileName,
                error.c_str());
            return false;
        }

        if (!loadedScene.Finalize())
        {
            AZ_Error("UniversalSceneImporter", false, "Scene '%s' was parsed but could not be finalized.", fileName);
            return false;
        }

        loadedScene.LogSummary("UniversalSceneImporter");
        if (loadedScene.GetVertexCount() == 0 && loadedScene.GetAnimations().empty())
        {
            AZ_Warning(
                "UniversalSceneImporter", false,
                "Scene '%s' does not contain any renderable geometry or animation. Importing it will not produce any output.",
                fileName);
        }

        m_scene = AZStd::move(loadedScene);
        return true;
    }

    bool UniversalSceneWrapper::LoadSceneFromFile(
        const char* fileName, const AZ::SceneAPI::SceneImportSettings& importSettings)
    {
        Clear();

        const SourceFormat format = DetectFormat(fileName);
        if (format == SourceFormat::Unknown)
        {
            AZ_Error(
                "UniversalSceneImporter", false,
                "Cannot determine the scene format of '%s'. Supported formats are .fbx, .gltf and .glb.", fileName);
            return false;
        }

        return LoadScene(fileName, format, importSettings);
    }

    const std::shared_ptr<AZ::SDKNode::NodeWrapper> UniversalSceneWrapper::GetRootNode() const
    {
        if (m_scene.IsEmpty())
        {
            return nullptr;
        }
        return std::make_shared<UniversalNodeWrapper>(&m_scene, 0);
    }

    std::shared_ptr<AZ::SDKNode::NodeWrapper> UniversalSceneWrapper::GetRootNode()
    {
        if (m_scene.IsEmpty())
        {
            return nullptr;
        }
        return std::make_shared<UniversalNodeWrapper>(&m_scene, 0);
    }

    void UniversalSceneWrapper::Clear()
    {
        m_scene.Clear();
    }

    AZStd::pair<AZ::SDKScene::SceneWrapperBase::AxisVector, int32_t> UniversalSceneWrapper::GetUpVectorAndSign() const
    {
        return { AxisVector::Z, 1 };
    }

    AZStd::pair<AZ::SDKScene::SceneWrapperBase::AxisVector, int32_t> UniversalSceneWrapper::GetFrontVectorAndSign() const
    {
        return { AxisVector::Y, -1 };
    }

    AZStd::pair<AZ::SDKScene::SceneWrapperBase::AxisVector, int32_t> UniversalSceneWrapper::GetRightVectorAndSign() const
    {
        return { AxisVector::X, 1 };
    }

    AZStd::optional<AZ::SceneAPI::DataTypes::MatrixType> UniversalSceneWrapper::UseForcedRootTransform() const
    {
        // The converters bake the source axes into the node transforms and vertex data, so no
        // additional root adjustment is required.
        return AZStd::nullopt;
    }

    float UniversalSceneWrapper::GetUnitSizeInMeters() const
    {
        // The converters bake the unit scale into vertex data and node transforms.
        return 1.0f;
    }

    AZ::Aabb UniversalSceneWrapper::GetAABB() const
    {
        return m_scene.GetAabb();
    }

    AZ::u32 UniversalSceneWrapper::GetVerticesCount() const
    {
        return m_scene.GetVertexCount();
    }

    AZStd::string UniversalSceneWrapper::GetSceneDirectory() const
    {
        const AZStd::string& sourcePath = m_scene.GetSourcePath();
        if (sourcePath.empty())
        {
            return AZStd::string();
        }
        return AZ::IO::Path(sourcePath).ParentPath().String();
    }

    //////////////////////////////////////////////////////////////////////////
    // UniversalNodeWrapper
    //////////////////////////////////////////////////////////////////////////

    UniversalNodeWrapper::UniversalNodeWrapper(const Scene* scene, int nodeIndex)
        : m_scene(scene)
        , m_nodeIndex(nodeIndex)
    {
    }

    const Node* UniversalNodeWrapper::GetNode() const
    {
        return m_scene ? m_scene->GetNode(m_nodeIndex) : nullptr;
    }

    const Mesh* UniversalNodeWrapper::GetMesh() const
    {
        const Node* node = GetNode();
        return (node && m_scene) ? m_scene->GetMesh(node->m_mesh) : nullptr;
    }

    bool UniversalNodeWrapper::IsBone() const
    {
        const Node* node = GetNode();
        return node && node->m_isBone;
    }

    bool UniversalNodeWrapper::IsRootNode() const
    {
        const Node* node = GetNode();
        return node && node->m_isRoot;
    }

    const char* UniversalNodeWrapper::GetName() const
    {
        const Node* node = GetNode();
        return node ? node->m_name.c_str() : "";
    }

    AZ::u64 UniversalNodeWrapper::GetUniqueId() const
    {
        if (!m_scene)
        {
            return 0;
        }
        // Stable for the lifetime of the scene and unique across scenes.
        return (m_scene->GetId() << 32) ^ (static_cast<AZ::u64>(m_nodeIndex) + 1);
    }

    int UniversalNodeWrapper::GetMaterialCount() const
    {
        const Node* node = GetNode();
        const Mesh* mesh = GetMesh();
        if (!node || !mesh)
        {
            return 0;
        }

        AZStd::unordered_set<int> materials;
        for (const MeshPrimitive& primitive : mesh->m_primitives)
        {
            if (primitive.m_material >= 0)
            {
                materials.insert(primitive.m_material);
            }
        }
        return static_cast<int>(materials.size());
    }

    int UniversalNodeWrapper::GetChildCount() const
    {
        const Node* node = GetNode();
        return node ? static_cast<int>(node->m_children.size()) : 0;
    }

    const std::shared_ptr<AZ::SDKNode::NodeWrapper> UniversalNodeWrapper::GetChild(int childIndex) const
    {
        const Node* node = GetNode();
        if (!node || childIndex < 0 || static_cast<size_t>(childIndex) >= node->m_children.size())
        {
            return nullptr;
        }
        return std::make_shared<UniversalNodeWrapper>(m_scene, node->m_children[childIndex]);
    }

} // namespace UniversalSceneImporter
