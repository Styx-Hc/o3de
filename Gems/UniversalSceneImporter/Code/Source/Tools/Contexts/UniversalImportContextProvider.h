/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/RTTI/RTTIMacros.h>
#include <AzCore/std/containers/unordered_set.h>
#include <SceneAPI/SceneBuilder/ImportContexts/ImportContextProvider.h>

namespace UniversalSceneImporter
{
    //! ImportContextProvider for the ufbx (FBX) and cgltf (glTF/GLB) importers.
    //!
    //! A single provider serves all three formats because the scene wrapper selects the converter
    //! based on the file content and extension. This keeps provider selection simple: the registry
    //! only has to answer "does UniversalSceneImporter handle .fbx/.gltf/.glb?".
    class UniversalImportContextProvider : public AZ::SceneAPI::SceneBuilder::ImportContextProvider
    {
    public:
        AZ_RTTI(UniversalImportContextProvider, "{22C62B85-E9C0-4171-AA4B-E5C4B395EAD9}");

        UniversalImportContextProvider() = default;
        ~UniversalImportContextProvider() override;

        AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::NodeEncounteredContext> CreateNodeEncounteredContext(
            AZ::SceneAPI::Containers::Scene& scene,
            AZ::SceneAPI::Containers::SceneGraph::NodeIndex currentGraphPosition,
            const AZ::SceneAPI::SceneSystem& sourceSceneSystem,
            AZ::SceneAPI::SceneBuilder::RenamedNodesMap& nodeNameMap,
            AZ::SDKScene::SceneWrapperBase& sourceScene,
            AZ::SDKNode::NodeWrapper& sourceNode) override;

        AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::SceneDataPopulatedContextBase> CreateSceneDataPopulatedContext(
            AZ::SceneAPI::SceneBuilder::NodeEncounteredContext& parent,
            AZStd::shared_ptr<AZ::SceneAPI::DataTypes::IGraphObject> graphData,
            const AZStd::string& dataName) override;

        AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::SceneNodeAppendedContextBase> CreateSceneNodeAppendedContext(
            AZ::SceneAPI::SceneBuilder::SceneDataPopulatedContextBase& parent,
            AZ::SceneAPI::Containers::SceneGraph::NodeIndex newIndex) override;

        AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::SceneAttributeDataPopulatedContextBase> CreateSceneAttributeDataPopulatedContext(
            AZ::SceneAPI::SceneBuilder::SceneNodeAppendedContextBase& parent,
            AZStd::shared_ptr<AZ::SceneAPI::DataTypes::IGraphObject> nodeData,
            const AZ::SceneAPI::Containers::SceneGraph::NodeIndex attributeNodeIndex,
            const AZStd::string& dataName) override;

        AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::SceneAttributeNodeAppendedContextBase> CreateSceneAttributeNodeAppendedContext(
            AZ::SceneAPI::SceneBuilder::SceneAttributeDataPopulatedContextBase& parent,
            AZ::SceneAPI::Containers::SceneGraph::NodeIndex newIndex) override;

        AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::SceneNodeAddedAttributesContextBase> CreateSceneNodeAddedAttributesContext(
            AZ::SceneAPI::SceneBuilder::SceneNodeAppendedContextBase& parent) override;

        AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::SceneNodeFinalizeContextBase> CreateSceneNodeFinalizeContext(
            AZ::SceneAPI::SceneBuilder::SceneNodeAddedAttributesContextBase& parent) override;

        AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::FinalizeSceneContextBase> CreateFinalizeSceneContext(
            AZ::SceneAPI::Containers::Scene& scene,
            const AZ::SceneAPI::SceneSystem& sourceSceneSystem,
            AZ::SDKScene::SceneWrapperBase& sourceScene,
            AZ::SceneAPI::SceneBuilder::RenamedNodesMap& nodeNameMap) override;

        bool CanHandleExtension(AZStd::string_view fileExtension) const override;
        void GetSupportedFileExtensions(AZStd::unordered_set<AZStd::string>& extensions) const override;

        AZStd::unique_ptr<AZ::SDKScene::SceneWrapperBase> CreateSceneWrapper() const override;

        AZStd::string GetImporterName() const override
        {
            return "UniversalSceneImporter";
        }

        static AZStd::string NormalizeExtension(AZStd::string_view extension);
        static void GetConfiguredExtensions(AZStd::unordered_set<AZStd::string>& extensions);
        static void GetDefaultExtensions(AZStd::unordered_set<AZStd::string>& extensions);

    private:
        const AZStd::unordered_set<AZStd::string>& GetCachedExtensions() const;

        mutable bool m_invalidWrapperReported = false;
        mutable AZStd::unordered_set<AZStd::string> m_configuredExtensions;
        mutable bool m_extensionsCached = false;
    };
} // namespace UniversalSceneImporter
