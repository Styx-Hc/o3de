/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "SkinWeightImporter.h"

#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzCore/std/sort.h>
#include <AzToolsFramework/Debug/TraceContext.h>
#include <SceneAPI/SceneCore/Utilities/Reporting.h>

#include "ImporterUtils.h"

namespace UniversalSceneImporter
{
    SkinWeightImporter::SkinWeightImporter()
    {
        BindToCall(&SkinWeightImporter::ImportSkinWeights);
        BindToCall(&SkinWeightImporter::FinalizeSkinWeights);
    }

    void SkinWeightImporter::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<SkinWeightImporter, AZ::SceneAPI::SceneCore::LoadingComponent>()->Version(2);
        }
    }

    AZ::SceneAPI::Events::ProcessingResult SkinWeightImporter::ImportSkinWeights(UniversalSceneNodeAppendedContext& context)
    {
        AZ_TraceContext("Importer", "Skin Weights");

        const Mesh* mesh = context.GetMesh();
        if (!mesh || mesh->m_primitives.empty() || !mesh->IsSkinned() || !context.m_universalNode ||
            context.m_universalNode->m_skin < 0)
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        auto skinWeightData = AZStd::make_shared<AZ::SceneData::GraphData::SkinWeightData>();
        skinWeightData->ResizeContainerSpace(mesh->GetVertexCount());

        const AZ::SceneAPI::Events::ProcessingResult result = AddAttributeNode(context, skinWeightData, s_skinWeightName);

        if (result != AZ::SceneAPI::Events::ProcessingResult::Failure)
        {
            m_pending.push_back({ context.m_universalScene, context.m_universalNodeIndex, AZStd::move(skinWeightData) });
        }
        return result;
    }

    AZ::SceneAPI::Events::ProcessingResult SkinWeightImporter::FinalizeSkinWeights(UniversalFinalizeSceneContext& context)
    {
        AZ_TraceContext("Importer", "Finalize Skin Weights");
        // An import that failed before its finalize phase leaves pending entries behind, and
        // their scene wrapper may already be destroyed by the time the next import finalizes.
        // Drop everything that does not belong to the scene being finalized.
        if (!context.m_universalScene)
        {
            m_pending.clear();
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }
        m_pending.erase(
            AZStd::remove_if(
                m_pending.begin(), m_pending.end(),
                [&context](const PendingSkinWeights& pending) { return pending.m_scene != context.m_universalScene; }),
            m_pending.end());

        if (m_pending.empty())
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        // Detect duplicate final bone names. Both nodes can still be part of the skeleton, but skin
        // weights address bones by name so duplicate names make the deformation ambiguous.
        {
            AZStd::unordered_map<AZStd::string, int> boneNameCounts;
            for (size_t nodeIndex = 0; nodeIndex < context.m_universalScene->GetNodes().size(); ++nodeIndex)
            {
                if (!context.m_universalScene->GetNodes()[nodeIndex].m_isBone)
                {
                    continue;
                }
                boneNameCounts[GetFinalNodeName(context.m_renamedNodesMap, *context.m_universalScene, static_cast<int>(nodeIndex))]++;
            }

            size_t reported = 0;
            for (const auto& [name, count] : boneNameCounts)
            {
                if (count > 1 && reported < 5)
                {
                    AZ_Warning(
                        "UniversalSceneImporter", false,
                        "The skeleton contains %d bones named '%s'. Skin weights reference bones by name, "
                        "so the deformation for these bones may be incorrect. Rename the duplicates in the source file.",
                        count, name.c_str());
                    ++reported;
                }
            }
        }

        AZ::SceneAPI::Events::ProcessingResultCombiner combinedResult;
        for (PendingSkinWeights& pending : m_pending)
        {
            if (!pending.m_scene || !pending.m_data)
            {
                combinedResult += AZ::SceneAPI::Events::ProcessingResult::Ignored;
                continue;
            }

            const Node* node = pending.m_scene->GetNode(pending.m_nodeIndex);
            const Mesh* mesh = node ? pending.m_scene->GetMesh(node->m_mesh) : nullptr;
            if (!mesh)
            {
                AZ_Warning("UniversalSceneImporter", false, "A pending skin weight data entry references a missing mesh.");
                combinedResult += AZ::SceneAPI::Events::ProcessingResult::Failure;
                continue;
            }

            size_t vertexIndex = 0;
            for (const MeshPrimitive& primitive : mesh->m_primitives)
            {
                for (size_t primitiveVertex = 0; primitiveVertex < primitive.m_positions.size(); ++primitiveVertex, ++vertexIndex)
                {
                    if (primitiveVertex >= primitive.m_skinInfluences.size())
                    {
                        continue;
                    }

                    // Merge duplicate joint indices, keep the strongest influences and renormalize
                    // so downstream skinning does not receive degenerate weights.
                    AZStd::unordered_map<AZ::u32, float> weightByJoint;
                    for (const SkinInfluence& influence : primitive.m_skinInfluences[primitiveVertex])
                    {
                        if (influence.m_weight <= 0.0f)
                        {
                            continue;
                        }
                        weightByJoint[influence.m_joint] += influence.m_weight;
                    }
                    if (weightByJoint.empty())
                    {
                        continue;
                    }

                    AZStd::vector<SkinInfluence> influences;
                    influences.reserve(weightByJoint.size());
                    for (const auto& [joint, weight] : weightByJoint)
                    {
                        influences.push_back({ joint, weight });
                    }
                    if (influences.size() > MaxSkinInfluences)
                    {
                        AZStd::sort(
                            influences.begin(), influences.end(),
                            [](const SkinInfluence& lhs, const SkinInfluence& rhs) { return lhs.m_weight > rhs.m_weight; });
                        influences.resize(MaxSkinInfluences);
                    }

                    float totalWeight = 0.0f;
                    for (const SkinInfluence& influence : influences)
                    {
                        totalWeight += influence.m_weight;
                    }
                    if (totalWeight <= 0.0f)
                    {
                        continue;
                    }

                    for (const SkinInfluence& influence : influences)
                    {
                        const AZStd::string boneName =
                            GetFinalNodeName(context.m_renamedNodesMap, *pending.m_scene, static_cast<int>(influence.m_joint));
                        if (boneName.empty())
                        {
                            continue;
                        }

                        const int boneId = pending.m_data->GetBoneId(boneName);
                        AZ::SceneAPI::DataTypes::ISkinWeightData::Link link;
                        link.boneId = boneId;
                        link.weight = influence.m_weight / totalWeight;
                        pending.m_data->AddAndSortLink(vertexIndex, link);
                    }
                }
            }
            combinedResult += AZ::SceneAPI::Events::ProcessingResult::Success;
        }

        m_pending.clear();
        return combinedResult.GetResult();
    }

} // namespace UniversalSceneImporter

