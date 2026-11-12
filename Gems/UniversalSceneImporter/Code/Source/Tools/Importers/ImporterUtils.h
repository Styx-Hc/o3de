/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/smart_ptr/shared_ptr.h>
#include <AzCore/std/string/string.h>
#include <SceneAPI/SceneCore/Components/LoadingComponent.h>

#include "../Scene/UniversalScene.h"
#include "../Contexts/UniversalImportContexts.h"

namespace AZ
{
    namespace SceneAPI
    {
        namespace SceneBuilder
        {
            class RenamedNodesMap;
        }
        namespace DataTypes
        {
            class IMeshData;
        }
    } // namespace SceneAPI
    namespace SceneData
    {
        namespace GraphData
        {
            class MeshData;
            class MeshVertexBitangentData;
            class MeshVertexColorData;
            class MeshVertexTangentData;
            class MeshVertexUVData;
        }
    } // namespace SceneData
} // namespace AZ

namespace UniversalSceneImporter
{
    //! Shared reflection for the gem's loading components. Kept in one place because unity builds
    //! concatenate the importer translation units.
    template<typename ComponentType>
    inline void ReflectLoadingComponent(AZ::ReflectContext* context)
    {
        if (auto* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<ComponentType, AZ::SceneAPI::SceneCore::LoadingComponent>()->Version(5);
        }
    }

    //! Returns the unique material indices used by a mesh in the order they are first used.
    //! Face material ids and the material nodes added under the mesh node are both derived from
    //! this list, which keeps them in sync.
    AZStd::vector<int> BuildMaterialSlots(const Mesh& mesh);

    //! Returns the index of a scene material in the slot list, or -1 when it is not present.
    int FindMaterialSlot(const AZStd::vector<int>& slots, int materialIndex);

    //! Adds an attribute node (UV set, color set, tangents, material, skin weights, ...) as a
    //! child of the current node and runs the attribute contexts for it. When fallbackPrefix is
    //! given it is used to build a unique name if the requested one is taken.
    AZ::SceneAPI::Events::ProcessingResult AddAttributeNode(
        UniversalSceneNodeAppendedContext& context,
        AZStd::shared_ptr<AZ::SceneAPI::DataTypes::IGraphObject> data,
        const AZStd::string& name,
        const char* fallbackPrefix = nullptr);

    //! Builds a MeshData (or a SkinMeshData when skinned is true) with all primitives
    //! concatenated in order. Vertex order matches all attribute stream builders below.
    AZStd::shared_ptr<AZ::SceneData::GraphData::MeshData> BuildMeshData(const Mesh& mesh, bool skinned);

    AZStd::shared_ptr<AZ::SceneData::GraphData::MeshVertexUVData> BuildUvSetData(const Mesh& mesh, size_t setIndex);
    AZStd::shared_ptr<AZ::SceneData::GraphData::MeshVertexColorData> BuildColorSetData(const Mesh& mesh, size_t setIndex);
    AZStd::shared_ptr<AZ::SceneData::GraphData::MeshVertexTangentData> BuildTangentData(const Mesh& mesh);
    AZStd::shared_ptr<AZ::SceneData::GraphData::MeshVertexBitangentData> BuildBitangentData(const Mesh& mesh);

    //! Name used for a material attribute node.
    AZStd::string GetMaterialSlotName(const Material& material, size_t slotIndex);

    //! Removes characters that are not safe in file names.
    AZStd::string SanitizeFileName(AZStd::string_view name);

    //! Returns the final scene graph name of a node, including the renaming that the SceneImporter
    //! applies after consulting the RenamedNodesMap.
    AZStd::string GetFinalNodeName(
        const AZ::SceneAPI::SceneBuilder::RenamedNodesMap& renamedNodesMap, const Scene& scene, int nodeIndex);

} // namespace UniversalSceneImporter
