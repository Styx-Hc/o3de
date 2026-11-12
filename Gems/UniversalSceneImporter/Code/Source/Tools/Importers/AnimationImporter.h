/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <SceneAPI/SceneCore/Components/LoadingComponent.h>

#include "../Contexts/UniversalImportContexts.h"

namespace UniversalSceneImporter
{
    //! Creates AnimationData nodes for animated bones and BlendShapeAnimationData nodes for
    //! animated morph channels.
    class AnimationImporter : public AZ::SceneAPI::SceneCore::LoadingComponent
    {
    public:
        AZ_COMPONENT(AnimationImporter, "{C6FCFBC5-8F62-46DA-AE93-30FC1A564666}", AZ::SceneAPI::SceneCore::LoadingComponent);

        AnimationImporter();
        ~AnimationImporter() override = default;

        static void Reflect(AZ::ReflectContext* context);
        AZ::SceneAPI::Events::ProcessingResult ImportAnimation(UniversalSceneNodeAppendedContext& context);
    };

} // namespace UniversalSceneImporter
