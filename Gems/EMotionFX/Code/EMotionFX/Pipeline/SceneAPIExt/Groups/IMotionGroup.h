#pragma once

/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <SceneAPI/SceneCore/DataTypes/Groups/IGroup.h>
#include <AzCore/RTTI/RTTI.h>

namespace EMotionFX
{
    namespace Pipeline
    {
        namespace Group
        {
            class IMotionGroup
                : public AZ::SceneAPI::DataTypes::IGroup
            {
            public:
                AZ_RTTI(IMotionGroup, "{1CA400A8-2C3E-423D-B8A3-C457EF88E533}", AZ::SceneAPI::DataTypes::IGroup);

                ~IMotionGroup() override = default;

                // Ability to specify root bone can be useful if there are multiple skeletons 
                // stored in a scene file or if the user wants to override the root bone automatically
                // selected by the code.
                virtual const AZStd::string& GetSelectedRootBone() const = 0;

                virtual void SetSelectedRootBone(const AZStd::string& selectedRootBone) = 0;

                // Ability to select which animation data set of the scene this motion group exports.
                // Importers that support multiple animation clips per file name their animation
                // data nodes after the clip; a motion group with a non-empty selected animation
                // name exports only the animation data nodes with that name. When empty, the
                // first animation data node found on each bone is used (single-clip behavior).
                virtual const AZStd::string& GetSelectedAnimationName() const = 0;

                virtual void SetSelectedAnimationName(const AZStd::string& selectedAnimationName) = 0;
            };
        }
    }
}
