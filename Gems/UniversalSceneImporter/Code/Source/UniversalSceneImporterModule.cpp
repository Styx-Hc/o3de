/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <AzCore/Module/Module.h>

#include "Importers/AnimationImporter.h"
#include "Importers/AttributeImporters.h"
#include "Importers/BlendShapeImporter.h"
#include "Importers/MeshImporters.h"
#include "Importers/SkinWeightImporter.h"
#include "UniversalSceneImporterComponent.h"

namespace UniversalSceneImporter
{
    //! Tool module for the UniversalSceneImporter gem.
    //!
    //! The module registers the scene system component that advertises the import context provider
    //! and every SceneAPI loading component used to translate the parsed scene into the scene graph.
    class UniversalSceneImporterModule : public AZ::Module
    {
    public:
        AZ_RTTI(UniversalSceneImporterModule, "{4FA192AF-E414-47EB-A7C5-5D6A46C49C16}", AZ::Module);

        UniversalSceneImporterModule()
            : Module()
        {
            m_descriptors.insert(
                m_descriptors.end(),
                {
                    UniversalSceneImporterComponent::CreateDescriptor(),

                    // Node level importers.
                    BoneImporter::CreateDescriptor(),
                    MeshImporter::CreateDescriptor(),
                    SkinMeshImporter::CreateDescriptor(),

                    // Attribute importers.
                    AnimationImporter::CreateDescriptor(),
                    BitangentStreamImporter::CreateDescriptor(),
                    BlendShapeImporter::CreateDescriptor(),
                    ColorStreamImporter::CreateDescriptor(),
                    MaterialImporter::CreateDescriptor(),
                    SkinWeightImporter::CreateDescriptor(),
                    TangentStreamImporter::CreateDescriptor(),
                    TransformImporter::CreateDescriptor(),
                    UvMapImporter::CreateDescriptor(),
                });
        }
    };
} // namespace UniversalSceneImporter

#if defined(O3DE_GEM_NAME)
AZ_DECLARE_MODULE_CLASS(AZ_JOIN(Gem_, O3DE_GEM_NAME, _Editor), UniversalSceneImporter::UniversalSceneImporterModule)
#else
AZ_DECLARE_MODULE_CLASS(Gem_UniversalSceneImporter_Editor, UniversalSceneImporter::UniversalSceneImporterModule)
#endif
