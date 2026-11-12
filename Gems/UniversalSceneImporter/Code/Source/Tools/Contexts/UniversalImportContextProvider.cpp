/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "UniversalImportContextProvider.h"

#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/Settings/SettingsRegistryVisitorUtils.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzCore/std/string/conversions.h>
#include <SceneAPI/SceneCore/Utilities/Reporting.h>
#include <SceneAPI/SceneBuilder/Importers/Utilities/RenamedNodesMap.h>
#include <SceneAPI/SceneBuilder/ImportContexts/ImportContexts.h>

#include "../Scene/UniversalSceneWrapper.h"
#include "UniversalImportContexts.h"

#include <AzToolsFramework/Asset/AssetUtils.h>

namespace UniversalSceneImporter
{
    namespace
    {
        constexpr const char* ProviderName = "UniversalSceneImporter";

        const UniversalSceneWrapper* CastSceneWrapper(AZ::SDKScene::SceneWrapperBase& sourceScene)
        {
            return azrtti_cast<const UniversalSceneWrapper*>(&sourceScene);
        }

        const UniversalNodeWrapper* CastNodeWrapper(AZ::SDKNode::NodeWrapper& sourceNode)
        {
            return azrtti_cast<const UniversalNodeWrapper*>(&sourceNode);
        }
    } // namespace

    UniversalImportContextProvider::~UniversalImportContextProvider() = default;

    AZStd::string UniversalImportContextProvider::NormalizeExtension(AZStd::string_view extension)
    {
        // Trim surrounding whitespace, then tolerate wildcard style entries such as "*.fbx".
        const size_t begin = extension.find_first_not_of(" \t\r\n");
        if (begin == AZStd::string_view::npos)
        {
            return AZStd::string();
        }
        const size_t end = extension.find_last_not_of(" \t\r\n");
        AZStd::string_view trimmed = extension.substr(begin, end - begin + 1);
        while (!trimmed.empty() && (trimmed.front() == '*' || trimmed.front() == '.'))
        {
            trimmed.remove_prefix(1);
        }
        if (trimmed.empty())
        {
            return AZStd::string();
        }

        AZStd::string normalized(trimmed);
        AZStd::to_lower(normalized.begin(), normalized.end());
        normalized.insert(normalized.begin(), '.');
        return normalized;
    }

    void UniversalImportContextProvider::GetDefaultExtensions(AZStd::unordered_set<AZStd::string>& extensions)
    {
        extensions.insert(".fbx");
        extensions.insert(".gltf");
        extensions.insert(".glb");
    }

    void UniversalImportContextProvider::GetConfiguredExtensions(AZStd::unordered_set<AZStd::string>& extensions)
    {
        AZ::SettingsRegistryInterface* settingsRegistry = AZ::SettingsRegistry::Get();
        if (!settingsRegistry)
        {
            GetDefaultExtensions(extensions);
            return;
        }

        const auto key = AZ::SettingsRegistryInterface::FixedValueString::format(
            "%s/%s", AzToolsFramework::AssetUtils::AssetImporterProviderFileTypesKey, ProviderName);

        bool foundConfiguration = false;
        auto collectExtension = [settingsRegistry, &extensions](const AZ::SettingsRegistryInterface::VisitArgs& visitArgs)
        {
            AZStd::string extension;
            if (settingsRegistry->Get(extension, visitArgs.m_jsonKeyPath))
            {
                const AZStd::string normalized = NormalizeExtension(extension);
                if (!normalized.empty())
                {
                    extensions.insert(normalized);
                }
            }
            return AZ::SettingsRegistryInterface::VisitResponse::Continue;
        };

        foundConfiguration =
            AZ::SettingsRegistryVisitorUtils::VisitArray(*settingsRegistry, collectExtension, key);

        if (!foundConfiguration)
        {
            GetDefaultExtensions(extensions);
        }
    }

    const AZStd::unordered_set<AZStd::string>& UniversalImportContextProvider::GetCachedExtensions() const
    {
        if (!m_extensionsCached)
        {
            GetConfiguredExtensions(m_configuredExtensions);
            m_extensionsCached = true;
        }
        return m_configuredExtensions;
    }

    bool UniversalImportContextProvider::CanHandleExtension(AZStd::string_view fileExtension) const
    {
        const AZStd::string normalized = NormalizeExtension(fileExtension);
        if (normalized.empty())
        {
            return false;
        }
        return GetCachedExtensions().contains(normalized);
    }

    void UniversalImportContextProvider::GetSupportedFileExtensions(AZStd::unordered_set<AZStd::string>& extensions) const
    {
        const AZStd::unordered_set<AZStd::string>& supported = GetCachedExtensions();
        extensions.insert(supported.begin(), supported.end());
    }

    AZStd::unique_ptr<AZ::SDKScene::SceneWrapperBase> UniversalImportContextProvider::CreateSceneWrapper() const
    {
        return AZStd::make_unique<UniversalSceneWrapper>();
    }

    AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::NodeEncounteredContext> UniversalImportContextProvider::CreateNodeEncounteredContext(
        AZ::SceneAPI::Containers::Scene& scene,
        AZ::SceneAPI::Containers::SceneGraph::NodeIndex currentGraphPosition,
        const AZ::SceneAPI::SceneSystem& sourceSceneSystem,
        AZ::SceneAPI::SceneBuilder::RenamedNodesMap& nodeNameMap,
        AZ::SDKScene::SceneWrapperBase& sourceScene,
        AZ::SDKNode::NodeWrapper& sourceNode)
    {
        const UniversalSceneWrapper* sceneWrapper = CastSceneWrapper(sourceScene);
        const UniversalNodeWrapper* nodeWrapper = CastNodeWrapper(sourceNode);
        if ((!sceneWrapper || !nodeWrapper) && !m_invalidWrapperReported)
        {
            m_invalidWrapperReported = true;
            AZ_Error(
                AZ::SceneAPI::Utilities::ErrorWindow, false,
                "UniversalSceneImporter received a scene or node wrapper of an unexpected type. The scene cannot be imported.");
        }

        auto context = AZStd::make_shared<UniversalNodeEncounteredContext>(
            scene, currentGraphPosition, sceneWrapper ? &sceneWrapper->GetScene() : nullptr,
            nodeWrapper ? nodeWrapper->GetNode() : nullptr, nodeWrapper ? nodeWrapper->GetNodeIndex() : -1,
            sourceSceneSystem, nodeNameMap);
        context->m_contextProvider = const_cast<UniversalImportContextProvider*>(this);
        return context;
    }

    AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::SceneDataPopulatedContextBase>
    UniversalImportContextProvider::CreateSceneDataPopulatedContext(
        AZ::SceneAPI::SceneBuilder::NodeEncounteredContext& parent,
        AZStd::shared_ptr<AZ::SceneAPI::DataTypes::IGraphObject> graphData,
        const AZStd::string& dataName)
    {
        UniversalNodeEncounteredContext* universalParent = azrtti_cast<UniversalNodeEncounteredContext*>(&parent);
        if (!universalParent)
        {
            AZ_Error(AZ::SceneAPI::Utilities::ErrorWindow, false, "Cannot create a scene data populated context from an unexpected context type.");
            return nullptr;
        }

        auto context = AZStd::make_shared<UniversalSceneDataPopulatedContext>(*universalParent, AZStd::move(graphData), dataName);
        context->m_contextProvider = const_cast<UniversalImportContextProvider*>(this);
        return context;
    }

    AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::SceneNodeAppendedContextBase>
    UniversalImportContextProvider::CreateSceneNodeAppendedContext(
        AZ::SceneAPI::SceneBuilder::SceneDataPopulatedContextBase& parent, AZ::SceneAPI::Containers::SceneGraph::NodeIndex newIndex)
    {
        UniversalSceneDataPopulatedContext* universalParent = azrtti_cast<UniversalSceneDataPopulatedContext*>(&parent);
        if (!universalParent)
        {
            AZ_Error(AZ::SceneAPI::Utilities::ErrorWindow, false, "Cannot create a scene node appended context from an unexpected context type.");
            return nullptr;
        }

        auto context = AZStd::make_shared<UniversalSceneNodeAppendedContext>(*universalParent, newIndex);
        context->m_contextProvider = const_cast<UniversalImportContextProvider*>(this);
        return context;
    }

    AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::SceneAttributeDataPopulatedContextBase>
    UniversalImportContextProvider::CreateSceneAttributeDataPopulatedContext(
        AZ::SceneAPI::SceneBuilder::SceneNodeAppendedContextBase& parent,
        AZStd::shared_ptr<AZ::SceneAPI::DataTypes::IGraphObject> nodeData,
        const AZ::SceneAPI::Containers::SceneGraph::NodeIndex attributeNodeIndex,
        const AZStd::string& dataName)
    {
        UniversalSceneNodeAppendedContext* universalParent = azrtti_cast<UniversalSceneNodeAppendedContext*>(&parent);
        if (!universalParent)
        {
            AZ_Error(
                AZ::SceneAPI::Utilities::ErrorWindow, false,
                "Cannot create a scene attribute data populated context from an unexpected context type.");
            return nullptr;
        }

        auto context = AZStd::make_shared<UniversalSceneAttributeDataPopulatedContext>(
            *universalParent, AZStd::move(nodeData), attributeNodeIndex, dataName);
        context->m_contextProvider = const_cast<UniversalImportContextProvider*>(this);
        return context;
    }

    AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::SceneAttributeNodeAppendedContextBase>
    UniversalImportContextProvider::CreateSceneAttributeNodeAppendedContext(
        AZ::SceneAPI::SceneBuilder::SceneAttributeDataPopulatedContextBase& parent,
        AZ::SceneAPI::Containers::SceneGraph::NodeIndex newIndex)
    {
        UniversalSceneAttributeDataPopulatedContext* universalParent =
            azrtti_cast<UniversalSceneAttributeDataPopulatedContext*>(&parent);
        if (!universalParent)
        {
            AZ_Error(
                AZ::SceneAPI::Utilities::ErrorWindow, false,
                "Cannot create a scene attribute node appended context from an unexpected context type.");
            return nullptr;
        }

        auto context = AZStd::make_shared<UniversalSceneAttributeNodeAppendedContext>(*universalParent, newIndex);
        context->m_contextProvider = const_cast<UniversalImportContextProvider*>(this);
        return context;
    }

    AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::SceneNodeAddedAttributesContextBase>
    UniversalImportContextProvider::CreateSceneNodeAddedAttributesContext(
        AZ::SceneAPI::SceneBuilder::SceneNodeAppendedContextBase& parent)
    {
        UniversalSceneNodeAppendedContext* universalParent = azrtti_cast<UniversalSceneNodeAppendedContext*>(&parent);
        if (!universalParent)
        {
            AZ_Error(AZ::SceneAPI::Utilities::ErrorWindow, false, "Cannot create a scene node added attributes context from an unexpected context type.");
            return nullptr;
        }

        auto context = AZStd::make_shared<UniversalSceneNodeAddedAttributesContext>(*universalParent);
        context->m_contextProvider = const_cast<UniversalImportContextProvider*>(this);
        return context;
    }

    AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::SceneNodeFinalizeContextBase>
    UniversalImportContextProvider::CreateSceneNodeFinalizeContext(
        AZ::SceneAPI::SceneBuilder::SceneNodeAddedAttributesContextBase& parent)
    {
        UniversalSceneNodeAddedAttributesContext* universalParent =
            azrtti_cast<UniversalSceneNodeAddedAttributesContext*>(&parent);
        if (!universalParent)
        {
            AZ_Error(AZ::SceneAPI::Utilities::ErrorWindow, false, "Cannot create a scene node finalize context from an unexpected context type.");
            return nullptr;
        }

        auto context = AZStd::make_shared<UniversalSceneNodeFinalizeContext>(*universalParent);
        context->m_contextProvider = const_cast<UniversalImportContextProvider*>(this);
        return context;
    }

    AZStd::shared_ptr<AZ::SceneAPI::SceneBuilder::FinalizeSceneContextBase> UniversalImportContextProvider::CreateFinalizeSceneContext(
        AZ::SceneAPI::Containers::Scene& scene,
        const AZ::SceneAPI::SceneSystem& sourceSceneSystem,
        AZ::SDKScene::SceneWrapperBase& sourceScene,
        AZ::SceneAPI::SceneBuilder::RenamedNodesMap& nodeNameMap)
    {
        const UniversalSceneWrapper* sceneWrapper = CastSceneWrapper(sourceScene);
        if (!sceneWrapper && !m_invalidWrapperReported)
        {
            m_invalidWrapperReported = true;
            AZ_Error(
                AZ::SceneAPI::Utilities::ErrorWindow, false,
                "UniversalSceneImporter cannot finalize a scene with an unexpected scene wrapper type.");
        }

        auto context = AZStd::make_shared<UniversalFinalizeSceneContext>(
            scene, sceneWrapper ? &sceneWrapper->GetScene() : nullptr, sourceSceneSystem, nodeNameMap);
        context->m_contextProvider = const_cast<UniversalImportContextProvider*>(this);
        return context;
    }

} // namespace UniversalSceneImporter

