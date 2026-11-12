/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/RTTI/RTTI.h>
#include <SceneAPI/SceneBuilder/ImportContexts/ImportContexts.h>
#include <SceneAPI/SceneBuilder/SceneSystem.h>

#include "../Scene/UniversalScene.h"

namespace UniversalSceneImporter
{
    //! Common data shared by all import contexts of this gem.
    //!
    //! The contexts carry pointers to the format-neutral scene and node so importers never have to
    //! cast SceneAPI wrappers. Pointers are defensive: a context is still valid for dispatch when a
    //! wrapper type does not match, in which case importers simply report "Ignored".
    struct UniversalImportContext
    {
        AZ_RTTI(UniversalImportContext, "{0259C55D-2159-43C2-A1A5-B2049EA282B5}");

        UniversalImportContext(const Scene* scene, const Node* node, int nodeIndex, const AZ::SceneAPI::SceneSystem* sceneSystem);

        const Scene* m_universalScene = nullptr;
        const Node* m_universalNode = nullptr;
        //! Index of m_universalNode in m_universalScene->GetNodes(), or -1 when unavailable.
        int m_universalNodeIndex = -1;
        const AZ::SceneAPI::SceneSystem* m_sceneSystem = nullptr;

        const Mesh* GetMesh() const;
    };

    struct UniversalNodeEncounteredContext
        : public UniversalImportContext
        , public AZ::SceneAPI::SceneBuilder::NodeEncounteredContext
    {
        AZ_RTTI(
            UniversalNodeEncounteredContext, "{23AB2553-8DBA-49D7-9E90-09D455A62033}", UniversalImportContext,
            AZ::SceneAPI::SceneBuilder::NodeEncounteredContext);

        UniversalNodeEncounteredContext(
            AZ::SceneAPI::Containers::Scene& scene,
            AZ::SceneAPI::Containers::SceneGraph::NodeIndex currentGraphPosition,
            const Scene* universalScene,
            const Node* universalNode,
            int universalNodeIndex,
            const AZ::SceneAPI::SceneSystem& sceneSystem,
            AZ::SceneAPI::SceneBuilder::RenamedNodesMap& nodeNameMap);
    };

    struct UniversalSceneDataPopulatedContext
        : public UniversalImportContext
        , public AZ::SceneAPI::SceneBuilder::SceneDataPopulatedContextBase
    {
        AZ_RTTI(
            UniversalSceneDataPopulatedContext, "{8CB1B216-4CE6-4355-B3CA-37CBA344C095}", UniversalImportContext,
            AZ::SceneAPI::SceneBuilder::SceneDataPopulatedContextBase);

        UniversalSceneDataPopulatedContext(
            UniversalNodeEncounteredContext& parent,
            AZStd::shared_ptr<AZ::SceneAPI::DataTypes::IGraphObject> graphData,
            const AZStd::string& dataName);
    };

    struct UniversalSceneNodeAppendedContext
        : public UniversalImportContext
        , public AZ::SceneAPI::SceneBuilder::SceneNodeAppendedContextBase
    {
        AZ_RTTI(
            UniversalSceneNodeAppendedContext, "{D698292D-140B-45C5-A16B-8DAFEE2E7B7A}", UniversalImportContext,
            AZ::SceneAPI::SceneBuilder::SceneNodeAppendedContextBase);

        UniversalSceneNodeAppendedContext(
            UniversalSceneDataPopulatedContext& parent, AZ::SceneAPI::Containers::SceneGraph::NodeIndex newIndex);
    };

    struct UniversalSceneAttributeDataPopulatedContext
        : public UniversalImportContext
        , public AZ::SceneAPI::SceneBuilder::SceneAttributeDataPopulatedContextBase
    {
        AZ_RTTI(
            UniversalSceneAttributeDataPopulatedContext, "{8B5E9BD3-A996-4D91-B162-B01B7DC27864}", UniversalImportContext,
            AZ::SceneAPI::SceneBuilder::SceneAttributeDataPopulatedContextBase);

        UniversalSceneAttributeDataPopulatedContext(
            UniversalSceneNodeAppendedContext& parent,
            AZStd::shared_ptr<AZ::SceneAPI::DataTypes::IGraphObject> nodeData,
            const AZ::SceneAPI::Containers::SceneGraph::NodeIndex attributeNodeIndex,
            const AZStd::string& dataName);
    };

    struct UniversalSceneAttributeNodeAppendedContext
        : public UniversalImportContext
        , public AZ::SceneAPI::SceneBuilder::SceneAttributeNodeAppendedContextBase
    {
        AZ_RTTI(
            UniversalSceneAttributeNodeAppendedContext, "{D11240AA-433A-4C86-86BF-794E4498F728}", UniversalImportContext,
            AZ::SceneAPI::SceneBuilder::SceneAttributeNodeAppendedContextBase);

        UniversalSceneAttributeNodeAppendedContext(
            UniversalSceneAttributeDataPopulatedContext& parent, AZ::SceneAPI::Containers::SceneGraph::NodeIndex newIndex);
    };

    struct UniversalSceneNodeAddedAttributesContext
        : public UniversalImportContext
        , public AZ::SceneAPI::SceneBuilder::SceneNodeAddedAttributesContextBase
    {
        AZ_RTTI(
            UniversalSceneNodeAddedAttributesContext, "{22A3BFA2-6528-43A6-8A91-B11BDFF7B8E1}", UniversalImportContext,
            AZ::SceneAPI::SceneBuilder::SceneNodeAddedAttributesContextBase);

        UniversalSceneNodeAddedAttributesContext(UniversalSceneNodeAppendedContext& parent);
    };

    struct UniversalSceneNodeFinalizeContext
        : public UniversalImportContext
        , public AZ::SceneAPI::SceneBuilder::SceneNodeFinalizeContextBase
    {
        AZ_RTTI(
            UniversalSceneNodeFinalizeContext, "{039B4768-74C9-4E20-9653-01D768BCB066}", UniversalImportContext,
            AZ::SceneAPI::SceneBuilder::SceneNodeFinalizeContextBase);

        UniversalSceneNodeFinalizeContext(UniversalSceneNodeAddedAttributesContext& parent);
    };

    struct UniversalFinalizeSceneContext : public AZ::SceneAPI::SceneBuilder::FinalizeSceneContextBase
    {
        AZ_RTTI(
            UniversalFinalizeSceneContext, "{D317015A-255C-4451-AEC2-437924C40448}",
            AZ::SceneAPI::SceneBuilder::FinalizeSceneContextBase);

        UniversalFinalizeSceneContext(
            AZ::SceneAPI::Containers::Scene& scene,
            const Scene* universalScene,
            const AZ::SceneAPI::SceneSystem& sceneSystem,
            AZ::SceneAPI::SceneBuilder::RenamedNodesMap& nodeNameMap);

        const Scene* m_universalScene = nullptr;
        const AZ::SceneAPI::SceneSystem* m_sceneSystem = nullptr;
        AZ::SceneAPI::SceneBuilder::RenamedNodesMap& m_renamedNodesMap;
    };

} // namespace UniversalSceneImporter
