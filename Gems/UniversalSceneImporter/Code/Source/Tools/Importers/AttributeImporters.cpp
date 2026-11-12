/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "AttributeImporters.h"

#include <AzCore/IO/FileIO.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/IO/SystemFile.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/StringFunc/StringFunc.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/Debug/TraceContext.h>
#include <SceneAPI/SceneCore/Utilities/Reporting.h>
#include <SceneAPI/SceneCore/DataTypes/IGraphObject.h>
#include <SceneAPI/SceneData/GraphData/MaterialData.h>
#include <SceneAPI/SceneData/GraphData/MeshVertexBitangentData.h>
#include <SceneAPI/SceneData/GraphData/MeshVertexColorData.h>
#include <SceneAPI/SceneData/GraphData/MeshVertexTangentData.h>
#include <SceneAPI/SceneData/GraphData/MeshVertexUVData.h>

#include "ImporterUtils.h"

namespace UniversalSceneImporter
{
    namespace
    {
        const char* GetTextureSlotName(MaterialTexture texture)
        {
            switch (texture)
            {
            case MaterialTexture::BaseColor:
                return "BaseColor";
            case MaterialTexture::Diffuse:
                return "Diffuse";
            case MaterialTexture::Specular:
                return "Specular";
            case MaterialTexture::Emissive:
                return "Emissive";
            case MaterialTexture::Normal:
                return "Normal";
            case MaterialTexture::Bump:
                return "Bump";
            case MaterialTexture::Metallic:
                return "Metallic";
            case MaterialTexture::Roughness:
                return "Roughness";
            case MaterialTexture::AmbientOcclusion:
                return "AmbientOcclusion";
            default:
                return "Texture";
            }
        }

        //! Converts a candidate path into a scan folder relative source path. The asset system only
        //! maps paths, it does not verify that the file is present, so check the disk here as well.
        //! A texture path that does not point at a real source asset would otherwise surface later as
        //! a "Could not find asset" warning from the material converter.
        bool TryResolveSourcePath(const AZStd::string& candidate, AZStd::string& relativePath)
        {
            relativePath.clear();
            if (candidate.empty())
            {
                return false;
            }

            AZStd::string rootPath;
            bool generatedRelativePath = false;
            AzToolsFramework::AssetSystemRequestBus::BroadcastResult(
                generatedRelativePath, &AzToolsFramework::AssetSystemRequestBus::Events::GenerateRelativeSourcePath,
                candidate.c_str(), relativePath, rootPath);
            if (!generatedRelativePath || relativePath.empty())
            {
                relativePath.clear();
                return false;
            }

            if (!rootPath.empty())
            {
                const AZStd::string fullPath = (AZ::IO::Path(rootPath.c_str()) / relativePath.c_str()).String();
                if (!AZ::IO::SystemFile::Exists(fullPath.c_str()))
                {
                    relativePath.clear();
                    return false;
                }
            }

            return true;
        }

        AZStd::string ResolveExternalTexturePath(const Scene& scene, const AZStd::string& textureUri)
        {
            if (textureUri.empty())
            {
                return AZStd::string();
            }

            // Normalize separators so FBX files authored on Windows resolve on all platforms.
            AZStd::string normalizedUri = textureUri;
            AZStd::replace(normalizedUri.begin(), normalizedUri.end(), '\\', '/');

            if (normalizedUri.starts_with("file://"))
            {
                normalizedUri = normalizedUri.substr(7);
            }
            else if (normalizedUri.starts_with("http://") || normalizedUri.starts_with("https://"))
            {
                AZ_TracePrintf(
                    "UniversalSceneImporter",
                    "Texture '%s' is referenced by URL, which the asset pipeline cannot load. The texture will be skipped.\n",
                    textureUri.c_str());
                return AZStd::string();
            }

            const AZStd::string sceneDirectory = AZ::IO::Path(scene.GetSourcePath()).ParentPath().String();
            const AZ::IO::PathView texturePath(normalizedUri);

            AZStd::string resolvedPath;
            if (texturePath.IsAbsolute())
            {
                // Assets are often authored with paths from the authoring machine. If the texture
                // can be resolved to a scan folder relative path anyway, use it.
                if (TryResolveSourcePath(normalizedUri, resolvedPath))
                {
                    return resolvedPath;
                }
            }
            else
            {
                const AZStd::string combinedPath =
                    (AZ::IO::Path(sceneDirectory.c_str()) / normalizedUri.c_str()).LexicallyNormal().String();

                // Convert to a path relative to a scan folder, which is what material assets reference.
                if (TryResolveSourcePath(combinedPath, resolvedPath))
                {
                    return resolvedPath;
                }
            }

            // Exporters frequently store a stale path from another machine while the texture actually
            // sits next to the scene file. Try the file name in the scene folder before giving up.
            const AZStd::string fileName = texturePath.Filename().String();
            if (!fileName.empty())
            {
                const AZStd::string adjacentPath = (AZ::IO::Path(sceneDirectory.c_str()) / fileName.c_str()).String();
                if (TryResolveSourcePath(adjacentPath, resolvedPath))
                {
                    return resolvedPath;
                }
            }

            AZ_TracePrintf(
                "UniversalSceneImporter",
                "Texture '%s' does not resolve to a source asset and will be skipped.\n",
                textureUri.c_str());
            return AZStd::string();
        }

        AZStd::string ExtractEmbeddedTexture(const Scene& scene, const Material& material, MaterialTexture textureType)
        {
            const Texture& texture = material.GetTexture(textureType);
            if (!texture.IsValid())
            {
                return AZStd::string();
            }

            if (!scene.GetExtractEmbeddedTextures())
            {
                AZ_Warning(
                    "UniversalSceneImporter", false,
                    "Material '%s' uses an embedded texture ('%s') but embedded texture extraction is disabled for this asset. "
                    "Enable 'Extract embedded textures' in the asset importer to extract it.",
                    material.m_name.c_str(), GetTextureSlotName(textureType));
                return AZStd::string();
            }

            AZStd::string relativePath;
            AZStd::string rootPath;
            bool generatedRelativePath = false;
            AzToolsFramework::AssetSystemRequestBus::BroadcastResult(
                generatedRelativePath, &AzToolsFramework::AssetSystemRequestBus::Events::GenerateRelativeSourcePath,
                scene.GetSourcePath().c_str(), relativePath, rootPath);
            if (!generatedRelativePath || relativePath.empty())
            {
                AZ_Warning(
                    "UniversalSceneImporter", false,
                    "Unable to determine a project relative path for the embedded texture of material '%s'.",
                    material.m_name.c_str());
                return AZStd::string();
            }

            const AZStd::string fileName = AZStd::string::format(
                "%s_%s.%s", SanitizeFileName(material.m_name).c_str(), GetTextureSlotName(textureType),
                texture.m_extension.empty() ? "png" : texture.m_extension.c_str());
            AZ::StringFunc::Path::ReplaceFullName(relativePath, fileName.c_str());

            AZ::IO::FileIOBase* fileIO = AZ::IO::FileIOBase::GetInstance();
            if (!fileIO)
            {
                AZ_Warning("UniversalSceneImporter", false, "FileIO is not available; embedded textures cannot be extracted.");
                return AZStd::string();
            }

            const char* projectRoot = fileIO->GetAlias("@projectroot@");
            if (!projectRoot)
            {
                AZ_Warning("UniversalSceneImporter", false, "The @projectroot@ alias is not available.");
                return AZStd::string();
            }

            const AZStd::string fullPath = AZ::IO::Path(AZ::IO::Path(projectRoot) / AZ::IO::Path(relativePath)).String();
            if (fileIO->Exists(fullPath.c_str()))
            {
                return relativePath;
            }

            AZ::IO::HandleType handle = AZ::IO::InvalidHandle;
            if (!fileIO->Open(
                    fullPath.c_str(), AZ::IO::OpenMode::ModeWrite | AZ::IO::OpenMode::ModeBinary | AZ::IO::OpenMode::ModeCreatePath,
                    handle))
            {
                AZ_Warning(
                    "UniversalSceneImporter", false, "Failed to open '%s' for writing an embedded texture.", fullPath.c_str());
                return AZStd::string();
            }

            AZ::u64 bytesWritten = 0;
            const bool writeSucceeded =
                fileIO->Write(handle, texture.m_embeddedData.data(), texture.m_embeddedData.size(), &bytesWritten) &&
                bytesWritten == texture.m_embeddedData.size();
            fileIO->Close(handle);

            if (!writeSucceeded)
            {
                AZ_Warning("UniversalSceneImporter", false, "Failed to write all bytes of texture '%s'.", fullPath.c_str());
            }

            return relativePath;
        }

        AZStd::string ResolveTexturePath(const Scene& scene, const Material& material, MaterialTexture textureType)
        {
            const Texture& texture = material.GetTexture(textureType);
            if (!texture.IsValid())
            {
                return AZStd::string();
            }
            if (texture.IsEmbedded())
            {
                return ExtractEmbeddedTexture(scene, material, textureType);
            }
            return ResolveExternalTexturePath(scene, texture.m_uri);
        }

        using MaterialMapType = AZ::SceneAPI::DataTypes::IMaterialData::TextureMapType;

        void ApplyMaterialTextures(
            const Scene& scene, const Material& source, AZ::SceneData::GraphData::MaterialData& destination)
        {
            // A single texture is often assigned to more than one slot (for example base color and
            // diffuse), and the converters copy the texture per slot. Resolve each external URI only
            // once so a missing texture is logged once and the same result is used for every slot.
            AZStd::unordered_map<AZStd::string, AZStd::string> resolvedExternalTextures;
            AZStd::unordered_map<const Texture*, AZStd::string> resolvedEmbeddedTextures;
            auto resolveTexture = [&](MaterialTexture texture) -> const AZStd::string&
            {
                const Texture& sourceTexture = source.GetTexture(texture);
                if (!sourceTexture.IsEmbedded() && !sourceTexture.m_uri.empty())
                {
                    auto [iterator, inserted] = resolvedExternalTextures.emplace(sourceTexture.m_uri, AZStd::string());
                    if (inserted)
                    {
                        iterator->second = ResolveTexturePath(scene, source, texture);
                    }
                    return iterator->second;
                }

                auto [iterator, inserted] = resolvedEmbeddedTextures.emplace(&sourceTexture, AZStd::string());
                if (inserted)
                {
                    iterator->second = ResolveTexturePath(scene, source, texture);
                }
                return iterator->second;
            };

            const AZStd::string& baseColor = resolveTexture(MaterialTexture::BaseColor);
            const AZStd::string& metallic = resolveTexture(MaterialTexture::Metallic);
            const AZStd::string& roughness = resolveTexture(MaterialTexture::Roughness);
            const AZStd::string& emissive = resolveTexture(MaterialTexture::Emissive);
            const AZStd::string& ambientOcclusion = resolveTexture(MaterialTexture::AmbientOcclusion);

            destination.SetTexture(MaterialMapType::BaseColor, baseColor);
            destination.SetTexture(MaterialMapType::Diffuse, resolveTexture(MaterialTexture::Diffuse));
            destination.SetTexture(MaterialMapType::Specular, resolveTexture(MaterialTexture::Specular));
            destination.SetTexture(MaterialMapType::Normal, resolveTexture(MaterialTexture::Normal));
            destination.SetTexture(MaterialMapType::Bump, resolveTexture(MaterialTexture::Bump));
            destination.SetTexture(MaterialMapType::Emissive, emissive);
            destination.SetTexture(MaterialMapType::Metallic, metallic);
            destination.SetTexture(MaterialMapType::Roughness, roughness);
            destination.SetTexture(MaterialMapType::AmbientOcclusion, ambientOcclusion);

            // Drive the material flags from the textures that actually resolved, not from the source
            // material, so skipped textures don't leave the material pointing at an empty map.
            destination.SetUseColorMap(!baseColor.empty());
            destination.SetUseMetallicMap(!metallic.empty());
            destination.SetUseRoughnessMap(!roughness.empty());
            destination.SetUseEmissiveMap(!emissive.empty());
            destination.SetUseAOMap(!ambientOcclusion.empty());
        }
    } // namespace

    //////////////////////////////////////////////////////////////////////////
    // UvMapImporter
    //////////////////////////////////////////////////////////////////////////

    UvMapImporter::UvMapImporter()
    {
        BindToCall(&UvMapImporter::ImportUvMaps);
    }

    void UvMapImporter::Reflect(AZ::ReflectContext* context)
    {
        ReflectLoadingComponent<UvMapImporter>(context);
    }

    AZ::SceneAPI::Events::ProcessingResult UvMapImporter::ImportUvMaps(UniversalSceneNodeAppendedContext& context)
    {
        AZ_TraceContext("Importer", "UV Maps");
        const Mesh* mesh = context.GetMesh();
        if (!mesh || mesh->m_primitives.empty())
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        const size_t uvSetCount = mesh->GetMaxUvSetCount();
        if (uvSetCount == 0)
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        AZ::SceneAPI::Events::ProcessingResultCombiner combinedResult;
        for (size_t setIndex = 0; setIndex < uvSetCount; ++setIndex)
        {
            auto uvData = BuildUvSetData(*mesh, setIndex);
            if (!uvData || uvData->GetCount() == 0)
            {
                continue;
            }
            const AZStd::string name = AZStd::string::format("UV%zu", setIndex);
            combinedResult += AddAttributeNode(context, AZStd::move(uvData), name);
        }
        return combinedResult.GetResult();
    }

    //////////////////////////////////////////////////////////////////////////
    // ColorStreamImporter
    //////////////////////////////////////////////////////////////////////////

    ColorStreamImporter::ColorStreamImporter()
    {
        BindToCall(&ColorStreamImporter::ImportColorStreams);
    }

    void ColorStreamImporter::Reflect(AZ::ReflectContext* context)
    {
        ReflectLoadingComponent<ColorStreamImporter>(context);
    }

    AZ::SceneAPI::Events::ProcessingResult ColorStreamImporter::ImportColorStreams(UniversalSceneNodeAppendedContext& context)
    {
        AZ_TraceContext("Importer", "Color Streams");
        const Mesh* mesh = context.GetMesh();
        if (!mesh || mesh->m_primitives.empty())
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        const size_t colorSetCount = mesh->GetMaxColorSetCount();
        if (colorSetCount == 0)
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        AZ::SceneAPI::Events::ProcessingResultCombiner combinedResult;
        for (size_t setIndex = 0; setIndex < colorSetCount; ++setIndex)
        {
            auto colorData = BuildColorSetData(*mesh, setIndex);
            if (!colorData || colorData->GetCount() == 0)
            {
                continue;
            }
            const AZStd::string name = AZStd::string::format("Col%zu", setIndex);
            combinedResult += AddAttributeNode(context, AZStd::move(colorData), name);
        }
        return combinedResult.GetResult();
    }

    //////////////////////////////////////////////////////////////////////////
    // TangentStreamImporter
    //////////////////////////////////////////////////////////////////////////

    TangentStreamImporter::TangentStreamImporter()
    {
        BindToCall(&TangentStreamImporter::ImportTangentStreams);
    }

    void TangentStreamImporter::Reflect(AZ::ReflectContext* context)
    {
        ReflectLoadingComponent<TangentStreamImporter>(context);
    }

    AZ::SceneAPI::Events::ProcessingResult TangentStreamImporter::ImportTangentStreams(UniversalSceneNodeAppendedContext& context)
    {
        AZ_TraceContext("Importer", "Tangent Streams");
        const Mesh* mesh = context.GetMesh();
        if (!mesh || mesh->m_primitives.empty() || !mesh->HasTangents())
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        auto tangentData = BuildTangentData(*mesh);
        if (!tangentData || tangentData->GetCount() == 0)
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }
        return AddAttributeNode(context, AZStd::move(tangentData), "Tangent0");
    }

    //////////////////////////////////////////////////////////////////////////
    // BitangentStreamImporter
    //////////////////////////////////////////////////////////////////////////

    BitangentStreamImporter::BitangentStreamImporter()
    {
        BindToCall(&BitangentStreamImporter::ImportBitangentStreams);
    }

    void BitangentStreamImporter::Reflect(AZ::ReflectContext* context)
    {
        ReflectLoadingComponent<BitangentStreamImporter>(context);
    }

    AZ::SceneAPI::Events::ProcessingResult BitangentStreamImporter::ImportBitangentStreams(UniversalSceneNodeAppendedContext& context)
    {
        AZ_TraceContext("Importer", "Bitangent Streams");
        const Mesh* mesh = context.GetMesh();
        if (!mesh || mesh->m_primitives.empty() || !mesh->HasTangents())
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        auto bitangentData = BuildBitangentData(*mesh);
        if (!bitangentData || bitangentData->GetCount() == 0)
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }
        return AddAttributeNode(context, AZStd::move(bitangentData), "Bitangent0");
    }

    //////////////////////////////////////////////////////////////////////////
    // MaterialImporter
    //////////////////////////////////////////////////////////////////////////

    MaterialImporter::MaterialImporter()
    {
        BindToCall(&MaterialImporter::ImportMaterials);
    }

    void MaterialImporter::Reflect(AZ::ReflectContext* context)
    {
        ReflectLoadingComponent<MaterialImporter>(context);
    }

    AZ::SceneAPI::Events::ProcessingResult MaterialImporter::ImportMaterials(UniversalSceneNodeAppendedContext& context)
    {
        AZ_TraceContext("Importer", "Materials");

        if (!context.m_universalScene)
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }
        const Mesh* mesh = context.GetMesh();
        if (!mesh || mesh->m_primitives.empty())
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        const AZStd::vector<int> materialSlots = BuildMaterialSlots(*mesh);
        if (materialSlots.empty())
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        AZ::SceneAPI::Events::ProcessingResultCombiner combinedResult;
        for (size_t slotIndex = 0; slotIndex < materialSlots.size(); ++slotIndex)
        {
            const Material* material = context.m_universalScene->GetMaterial(materialSlots[slotIndex]);
            if (!material)
            {
                combinedResult += AZ::SceneAPI::Events::ProcessingResult::Ignored;
                continue;
            }

            auto materialData = AZStd::make_shared<AZ::SceneData::GraphData::MaterialData>();
            const AZStd::string materialName = GetMaterialSlotName(*material, slotIndex);
            materialData->SetMaterialName(materialName);
            materialData->SetUniqueId(material->m_uniqueId);
            materialData->SetBaseColor(material->m_baseColor);
            materialData->SetDiffuseColor(material->m_diffuseColor);
            materialData->SetSpecularColor(material->m_specularColor);
            materialData->SetEmissiveColor(material->m_emissiveColor);
            materialData->SetEmissiveIntensity(material->m_emissiveIntensity);
            materialData->SetMetallicFactor(material->m_metallic);
            materialData->SetRoughnessFactor(material->m_roughness);
            materialData->SetShininess(material->m_shininess);
            materialData->SetOpacity(material->m_alphaMasked ? 1.0f : material->m_opacity);

            ApplyMaterialTextures(*context.m_universalScene, *material, *materialData);
            combinedResult += AddAttributeNode(context, AZStd::move(materialData), materialName);
        }

        return combinedResult.GetResult();
    }

} // namespace UniversalSceneImporter



