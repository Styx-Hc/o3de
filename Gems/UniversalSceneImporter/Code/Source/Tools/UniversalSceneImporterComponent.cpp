/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "UniversalSceneImporterComponent.h"

#include <AzCore/Memory/Memory.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <SceneAPI/SceneBuilder/ImportContextRegistry.h>
#include <SceneAPI/SceneCore/Events/AssetImportRequest.h>

#include "Contexts/UniversalImportContextProvider.h"

namespace UniversalSceneImporter
{
    void UniversalSceneImporterComponent::Activate()
    {
        auto* registry = AZ::SceneAPI::SceneBuilder::ImportContextRegistryInterface::Get();
        if (!registry)
        {
            AZ_Error(
                "UniversalSceneImporter", false,
                "The SceneAPI ImportContextRegistry is not available. UniversalSceneImporter was not registered.");
            return;
        }

        m_provider = aznew UniversalImportContextProvider();
        registry->RegisterContextProvider(m_provider);
        AZ_Info("UniversalSceneImporter", "UniversalSceneImporter import context provider was registered.");
    }

    void UniversalSceneImporterComponent::Deactivate()
    {
        if (auto* registry = AZ::SceneAPI::SceneBuilder::ImportContextRegistryInterface::Get(); registry && m_provider)
        {
            registry->UnregisterContextProvider(m_provider);
        }
        m_provider = nullptr;
    }

    void UniversalSceneImporterComponent::GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided)
    {
        provided.push_back(AZ_CRC_CE("UniversalSceneImporterService"));
    }

    void UniversalSceneImporterComponent::GetDependentServices(AZ::ComponentDescriptor::DependencyArrayType& dependent)
    {
        // Activate after the registry exists so the provider can be registered immediately.
        // This gem and AssImp provide the same unique gem service ('SceneImporter'), so they can
        // never be enabled in the same project and no overlap resolution is needed here.
        dependent.push_back(AZ_CRC_CE("ImportContextRegistryService"));
    }

    void UniversalSceneImporterComponent::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<UniversalSceneImporterComponent, AZ::SceneAPI::SceneCore::SceneSystemComponent>()
                ->Version(2)
                ->Attribute(
                    AZ::Edit::Attributes::SystemComponentTags,
                    AZStd::vector<AZ::Crc32>({ AZ::SceneAPI::Events::AssetImportRequest::GetAssetImportRequestComponentTag() }));
        }
    }
} // namespace UniversalSceneImporter
