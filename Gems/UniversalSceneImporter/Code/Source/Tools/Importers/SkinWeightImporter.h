/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/std/containers/vector.h>
#include <AzCore/std/smart_ptr/shared_ptr.h>
#include <SceneAPI/SceneCore/Components/LoadingComponent.h>
#include <SceneAPI/SceneData/GraphData/SkinWeightData.h>

#include "../Contexts/UniversalImportContexts.h"

namespace UniversalSceneImporter
{
    //! Creates SkinWeightData for skinned meshes.
    //!
    //! Bone ids in SkinWeightData are bone names resolved against the final scene graph names, so
    //! the links are filled during the finalize phase once every node has been renamed.
    class SkinWeightImporter : public AZ::SceneAPI::SceneCore::LoadingComponent
    {
    public:
        AZ_COMPONENT(SkinWeightImporter, "{037D8B1C-0720-472D-8C4E-8B4A106E229B}", AZ::SceneAPI::SceneCore::LoadingComponent);

        SkinWeightImporter();
        ~SkinWeightImporter() override = default;

        static void Reflect(AZ::ReflectContext* context);

        AZ::SceneAPI::Events::ProcessingResult ImportSkinWeights(UniversalSceneNodeAppendedContext& context);
        AZ::SceneAPI::Events::ProcessingResult FinalizeSkinWeights(UniversalFinalizeSceneContext& context);

        static constexpr const char* s_skinWeightName = "SkinWeight_0";

    private:
        struct PendingSkinWeights
        {
            const Scene* m_scene = nullptr;
            int m_nodeIndex = -1;
            AZStd::shared_ptr<AZ::SceneData::GraphData::SkinWeightData> m_data;
        };

        AZStd::vector<PendingSkinWeights> m_pending;
    };

} // namespace UniversalSceneImporter
