/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/std/string/string.h>

#include "../Scene/UniversalScene.h"

namespace UniversalSceneImporter
{
    //! Reads a .gltf or .glb file with cgltf and fills a format-neutral scene.
    //! All geometry and transforms are converted to O3DE's Z-up, meters space.
    //! Returns false and fills outError when the file cannot be imported.
    bool ConvertGltfScene(const char* fileName, Scene& outScene, AZStd::string& outError);

} // namespace UniversalSceneImporter
