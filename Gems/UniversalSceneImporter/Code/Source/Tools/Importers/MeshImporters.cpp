/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "MeshImporters.h"

#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzToolsFramework/Debug/TraceContext.h>
#include <SceneAPI/SceneBuilder/Importers/ImporterUtilities.h>
#include <SceneAPI/SceneCore/Utilities/Reporting.h>
#include <SceneAPI/SceneData/GraphData/BoneData.h>
#include <SceneAPI/SceneData/GraphData/MeshData.h>
#include <SceneAPI/SceneData/GraphData/RootBoneData.h>
#include <SceneAPI/SceneData/GraphData/TransformData.h>

#include "ImporterUtils.h"

namespace UniversalSceneImporter
{
    //////////////////////////////////////////////////////////////////////////
    // TransformImporter
    //////////////////////////////////////////////////////////////////////////

    TransformImporter::TransformImporter()
    {
        BindToCall(&TransformImporter::ImportTransform);
    }

    void TransformImporter::Reflect(AZ::ReflectContext* context)
    {
        ReflectLoadingComponent<TransformImporter>(context);
    }

    AZ::SceneAPI::Events::ProcessingResult TransformImporter::ImportTransform(UniversalSceneNodeAppendedContext& context)
    {
        AZ_TraceContext("Importer", "Transform");

        if (!context.m_universalNode)
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        AZ::SceneAPI::DataTypes::MatrixType localTransform = context.m_universalNode->m_localTransform;
        if (context.m_universalNode->m_hasLocalOverride)
        {
            // Skinned mesh: the vertices stay in the mesh node's local space and the model
            // builder bakes the node's graph world into them exactly once. The converter
            // computed this override so the graph world equals the shared bind world of the
            // skin; emitting it here is what keeps the exported vertices in bind space.
            localTransform = context.m_universalNode->m_localOverride;
        }
        else if (context.m_universalNode->m_skin >= 0)
        {
            // Skinned mesh without bind information: the node transform is baked into the
            // vertices by SkinMeshImporter so the mesh shares the single world/bind space of
            // the skeleton's inverse binds. Emitting a transform node here would apply it a
            // second time.
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        if (localTransform == AZ::SceneAPI::DataTypes::MatrixType::Identity())
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        auto transformData = AZStd::make_shared<AZ::SceneData::GraphData::TransformData>(localTransform);

        if (context.m_scene.GetGraph().HasNodeContent(context.m_currentGraphPosition))
        {
            if (context.m_scene.GetGraph().IsNodeEndPoint(context.m_currentGraphPosition))
            {
                // Endpoints (mesh attributes, bones, ...) carry their own data; adding a transform
                // child would change how downstream exporters interpret the node.
                return AZ::SceneAPI::Events::ProcessingResult::Ignored;
            }

            return AddAttributeNode(context, transformData, s_transformNodeName);
        }

        const bool addedData = context.m_scene.GetGraph().SetContent(context.m_currentGraphPosition, transformData);
        AZ_Error(AZ::SceneAPI::Utilities::ErrorWindow, addedData, "Failed to add transform data to the scene node.");
        return addedData ? AZ::SceneAPI::Events::ProcessingResult::Success : AZ::SceneAPI::Events::ProcessingResult::Failure;
    }

    //////////////////////////////////////////////////////////////////////////
    // BoneImporter
    //////////////////////////////////////////////////////////////////////////

    BoneImporter::BoneImporter()
    {
        BindToCall(&BoneImporter::ImportBone);
    }

    void BoneImporter::Reflect(AZ::ReflectContext* context)
    {
        ReflectLoadingComponent<BoneImporter>(context);
    }

    AZ::SceneAPI::Events::ProcessingResult BoneImporter::ImportBone(UniversalNodeEncounteredContext& context)
    {
        AZ_TraceContext("Importer", "Bone");

        if (!context.m_universalScene || !context.m_universalNode || !context.m_universalNode->m_isBone)
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        AZStd::shared_ptr<AZ::SceneData::GraphData::BoneData> boneData;
        if (AZ::SceneAPI::SceneBuilder::NodeHasAncestorOfType(
                context.m_scene.GetGraph(), context.m_currentGraphPosition, AZ::SceneAPI::DataTypes::IBoneData::TYPEINFO_Uuid()))
        {
            boneData = AZStd::make_shared<AZ::SceneData::GraphData::BoneData>();
        }
        else
        {
            boneData = AZStd::make_shared<AZ::SceneData::GraphData::RootBoneData>();
        }

        const AZ::SceneAPI::DataTypes::MatrixType worldTransform =
            context.m_universalScene->GetNodeSkeletonSpaceTransform(context.m_universalNodeIndex);
        boneData->SetWorldTransform(worldTransform);

        // Bone data has to be processed before mesh data so that skinned meshes are nested under
        // (or at least associated with) their skeleton.
        context.m_createdData.insert(context.m_createdData.begin(), AZStd::move(boneData));
        return AZ::SceneAPI::Events::ProcessingResult::Success;
    }

    //////////////////////////////////////////////////////////////////////////
    // MeshImporter / SkinMeshImporter
    //////////////////////////////////////////////////////////////////////////

    MeshImporter::MeshImporter()
    {
        BindToCall(&MeshImporter::ImportMesh);
    }

    void MeshImporter::Reflect(AZ::ReflectContext* context)
    {
        ReflectLoadingComponent<MeshImporter>(context);
    }

    AZ::SceneAPI::Events::ProcessingResult MeshImporter::ImportMesh(UniversalNodeEncounteredContext& context)
    {
        AZ_TraceContext("Importer", "Mesh");

        const Mesh* mesh = context.GetMesh();
        if (!mesh || mesh->m_primitives.empty() || mesh->IsSkinned())
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        auto meshData = BuildMeshData(*mesh, false);
        if (!meshData || meshData->GetVertexCount() == 0 || meshData->GetFaceCount() == 0)
        {
            AZ_Warning(
                "UniversalSceneImporter", false, "Mesh '%s' does not contain any renderable triangles and will be skipped.",
                mesh->m_name.c_str());
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        context.m_createdData.push_back(AZStd::move(meshData));
        return AZ::SceneAPI::Events::ProcessingResult::Success;
    }

    SkinMeshImporter::SkinMeshImporter()
    {
        BindToCall(&SkinMeshImporter::ImportSkinMesh);
    }

    void SkinMeshImporter::Reflect(AZ::ReflectContext* context)
    {
        ReflectLoadingComponent<SkinMeshImporter>(context);
    }

    AZ::SceneAPI::Events::ProcessingResult SkinMeshImporter::ImportSkinMesh(UniversalNodeEncounteredContext& context)
    {
        AZ_TraceContext("Importer", "Skin Mesh");

        const Mesh* mesh = context.GetMesh();
        if (!mesh || mesh->m_primitives.empty() || !mesh->IsSkinned())
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        auto meshData = BuildMeshData(*mesh, true);
        if (!meshData || meshData->GetVertexCount() == 0 || meshData->GetFaceCount() == 0)
        {
            AZ_Warning(
                "UniversalSceneImporter", false, "Skinned mesh '%s' does not contain any renderable triangles and will be skipped.",
                mesh->m_name.c_str());
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        // Skinned vertices must live in the same world/bind space as the skeleton's inverse
        // bind matrices (the runtime skinning contract: skinMatrix = jointWorld * inverseBind).
        // When the converter produced a bind-space local override for this node, the vertices
        // stay in mesh-local space: the graph carries the override (TransformImporter) and the
        // model builder performs the single world bake. Without an override, fall back to
        // baking the node's world transform in here; the matching TransformData node is
        // suppressed in TransformImporter.
        if (context.m_universalScene && context.m_universalNodeIndex >= 0 && context.m_universalNode &&
            !context.m_universalNode->m_hasLocalOverride)
        {
            const AZ::Matrix3x4 meshWorld = context.m_universalScene->GetNodeWorldTransform(context.m_universalNodeIndex);
            if (meshWorld != AZ::Matrix3x4::CreateIdentity())
            {
                for (AZ::Vector3& position : meshData->GetPositions())
                {
                    position = meshWorld.TransformPoint(position);
                }
                for (AZ::Vector3& normal : meshData->GetNormals())
                {
                    const AZ::Vector3 transformed = meshWorld.TransformVector(normal);
                    if (transformed.GetLengthSq() > AZ::Constants::FloatEpsilon)
                    {
                        normal = transformed.GetNormalized();
                    }
                }
            }
        }

        context.m_createdData.push_back(AZStd::move(meshData));
        return AZ::SceneAPI::Events::ProcessingResult::Success;
    }

} // namespace UniversalSceneImporter

