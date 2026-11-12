/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "UniversalImportContexts.h"


namespace UniversalSceneImporter
{
    UniversalImportContext::UniversalImportContext(
        const Scene* scene, const Node* node, int nodeIndex, const AZ::SceneAPI::SceneSystem* sceneSystem)
        : m_universalScene(scene)
        , m_universalNode(node)
        , m_universalNodeIndex(nodeIndex)
        , m_sceneSystem(sceneSystem)
    {
    }

    const Mesh* UniversalImportContext::GetMesh() const
    {
        if (!m_universalScene || !m_universalNode)
        {
            return nullptr;
        }
        return m_universalScene->GetMesh(m_universalNode->m_mesh);
    }

    UniversalNodeEncounteredContext::UniversalNodeEncounteredContext(
        AZ::SceneAPI::Containers::Scene& scene,
        AZ::SceneAPI::Containers::SceneGraph::NodeIndex currentGraphPosition,
        const Scene* universalScene,
        const Node* universalNode,
        int universalNodeIndex,
        const AZ::SceneAPI::SceneSystem& sceneSystem,
        AZ::SceneAPI::SceneBuilder::RenamedNodesMap& nodeNameMap)
        : UniversalImportContext(universalScene, universalNode, universalNodeIndex, &sceneSystem)
        , NodeEncounteredContext(scene, currentGraphPosition, nodeNameMap)
    {
    }

    UniversalSceneDataPopulatedContext::UniversalSceneDataPopulatedContext(
        UniversalNodeEncounteredContext& parent,
        AZStd::shared_ptr<AZ::SceneAPI::DataTypes::IGraphObject> graphData,
        const AZStd::string& dataName)
        : UniversalImportContext(parent.m_universalScene, parent.m_universalNode, parent.m_universalNodeIndex, parent.m_sceneSystem)
        , SceneDataPopulatedContextBase(parent, AZStd::move(graphData), dataName)
    {
    }

    UniversalSceneNodeAppendedContext::UniversalSceneNodeAppendedContext(
        UniversalSceneDataPopulatedContext& parent, AZ::SceneAPI::Containers::SceneGraph::NodeIndex newIndex)
        : UniversalImportContext(parent.m_universalScene, parent.m_universalNode, parent.m_universalNodeIndex, parent.m_sceneSystem)
        , SceneNodeAppendedContextBase(parent, newIndex)
    {
    }

    UniversalSceneAttributeDataPopulatedContext::UniversalSceneAttributeDataPopulatedContext(
        UniversalSceneNodeAppendedContext& parent,
        AZStd::shared_ptr<AZ::SceneAPI::DataTypes::IGraphObject> nodeData,
        const AZ::SceneAPI::Containers::SceneGraph::NodeIndex attributeNodeIndex,
        const AZStd::string& dataName)
        : UniversalImportContext(parent.m_universalScene, parent.m_universalNode, parent.m_universalNodeIndex, parent.m_sceneSystem)
        , SceneAttributeDataPopulatedContextBase(parent, AZStd::move(nodeData), attributeNodeIndex, dataName)
    {
    }

    UniversalSceneAttributeNodeAppendedContext::UniversalSceneAttributeNodeAppendedContext(
        UniversalSceneAttributeDataPopulatedContext& parent, AZ::SceneAPI::Containers::SceneGraph::NodeIndex newIndex)
        : UniversalImportContext(parent.m_universalScene, parent.m_universalNode, parent.m_universalNodeIndex, parent.m_sceneSystem)
        , SceneAttributeNodeAppendedContextBase(parent, newIndex)
    {
    }

    UniversalSceneNodeAddedAttributesContext::UniversalSceneNodeAddedAttributesContext(UniversalSceneNodeAppendedContext& parent)
        : UniversalImportContext(parent.m_universalScene, parent.m_universalNode, parent.m_universalNodeIndex, parent.m_sceneSystem)
        , SceneNodeAddedAttributesContextBase(parent)
    {
    }

    UniversalSceneNodeFinalizeContext::UniversalSceneNodeFinalizeContext(UniversalSceneNodeAddedAttributesContext& parent)
        : UniversalImportContext(parent.m_universalScene, parent.m_universalNode, parent.m_universalNodeIndex, parent.m_sceneSystem)
        , SceneNodeFinalizeContextBase(parent)
    {
    }

    UniversalFinalizeSceneContext::UniversalFinalizeSceneContext(
        AZ::SceneAPI::Containers::Scene& scene,
        const Scene* universalScene,
        const AZ::SceneAPI::SceneSystem& sceneSystem,
        AZ::SceneAPI::SceneBuilder::RenamedNodesMap& nodeNameMap)
        : FinalizeSceneContextBase(scene, nodeNameMap)
        , m_universalScene(universalScene)
        , m_sceneSystem(&sceneSystem)
        , m_renamedNodesMap(nodeNameMap)
    {
    }

} // namespace UniversalSceneImporter
