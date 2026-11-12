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
    //! Creates one BlendShapeData node per morph target of a mesh.
    class BlendShapeImporter : public AZ::SceneAPI::SceneCore::LoadingComponent
    {
    public:
        AZ_COMPONENT(BlendShapeImporter, "{CB6B1B35-E77C-481E-AD05-14C80AEB1143}", AZ::SceneAPI::SceneCore::LoadingComponent);

        BlendShapeImporter();
        ~BlendShapeImporter() override = default;

        static void Reflect(AZ::ReflectContext* context);
        AZ::SceneAPI::Events::ProcessingResult ImportBlendShapes(UniversalSceneNodeAppendedContext& context);
    };

} // namespace UniversalSceneImporter
