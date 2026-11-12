/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "AnimationImporter.h"

#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzToolsFramework/Debug/TraceContext.h>
#include <SceneAPI/SceneBuilder/Importers/Utilities/RenamedNodesMap.h>
#include <SceneAPI/SceneCore/Utilities/Reporting.h>
#include <SceneAPI/SceneData/GraphData/AnimationData.h>

namespace UniversalSceneImporter
{
    AnimationImporter::AnimationImporter()
    {
        BindToCall(&AnimationImporter::ImportAnimation);
    }

    void AnimationImporter::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<AnimationImporter, AZ::SceneAPI::SceneCore::LoadingComponent>()->Version(1);
        }
    }

    AZ::SceneAPI::Events::ProcessingResult AnimationImporter::ImportAnimation(UniversalSceneNodeAppendedContext& context)
    {
        AZ_TraceContext("Importer", "Animation");

        if (!context.m_universalScene || !context.m_universalNode)
        {
            return AZ::SceneAPI::Events::ProcessingResult::Ignored;
        }

        const int nodeIndex = context.m_universalNodeIndex;
        AZ::SceneAPI::Events::ProcessingResultCombiner combinedResult;
        bool createdAnimation = false;

        for (const Animation& animation : context.m_universalScene->GetAnimations())
        {
            const NodeAnimation* nodeAnimation = nullptr;
            size_t clipFrameCount = 0;
            for (const NodeAnimation& candidate : animation.m_nodeAnimations)
            {
                if (candidate.m_keyFrames.empty())
                {
                    continue;
                }
                clipFrameCount = AZStd::max(clipFrameCount, candidate.m_keyFrames.size());
                if (candidate.m_node == nodeIndex)
                {
                    nodeAnimation = &candidate;
                }
            }

            // Bones that this clip does not animate get a constant filler track at the default pose,
            // so that every exported motion covers the complete joint hierarchy: EMotionFX derives
            // the motion joint hierarchy from the breadth-first joint order, so a bone without any
            // animation data would re-parent its animated child joints. Non-bone nodes are only
            // emitted when the clip actually animates them.
            const bool createFiller = !nodeAnimation && context.m_universalNode->m_isBone && clipFrameCount > 0;
            if (!nodeAnimation && !createFiller)
            {
                continue;
            }

            // The top most bone of a skeleton carries the transforms of the nodes above it, which
            // EMotionFX does not evaluate, in its bind pose (see Scene::BakeBindPoseIntoGraph).
            // Its tracks have to carry them as well to stay in the same space.
            AZ::Matrix3x4 trackSpace = AZ::Matrix3x4::CreateIdentity();
            const Node* parentNode = context.m_universalScene->GetNode(context.m_universalNode->m_parent);
            if (context.m_universalNode->m_isBone && !(parentNode && parentNode->m_isBone))
            {
                trackSpace = context.m_universalScene->GetSkeletonParentWorldTransform(nodeIndex);
            }

            auto boneAnimationData = AZStd::make_shared<AZ::SceneData::GraphData::AnimationData>();
            boneAnimationData->SetTimeStepBetweenFrames(animation.m_timeStep);
            if (nodeAnimation)
            {
                boneAnimationData->ReserveKeyFrames(nodeAnimation->m_keyFrames.size());
                for (const AZ::SceneAPI::DataTypes::MatrixType& keyFrame : nodeAnimation->m_keyFrames)
                {
                    boneAnimationData->AddKeyFrame(trackSpace * keyFrame);
                }
            }
            else
            {
                // The local override of a bone is its bind pose; an unanimated bone stays at the
                // default pose of the file, which is what the animated bones are relative to.
                const AZ::Matrix3x4 bindTransform = trackSpace * context.m_universalNode->m_localTransform;
                boneAnimationData->ReserveKeyFrames(clipFrameCount);
                for (size_t frame = 0; frame < clipFrameCount; ++frame)
                {
                    boneAnimationData->AddKeyFrame(bindTransform);
                }
            }

            AZStd::string boneNodeName = animation.m_name;
            if (AZ::SceneAPI::SceneBuilder::RenamedNodesMap::SanitizeNodeName(
                    boneNodeName, context.m_scene.GetGraph(), context.m_currentGraphPosition, "animation"))
            {
                AZ_Warning(
                    "UniversalSceneImporter", false,
                    "Animation node '%s' was renamed to '%s' because another node already uses that name.",
                    animation.m_name.c_str(), boneNodeName.c_str());
            }

            const AZ::SceneAPI::Containers::SceneGraph::NodeIndex boneNodeIndex = context.m_scene.GetGraph().AddChild(
                context.m_currentGraphPosition, boneNodeName.c_str(), AZStd::move(boneAnimationData));
            if (boneNodeIndex.IsValid())
            {
                context.m_scene.GetGraph().MakeEndPoint(boneNodeIndex);
                createdAnimation = true;
                combinedResult += AZ::SceneAPI::Events::ProcessingResult::Success;
            }
            else
            {
                combinedResult += AZ::SceneAPI::Events::ProcessingResult::Failure;
            }

            for (const MorphAnimation& morphAnimation : animation.m_morphAnimations)
            {
                if (morphAnimation.m_node != nodeIndex || morphAnimation.m_keyFrames.empty())
                {
                    continue;
                }

                auto animationData = AZStd::make_shared<AZ::SceneData::GraphData::BlendShapeAnimationData>();
                animationData->ReserveKeyFrames(morphAnimation.m_keyFrames.size());
                animationData->SetTimeStepBetweenFrames(animation.m_timeStep);
                for (float weight : morphAnimation.m_keyFrames)
                {
                    animationData->AddKeyFrame(weight);
                }
                animationData->SetBlendShapeName(morphAnimation.m_blendShapeName.c_str());

                AZStd::string nodeName = AZStd::string::format("%s_%s", animation.m_name.c_str(), morphAnimation.m_blendShapeName.c_str());
                AZ::SceneAPI::SceneBuilder::RenamedNodesMap::SanitizeNodeName(
                    nodeName, context.m_scene.GetGraph(), context.m_currentGraphPosition, "blendShapeAnimation");

                const AZ::SceneAPI::Containers::SceneGraph::NodeIndex newIndex = context.m_scene.GetGraph().AddChild(
                    context.m_currentGraphPosition, nodeName.c_str(), AZStd::move(animationData));
                if (newIndex.IsValid())
                {
                    context.m_scene.GetGraph().MakeEndPoint(newIndex);
                    createdAnimation = true;
                    combinedResult += AZ::SceneAPI::Events::ProcessingResult::Success;
                }
                else
                {
                    combinedResult += AZ::SceneAPI::Events::ProcessingResult::Failure;
                }
            }
        }

        return createdAnimation ? combinedResult.GetResult() : AZ::SceneAPI::Events::ProcessingResult::Ignored;
    }

} // namespace UniversalSceneImporter
