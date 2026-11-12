/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "ImporterUtils.h"

#include <AzCore/std/algorithm.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <SceneAPI/SceneBuilder/Importers/Utilities/RenamedNodesMap.h>

#include <AzToolsFramework/Debug/TraceContext.h>
#include <SceneAPI/SceneBuilder/Importers/ImporterUtilities.h>
#include <SceneAPI/SceneCore/Utilities/Reporting.h>
#include <SceneAPI/SceneData/GraphData/MeshData.h>
#include <SceneAPI/SceneData/GraphData/MeshVertexBitangentData.h>
#include <SceneAPI/SceneData/GraphData/MeshVertexColorData.h>
#include <SceneAPI/SceneData/GraphData/MeshVertexTangentData.h>
#include <SceneAPI/SceneData/GraphData/MeshVertexUVData.h>
#include <SceneAPI/SceneData/GraphData/SkinMeshData.h>

#include "../Converters/SceneConverterUtils.h"
#include "../Scene/UniversalSceneWrapper.h"

namespace UniversalSceneImporter
{
    namespace
    {
        template<typename MeshDataType>
        void FillMeshData(MeshDataType& meshData, const Mesh& mesh)
        {
            const AZStd::vector<int> materialSlots = BuildMaterialSlots(mesh);

            meshData.SetUnitSizeInMeters(1.0f);
            meshData.SetOriginalUnitSizeInMeters(1.0f);

            for (const MeshPrimitive& primitive : mesh.m_primitives)
            {
                if (primitive.m_positions.empty() || primitive.m_indices.empty())
                {
                    continue;
                }

                AZStd::vector<AZ::Vector3> normals = primitive.m_normals;
                if (normals.size() != primitive.m_positions.size())
                {
                    Converters::GenerateSmoothNormals(primitive.m_positions, primitive.m_indices, normals);
                }

                const AZ::u32 vertexBase = static_cast<AZ::u32>(meshData.GetVertexCount());
                for (size_t vertexIndex = 0; vertexIndex < primitive.m_positions.size(); ++vertexIndex)
                {
                    meshData.AddPosition(primitive.m_positions[vertexIndex]);
                    meshData.AddNormal(normals[vertexIndex]);
                }

                const int materialSlot = FindMaterialSlot(materialSlots, primitive.m_material);
                const AZ::u32 faceMaterialId = materialSlot >= 0 ? static_cast<AZ::u32>(materialSlot) : 0;
                for (size_t index = 0; index + 2 < primitive.m_indices.size(); index += 3)
                {
                    AZ::SceneAPI::DataTypes::IMeshData::Face face;
                    face.vertexIndex[0] = vertexBase + primitive.m_indices[index + 0];
                    face.vertexIndex[1] = vertexBase + primitive.m_indices[index + 1];
                    face.vertexIndex[2] = vertexBase + primitive.m_indices[index + 2];
                    meshData.AddFace(face, faceMaterialId);
                }
            }
        }
    } // namespace

    AZStd::vector<int> BuildMaterialSlots(const Mesh& mesh)
    {
        AZStd::vector<int> slots;
        for (const MeshPrimitive& primitive : mesh.m_primitives)
        {
            if (primitive.m_material < 0)
            {
                continue;
            }
            if (AZStd::find(slots.begin(), slots.end(), primitive.m_material) == slots.end())
            {
                slots.push_back(primitive.m_material);
            }
        }
        return slots;
    }

    int FindMaterialSlot(const AZStd::vector<int>& slots, int materialIndex)
    {
        if (materialIndex < 0)
        {
            return -1;
        }
        const auto found = AZStd::find(slots.begin(), slots.end(), materialIndex);
        return found != slots.end() ? static_cast<int>(AZStd::distance(slots.begin(), found)) : -1;
    }

    AZ::SceneAPI::Events::ProcessingResult AddAttributeNode(
        UniversalSceneNodeAppendedContext& context,
        AZStd::shared_ptr<AZ::SceneAPI::DataTypes::IGraphObject> data,
        const AZStd::string& name,
        const char* fallbackPrefix)
    {
        AZStd::string nodeName = name;
        if (fallbackPrefix)
        {
            AZ::SceneAPI::SceneBuilder::RenamedNodesMap::SanitizeNodeName(
                nodeName, context.m_scene.GetGraph(), context.m_currentGraphPosition, fallbackPrefix);
        }
        else
        {
            AZ::SceneAPI::SceneBuilder::RenamedNodesMap::SanitizeNodeName(
                nodeName, context.m_scene.GetGraph(), context.m_currentGraphPosition);
        }
        AZ_TraceContext("Attribute node name", nodeName);

        const AZ::SceneAPI::Containers::SceneGraph::NodeIndex newIndex =
            context.m_scene.GetGraph().AddChild(context.m_currentGraphPosition, nodeName.c_str());
        AZ_Error(
            AZ::SceneAPI::Utilities::ErrorWindow, newIndex.IsValid(), "Failed to create a SceneGraph node for an attribute.");
        if (!newIndex.IsValid())
        {
            return AZ::SceneAPI::Events::ProcessingResult::Failure;
        }

        auto dataPopulated =
            context.m_contextProvider->CreateSceneAttributeDataPopulatedContext(context, AZStd::move(data), newIndex, nodeName);
        if (!dataPopulated)
        {
            return AZ::SceneAPI::Events::ProcessingResult::Failure;
        }

        AZ::SceneAPI::Events::ProcessingResult result = AZ::SceneAPI::Events::Process(*dataPopulated);
        if (result != AZ::SceneAPI::Events::ProcessingResult::Failure)
        {
            result = AZ::SceneAPI::SceneBuilder::AddAttributeDataNodeWithContexts(*dataPopulated);
        }
        return result;
    }

    AZStd::shared_ptr<AZ::SceneData::GraphData::MeshData> BuildMeshData(const Mesh& mesh, bool skinned)
    {
        if (skinned)
        {
            auto meshData = AZStd::make_shared<AZ::SceneData::GraphData::SkinMeshData>();
            FillMeshData(*meshData, mesh);
            return meshData;
        }

        auto meshData = AZStd::make_shared<AZ::SceneData::GraphData::MeshData>();
        FillMeshData(*meshData, mesh);
        return meshData;
    }

    AZStd::shared_ptr<AZ::SceneData::GraphData::MeshVertexUVData> BuildUvSetData(const Mesh& mesh, size_t setIndex)
    {
        if (setIndex >= MaxUvSets)
        {
            return nullptr;
        }

        auto uvData = AZStd::make_shared<AZ::SceneData::GraphData::MeshVertexUVData>();
        uvData->ReserveContainerSpace(mesh.GetVertexCount());
        for (const MeshPrimitive& primitive : mesh.m_primitives)
        {
            const bool hasSet = !primitive.m_uvSets[setIndex].empty();
            for (size_t vertexIndex = 0; vertexIndex < primitive.m_positions.size(); ++vertexIndex)
            {
                uvData->AppendUV(hasSet ? primitive.m_uvSets[setIndex][vertexIndex] : AZ::Vector2::CreateZero());
            }
        }
        return uvData;
    }

    AZStd::shared_ptr<AZ::SceneData::GraphData::MeshVertexColorData> BuildColorSetData(const Mesh& mesh, size_t setIndex)
    {
        if (setIndex >= MaxColorSets)
        {
            return nullptr;
        }

        auto colorData = AZStd::make_shared<AZ::SceneData::GraphData::MeshVertexColorData>();
        colorData->ReserveContainerSpace(mesh.GetVertexCount());
        for (const MeshPrimitive& primitive : mesh.m_primitives)
        {
            const bool hasSet = !primitive.m_colorSets[setIndex].empty();
            for (size_t vertexIndex = 0; vertexIndex < primitive.m_positions.size(); ++vertexIndex)
            {
                const AZ::Vector4 color = hasSet ? primitive.m_colorSets[setIndex][vertexIndex] : AZ::Vector4::CreateOne();
                colorData->AppendColor(AZ::SceneAPI::DataTypes::Color(color.GetX(), color.GetY(), color.GetZ(), color.GetW()));
            }
        }
        return colorData;
    }

    AZStd::shared_ptr<AZ::SceneData::GraphData::MeshVertexTangentData> BuildTangentData(const Mesh& mesh)
    {
        auto tangentData = AZStd::make_shared<AZ::SceneData::GraphData::MeshVertexTangentData>();
        tangentData->SetGenerationMethod(AZ::SceneAPI::DataTypes::TangentGenerationMethod::FromSourceScene);
        tangentData->SetTangentSetIndex(0);
        tangentData->ReserveContainerSpace(mesh.GetVertexCount());

        for (const MeshPrimitive& primitive : mesh.m_primitives)
        {
            const bool hasTangents = primitive.HasTangents();
            for (size_t vertexIndex = 0; vertexIndex < primitive.m_positions.size(); ++vertexIndex)
            {
                tangentData->AppendTangent(
                    hasTangents ? primitive.m_tangents[vertexIndex] : AZ::Vector4(1.0f, 0.0f, 0.0f, 1.0f));
            }
        }
        return tangentData;
    }

    AZStd::shared_ptr<AZ::SceneData::GraphData::MeshVertexBitangentData> BuildBitangentData(const Mesh& mesh)
    {
        auto bitangentData = AZStd::make_shared<AZ::SceneData::GraphData::MeshVertexBitangentData>();
        bitangentData->SetGenerationMethod(AZ::SceneAPI::DataTypes::TangentGenerationMethod::FromSourceScene);
        bitangentData->SetBitangentSetIndex(0);
        bitangentData->ReserveContainerSpace(mesh.GetVertexCount());

        for (const MeshPrimitive& primitive : mesh.m_primitives)
        {
            const bool hasBitangents = primitive.HasBitangents();
            for (size_t vertexIndex = 0; vertexIndex < primitive.m_positions.size(); ++vertexIndex)
            {
                bitangentData->AppendBitangent(
                    hasBitangents ? primitive.m_bitangents[vertexIndex] : AZ::Vector3(0.0f, 1.0f, 0.0f));
            }
        }
        return bitangentData;
    }

    AZStd::string GetMaterialSlotName(const Material& material, size_t slotIndex)
    {
        if (!material.m_name.empty())
        {
            return material.m_name;
        }
        return AZStd::string::format("Material_%zu", slotIndex);
    }

    AZStd::string SanitizeFileName(AZStd::string_view name)
    {
        AZStd::string result;
        result.reserve(name.size());
        for (char character : name)
        {
            const bool allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.';
            result.push_back(allowed ? character : '_');
        }
        if (result.empty())
        {
            result = "texture";
        }
        return result;
    }

    AZStd::string GetFinalNodeName(
        const AZ::SceneAPI::SceneBuilder::RenamedNodesMap& renamedNodesMap, const Scene& scene, int nodeIndex)
    {
        UniversalNodeWrapper wrapper(&scene, nodeIndex);
        AZStd::string name = renamedNodesMap.GetNodeName(wrapper);
        // SceneImporter replaces '%' after consulting the rename map so the name is printf safe.
        AZStd::replace(name.begin(), name.end(), '%', '_');
        return name;
    }

} // namespace UniversalSceneImporter
