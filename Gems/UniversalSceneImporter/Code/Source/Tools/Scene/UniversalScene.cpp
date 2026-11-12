/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "UniversalScene.h"
#include "../Converters/SceneConverterUtils.h"

#include <AzCore/Debug/Trace.h>
#include <AzCore/Math/MathUtils.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/sort.h>
#include <AzCore/std/containers/unordered_set.h>
#include <AzCore/std/limits.h>
#include <AzCore/std/parallel/atomic.h>
#include <SceneAPI/SceneCore/Utilities/Reporting.h>

#include <cmath>

namespace UniversalSceneImporter
{
    namespace
    {
        AZStd::atomic<AZ::u64> s_nextSceneId{ 1 };

        constexpr int MaxHierarchyDepth = 4096;
    } // namespace

    const char* ToString(SourceFormat format)
    {
        switch (format)
        {
        case SourceFormat::Fbx:
            return "FBX";
        case SourceFormat::Gltf:
            return "glTF";
        default:
            return "Unknown";
        }
    }

    Scene::Scene()
        : m_id(s_nextSceneId.fetch_add(1))
    {
    }

    void Scene::Clear()
    {
        m_format = SourceFormat::Unknown;
        m_sourcePath.clear();
        m_extractEmbeddedTextures = false;
        m_nodes.clear();
        m_meshes.clear();
        m_materials.clear();
        m_skins.clear();
        m_animations.clear();
        m_aabb = AZ::Aabb::CreateNull();
        m_vertexCount = 0;
    }

    const Node* Scene::GetNode(int index) const
    {
        return (index >= 0 && static_cast<size_t>(index) < m_nodes.size()) ? &m_nodes[index] : nullptr;
    }

    Node* Scene::GetNode(int index)
    {
        return (index >= 0 && static_cast<size_t>(index) < m_nodes.size()) ? &m_nodes[index] : nullptr;
    }

    const Mesh* Scene::GetMesh(int index) const
    {
        return (index >= 0 && static_cast<size_t>(index) < m_meshes.size()) ? &m_meshes[index] : nullptr;
    }

    Mesh* Scene::GetMesh(int index)
    {
        return (index >= 0 && static_cast<size_t>(index) < m_meshes.size()) ? &m_meshes[index] : nullptr;
    }

    const Material* Scene::GetMaterial(int index) const
    {
        return (index >= 0 && static_cast<size_t>(index) < m_materials.size()) ? &m_materials[index] : nullptr;
    }

    Material* Scene::GetMaterial(int index)
    {
        return (index >= 0 && static_cast<size_t>(index) < m_materials.size()) ? &m_materials[index] : nullptr;
    }

    AZ::Matrix3x4 Scene::GetNodeWorldTransform(int nodeIndex) const
    {
        AZ::Matrix3x4 result = AZ::Matrix3x4::CreateIdentity();
        int current = nodeIndex;
        int depth = 0;
        while (current >= 0 && static_cast<size_t>(current) < m_nodes.size() && depth++ < MaxHierarchyDepth)
        {
            result = m_nodes[current].m_localTransform * result;
            current = m_nodes[current].m_parent;
        }
        return result;
    }

    AZ::Matrix3x4 Scene::GetNodeGraphWorldTransform(int nodeIndex) const
    {
        AZ::Matrix3x4 result = AZ::Matrix3x4::CreateIdentity();
        int current = nodeIndex;
        int depth = 0;
        while (current >= 0 && static_cast<size_t>(current) < m_nodes.size() && depth++ < MaxHierarchyDepth)
        {
            const Node& node = m_nodes[current];
            result = (node.m_hasLocalOverride ? node.m_localOverride : node.m_localTransform) * result;
            current = node.m_parent;
        }
        return result;
    }

    AZ::Matrix3x4 Scene::GetSkeletonParentWorldTransform(int nodeIndex) const
    {
        int top = nodeIndex;
        int depth = 0;
        while (top >= 0 && static_cast<size_t>(top) < m_nodes.size() && m_nodes[top].m_parent >= 0 &&
            m_nodes[m_nodes[top].m_parent].m_isBone && depth++ < MaxHierarchyDepth)
        {
            top = m_nodes[top].m_parent;
        }
        const Node* topNode = GetNode(top);
        return topNode ? GetNodeGraphWorldTransform(topNode->m_parent) : AZ::Matrix3x4::CreateIdentity();
    }

    AZ::Matrix3x4 Scene::GetNodeSkeletonSpaceTransform(int nodeIndex) const
    {
        const AZ::Matrix3x4 skeletonParentWorld = GetSkeletonParentWorldTransform(nodeIndex);
        const AZ::Matrix3x4 graphWorld = GetNodeGraphWorldTransform(nodeIndex);
        return Converters::IsInvertible(skeletonParentWorld) ? Converters::InvertAffine(skeletonParentWorld) * graphWorld : graphWorld;
    }

    bool Scene::TryGetAuthoredJointBind(int nodeIndex, AZ::Matrix3x4& outJointBind) const
    {
        for (size_t meshNode = 0; meshNode < m_nodes.size(); ++meshNode)
        {
            const int skinIndex = m_nodes[meshNode].m_skin;
            if (skinIndex < 0 || static_cast<size_t>(skinIndex) >= m_skins.size())
            {
                continue;
            }
            const Skin& skin = m_skins[skinIndex];
            for (size_t slot = 0; slot < skin.m_joints.size() && slot < skin.m_inverseBindMatrices.size(); ++slot)
            {
                if (skin.m_joints[slot] != nodeIndex || !Converters::IsInvertible(skin.m_inverseBindMatrices[slot]))
                {
                    continue;
                }
                const AZ::Matrix3x4 jointBind = Converters::InvertAffine(skin.m_inverseBindMatrices[slot]);
                outJointBind = m_format == SourceFormat::Gltf ? Converters::ConvertGltfTransform(jointBind) : jointBind;
                return true;
            }
        }
        return false;
    }

    AZStd::vector<float> Scene::NormalizeSkinScales()
    {
        // A skinning matrix is jointWorld * inverseBind. When the vertices of a skin are authored
        // in different units than its skeleton (for example centimeter vertices on a meter
        // skeleton), the inverse binds carry that unit scale and so does every skinning matrix.
        // Linear skinning handles that, but dual quaternion skinning (the default of the actor
        // component) can not represent scale and drops it, which tears the mesh apart. Move the
        // scale out of the inverse binds and into the vertices so the skinning matrices are rigid.
        auto uniformScale = [](const AZ::Matrix3x4& matrix)
        {
            return std::cbrt(AZ::Abs(matrix.GetDeterminant3x3()));
        };

        AZStd::vector<float> vertexScales(m_skins.size(), 1.0f);
        for (size_t skinIndex = 0; skinIndex < m_skins.size(); ++skinIndex)
        {
            Skin& skin = m_skins[skinIndex];
            if (skin.m_inverseBindMatrices.size() != skin.m_joints.size())
            {
                continue;
            }

            // The joints that actually carry weights in the meshes bound to this skin.
            AZStd::unordered_set<int> weightedJoints;
            bool canRescale = true;
            for (const Node& node : m_nodes)
            {
                if (node.m_skin != static_cast<int>(skinIndex))
                {
                    continue;
                }
                const Mesh* mesh = GetMesh(node.m_mesh);
                if (!node.m_hasLocalOverride || !mesh)
                {
                    // Without a bind space override the vertices are placed by the mesh importer.
                    canRescale = false;
                    break;
                }
                for (const MeshPrimitive& primitive : mesh->m_primitives)
                {
                    for (const AZStd::vector<SkinInfluence>& influences : primitive.m_skinInfluences)
                    {
                        for (const SkinInfluence& influence : influences)
                        {
                            if (influence.m_weight > 0.0f)
                            {
                                weightedJoints.insert(static_cast<int>(influence.m_joint));
                            }
                        }
                    }
                }
            }
            if (!canRescale || weightedJoints.empty())
            {
                continue;
            }

            AZStd::vector<float> scales;
            for (size_t slot = 0; slot < skin.m_joints.size(); ++slot)
            {
                const int joint = skin.m_joints[slot];
                if (!weightedJoints.contains(joint) || !Converters::IsInvertible(skin.m_inverseBindMatrices[slot]))
                {
                    continue;
                }
                // The skeleton is evaluated relative to the parent of its top most bone.
                int top = joint;
                int depth = 0;
                while (m_nodes[top].m_parent >= 0 && m_nodes[m_nodes[top].m_parent].m_isBone && depth++ < MaxHierarchyDepth)
                {
                    top = m_nodes[top].m_parent;
                }
                const float skeletonSpaceScale = uniformScale(GetNodeWorldTransform(m_nodes[top].m_parent));
                if (skeletonSpaceScale <= AZ::Constants::FloatEpsilon)
                {
                    continue;
                }
                scales.push_back(
                    uniformScale(GetNodeWorldTransform(joint)) / skeletonSpaceScale * uniformScale(skin.m_inverseBindMatrices[slot]));
            }
            if (scales.empty())
            {
                continue;
            }
            AZStd::sort(scales.begin(), scales.end());
            const float scale = scales[scales.size() / 2];
            if (scale <= AZ::Constants::FloatEpsilon || AZ::IsClose(scale, 1.0f, 0.001f))
            {
                continue;
            }

            vertexScales[skinIndex] = scale;
            for (AZ::Matrix3x4& inverseBind : skin.m_inverseBindMatrices)
            {
                inverseBind.MultiplyByScale(AZ::Vector3(1.0f / scale));
            }
        }
        return vertexScales;
    }

    void Scene::ApplySkinVertexScales(const AZStd::vector<float>& skinVertexScales)
    {
        // NormalizeSkinScales divided the inverse bind matrices of a skin by a unit scale; the
        // vertices of that skin have to carry that scale instead, otherwise every skinning matrix
        // inflates the mesh by its reciprocal once a motion plays. Bake it into the imported
        // vertex data so the mesh data, the blend shapes and the bounds all live in the same
        // space as the normalized inverse binds.
        AZStd::vector<bool> handled(m_meshes.size(), false);
        for (const Node& node : m_nodes)
        {
            const int meshIndex = node.m_mesh;
            const int skinIndex = node.m_skin;
            if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= m_meshes.size() || handled[meshIndex] ||
                skinIndex < 0 || static_cast<size_t>(skinIndex) >= skinVertexScales.size())
            {
                continue;
            }
            handled[meshIndex] = true;

            const float scale = skinVertexScales[skinIndex];
            if (AZ::IsClose(scale, 1.0f, AZ::Constants::FloatEpsilon))
            {
                continue;
            }

            Mesh& mesh = m_meshes[meshIndex];
            for (MeshPrimitive& primitive : mesh.m_primitives)
            {
                for (AZ::Vector3& position : primitive.m_positions)
                {
                    position *= scale;
                }
                for (MorphTarget& morphTarget : primitive.m_morphTargets)
                {
                    for (AZ::Vector3& delta : morphTarget.m_positionDeltas)
                    {
                        delta *= scale;
                    }
                }
            }
        }
    }

    void Scene::BakeBindPoseIntoGraph()
    {
        // The actor builder takes the skeleton's bind pose from the transforms the scene graph
        // carries for the bone nodes, and EMotionFX derives the inverse bind matrices from that
        // pose; the authored inverse bind matrices of the skins never reach the runtime. The
        // default node transforms of a file are frequently not the bind pose (they are often the
        // first animation frame), so the graph has to carry the authored bind pose instead:
        // otherwise the skinning matrices are relative to the wrong pose and the mesh deforms as
        // soon as a motion plays. Animation tracks are unaffected, they stay parent relative.
        const size_t nodeCount = m_nodes.size();
        const AZStd::vector<float> skinVertexScales = NormalizeSkinScales();
        ApplySkinVertexScales(skinVertexScales);
        AZStd::vector<AZ::Matrix3x4> jointBinds(nodeCount, AZ::Matrix3x4::CreateIdentity());
        AZStd::vector<bool> hasJointBind(nodeCount, false);
        bool anyJointBind = false;
        for (size_t index = 0; index < nodeCount; ++index)
        {
            if (m_nodes[index].m_isBone && TryGetAuthoredJointBind(static_cast<int>(index), jointBinds[index]))
            {
                hasJointBind[index] = true;
                anyJointBind = true;
            }
        }
        if (!anyJointBind)
        {
            return;
        }

        // Visit parents before their children.
        AZStd::vector<int> order;
        order.reserve(nodeCount);
        for (size_t index = 0; index < nodeCount; ++index)
        {
            if (m_nodes[index].m_parent < 0)
            {
                order.push_back(static_cast<int>(index));
            }
        }
        for (size_t visit = 0; visit < order.size(); ++visit)
        {
            for (int child : m_nodes[order[visit]].m_children)
            {
                order.push_back(child);
            }
        }

        const AZ::Matrix3x4 identity = AZ::Matrix3x4::CreateIdentity();
        AZStd::vector<AZ::Matrix3x4> defaultWorlds(nodeCount, identity);
        AZStd::vector<AZ::Matrix3x4> graphWorlds(nodeCount, identity);
        // Graph world of the parent of the top most bone above each bone. EMotionFX evaluates the
        // skeleton relative to its root bone's parent, while the authored binds and the baked
        // vertices are in scene space, so the bind worlds are placed below that transform.
        AZStd::vector<AZ::Matrix3x4> skeletonSpaces(nodeCount, identity);
        for (int nodeIndex : order)
        {
            Node& node = m_nodes[nodeIndex];
            const int parent = node.m_parent;
            const AZ::Matrix3x4 parentDefaultWorld = parent >= 0 ? defaultWorlds[parent] : identity;
            const AZ::Matrix3x4 parentGraphWorld = parent >= 0 ? graphWorlds[parent] : identity;
            defaultWorlds[nodeIndex] = parentDefaultWorld * node.m_localTransform;
            graphWorlds[nodeIndex] = parentGraphWorld * (node.m_hasLocalOverride ? node.m_localOverride : node.m_localTransform);

            if (node.m_isBone)
            {
                const bool isTopBone = !(parent >= 0 && m_nodes[parent].m_isBone);
                skeletonSpaces[nodeIndex] = isTopBone ? parentGraphWorld : skeletonSpaces[parent];
                if (!Converters::IsInvertible(parentGraphWorld))
                {
                    continue;
                }
                // The transforms above the skeleton are not part of what EMotionFX evaluates, so
                // the top most bone carries them itself: its skeleton space transform is its
                // scene space transform (the animation importer does the same for its tracks).
                // Without this the actor loses the placement, orientation and scale of the nodes
                // above its skeleton.
                const AZ::Matrix3x4 bindWorld = hasJointBind[nodeIndex]
                    ? skeletonSpaces[nodeIndex] * jointBinds[nodeIndex]
                    : (isTopBone ? parentGraphWorld * parentGraphWorld : parentGraphWorld) * node.m_localTransform;
                node.m_hasLocalOverride = true;
                node.m_localOverride = Converters::InvertAffine(parentGraphWorld) * bindWorld;
                graphWorlds[nodeIndex] = bindWorld;
            }
            else if (node.m_hasLocalOverride && Converters::IsInvertible(parentGraphWorld))
            {
                // Skinned mesh node: the converter computed the override against the default
                // pose of its ancestors. Keep the graph world it asked for now that the bones
                // above it carry the bind pose. The unit scale NormalizeSkinScales moved out of
                // the inverse binds is carried by the vertices themselves (see
                // ApplySkinVertexScales), never by this transform.
                const AZ::Matrix3x4 meshWorld = parentDefaultWorld * node.m_localOverride;
                node.m_localOverride = Converters::InvertAffine(parentGraphWorld) * meshWorld;
                graphWorlds[nodeIndex] = meshWorld;
            }
        }
    }

    AZ::Matrix3x4 Scene::GetNodeBindWorldTransform(int nodeIndex) const
    {
        for (size_t meshNode = 0; meshNode < m_nodes.size(); ++meshNode)
        {
            const int skinIndex = m_nodes[meshNode].m_skin;
            if (skinIndex < 0 || static_cast<size_t>(skinIndex) >= m_skins.size())
            {
                continue;
            }
            const Skin& skin = m_skins[skinIndex];
            for (size_t slot = 0; slot < skin.m_joints.size() && slot < skin.m_inverseBindMatrices.size(); ++slot)
            {
                if (skin.m_joints[slot] != nodeIndex)
                {
                    continue;
                }
                AZ::Matrix3x4 inverseBind = skin.m_inverseBindMatrices[slot];
                if (!Converters::IsInvertible(inverseBind))
                {
                    continue;
                }
                const AZ::Matrix3x4 jointBind = Converters::InvertAffine(inverseBind);
                if (m_format == SourceFormat::Gltf)
                {
                    // glTF ignores the transform of a skinned mesh node: the vertices are skinned
                    // as jointWorld * inverseBind * vertex with the vertex in mesh-local space.
                    // The converter keeps the exported vertices in that space (the skinned mesh
                    // node's graph world is identity), so the joint's bind world is the inverse
                    // of the authored inverse bind, converted to O3DE's axis convention. Adding
                    // the mesh node's world here would put the skeleton's bind space one
                    // transform away from the vertices and deform the mesh once it animates.
                    return Converters::ConvertGltfTransform(jointBind);
                }
                return jointBind;
            }
        }
        return GetNodeWorldTransform(nodeIndex);
    }

    bool Scene::IsNodeOrAncestorBone(int nodeIndex) const
    {
        int current = nodeIndex;
        int depth = 0;
        while (current >= 0 && static_cast<size_t>(current) < m_nodes.size() && depth++ < MaxHierarchyDepth)
        {
            if (m_nodes[current].m_isBone)
            {
                return true;
            }
            current = m_nodes[current].m_parent;
        }
        return false;
    }

    void Scene::CalculateNodeBounds(int nodeIndex, AZ::Vector3& min, AZ::Vector3& max) const
    {
        const Node* node = GetNode(nodeIndex);
        if (!node)
        {
            return;
        }

        const AZ::Matrix3x4 world = GetNodeWorldTransform(nodeIndex);
        if (const Mesh* mesh = GetMesh(node->m_mesh))
        {
            for (const MeshPrimitive& primitive : mesh->m_primitives)
            {
                for (const AZ::Vector3& position : primitive.m_positions)
                {
                    const AZ::Vector3 transformed = world.TransformPoint(position);
                    min = min.GetMin(transformed);
                    max = max.GetMax(transformed);
                }
            }
        }

        for (int child : node->m_children)
        {
            CalculateNodeBounds(child, min, max);
        }
    }

    void Scene::CalculateBounds()
    {
        AZ::Vector3 min(AZStd::numeric_limits<float>::max());
        AZ::Vector3 max(-AZStd::numeric_limits<float>::max());

        // Count every imported mesh once. Meshes can be instanced by multiple nodes; the vertex
        // count reported to the SceneAPI is the number of unique imported vertices.
        m_vertexCount = 0;
        for (const Mesh& mesh : m_meshes)
        {
            m_vertexCount += static_cast<AZ::u32>(mesh.GetVertexCount());
        }

        for (size_t index = 0; index < m_nodes.size(); ++index)
        {
            if (m_nodes[index].m_parent < 0)
            {
                CalculateNodeBounds(static_cast<int>(index), min, max);
            }
        }

        if (min.GetX() <= max.GetX() && min.GetY() <= max.GetY() && min.GetZ() <= max.GetZ())
        {
            m_aabb = AZ::Aabb::CreateFromMinMax(min, max);
        }
        else
        {
            m_aabb = AZ::Aabb::CreateNull();
        }
    }

    bool Scene::Finalize()
    {
        if (m_nodes.empty())
        {
            AZ_Error("UniversalSceneImporter", false, "Scene '%s' does not contain any nodes.", m_sourcePath.c_str());
            return false;
        }

        const bool hadExplicitRoot = AZStd::any_of(
            m_nodes.begin(), m_nodes.end(), [](const Node& node) { return node.m_isRoot || node.m_parent < 0; });
        if (!hadExplicitRoot)
        {
            AZ_Warning("UniversalSceneImporter", false, "Scene '%s' has no root node, node 0 will be used as the root.", m_sourcePath.c_str());
            m_nodes[0].m_parent = -1;
        }

        // Validate the hierarchy and rebuild the child lists. Rebuilding guarantees that the
        // parent/child links are consistent even when a converter produced a partially broken
        // hierarchy, and prevents infinite recursion during traversal.
        for (size_t index = 0; index < m_nodes.size(); ++index)
        {
            Node& node = m_nodes[index];
            if (node.m_parent == static_cast<int>(index))
            {
                AZ_Warning(
                    "UniversalSceneImporter", false, "Node '%s' is its own parent, it will be treated as a root node.",
                    node.m_name.c_str());
                node.m_parent = -1;
            }
            else if (node.m_parent < 0 || static_cast<size_t>(node.m_parent) >= m_nodes.size())
            {
                node.m_parent = -1;
            }

            if (node.m_name.empty())
            {
                node.m_name = AZStd::string::format("Node_%zu", index);
            }

            node.m_children.clear();
        }

        for (size_t index = 0; index < m_nodes.size(); ++index)
        {
            Node& node = m_nodes[index];
            if (node.m_parent >= 0)
            {
                m_nodes[node.m_parent].m_children.push_back(static_cast<int>(index));
            }
        }

        // Detect cycles. A node whose ancestors loop forever is detached and treated as a root.
        // A chain longer than MaxHierarchyDepth is treated as a cycle as well; the converters cap
        // their node depth well below this limit, so a legitimate hierarchy never hits it.
        for (size_t index = 0; index < m_nodes.size(); ++index)
        {
            int current = static_cast<int>(index);
            int depth = 0;
            while (current >= 0 && depth++ < MaxHierarchyDepth)
            {
                current = m_nodes[current].m_parent;
            }
            if (current >= 0)
            {
                AZ_Error(
                    "UniversalSceneImporter", false, "Node '%s' is part of a hierarchy cycle; breaking the cycle.",
                    m_nodes[index].m_name.c_str());
                m_nodes[index].m_parent = -1;
            }
        }

        // Rebuild children again in case a cycle was broken above.
        for (Node& node : m_nodes)
        {
            node.m_children.clear();
        }
        for (size_t index = 0; index < m_nodes.size(); ++index)
        {
            if (m_nodes[index].m_parent >= 0)
            {
                m_nodes[m_nodes[index].m_parent].m_children.push_back(static_cast<int>(index));
            }
        }

        int rootCount = 0;
        for (size_t index = 0; index < m_nodes.size(); ++index)
        {
            Node& node = m_nodes[index];
            node.m_isRoot = node.m_parent < 0;
            if (node.m_isRoot)
            {
                ++rootCount;
            }
            if (node.m_mesh < 0 || static_cast<size_t>(node.m_mesh) >= m_meshes.size())
            {
                node.m_mesh = -1;
            }
            if (node.m_skin < 0 || static_cast<size_t>(node.m_skin) >= m_skins.size())
            {
                node.m_skin = -1;
            }
            if (node.m_mesh >= 0 && node.m_skin >= 0)
            {
                if (!m_skins[node.m_skin].m_joints.empty())
                {
                    // Bones that are referenced by a skin are joints, even if the converter missed the flag.
                    for (int joint : m_skins[node.m_skin].m_joints)
                    {
                        if (Node* jointNode = GetNode(joint))
                        {
                            jointNode->m_isBone = true;
                        }
                    }
                }
                else
                {
                    node.m_skin = -1;
                }
            }
        }

        if (rootCount > 1)
        {
            AZ_TracePrintf(
                AZ::SceneAPI::Utilities::LogWindow, "Scene '%s' has %d root nodes; all of them will be imported.", m_sourcePath.c_str(), rootCount);
        }

        // Validate skins and mesh attribute alignment. Broken data is dropped rather than
        // producing a corrupt scene graph later.
        for (Mesh& mesh : m_meshes)
        {
            for (MeshPrimitive& primitive : mesh.m_primitives)
            {
                const size_t vertexCount = primitive.m_positions.size();
                if (primitive.m_normals.size() != vertexCount)
                {
                    primitive.m_normals.clear();
                }
                for (size_t set = 0; set < MaxUvSets; ++set)
                {
                    if (primitive.m_uvSets[set].size() != vertexCount)
                    {
                        primitive.m_uvSets[set].clear();
                    }
                }
                for (size_t set = 0; set < MaxColorSets; ++set)
                {
                    if (primitive.m_colorSets[set].size() != vertexCount)
                    {
                        primitive.m_colorSets[set].clear();
                    }
                }
                if (primitive.m_tangents.size() != vertexCount)
                {
                    primitive.m_tangents.clear();
                }
                if (primitive.m_bitangents.size() != vertexCount)
                {
                    primitive.m_bitangents.clear();
                }
                if (!primitive.m_skinInfluences.empty() && primitive.m_skinInfluences.size() != vertexCount)
                {
                    primitive.m_skinInfluences.clear();
                }

                // Drop out of range indices. Some exporters emit stale indices for degenerate
                // primitives; removing them is preferable to reading out of bounds later.
                size_t writeIndex = 0;
                for (size_t readIndex = 0; readIndex + 2 < primitive.m_indices.size(); readIndex += 3)
                {
                    const AZ::u32 i0 = primitive.m_indices[readIndex + 0];
                    const AZ::u32 i1 = primitive.m_indices[readIndex + 1];
                    const AZ::u32 i2 = primitive.m_indices[readIndex + 2];
                    if (i0 < vertexCount && i1 < vertexCount && i2 < vertexCount && i0 != i1 && i1 != i2 && i0 != i2)
                    {
                        primitive.m_indices[writeIndex++] = i0;
                        primitive.m_indices[writeIndex++] = i1;
                        primitive.m_indices[writeIndex++] = i2;
                    }
                }
                primitive.m_indices.resize(writeIndex);

                for (MorphTarget& morphTarget : primitive.m_morphTargets)
                {
                    if (morphTarget.m_positionDeltas.size() != vertexCount)
                    {
                        morphTarget.m_positionDeltas.clear();
                    }
                    if (morphTarget.m_normalDeltas.size() != vertexCount)
                    {
                        morphTarget.m_normalDeltas.clear();
                    }
                    if (morphTarget.m_tangentDeltas.size() != vertexCount)
                    {
                        morphTarget.m_tangentDeltas.clear();
                    }
                }
                primitive.m_morphTargets.erase(
                    AZStd::remove_if(
                        primitive.m_morphTargets.begin(), primitive.m_morphTargets.end(),
                        [](const MorphTarget& target) { return target.IsEmpty(); }),
                    primitive.m_morphTargets.end());
            }
        }

        for (Skin& skin : m_skins)
        {
            skin.m_joints.erase(
                AZStd::remove_if(
                    skin.m_joints.begin(), skin.m_joints.end(),
                    [this](int joint) { return joint < 0 || static_cast<size_t>(joint) >= m_nodes.size(); }),
                skin.m_joints.end());
            if (skin.m_inverseBindMatrices.size() != skin.m_joints.size())
            {
                skin.m_inverseBindMatrices.clear();
            }
        }

        // Stable, non-zero material ids. The id is derived from the material name and its index so
        // it stays the same between imports of the same file, which lets material assets stay
        // associated with their source material.
        for (size_t index = 0; index < m_materials.size(); ++index)
        {
            Material& material = m_materials[index];
            if (material.m_name.empty())
            {
                material.m_name = AZStd::string::format("Material_%zu", index);
            }
            if (material.m_uniqueId == 0)
            {
                // FNV-1a hash of the material name, salted with the material index.
                AZ::u64 hash = 1469598103934665603ull;
                for (char character : material.m_name)
                {
                    hash ^= static_cast<AZ::u8>(character);
                    hash *= 1099511628211ull;
                }
                hash ^= (static_cast<AZ::u64>(index) + 1ull) * 1099511628211ull;
                material.m_uniqueId = hash != 0 ? hash : (static_cast<AZ::u64>(index) + 1ull);
            }
        }

        if (m_animations.empty())
        {
            AZ_TracePrintf(AZ::SceneAPI::Utilities::LogWindow, "Scene '%s' does not contain animations.", m_sourcePath.c_str());
        }

        BakeBindPoseIntoGraph();
        CalculateBounds();
        return true;
    }

    void Scene::LogSummary(const char* context) const
    {
        AZ_TracePrintf(
            AZ::SceneAPI::Utilities::LogWindow,
            "[%s] Imported %s scene '%s': %zu nodes, %zu meshes, %zu materials, %zu skins, %zu animations, %u vertices.",
            context, ToString(m_format), m_sourcePath.c_str(), m_nodes.size(), m_meshes.size(), m_materials.size(), m_skins.size(),
            m_animations.size(), m_vertexCount);
    }

} // namespace UniversalSceneImporter
