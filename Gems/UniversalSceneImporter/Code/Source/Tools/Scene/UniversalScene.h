/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Math/Aabb.h>
#include <AzCore/Math/Matrix3x4.h>
#include <AzCore/Math/Vector2.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/Vector4.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/string.h>

namespace UniversalSceneImporter
{
    //! Maximum number of texture coordinate sets that O3DE's material and vertex pipelines consume.
    //! Additional sets in the source file are dropped with a warning.
    constexpr size_t MaxUvSets = 4;
    //! Maximum number of vertex color sets that O3DE consumes. Additional sets are dropped with a warning.
    constexpr size_t MaxColorSets = 4;
    //! Number of skin influences kept per vertex before the configured skin rule is applied.
    //! The SceneAPI skin rule defaults to 8; keeping the same upper bound avoids silently dropping
    //! influences that the rule would have kept.
    constexpr size_t MaxSkinInfluences = 8;

    enum class SourceFormat : AZ::u8
    {
        Unknown = 0,
        Fbx,
        Gltf
    };

    const char* ToString(SourceFormat format);

    //! Texture slots that can be filled from a source scene. The order is not significant.
    enum class MaterialTexture : AZ::u8
    {
        BaseColor = 0,
        Diffuse,
        Specular,
        Emissive,
        Normal,
        Bump,
        Metallic,
        Roughness,
        AmbientOcclusion,
        Count
    };

    //! A texture referenced by a material.
    //! External textures keep the URI authored in the source file. Embedded textures (FBX embedded
    //! media, GLB buffer views, data URIs) keep their bytes so the material importer can write them
    //! to disk when embedded texture extraction is enabled.
    struct Texture
    {
        AZStd::string m_name;
        AZStd::string m_uri;
        AZStd::vector<AZ::u8> m_embeddedData;
        AZStd::string m_extension; //!< Lower case extension without a leading dot, used when extracting embedded data.
        //! True when the source embeds image bytes. The bytes themselves are only kept when embedded
        //! texture extraction is enabled; the flag keeps validity reporting and the
        //! "extraction is disabled" warning working when the bytes are not stored.
        bool m_hasEmbeddedData = false;

        bool IsValid() const
        {
            return !m_uri.empty() || m_hasEmbeddedData;
        }

        bool IsEmbedded() const
        {
            return m_hasEmbeddedData;
        }
    };

    struct Material
    {
        AZStd::string m_name;
        AZ::Vector3 m_baseColor = AZ::Vector3::CreateOne();
        float m_metallic = 0.0f;
        float m_roughness = 0.5f;
        AZ::Vector3 m_emissiveColor = AZ::Vector3::CreateZero();
        float m_emissiveIntensity = 1.0f;
        float m_opacity = 1.0f;
        bool m_alphaMasked = false;
        float m_alphaCutoff = 0.5f;
        //! Legacy (non-PBR) values that are still consumed by some asset pipelines.
        AZ::Vector3 m_diffuseColor = AZ::Vector3::CreateOne();
        AZ::Vector3 m_specularColor = AZ::Vector3::CreateZero();
        float m_shininess = 0.0f;

        Texture m_textures[static_cast<size_t>(MaterialTexture::Count)];

        AZ::u64 m_uniqueId = 0;

        Texture& GetTexture(MaterialTexture texture)
        {
            return m_textures[static_cast<size_t>(texture)];
        }

        const Texture& GetTexture(MaterialTexture texture) const
        {
            return m_textures[static_cast<size_t>(texture)];
        }
    };

    struct SkinInfluence
    {
        //! Index into Scene::m_nodes of the joint node. The skin weight importer converts this to a
        //! bone name so the engine can resolve it against the skeleton.
        AZ::u32 m_joint = 0;
        float m_weight = 0.0f;
    };

    //! Morph target deltas for a single primitive. Deltas are parallel to the primitive vertex arrays.
    struct MorphTarget
    {
        AZStd::string m_name;
        AZStd::vector<AZ::Vector3> m_positionDeltas;
        AZStd::vector<AZ::Vector3> m_normalDeltas;
        AZStd::vector<AZ::Vector3> m_tangentDeltas;

        bool IsEmpty() const
        {
            return m_positionDeltas.empty() && m_normalDeltas.empty() && m_tangentDeltas.empty();
        }
    };

    //! One drawable range of a mesh. All vertex attribute arrays have the same number of elements
    //! (equal to m_positions.size()) so the SceneAPI importers can emit aligned vertex streams.
    struct MeshPrimitive
    {
        //! Index into Scene::m_materials, or -1 when the primitive has no material.
        int m_material = -1;
        AZStd::vector<AZ::Vector3> m_positions;
        AZStd::vector<AZ::Vector3> m_normals;
        AZStd::vector<AZ::Vector2> m_uvSets[MaxUvSets];
        AZStd::vector<AZ::Vector4> m_colorSets[MaxColorSets];
        //! Tangent (xyz) plus handedness sign (w).
        AZStd::vector<AZ::Vector4> m_tangents;
        AZStd::vector<AZ::Vector3> m_bitangents;
        //! Triangle list. Every value is a valid index into the vertex arrays above.
        AZStd::vector<AZ::u32> m_indices;
        //! Per-vertex skin influences. Empty when the primitive is not skinned.
        AZStd::vector<AZStd::vector<SkinInfluence>> m_skinInfluences;
        AZStd::vector<MorphTarget> m_morphTargets;

        size_t GetVertexCount() const
        {
            return m_positions.size();
        }

        bool IsSkinned() const
        {
            return !m_skinInfluences.empty();
        }

        bool HasTangents() const
        {
            return m_tangents.size() == m_positions.size() && !m_positions.empty();
        }

        bool HasBitangents() const
        {
            return m_bitangents.size() == m_positions.size() && !m_positions.empty();
        }
    };

    struct Mesh
    {
        AZStd::string m_name;
        AZStd::vector<MeshPrimitive> m_primitives;

        bool IsSkinned() const
        {
            for (const MeshPrimitive& primitive : m_primitives)
            {
                if (primitive.IsSkinned())
                {
                    return true;
                }
            }
            return false;
        }

        bool HasTangents() const
        {
            for (const MeshPrimitive& primitive : m_primitives)
            {
                if (primitive.HasTangents())
                {
                    return true;
                }
            }
            return false;
        }

        size_t GetVertexCount() const
        {
            size_t count = 0;
            for (const MeshPrimitive& primitive : m_primitives)
            {
                count += primitive.GetVertexCount();
            }
            return count;
        }

        size_t GetMaxUvSetCount() const
        {
            size_t count = 0;
            for (const MeshPrimitive& primitive : m_primitives)
            {
                for (size_t index = 0; index < MaxUvSets; ++index)
                {
                    if (!primitive.m_uvSets[index].empty())
                    {
                        count = AZStd::max(count, index + 1);
                    }
                }
            }
            return count;
        }

        size_t GetMaxColorSetCount() const
        {
            size_t count = 0;
            for (const MeshPrimitive& primitive : m_primitives)
            {
                for (size_t index = 0; index < MaxColorSets; ++index)
                {
                    if (!primitive.m_colorSets[index].empty())
                    {
                        count = AZStd::max(count, index + 1);
                    }
                }
            }
            return count;
        }

        size_t GetMorphTargetCount() const
        {
            size_t count = 0;
            for (const MeshPrimitive& primitive : m_primitives)
            {
                count = AZStd::max(count, primitive.m_morphTargets.size());
            }
            return count;
        }
    };

    struct Node
    {
        AZStd::string m_name;
        int m_parent = -1;
        AZStd::vector<int> m_children;
        AZ::Matrix3x4 m_localTransform = AZ::Matrix3x4::CreateIdentity();
        //! Index into Scene::m_meshes, or -1 when the node has no mesh.
        int m_mesh = -1;
        //! Index into Scene::m_skins, or -1 when the node is not skinned.
        int m_skin = -1;
        //! True when the node is part of a skeleton (a joint or an ancestor of a joint).
        bool m_isBone = false;
        bool m_isRoot = false;
        //! Bone nodes: the bind pose local transform (set by Scene::Finalize), while
        //! m_localTransform stays the default pose that animation tracks are relative to.
        //! Skinned mesh nodes: the local transform the scene graph must carry for this
        //! node so its graph world equals the shared bind world of the skin. The model builder
        //! bakes the graph world into the vertices exactly once, so this override is what makes
        //! the exported vertices land in the bind space the skeleton's inverse binds expect.
        //! Set by the converters from the authored cluster binds; when absent the transform
        //! importer keeps the node transformless (vertices carry their own world placement).
        bool m_hasLocalOverride = false;
        AZ::Matrix3x4 m_localOverride = AZ::Matrix3x4::CreateIdentity();
    };

    struct Skin
    {
        //! Joint node indices, in the order used by the primitive skin influence arrays.
        AZStd::vector<int> m_joints;
        //! Optional authored inverse bind matrices, independent of the default node pose.
        AZStd::vector<AZ::Matrix3x4> m_inverseBindMatrices;
    };

    struct NodeAnimation
    {
        int m_node = -1;
        //! Sampled local transforms, starting at t=0, spaced by Animation::m_timeStep seconds.
        AZStd::vector<AZ::Matrix3x4> m_keyFrames;
    };

    struct MorphAnimation
    {
        int m_node = -1;
        int m_mesh = -1;
        int m_primitive = -1;
        int m_morphTarget = -1;
        //! Name of the blend shape channel, used to match the BlendShapeData node.
        AZStd::string m_blendShapeName;
        AZStd::vector<float> m_keyFrames;
    };

    struct Animation
    {
        AZStd::string m_name;
        float m_timeStep = 1.0f / 30.0f;
        AZStd::vector<NodeAnimation> m_nodeAnimations;
        AZStd::vector<MorphAnimation> m_morphAnimations;
    };

    //! Format-neutral scene description produced by the FBX and glTF converters.
    //!
    //! All coordinates are already converted to O3DE's Z-up, right handed space and all
    //! distances are in meters. Converters are responsible for that conversion so the
    //! SceneAPI importers never have to deal with source format axis conventions.
    class Scene
    {
    public:
        Scene();
        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;
        Scene(Scene&&) = default;
        Scene& operator=(Scene&&) = default;
        ~Scene() = default;

        void Clear();

        bool IsEmpty() const
        {
            return m_nodes.empty();
        }

        SourceFormat GetFormat() const
        {
            return m_format;
        }
        void SetFormat(SourceFormat format)
        {
            m_format = format;
        }

        const AZStd::string& GetSourcePath() const
        {
            return m_sourcePath;
        }
        void SetSourcePath(AZStd::string sourcePath)
        {
            m_sourcePath = AZStd::move(sourcePath);
        }

        //! Mirrors the import setting that controls whether embedded textures are written to disk.
        bool GetExtractEmbeddedTextures() const
        {
            return m_extractEmbeddedTextures;
        }
        void SetExtractEmbeddedTextures(bool extract)
        {
            m_extractEmbeddedTextures = extract;
        }

        AZ::u64 GetId() const
        {
            return m_id;
        }

        AZStd::vector<Node>& GetNodes()
        {
            return m_nodes;
        }
        const AZStd::vector<Node>& GetNodes() const
        {
            return m_nodes;
        }
        AZStd::vector<Mesh>& GetMeshes()
        {
            return m_meshes;
        }
        const AZStd::vector<Mesh>& GetMeshes() const
        {
            return m_meshes;
        }
        AZStd::vector<Material>& GetMaterials()
        {
            return m_materials;
        }
        const AZStd::vector<Material>& GetMaterials() const
        {
            return m_materials;
        }
        AZStd::vector<Skin>& GetSkins()
        {
            return m_skins;
        }
        const AZStd::vector<Skin>& GetSkins() const
        {
            return m_skins;
        }
        AZStd::vector<Animation>& GetAnimations()
        {
            return m_animations;
        }
        const AZStd::vector<Animation>& GetAnimations() const
        {
            return m_animations;
        }

        const Node* GetNode(int index) const;
        Node* GetNode(int index);
        const Mesh* GetMesh(int index) const;
        Mesh* GetMesh(int index);
        const Material* GetMaterial(int index) const;
        Material* GetMaterial(int index);

        //! World transform of a node, computed by walking the node hierarchy.
        AZ::Matrix3x4 GetNodeWorldTransform(int nodeIndex) const;
        AZ::Matrix3x4 GetNodeBindWorldTransform(int nodeIndex) const;
        //! World transform of a node as the scene graph carries it: like GetNodeWorldTransform,
        //! but using the local overrides (bind pose of bones, bind space of skinned meshes).
        AZ::Matrix3x4 GetNodeGraphWorldTransform(int nodeIndex) const;
        //! Graph world transform of the parent of the top most bone at or above the given bone.
        //! EMotionFX evaluates a skeleton relative to that transform.
        AZ::Matrix3x4 GetSkeletonParentWorldTransform(int nodeIndex) const;
        //! Bind transform of a bone in the space EMotionFX evaluates the skeleton in: the
        //! accumulated bind pose locals from the top most bone down to the given bone.
        AZ::Matrix3x4 GetNodeSkeletonSpaceTransform(int nodeIndex) const;
        //! True when the node is the given node or one of its ancestors is.
        bool IsNodeOrAncestorBone(int nodeIndex) const;

        //! Computes the scene AABB and total vertex count. Call after the converters populated the scene.
        void CalculateBounds();
        const AZ::Aabb& GetAabb() const
        {
            return m_aabb;
        }
        AZ::u32 GetVertexCount() const
        {
            return m_vertexCount;
        }

        //! Assigns stable ids, fills in missing root information and drops invalid references.
        //! Returns false if the scene is unusable (for example no nodes or no geometry).
        bool Finalize();

        //! Logs a summary of the imported scene. Used by the wrapper after loading.
        void LogSummary(const char* context) const;

    private:
        void CalculateNodeBounds(int nodeIndex, AZ::Vector3& min, AZ::Vector3& max) const;
        //! Bind transform of a joint in scene space, from the authored inverse bind matrices.
        bool TryGetAuthoredJointBind(int nodeIndex, AZ::Matrix3x4& outJointBind) const;
        //! Moves a unit scale shared by the inverse binds of a skin into its vertices. Returns the
        //! scale to apply to the vertices of each skin.
        AZStd::vector<float> NormalizeSkinScales();
        //! Bakes the scales returned by NormalizeSkinScales into the vertex positions and the
        //! morph target position deltas of the meshes bound to each skin.
        void ApplySkinVertexScales(const AZStd::vector<float>& skinVertexScales);
        //! Makes the bone nodes carry the authored bind pose as their graph transform.
        void BakeBindPoseIntoGraph();

        AZ::u64 m_id = 0;
        SourceFormat m_format = SourceFormat::Unknown;
        AZStd::string m_sourcePath;
        bool m_extractEmbeddedTextures = false;
        AZStd::vector<Node> m_nodes;
        AZStd::vector<Mesh> m_meshes;
        AZStd::vector<Material> m_materials;
        AZStd::vector<Skin> m_skins;
        AZStd::vector<Animation> m_animations;
        AZ::Aabb m_aabb = AZ::Aabb::CreateNull();
        AZ::u32 m_vertexCount = 0;
    };

} // namespace UniversalSceneImporter
