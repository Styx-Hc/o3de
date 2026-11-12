/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <SceneAPI/SceneCore/Components/SceneSystemComponent.h>

namespace AZ::SceneAPI::SceneBuilder
{
    struct ImportContextProvider;
}

namespace UniversalSceneImporter
{
    //! SceneSystemComponent that registers the UniversalSceneImporter import context provider with
    //! the SceneAPI ImportContextRegistry.
    //!
    //! The component is created automatically on the Scene System entity used by the Scene Builder
    //! and by the editor asset importer when the gem is enabled.
    class UniversalSceneImporterComponent : public AZ::SceneAPI::SceneCore::SceneSystemComponent
    {
    public:
        AZ_COMPONENT(
            UniversalSceneImporterComponent, "{F17F7CC7-B428-4AC8-AC2C-8B938051FE83}",
            AZ::SceneAPI::SceneCore::SceneSystemComponent);

        ~UniversalSceneImporterComponent() override = default;

        void Activate() override;
        void Deactivate() override;

        static void GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided);
        static void GetDependentServices(AZ::ComponentDescriptor::DependencyArrayType& dependent);
        static void Reflect(AZ::ReflectContext* context);

    private:
        AZ::SceneAPI::SceneBuilder::ImportContextProvider* m_provider = nullptr;
    };
} // namespace UniversalSceneImporter
