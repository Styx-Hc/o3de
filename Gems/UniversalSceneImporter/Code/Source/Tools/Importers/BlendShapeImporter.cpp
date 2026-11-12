/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "BlendShapeImporter.h"

#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/std/containers/bitset.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzToolsFramework/Debug/TraceContext.h>
#include <SceneAPI/SceneCore/DataTypes/GraphData/IMeshData.h>
#include <SceneAPI/SceneData/GraphData/BlendShapeData.h>

#include "ImporterUtils.h"

namespace UniversalSceneImporter
{
    BlendShapeImporter::BlendShapeImporter()
    {
        BindToCall(&BlendShapeImporter::ImportBlendShapes);
    }

    void BlendShapeImporter::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<BlendShapeImporter, AZ::SceneAPI::SceneCore::LoadingComponent>()->Version(1);
        }
    }

    AZ::SceneAPI::Events::ProcessingResult BlendShapeImporter::ImportBlendShapes(UniversalSceneNodeAppendedContext& context)
    {
        AZ_TraceContext("Importer", "Blend Shapes");

        const Mesh* mesh = context.GetMesh();
        if (!mesh || mesh->m_primitives.empty())
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        const size_t targetCount = mesh->GetMorphTargetCount();
        if (targetCount == 0)
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        const size_t uvSetCount = mesh->GetMaxUvSetCount();
        const size_t colorSetCount = mesh->GetMaxColorSetCount();
        const bool hasTangents = mesh->HasTangents();

        AZStd::bitset<AZ::SceneData::GraphData::BlendShapeData::MaxNumUVSets> uvSetFlags;
        for (size_t setIndex = 0; setIndex < uvSetCount; ++setIndex)
        {
            uvSetFlags.set(setIndex, true);
        }
        AZStd::bitset<AZ::SceneData::GraphData::BlendShapeData::MaxNumColorSets> colorSetFlags;
        for (size_t setIndex = 0; setIndex < colorSetCount; ++setIndex)
        {
            colorSetFlags.set(setIndex, true);
        }

        AZ::SceneAPI::Events::ProcessingResultCombiner combinedResult;
        for (size_t targetIndex = 0; targetIndex < targetCount; ++targetIndex)
        {
            auto blendShapeData = AZStd::make_shared<AZ::SceneData::GraphData::BlendShapeData>();
            blendShapeData->ReserveData(
                static_cast<unsigned int>(mesh->GetVertexCount()), hasTangents, uvSetFlags, colorSetFlags);

            AZStd::string blendShapeName;
            AZ::u32 vertexBase = 0;

            for (const MeshPrimitive& primitive : mesh->m_primitives)
            {
                const MorphTarget* target =
                    targetIndex < primitive.m_morphTargets.size() ? &primitive.m_morphTargets[targetIndex] : nullptr;
                if (blendShapeName.empty() && target && !target->m_name.empty())
                {
                    blendShapeName = target->m_name;
                }

                const bool hasPositionDeltas = target && target->m_positionDeltas.size() == primitive.m_positions.size();
                const bool hasNormalDeltas = target && target->m_normalDeltas.size() == primitive.m_positions.size();
                const bool hasTangentDeltas = target && target->m_tangentDeltas.size() == primitive.m_positions.size();

                for (size_t vertexIndex = 0; vertexIndex < primitive.m_positions.size(); ++vertexIndex)
                {
                    const AZ::Vector3 position = primitive.m_positions[vertexIndex] +
                        (hasPositionDeltas ? target->m_positionDeltas[vertexIndex] : AZ::Vector3::CreateZero());
                    AZ::Vector3 normal = primitive.m_normals.size() == primitive.m_positions.size()
                        ? primitive.m_normals[vertexIndex]
                        : AZ::Vector3::CreateAxisZ();
                    if (hasNormalDeltas)
                    {
                        normal += target->m_normalDeltas[vertexIndex];
                    }
                    normal.NormalizeSafe();
                    blendShapeData->AddPosition(position);
                    blendShapeData->AddNormal(normal);

                    if (hasTangents)
                    {
                        AZ::Vector4 tangent = primitive.HasTangents() ? primitive.m_tangents[vertexIndex]
                                                                     : AZ::Vector4(1.0f, 0.0f, 0.0f, 1.0f);
                        if (hasTangentDeltas)
                        {
                            tangent += AZ::Vector4(
                                target->m_tangentDeltas[vertexIndex].GetX(), target->m_tangentDeltas[vertexIndex].GetY(),
                                target->m_tangentDeltas[vertexIndex].GetZ(), 0.0f);
                        }
                        const AZ::Vector3 bitangent = primitive.HasBitangents()
                            ? primitive.m_bitangents[vertexIndex]
                            : normal.Cross(AZ::Vector3(tangent.GetX(), tangent.GetY(), tangent.GetZ())) * tangent.GetW();
                        blendShapeData->AddTangentAndBitangent(tangent, bitangent);
                    }

                    for (size_t setIndex = 0; setIndex < uvSetCount; ++setIndex)
                    {
                        const AZ::Vector2 uv = primitive.m_uvSets[setIndex].empty()
                            ? AZ::Vector2::CreateZero()
                            : primitive.m_uvSets[setIndex][vertexIndex];
                        blendShapeData->AddUV(uv, static_cast<AZ::u8>(setIndex));
                    }
                    for (size_t setIndex = 0; setIndex < colorSetCount; ++setIndex)
                    {
                        const AZ::Vector4 color = primitive.m_colorSets[setIndex].empty()
                            ? AZ::Vector4::CreateOne()
                            : primitive.m_colorSets[setIndex][vertexIndex];
                        blendShapeData->AddColor(
                            AZ::SceneAPI::DataTypes::Color(color.GetX(), color.GetY(), color.GetZ(), color.GetW()),
                            static_cast<AZ::u8>(setIndex));
                    }
                }

                for (size_t index = 0; index + 2 < primitive.m_indices.size(); index += 3)
                {
                    AZ::SceneAPI::DataTypes::IBlendShapeData::Face face;
                    face.vertexIndex[0] = vertexBase + primitive.m_indices[index + 0];
                    face.vertexIndex[1] = vertexBase + primitive.m_indices[index + 1];
                    face.vertexIndex[2] = vertexBase + primitive.m_indices[index + 2];
                    blendShapeData->AddFace(face);
                }

                vertexBase += static_cast<AZ::u32>(primitive.m_positions.size());
            }

            if (blendShapeName.empty())
            {
                blendShapeName = AZStd::string::format("BlendShape_%zu", targetIndex);
            }

            combinedResult += AddAttributeNode(context, blendShapeData, blendShapeName, "BlendShape");
        }

        return combinedResult.GetResult();
    }

} // namespace UniversalSceneImporter
