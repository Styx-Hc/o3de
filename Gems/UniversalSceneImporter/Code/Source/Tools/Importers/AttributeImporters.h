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
    //! Adds one MeshVertexUVData node per texture coordinate set.
    class UvMapImporter : public AZ::SceneAPI::SceneCore::LoadingComponent
    {
    public:
        AZ_COMPONENT(UvMapImporter, "{17EA3119-0B70-48D0-97E1-621A2F98441A}", AZ::SceneAPI::SceneCore::LoadingComponent);

        UvMapImporter();
        ~UvMapImporter() override = default;

        static void Reflect(AZ::ReflectContext* context);
        AZ::SceneAPI::Events::ProcessingResult ImportUvMaps(UniversalSceneNodeAppendedContext& context);
    };

    //! Adds one MeshVertexColorData node per vertex color set.
    class ColorStreamImporter : public AZ::SceneAPI::SceneCore::LoadingComponent
    {
    public:
        AZ_COMPONENT(ColorStreamImporter, "{E62D6B43-E1C7-42B9-87F9-779D02572639}", AZ::SceneAPI::SceneCore::LoadingComponent);

        ColorStreamImporter();
        ~ColorStreamImporter() override = default;

        static void Reflect(AZ::ReflectContext* context);
        AZ::SceneAPI::Events::ProcessingResult ImportColorStreams(UniversalSceneNodeAppendedContext& context);
    };

    //! Adds the MeshVertexTangentData node for meshes that have tangents.
    class TangentStreamImporter : public AZ::SceneAPI::SceneCore::LoadingComponent
    {
    public:
        AZ_COMPONENT(TangentStreamImporter, "{95E1799B-6CE1-45F9-BCC9-699CFCF0F34C}", AZ::SceneAPI::SceneCore::LoadingComponent);

        TangentStreamImporter();
        ~TangentStreamImporter() override = default;

        static void Reflect(AZ::ReflectContext* context);
        AZ::SceneAPI::Events::ProcessingResult ImportTangentStreams(UniversalSceneNodeAppendedContext& context);
    };

    //! Adds the MeshVertexBitangentData node for meshes that have bitangents.
    class BitangentStreamImporter : public AZ::SceneAPI::SceneCore::LoadingComponent
    {
    public:
        AZ_COMPONENT(BitangentStreamImporter, "{4B9EDD98-4981-4107-B804-6781BBBEE13F}", AZ::SceneAPI::SceneCore::LoadingComponent);

        BitangentStreamImporter();
        ~BitangentStreamImporter() override = default;

        static void Reflect(AZ::ReflectContext* context);
        AZ::SceneAPI::Events::ProcessingResult ImportBitangentStreams(UniversalSceneNodeAppendedContext& context);
    };

    //! Adds one MaterialData node per material slot used by a mesh.
    class MaterialImporter : public AZ::SceneAPI::SceneCore::LoadingComponent
    {
    public:
        AZ_COMPONENT(MaterialImporter, "{EADBDAA1-019F-4294-B35A-58BE7D743437}", AZ::SceneAPI::SceneCore::LoadingComponent);

        MaterialImporter();
        ~MaterialImporter() override = default;

        static void Reflect(AZ::ReflectContext* context);
        AZ::SceneAPI::Events::ProcessingResult ImportMaterials(UniversalSceneNodeAppendedContext& context);
    };

} // namespace UniversalSceneImporter
