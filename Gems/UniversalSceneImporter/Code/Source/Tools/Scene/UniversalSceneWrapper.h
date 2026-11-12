/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <SceneAPI/SDKWrapper/NodeWrapper.h>
#include <SceneAPI/SDKWrapper/SceneWrapper.h>
#include <SceneAPI/SceneCore/Import/SceneImportSettings.h>

#include "UniversalScene.h"

namespace UniversalSceneImporter
{
    class UniversalSceneWrapper;

    //! NodeWrapper implementation over the format-neutral scene.
    //!
    //! A node wrapper is a lightweight, non-owning view of a node in the UniversalScene. It can be
    //! created cheaply at any time; GetUniqueId() is stable for the same node across instances, which
    //! is what SceneAPI's RenamedNodesMap and skin weight resolution rely on.
    class UniversalNodeWrapper : public AZ::SDKNode::NodeWrapper
    {
    public:
        AZ_RTTI(UniversalNodeWrapper, "{A4A92F5C-5D52-4B57-9C23-30E3B4DCCD31}", AZ::SDKNode::NodeWrapper);

        UniversalNodeWrapper(const Scene* scene, int nodeIndex);
        ~UniversalNodeWrapper() override = default;

        const char* GetName() const override;
        AZ::u64 GetUniqueId() const override;
        int GetMaterialCount() const override;
        int GetChildCount() const override;
        const std::shared_ptr<AZ::SDKNode::NodeWrapper> GetChild(int childIndex) const override;

        int GetNodeIndex() const
        {
            return m_nodeIndex;
        }
        const Node* GetNode() const;
        const Mesh* GetMesh() const;
        bool IsBone() const;
        bool IsRootNode() const;
        const Scene* GetScene() const
        {
            return m_scene;
        }

    private:
        const Scene* m_scene = nullptr;
        int m_nodeIndex = -1;
    };

    //! SceneWrapperBase implementation that owns a UniversalScene and loads it with either the
    //! ufbx or cgltf based converter.
    class UniversalSceneWrapper : public AZ::SDKScene::SceneWrapperBase
    {
    public:
        AZ_RTTI(UniversalSceneWrapper, "{4C1B4A0E-5B1E-4E4E-9A5A-0E6E9F0B7A21}", AZ::SDKScene::SceneWrapperBase);

        UniversalSceneWrapper();
        ~UniversalSceneWrapper() override;

        bool LoadSceneFromFile(const char* fileName, const AZ::SceneAPI::SceneImportSettings& importSettings) override;
        const std::shared_ptr<AZ::SDKNode::NodeWrapper> GetRootNode() const override;
        std::shared_ptr<AZ::SDKNode::NodeWrapper> GetRootNode() override;
        void Clear() override;

        AZStd::pair<AxisVector, int32_t> GetUpVectorAndSign() const override;
        AZStd::pair<AxisVector, int32_t> GetFrontVectorAndSign() const override;
        AZStd::pair<AxisVector, int32_t> GetRightVectorAndSign() const override;
        AZStd::optional<AZ::SceneAPI::DataTypes::MatrixType> UseForcedRootTransform() const override;

        float GetUnitSizeInMeters() const override;
        AZ::Aabb GetAABB() const override;
        AZ::u32 GetVerticesCount() const override;

        const Scene& GetScene() const
        {
            return m_scene;
        }
        Scene& GetScene()
        {
            return m_scene;
        }
        AZStd::string GetSceneDirectory() const;

        //! Loads a scene from a file with an explicitly selected format. Used by tests and by
        //! LoadSceneFromFile after detecting the format.
        bool LoadScene(const char* fileName, SourceFormat format, const AZ::SceneAPI::SceneImportSettings& importSettings);

        //! Detects the format of a scene file using the extension first and the file content as a
        //! fallback. Returns SourceFormat::Unknown for unsupported files.
        static SourceFormat DetectFormat(const char* fileName);

    private:
        Scene m_scene;
    };

} // namespace UniversalSceneImporter
