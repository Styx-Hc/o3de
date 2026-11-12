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
    //! Adds the local transform of a node to the scene graph as a TransformData child (or as the
    //! node content when the node has no other data).
    class TransformImporter : public AZ::SceneAPI::SceneCore::LoadingComponent
    {
    public:
        AZ_COMPONENT(TransformImporter, "{0A32FB62-D565-4796-A7F5-DC341698EA12}", AZ::SceneAPI::SceneCore::LoadingComponent);

        TransformImporter();
        ~TransformImporter() override = default;

        static void Reflect(AZ::ReflectContext* context);
        AZ::SceneAPI::Events::ProcessingResult ImportTransform(UniversalSceneNodeAppendedContext& context);

        static constexpr const char* s_transformNodeName = "transform";
    };

    //! Creates BoneData / RootBoneData for nodes that are part of a skeleton.
    class BoneImporter : public AZ::SceneAPI::SceneCore::LoadingComponent
    {
    public:
        AZ_COMPONENT(BoneImporter, "{23137C95-F73B-45E4-9C68-7201AA031DC8}", AZ::SceneAPI::SceneCore::LoadingComponent);

        BoneImporter();
        ~BoneImporter() override = default;

        static void Reflect(AZ::ReflectContext* context);
        AZ::SceneAPI::Events::ProcessingResult ImportBone(UniversalNodeEncounteredContext& context);
    };

    //! Creates MeshData for static meshes.
    class MeshImporter : public AZ::SceneAPI::SceneCore::LoadingComponent
    {
    public:
        AZ_COMPONENT(MeshImporter, "{DCE7A585-F191-4DF7-983A-CCC68D999A2F}", AZ::SceneAPI::SceneCore::LoadingComponent);

        MeshImporter();
        ~MeshImporter() override = default;

        static void Reflect(AZ::ReflectContext* context);
        AZ::SceneAPI::Events::ProcessingResult ImportMesh(UniversalNodeEncounteredContext& context);
    };

    //! Creates SkinMeshData for skinned meshes.
    class SkinMeshImporter : public AZ::SceneAPI::SceneCore::LoadingComponent
    {
    public:
        AZ_COMPONENT(SkinMeshImporter, "{9AE1A114-596A-4379-864D-E27D5E9804AF}", AZ::SceneAPI::SceneCore::LoadingComponent);

        SkinMeshImporter();
        ~SkinMeshImporter() override = default;

        static void Reflect(AZ::ReflectContext* context);
        AZ::SceneAPI::Events::ProcessingResult ImportSkinMesh(UniversalNodeEncounteredContext& context);
    };

} // namespace UniversalSceneImporter
