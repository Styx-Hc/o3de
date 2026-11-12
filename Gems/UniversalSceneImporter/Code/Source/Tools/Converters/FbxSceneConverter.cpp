/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "FbxSceneConverter.h"

#include <AzCore/Debug/Trace.h>
#include <AzCore/Math/MathUtils.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/sort.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/containers/unordered_set.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/conversions.h>
#include <SceneAPI/SceneCore/Utilities/Reporting.h>

#include "SceneConverterUtils.h"
#include "ufbx.h"

#include <cmath>
#include <memory>

namespace UniversalSceneImporter
{
    namespace
    {
        constexpr size_t FbxMaxVertexCount = 0x7FFFFFFF;

        struct FbxSceneDeleter
        {
            void operator()(ufbx_scene* scene) const
            {
                if (scene)
                {
                    ufbx_free_scene(scene);
                }
            }
        };

        using FbxScenePtr = std::unique_ptr<ufbx_scene, FbxSceneDeleter>;

        AZ::Vector2 ToVector2(const ufbx_vec2& value)
        {
            return AZ::Vector2(static_cast<float>(value.x), static_cast<float>(value.y));
        }

        AZ::Vector3 ToVector3(const ufbx_vec3& value)
        {
            return AZ::Vector3(static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z));
        }

        AZ::Vector4 ToVector4(const ufbx_vec4& value)
        {
            return AZ::Vector4(
                static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z), static_cast<float>(value.w));
        }

        AZ::Matrix3x4 ToMatrix(const ufbx_matrix& value)
        {
            // ufbx uses double precision reals by default; AZ::Matrix3x4 stores floats.
            float columnMajor[12];
            for (size_t index = 0; index < 12; ++index)
            {
                columnMajor[index] = static_cast<float>(value.v[index]);
            }
            return AZ::Matrix3x4::CreateFromColumnMajorFloat12(columnMajor);
        }

        AZStd::string ToAzString(const ufbx_string& value)
        {
            return value.data ? AZStd::string(value.data, value.length) : AZStd::string();
        }

        AZStd::string ToAzString(const ufbx_blob& value)
        {
            return value.data ? AZStd::string(static_cast<const char*>(value.data), value.size) : AZStd::string();
        }

        bool IsAnimatedTransformProperty(AZStd::string_view propertyName)
        {
            return propertyName == "Lcl Translation" || propertyName == "Lcl Rotation" || propertyName == "Lcl Scaling";
        }

        //! Inverts an authored matrix, falling back to identity when it is degenerate. Authored
        //! bind data is untrusted input, so a degenerate matrix must not reach GetInverseFull.
        AZ::Matrix3x4 SafeInvert(const AZ::Matrix3x4& matrix, const char* what)
        {
            if (Converters::IsInvertible(matrix))
            {
                return matrix.GetInverseFull();
            }
            AZ_Warning("UniversalSceneImporter", false, "The %s is not invertible; an identity matrix was used instead.", what);
            return AZ::Matrix3x4::CreateIdentity();
        }

        class FbxConverter
        {
        public:
            FbxConverter(ufbx_scene* scene, Scene& output)
                : m_fbx(scene)
                , m_scene(output)
            {
            }

            bool Convert(AZStd::string& outError);

        private:
            int AddNode(ufbx_node* node, int parentIndex);
            void MarkBoneHierarchy();
            int BuildMesh(ufbx_node* node, int nodeIndex);
            void BuildPrimitive(
                ufbx_mesh* mesh,
                const AZStd::vector<size_t>& faceIndices,
                int materialIndex,
                ufbx_skin_deformer* skinDeformer,
                const AZStd::vector<AZStd::string>& morphNames,
                const AZStd::vector<const ufbx_blend_shape*>& morphShapes,
                MeshPrimitive& outPrimitive);
            int GetMaterialIndex(ufbx_material* material);
            Texture MakeTexture(ufbx_texture* texture) const;
            void ConvertMaterial(ufbx_material* material, Material& outMaterial) const;
            void CollectMorphChannels(
                ufbx_mesh* mesh, AZStd::vector<AZStd::string>& outNames, AZStd::vector<const ufbx_blend_shape*>& outShapes) const;
            void BuildAnimations();

            ufbx_scene* m_fbx = nullptr;
            Scene& m_scene;
            AZStd::unordered_map<const ufbx_node*, int> m_nodeIndices;
            AZStd::unordered_map<const ufbx_material*, int> m_materialIndices;
            AZStd::vector<ufbx_node*> m_ufbxNodes;
            AZStd::unordered_map<int, AZ::Matrix3x4> m_sharedGeometryToBone;
            AZ::Matrix3x4 m_sharedBindReference = AZ::Matrix3x4::CreateIdentity();
            bool m_hasSharedBindReference = false;
            bool m_hitNodeLimit = false;
        };

        int FbxConverter::AddNode(ufbx_node* node, int parentIndex)
        {
            if (!node || m_hitNodeLimit)
            {
                return -1;
            }
            if (m_scene.GetNodes().size() >= Converters::MaxNodeCount)
            {
                m_hitNodeLimit = true;
                AZ_Error(
                    "UniversalSceneImporter", false, "FBX scene '%s' has more than %zu nodes and will not be imported.",
                    m_scene.GetSourcePath().c_str(), Converters::MaxNodeCount);
                return -1;
            }

            const int nodeIndex = static_cast<int>(m_scene.GetNodes().size());
            m_scene.GetNodes().emplace_back();
            m_ufbxNodes.push_back(node);
            m_nodeIndices.emplace(node, nodeIndex);

            Node& outputNode = m_scene.GetNodes().back();
            outputNode.m_parent = parentIndex;
            outputNode.m_localTransform = ToMatrix(node->node_to_parent);
            outputNode.m_isBone = node->bone != nullptr;
            outputNode.m_isRoot = node->is_root;

            AZStd::string name = ToAzString(node->name);
            if (name.empty())
            {
                name = node->is_root ? "RootNode" : AZStd::string::format("Node_%d", static_cast<int>(node->typed_id));
            }
            outputNode.m_name = AZStd::move(name);

            for (size_t childIndex = 0; childIndex < node->children.count; ++childIndex)
            {
                ufbx_node* child = node->children.data[childIndex];
                const int childNodeIndex = AddNode(child, nodeIndex);
                if (childNodeIndex < 0)
                {
                    return -1;
                }
                m_scene.GetNodes()[nodeIndex].m_children.push_back(childNodeIndex);
            }

            return nodeIndex;
        }

        void FbxConverter::MarkBoneHierarchy()
        {
            // Every skin cluster bone is a joint.
            for (size_t meshIndex = 0; meshIndex < m_fbx->meshes.count; ++meshIndex)
            {
                ufbx_mesh* mesh = m_fbx->meshes.data[meshIndex];
                for (size_t deformerIndex = 0; deformerIndex < mesh->skin_deformers.count; ++deformerIndex)
                {
                    ufbx_skin_deformer* deformer = mesh->skin_deformers.data[deformerIndex];
                    if (!deformer)
                    {
                        continue;
                    }
                    for (size_t clusterIndex = 0; clusterIndex < deformer->clusters.count; ++clusterIndex)
                    {
                        ufbx_skin_cluster* cluster = deformer->clusters.data[clusterIndex];
                        if (!cluster || !cluster->bone_node)
                        {
                            continue;
                        }
                        const auto found = m_nodeIndices.find(cluster->bone_node);
                        if (found != m_nodeIndices.end())
                        {
                            m_scene.GetNodes()[found->second].m_isBone = true;
                        }
                    }
                }
            }

            Converters::MarkBoneAncestors(m_scene.GetNodes());
        }

        Texture FbxConverter::MakeTexture(ufbx_texture* texture) const
        {
            Texture result;
            if (!texture)
            {
                return result;
            }

            ufbx_texture* fileTexture = texture;
            if (fileTexture->type != UFBX_TEXTURE_FILE && fileTexture->file_textures.count > 0)
            {
                fileTexture = fileTexture->file_textures.data[0];
                if (!fileTexture)
                {
                    return result;
                }
            }

            result.m_name = ToAzString(fileTexture->name);
            if (result.m_name.empty())
            {
                result.m_name = ToAzString(fileTexture->filename);
            }

            if (fileTexture->content.size > 0)
            {
                result.m_hasEmbeddedData = true;
                if (m_scene.GetExtractEmbeddedTextures())
                {
                    result.m_embeddedData.assign(
                        static_cast<const AZ::u8*>(fileTexture->content.data),
                        static_cast<const AZ::u8*>(fileTexture->content.data) + fileTexture->content.size);
                    result.m_extension = Converters::SniffImageExtension(result.m_embeddedData.data(), result.m_embeddedData.size());
                }
            }

            if (result.m_extension.empty())
            {
                result.m_extension = Converters::ExtractExtension(ToAzString(fileTexture->filename));
            }
            if (result.m_extension.empty())
            {
                result.m_extension = Converters::ExtractExtension(result.m_name);
            }
            if (result.m_extension.empty())
            {
                result.m_extension = "png";
            }

            const AZStd::string resolvedFileName = ToAzString(fileTexture->filename);
            result.m_uri = resolvedFileName;
            if (result.m_uri.empty())
            {
                result.m_uri = ToAzString(fileTexture->relative_filename);
            }
            if (result.m_uri.empty())
            {
                result.m_uri = ToAzString(fileTexture->absolute_filename);
            }

            return result;
        }

        void FbxConverter::ConvertMaterial(ufbx_material* material, Material& outMaterial) const
        {
            outMaterial.m_name = ToAzString(material->name);
            if (outMaterial.m_name.empty())
            {
                outMaterial.m_name = "Material";
            }

            const ufbx_material_pbr_maps& pbr = material->pbr;
            const ufbx_material_fbx_maps& fbx = material->fbx;

            if (pbr.base_color.has_value)
            {
                outMaterial.m_baseColor = ToVector3(pbr.base_color.value_vec3);
            }
            else if (fbx.diffuse_color.has_value)
            {
                outMaterial.m_baseColor = ToVector3(fbx.diffuse_color.value_vec3);
            }

            if (pbr.metalness.has_value)
            {
                outMaterial.m_metallic = AZ::GetClamp(static_cast<float>(pbr.metalness.value_real), 0.0f, 1.0f);
            }
            if (pbr.roughness.has_value)
            {
                outMaterial.m_roughness = AZ::GetClamp(static_cast<float>(pbr.roughness.value_real), 0.0f, 1.0f);
            }

            if (pbr.emission_color.has_value)
            {
                outMaterial.m_emissiveColor = ToVector3(pbr.emission_color.value_vec3);
            }
            else if (fbx.emission_color.has_value)
            {
                outMaterial.m_emissiveColor = ToVector3(fbx.emission_color.value_vec3);
            }
            if (pbr.emission_factor.has_value)
            {
                outMaterial.m_emissiveIntensity = static_cast<float>(pbr.emission_factor.value_real);
            }

            if (pbr.opacity.has_value)
            {
                outMaterial.m_opacity = AZ::GetClamp(static_cast<float>(pbr.opacity.value_real), 0.0f, 1.0f);
            }
            else if (fbx.transparency_factor.has_value)
            {
                outMaterial.m_opacity = AZ::GetClamp(1.0f - static_cast<float>(fbx.transparency_factor.value_real), 0.0f, 1.0f);
            }

            if (fbx.diffuse_color.has_value)
            {
                outMaterial.m_diffuseColor = ToVector3(fbx.diffuse_color.value_vec3);
            }
            else
            {
                outMaterial.m_diffuseColor = outMaterial.m_baseColor;
            }
            if (fbx.specular_color.has_value)
            {
                outMaterial.m_specularColor = ToVector3(fbx.specular_color.value_vec3);
            }
            if (fbx.specular_exponent.has_value)
            {
                outMaterial.m_shininess = static_cast<float>(fbx.specular_exponent.value_real);
            }
            if (outMaterial.m_shininess <= 0.0f && pbr.specular_ior.has_value)
            {
                outMaterial.m_shininess = static_cast<float>(pbr.specular_ior.value_real);
            }

            auto pickTexture = [](const ufbx_material_map& preferred, const ufbx_material_map& fallback) -> ufbx_texture*
            {
                if (preferred.texture && preferred.texture_enabled)
                {
                    return preferred.texture;
                }
                if (fallback.texture && fallback.texture_enabled)
                {
                    return fallback.texture;
                }
                return nullptr;
            };

            outMaterial.GetTexture(MaterialTexture::BaseColor) = MakeTexture(pickTexture(pbr.base_color, fbx.diffuse_color));
            outMaterial.GetTexture(MaterialTexture::Diffuse) = MakeTexture(pickTexture(fbx.diffuse_color, pbr.base_color));
            outMaterial.GetTexture(MaterialTexture::Specular) = MakeTexture(pickTexture(pbr.specular_color, fbx.specular_color));
            outMaterial.GetTexture(MaterialTexture::Normal) = MakeTexture(pickTexture(pbr.normal_map, fbx.normal_map));
            outMaterial.GetTexture(MaterialTexture::Bump) = MakeTexture(fbx.bump.texture);
            outMaterial.GetTexture(MaterialTexture::Emissive) = MakeTexture(pickTexture(pbr.emission_color, fbx.emission_color));
            outMaterial.GetTexture(MaterialTexture::Metallic) = MakeTexture(pbr.metalness.texture);
            outMaterial.GetTexture(MaterialTexture::Roughness) = MakeTexture(pbr.roughness.texture);
            outMaterial.GetTexture(MaterialTexture::AmbientOcclusion) = MakeTexture(pbr.ambient_occlusion.texture);
        }

        int FbxConverter::GetMaterialIndex(ufbx_material* material)
        {
            if (!material)
            {
                return -1;
            }

            const auto existing = m_materialIndices.find(material);
            if (existing != m_materialIndices.end())
            {
                return existing->second;
            }

            const int materialIndex = static_cast<int>(m_scene.GetMaterials().size());
            m_scene.GetMaterials().emplace_back();
            ConvertMaterial(material, m_scene.GetMaterials().back());
            m_materialIndices.emplace(material, materialIndex);
            return materialIndex;
        }

        void FbxConverter::CollectMorphChannels(
            ufbx_mesh* mesh, AZStd::vector<AZStd::string>& outNames, AZStd::vector<const ufbx_blend_shape*>& outShapes) const
        {
            outNames.clear();
            outShapes.clear();

            for (size_t deformerIndex = 0; deformerIndex < mesh->blend_deformers.count; ++deformerIndex)
            {
                ufbx_blend_deformer* deformer = mesh->blend_deformers.data[deformerIndex];
                if (!deformer)
                {
                    continue;
                }
                for (size_t channelIndex = 0; channelIndex < deformer->channels.count; ++channelIndex)
                {
                    ufbx_blend_channel* channel = deformer->channels.data[channelIndex];
                    if (!channel || !channel->target_shape)
                    {
                        continue;
                    }

                    AZStd::string channelName = ToAzString(channel->name);
                    if (channelName.empty())
                    {
                        channelName = AZStd::string::format("BlendShape_%zu", outNames.size());
                    }
                    channelName = Converters::SanitizeNodeName(channelName, "BlendShape");

                    const auto existing =
                        AZStd::find_if(outNames.begin(), outNames.end(), [&channelName](const AZStd::string& name)
                                       { return name == channelName; });
                    if (existing != outNames.end())
                    {
                        // Multiple deformers can expose the same channel; keep the first one.
                        continue;
                    }

                    outNames.push_back(AZStd::move(channelName));
                    outShapes.push_back(channel->target_shape);
                }
            }
        }

        void FbxConverter::BuildPrimitive(
            ufbx_mesh* mesh,
            const AZStd::vector<size_t>& faceIndices,
            int materialIndex,
            ufbx_skin_deformer* skinDeformer,
            const AZStd::vector<AZStd::string>& morphNames,
            const AZStd::vector<const ufbx_blend_shape*>& morphShapes,
            MeshPrimitive& outPrimitive)
        {
            outPrimitive.m_material = materialIndex;

            const bool hasNormals = mesh->vertex_normal.exists;
            const size_t uvSetCount = AZStd::min(mesh->uv_sets.count, MaxUvSets);
            const size_t colorSetCount = AZStd::min(mesh->color_sets.count, MaxColorSets);
            const bool hasTangents =
                mesh->uv_sets.count > 0 && mesh->uv_sets.data[0].vertex_tangent.exists;

            size_t expectedVertexCount = 0;
            for (size_t faceIndex : faceIndices)
            {
                const ufbx_face& face = mesh->faces.data[faceIndex];
                if (face.num_indices >= 3)
                {
                    expectedVertexCount += face.num_indices;
                }
            }
            if (expectedVertexCount == 0)
            {
                return;
            }

            outPrimitive.m_positions.reserve(expectedVertexCount);
            if (hasNormals)
            {
                outPrimitive.m_normals.reserve(expectedVertexCount);
            }
            for (size_t set = 0; set < uvSetCount; ++set)
            {
                outPrimitive.m_uvSets[set].reserve(expectedVertexCount);
            }
            for (size_t set = 0; set < colorSetCount; ++set)
            {
                outPrimitive.m_colorSets[set].reserve(expectedVertexCount);
            }
            if (hasTangents)
            {
                outPrimitive.m_tangents.reserve(expectedVertexCount);
                outPrimitive.m_bitangents.reserve(expectedVertexCount);
            }
            if (skinDeformer)
            {
                outPrimitive.m_skinInfluences.reserve(expectedVertexCount);
            }
            outPrimitive.m_morphTargets.resize(morphNames.size());
            for (size_t morphIndex = 0; morphIndex < morphNames.size(); ++morphIndex)
            {
                outPrimitive.m_morphTargets[morphIndex].m_name = morphNames[morphIndex];
                outPrimitive.m_morphTargets[morphIndex].m_positionDeltas.reserve(expectedVertexCount);
                outPrimitive.m_morphTargets[morphIndex].m_normalDeltas.reserve(expectedVertexCount);
            }

            AZStd::vector<AZ::u32> triangles;
            triangles.resize(AZStd::max<size_t>(mesh->max_face_triangles, 1) * 3);

            for (size_t faceIndex : faceIndices)
            {
                const ufbx_face& face = mesh->faces.data[faceIndex];
                if (face.num_indices < 3 || face.index_begin + face.num_indices > mesh->num_indices)
                {
                    continue;
                }

                const AZ::u32 vertexBase = static_cast<AZ::u32>(outPrimitive.m_positions.size());
                for (AZ::u32 cornerOffset = 0; cornerOffset < face.num_indices; ++cornerOffset)
                {
                    const AZ::u32 corner = face.index_begin + cornerOffset;
                    const AZ::u32 logicalVertex =
                        (corner < mesh->vertex_indices.count) ? mesh->vertex_indices.data[corner] : UFBX_NO_INDEX;

                    outPrimitive.m_positions.push_back(ToVector3(ufbx_get_vertex_vec3(&mesh->vertex_position, corner)));
                    if (hasNormals)
                    {
                        AZ::Vector3 normal = ToVector3(ufbx_get_vertex_vec3(&mesh->vertex_normal, corner));
                        normal.NormalizeSafe();
                        outPrimitive.m_normals.push_back(normal);
                    }

                    for (size_t set = 0; set < uvSetCount; ++set)
                    {
                        AZ::Vector2 uv = AZ::Vector2::CreateZero();
                        if (mesh->uv_sets.data[set].vertex_uv.exists)
                        {
                            uv = ToVector2(ufbx_get_vertex_vec2(&mesh->uv_sets.data[set].vertex_uv, corner));
                            // FBX UVs use a bottom-left origin; O3DE uses a top-left origin.
                            uv.SetY(1.0f - uv.GetY());
                        }
                        outPrimitive.m_uvSets[set].push_back(uv);
                    }

                    for (size_t set = 0; set < colorSetCount; ++set)
                    {
                        if (mesh->color_sets.data[set].vertex_color.exists)
                        {
                            outPrimitive.m_colorSets[set].push_back(ToVector4(ufbx_get_vertex_vec4(
                                &mesh->color_sets.data[set].vertex_color, corner)));
                        }
                        else
                        {
                            outPrimitive.m_colorSets[set].push_back(AZ::Vector4::CreateOne());
                        }
                    }

                    if (hasTangents)
                    {
                        const ufbx_uv_set& uvSet = mesh->uv_sets.data[0];
                        const AZ::Vector3 tangent = ToVector3(ufbx_get_vertex_vec3(&uvSet.vertex_tangent, corner));
                        // Some files ship a tangent layer without a bitangent layer; reading the
                        // attribute anyway would dereference null indices. Derive the bitangent
                        // from the normal like the glTF importer does, or fall back to a default.
                        AZ::Vector3 bitangent = AZ::Vector3::CreateAxisY();
                        if (uvSet.vertex_bitangent.exists)
                        {
                            bitangent = ToVector3(ufbx_get_vertex_vec3(&uvSet.vertex_bitangent, corner));
                        }
                        else if (hasNormals)
                        {
                            bitangent = outPrimitive.m_normals.back().Cross(tangent);
                        }
                        outPrimitive.m_tangents.emplace_back(tangent.GetX(), tangent.GetY(), tangent.GetZ(), 1.0f);
                        outPrimitive.m_bitangents.push_back(bitangent);
                    }

                    if (skinDeformer && logicalVertex != UFBX_NO_INDEX && logicalVertex < skinDeformer->vertices.count)
                    {
                        const ufbx_skin_vertex& skinVertex = skinDeformer->vertices.data[logicalVertex];
                        AZStd::vector<SkinInfluence> influences;
                        influences.reserve(AZStd::min<size_t>(skinVertex.num_weights, MaxSkinInfluences));
                        float totalWeight = 0.0f;
                        for (size_t weightIndex = 0;
                             weightIndex < skinVertex.num_weights && influences.size() < MaxSkinInfluences; ++weightIndex)
                        {
                            const size_t sourceIndex = skinVertex.weight_begin + weightIndex;
                            if (sourceIndex >= skinDeformer->weights.count)
                            {
                                break;
                            }
                            const ufbx_skin_weight& weight = skinDeformer->weights.data[sourceIndex];
                            if (weight.cluster_index >= skinDeformer->clusters.count || weight.weight <= 0.0f)
                            {
                                continue;
                            }
                            ufbx_skin_cluster* cluster = skinDeformer->clusters.data[weight.cluster_index];
                            if (!cluster || !cluster->bone_node)
                            {
                                continue;
                            }
                            const auto found = m_nodeIndices.find(cluster->bone_node);
                            if (found == m_nodeIndices.end())
                            {
                                continue;
                            }
                            const float weightValue = static_cast<float>(weight.weight);
                            influences.push_back({ static_cast<AZ::u32>(found->second), weightValue });
                            totalWeight += weightValue;
                        }

                        if (totalWeight > 0.0f)
                        {
                            for (SkinInfluence& influence : influences)
                            {
                                influence.m_weight /= totalWeight;
                            }
                        }
                        outPrimitive.m_skinInfluences.push_back(AZStd::move(influences));
                    }
                    else if (skinDeformer)
                    {
                        // Keep the skin influence array aligned with the vertex arrays even when a
                        // corner cannot be resolved to a logical vertex.
                        outPrimitive.m_skinInfluences.emplace_back();
                    }

                    for (size_t morphIndex = 0; morphIndex < morphShapes.size(); ++morphIndex)
                    {
                        const ufbx_blend_shape* shape = morphShapes[morphIndex];
                        MorphTarget& target = outPrimitive.m_morphTargets[morphIndex];
                        AZ::Vector3 positionDelta = AZ::Vector3::CreateZero();
                        AZ::Vector3 normalDelta = AZ::Vector3::CreateZero();
                        if (shape && logicalVertex != UFBX_NO_INDEX)
                        {
                            const AZ::u32 offsetIndex = ufbx_get_blend_shape_offset_index(shape, logicalVertex);
                            if (offsetIndex != UFBX_NO_INDEX)
                            {
                                if (offsetIndex < shape->position_offsets.count)
                                {
                                    positionDelta = ToVector3(shape->position_offsets.data[offsetIndex]);
                                }
                                if (offsetIndex < shape->normal_offsets.count)
                                {
                                    normalDelta = ToVector3(shape->normal_offsets.data[offsetIndex]);
                                }
                                if (offsetIndex < shape->offset_weights.count)
                                {
                                    const float offsetWeight = static_cast<float>(shape->offset_weights.data[offsetIndex]);
                                    positionDelta *= offsetWeight;
                                    normalDelta *= offsetWeight;
                                }
                            }
                        }
                        target.m_positionDeltas.push_back(positionDelta);
                        target.m_normalDeltas.push_back(normalDelta);
                    }
                }

                const AZ::u32 triangleCount = ufbx_triangulate_face(
                    triangles.data(), triangles.size(), mesh, face);
                for (AZ::u32 triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
                {
                    const AZ::u32 i0 = triangles[triangleIndex * 3 + 0];
                    const AZ::u32 i1 = triangles[triangleIndex * 3 + 1];
                    const AZ::u32 i2 = triangles[triangleIndex * 3 + 2];
                    if (i0 < face.index_begin || i1 < face.index_begin || i2 < face.index_begin)
                    {
                        continue;
                    }
                    const AZ::u32 local0 = i0 - face.index_begin;
                    const AZ::u32 local1 = i1 - face.index_begin;
                    const AZ::u32 local2 = i2 - face.index_begin;
                    if (local0 >= face.num_indices || local1 >= face.num_indices || local2 >= face.num_indices)
                    {
                        continue;
                    }
                    outPrimitive.m_indices.push_back(vertexBase + local0);
                    outPrimitive.m_indices.push_back(vertexBase + local1);
                    outPrimitive.m_indices.push_back(vertexBase + local2);
                }
            }

            // Skinned meshes need a complete tangent frame downstream. Generate one when the
            // source file does not provide tangents.
            if (skinDeformer && !outPrimitive.m_skinInfluences.empty())
            {
                Converters::EnsureTangentStreams(outPrimitive);
            }
        }

        int FbxConverter::BuildMesh(ufbx_node* node, int nodeIndex)
        {
            ufbx_mesh* mesh = node->mesh;
            if (!mesh || mesh->num_faces == 0 || mesh->num_indices == 0)
            {
                if (mesh && mesh->num_faces == 0)
                {
                    AZ_Warning(
                        "UniversalSceneImporter", false,
                        "Mesh '%s' on node '%s' has no faces and will be skipped.", ToAzString(mesh->name).c_str(),
                        ToAzString(node->name).c_str());
                }
                return -1;
            }

            if (mesh->num_indices > FbxMaxVertexCount)
            {
                AZ_Error(
                    "UniversalSceneImporter", false, "Mesh '%s' has too many vertices (%zu) and will be skipped.",
                    ToAzString(mesh->name).c_str(), mesh->num_indices);
                return -1;
            }

            Mesh outputMesh;
            outputMesh.m_name = ToAzString(mesh->name);
            if (outputMesh.m_name.empty())
            {
                outputMesh.m_name = ToAzString(node->name);
            }
            if (outputMesh.m_name.empty())
            {
                outputMesh.m_name = AZStd::string::format("Mesh_%d", nodeIndex);
            }

            // Prefer per-instance materials, but fall back to the mesh material list when the node
            // does not override them.
            const ufbx_material_list& nodeMaterials =
                node->materials.count > 0 ? node->materials : mesh->materials;
            const size_t slotCount = AZStd::max<size_t>(nodeMaterials.count, 1);
            AZStd::vector<AZStd::vector<size_t>> facesBySlot(slotCount);
            for (size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex)
            {
                size_t slot = 0;
                if (faceIndex < mesh->face_material.count)
                {
                    const AZ::u32 materialIndex = mesh->face_material.data[faceIndex];
                    if (materialIndex != UFBX_NO_INDEX && materialIndex < nodeMaterials.count)
                    {
                        slot = materialIndex;
                    }
                }
                facesBySlot[slot].push_back(faceIndex);
            }

            ufbx_skin_deformer* skinDeformer = nullptr;
            if (mesh->skin_deformers.count > 0)
            {
                skinDeformer = mesh->skin_deformers.data[0];
                if (mesh->skin_deformers.count > 1)
                {
                    AZ_Warning(
                        "UniversalSceneImporter", false,
                        "Mesh '%s' has %zu skin deformers; only the first one will be imported.",
                        outputMesh.m_name.c_str(), mesh->skin_deformers.count);
                }
            }

            AZStd::vector<AZStd::string> morphNames;
            AZStd::vector<const ufbx_blend_shape*> morphShapes;
            CollectMorphChannels(mesh, morphNames, morphShapes);

            bool anyWeights = false;
            for (size_t slot = 0; slot < slotCount; ++slot)
            {
                if (facesBySlot[slot].empty())
                {
                    continue;
                }

                int materialIndex = -1;
                if (slot < nodeMaterials.count && nodeMaterials.data[slot])
                {
                    materialIndex = GetMaterialIndex(nodeMaterials.data[slot]);
                }

                outputMesh.m_primitives.emplace_back();
                BuildPrimitive(
                    mesh, facesBySlot[slot], materialIndex, skinDeformer, morphNames, morphShapes, outputMesh.m_primitives.back());

                MeshPrimitive& primitive = outputMesh.m_primitives.back();
                if (primitive.m_positions.empty() || primitive.m_indices.empty())
                {
                    outputMesh.m_primitives.pop_back();
                    continue;
                }
                for (const AZStd::vector<SkinInfluence>& influences : primitive.m_skinInfluences)
                {
                    if (!influences.empty())
                    {
                        anyWeights = true;
                        break;
                    }
                }
            }

            if (outputMesh.m_primitives.empty())
            {
                return -1;
            }

            if (!anyWeights)
            {
                for (MeshPrimitive& primitive : outputMesh.m_primitives)
                {
                    primitive.m_skinInfluences.clear();
                }
                skinDeformer = nullptr;
            }

            const int meshIndex = static_cast<int>(m_scene.GetMeshes().size());
            m_scene.GetMeshes().push_back(AZStd::move(outputMesh));

            if (skinDeformer)
            {
                Skin skin;
                AZStd::unordered_set<AZ::u32> joints;
                for (const MeshPrimitive& primitive : m_scene.GetMeshes()[meshIndex].m_primitives)
                {
                    for (const AZStd::vector<SkinInfluence>& influences : primitive.m_skinInfluences)
                    {
                        for (const SkinInfluence& influence : influences)
                        {
                            joints.insert(influence.m_joint);
                        }
                    }
                }
                skin.m_joints.reserve(joints.size());
                for (AZ::u32 joint : joints)
                {
                    skin.m_joints.push_back(static_cast<int>(joint));
                }
                AZStd::sort(skin.m_joints.begin(), skin.m_joints.end());
                // The runtime stores a single inverse-bind per joint shared by every mesh, so
                // the pipeline uses the authored-bind contract: the shared inverse bind of a
                // joint is the geometry_to_bone matrix of the FIRST skin that binds it, and
                // every other mesh bakes the constant difference between its own cluster binds
                // and that reference into its vertices (C = geo2boneFirst^-1 * geo2boneMesh,
                // which is joint-independent for any valid skin). This keeps rigs whose
                // authored bind pose differs from their default pose (the FPS sample's
                // Arm_saiga.fbx binds its arms extended while the default pose grips the
                // rifle) skinning correctly, and is identical to the default-pose contract for
                // rigs where the two poses agree.
                AZStd::vector<AZ::Matrix3x4> geometryToBone(skin.m_joints.size());
                AZStd::vector<bool> hasGeometryToBone(skin.m_joints.size(), false);
                for (size_t clusterIndex = 0; clusterIndex < skinDeformer->clusters.count; ++clusterIndex)
                {
                    const ufbx_skin_cluster* cluster = skinDeformer->clusters.data[clusterIndex];
                    if (!cluster || !cluster->bone_node)
                    {
                        continue;
                    }
                    const auto found = m_nodeIndices.find(cluster->bone_node);
                    if (found == m_nodeIndices.end())
                    {
                        continue;
                    }
                    for (size_t jointSlot = 0; jointSlot < skin.m_joints.size(); ++jointSlot)
                    {
                        if (skin.m_joints[jointSlot] == found->second)
                        {
                            geometryToBone[jointSlot] = ToMatrix(cluster->geometry_to_bone);
                            hasGeometryToBone[jointSlot] = true;
                            break;
                        }
                    }
                }

                // Reference bind B_ref: the default-pose bind world of the first cluster that
                // carries an authored bind, fixed once by the first skin and reused by every
                // later skin. It only chooses the space the exported vertices live in (the
                // skeleton's model space, so model bounds and preview framing stay sane); the
                // runtime skinning is invariant to it because IBM * bake cancels B_ref.
                if (!m_hasSharedBindReference)
                {
                    for (size_t jointSlot = 0; jointSlot < skin.m_joints.size(); ++jointSlot)
                    {
                        if (hasGeometryToBone[jointSlot])
                        {
                            m_sharedBindReference =
                                m_scene.GetNodeWorldTransform(skin.m_joints[jointSlot]) * geometryToBone[jointSlot];
                            m_hasSharedBindReference = true;
                            break;
                        }
                    }
                    if (!m_hasSharedBindReference)
                    {
                        m_sharedBindReference = AZ::Matrix3x4::CreateIdentity();
                        m_hasSharedBindReference = true;
                    }
                }
                const AZ::Matrix3x4 bindReferenceInverse = SafeInvert(m_sharedBindReference, "shared bind reference");

                // Per-mesh bind constant C against the first skin that bound these joints.
                AZ::Matrix3x4 bindVertexTransform = AZ::Matrix3x4::CreateIdentity();
                for (size_t jointSlot = 0; jointSlot < skin.m_joints.size(); ++jointSlot)
                {
                    if (!hasGeometryToBone[jointSlot])
                    {
                        continue;
                    }
                    const int jointNode = skin.m_joints[jointSlot];
                    const auto shared = m_sharedGeometryToBone.find(jointNode);
                    if (shared == m_sharedGeometryToBone.end())
                    {
                        continue; // not bound by an earlier skin; try the next joint of this mesh.
                    }
                    bindVertexTransform = SafeInvert(shared->second, "shared joint bind matrix") * geometryToBone[jointSlot];
                    break;
                }
                bindVertexTransform = m_sharedBindReference * bindVertexTransform;

                Node& meshNode = m_scene.GetNodes()[nodeIndex];
                const AZ::Matrix3x4 ancestorWorld = m_scene.GetNodeWorldTransform(meshNode.m_parent);
                meshNode.m_hasLocalOverride = true;
                // The model builder bakes the node's graph world into the vertices exactly
                // once, so the override makes that bake land the vertices in the shared bind
                // space. Baking the fix into the vertices here instead double-applies
                // ancestor transforms (such as the axis-conversion adjust ufbx puts on a root
                // bone) and the exported mesh sat one conversion away from the skeleton's
                // bind space, deforming the moment anything animated.
                meshNode.m_localOverride = SafeInvert(ancestorWorld, "skinned mesh ancestor world transform") * bindVertexTransform;

                skin.m_inverseBindMatrices.reserve(skin.m_joints.size());
                for (size_t jointSlot = 0; jointSlot < skin.m_joints.size(); ++jointSlot)
                {
                    if (hasGeometryToBone[jointSlot])
                    {
                        // Normalized against B_ref so that bake * IBM reproduces the authored
                        // cluster bind exactly.
                        skin.m_inverseBindMatrices.push_back(geometryToBone[jointSlot] * bindReferenceInverse);
                        m_sharedGeometryToBone.emplace(skin.m_joints[jointSlot], geometryToBone[jointSlot]);
                    }
                    else
                    {
                        // No cluster for this joint (weights below threshold): fall back to the
                        // default-pose world inverse so the joint still has a usable bind.
                        skin.m_inverseBindMatrices.push_back(SafeInvert(
                            m_scene.GetNodeWorldTransform(skin.m_joints[jointSlot]), "joint default world transform") *
                            bindReferenceInverse);
                    }
                }
                const int skinIndex = static_cast<int>(m_scene.GetSkins().size());
                m_scene.GetSkins().push_back(AZStd::move(skin));
                m_scene.GetNodes()[nodeIndex].m_skin = skinIndex;
            }

            return meshIndex;
        }

        void FbxConverter::BuildAnimations()
        {
            for (size_t stackIndex = 0; stackIndex < m_fbx->anim_stacks.count; ++stackIndex)
            {
                ufbx_anim_stack* stack = m_fbx->anim_stacks.data[stackIndex];
                if (!stack || !stack->anim)
                {
                    continue;
                }

                const double duration = stack->time_end - stack->time_begin;
                if (!(duration > 0.0))
                {
                    AZ_Warning(
                        "UniversalSceneImporter", false,
                        "Animation '%s' has a non positive duration and will be skipped.",
                        ToAzString(stack->name).c_str());
                    continue;
                }

                size_t frameCount = static_cast<size_t>(std::ceil(duration / Converters::DefaultAnimationTimeStep)) + 1;
                frameCount = AZ::GetClamp<size_t>(frameCount, 2, Converters::MaxAnimationFrames);

                Animation animation;
                animation.m_name = ToAzString(stack->name);
                if (animation.m_name.empty())
                {
                    animation.m_name = AZStd::string::format("Animation_%zu", stackIndex);
                }
                animation.m_timeStep = static_cast<float>(Converters::DefaultAnimationTimeStep);

                // Collect the nodes and morph channels that are actually animated by this stack.
                AZStd::unordered_set<const ufbx_node*> animatedNodes;
                AZStd::unordered_set<const ufbx_blend_channel*> animatedChannels;
                const ufbx_anim* anim = stack->anim;
                for (size_t layerIndex = 0; layerIndex < anim->layers.count; ++layerIndex)
                {
                    const ufbx_anim_layer* layer = anim->layers.data[layerIndex];
                    if (!layer)
                    {
                        continue;
                    }
                    for (size_t propIndex = 0; propIndex < layer->anim_props.count; ++propIndex)
                    {
                        const ufbx_anim_prop& prop = layer->anim_props.data[propIndex];
                        if (ufbx_node* animatedNode = ufbx_as_node(prop.element))
                        {
                            if (IsAnimatedTransformProperty(ToAzString(prop.prop_name)))
                            {
                                animatedNodes.insert(animatedNode);
                            }
                        }
                        else if (ufbx_blend_channel* channel = ufbx_as_blend_channel(prop.element))
                        {
                            if (ToAzString(prop.prop_name) == "DeformPercent")
                            {
                                animatedChannels.insert(channel);
                            }
                        }
                    }
                }

                for (size_t nodeIndex = 0; nodeIndex < m_scene.GetNodes().size(); ++nodeIndex)
                {
                    ufbx_node* node = m_ufbxNodes[nodeIndex];
                    if (!node)
                    {
                        continue;
                    }
                    if (!animatedNodes.contains(node))
                    {
                        continue;
                    }

                    NodeAnimation nodeAnimation;
                    nodeAnimation.m_node = static_cast<int>(nodeIndex);
                    nodeAnimation.m_keyFrames.reserve(frameCount);
                    for (size_t frame = 0; frame < frameCount; ++frame)
                    {
                        double time = stack->time_begin + static_cast<double>(frame) * Converters::DefaultAnimationTimeStep;
                        time = AZStd::min(time, stack->time_end);
                        const ufbx_transform transform = ufbx_evaluate_transform(anim, node, time);
                        const ufbx_matrix matrix = ufbx_transform_to_matrix(&transform);
                        nodeAnimation.m_keyFrames.push_back(ToMatrix(matrix));
                    }
                    animation.m_nodeAnimations.push_back(AZStd::move(nodeAnimation));
                }

                // Morph target animation tracks, grouped per mesh node so the importer can create
                // one BlendShapeAnimationData node per channel.
                for (size_t nodeIndex = 0; nodeIndex < m_scene.GetNodes().size(); ++nodeIndex)
                {
                    ufbx_node* node = m_ufbxNodes[nodeIndex];
                    ufbx_mesh* mesh = node ? node->mesh : nullptr;
                    if (!mesh)
                    {
                        continue;
                    }
                    const int meshIndex = m_scene.GetNodes()[nodeIndex].m_mesh;
                    const Mesh* outputMesh = m_scene.GetMesh(meshIndex);
                    if (!outputMesh || outputMesh->m_primitives.empty() || outputMesh->m_primitives[0].m_morphTargets.empty())
                    {
                        continue;
                    }

                    const MeshPrimitive& firstPrimitive = outputMesh->m_primitives[0];
                    for (size_t deformerIndex = 0; deformerIndex < mesh->blend_deformers.count; ++deformerIndex)
                    {
                        ufbx_blend_deformer* deformer = mesh->blend_deformers.data[deformerIndex];
                        if (!deformer)
                        {
                            continue;
                        }
                        for (size_t channelIndex = 0; channelIndex < deformer->channels.count; ++channelIndex)
                        {
                            ufbx_blend_channel* channel = deformer->channels.data[channelIndex];
                            if (!channel || !animatedChannels.contains(channel))
                            {
                                continue;
                            }

                            const AZStd::string channelName =
                                Converters::SanitizeNodeName(ToAzString(channel->name), "BlendShape");
                            size_t morphIndex = firstPrimitive.m_morphTargets.size();
                            for (size_t targetIndex = 0; targetIndex < firstPrimitive.m_morphTargets.size(); ++targetIndex)
                            {
                                if (firstPrimitive.m_morphTargets[targetIndex].m_name == channelName)
                                {
                                    morphIndex = targetIndex;
                                    break;
                                }
                            }
                            if (morphIndex >= firstPrimitive.m_morphTargets.size())
                            {
                                continue;
                            }

                            MorphAnimation morphAnimation;
                            morphAnimation.m_node = static_cast<int>(nodeIndex);
                            morphAnimation.m_mesh = meshIndex;
                            morphAnimation.m_morphTarget = static_cast<int>(morphIndex);
                            morphAnimation.m_blendShapeName = channelName;
                            morphAnimation.m_keyFrames.reserve(frameCount);
                            for (size_t frame = 0; frame < frameCount; ++frame)
                            {
                                double time = stack->time_begin + static_cast<double>(frame) * Converters::DefaultAnimationTimeStep;
                                time = AZStd::min(time, stack->time_end);
                                morphAnimation.m_keyFrames.push_back(static_cast<float>(ufbx_evaluate_blend_weight(anim, channel, time)));
                            }
                            animation.m_morphAnimations.push_back(AZStd::move(morphAnimation));
                        }
                    }
                }

                if (!animation.m_nodeAnimations.empty() || !animation.m_morphAnimations.empty())
                {
                    m_scene.GetAnimations().push_back(AZStd::move(animation));
                }
            }
        }

        bool FbxConverter::Convert(AZStd::string& outError)
        {
            // Nodes first: mesh and skin conversion needs the node lookup table.
            const int rootIndex = AddNode(m_fbx->root_node, -1);
            if (rootIndex != 0 || m_hitNodeLimit)
            {
                outError = "Failed to build the FBX node hierarchy.";
                return false;
            }

            // Warn about nodes that are not reachable from the root. ufbx parents lone nodes under
            // the root by default, so this mainly catches pathological files.
            if (m_nodeIndices.size() < m_fbx->nodes.count)
            {
                AZ_Warning(
                    "UniversalSceneImporter", false,
                    "FBX scene '%s' contains %zu nodes that are not reachable from the root node.",
                    m_scene.GetSourcePath().c_str(), m_fbx->nodes.count - m_nodeIndices.size());
            }

            MarkBoneHierarchy();

            for (size_t nodeIndex = 0; nodeIndex < m_scene.GetNodes().size(); ++nodeIndex)
            {
                ufbx_node* node = m_ufbxNodes[nodeIndex];
                if (!node)
                {
                    continue;
                }
                const int meshIndex = BuildMesh(node, static_cast<int>(nodeIndex));
                m_scene.GetNodes()[nodeIndex].m_mesh = meshIndex;
                if (meshIndex < 0)
                {
                    m_scene.GetNodes()[nodeIndex].m_skin = -1;
                }
            }

            BuildAnimations();
            return true;
        }

    } // namespace

    bool ConvertFbxScene(const char* fileName, Scene& outScene, AZStd::string& outError)
    {
        ufbx_load_opts options = {};
        // Convert to O3DE's up axis and to meters while loading. Geometry transforms and pivots are
        // baked into the geometry so the resulting scene graph contains only regular nodes.
        options.target_axes = ufbx_axes_right_handed_z_up;
        options.target_unit_meters = 1.0;
        options.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
        options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
        options.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
        options.pivot_handling = UFBX_PIVOT_HANDLING_ADJUST_TO_PIVOT;
        options.generate_missing_normals = true;
        options.normalize_normals = true;
        options.normalize_tangents = true;
        options.clean_skin_weights = true;
        options.load_external_files = true;
        // Missing texture files must not fail the whole import; the material importer reports them.
        options.ignore_missing_external_files = true;
        options.node_depth_limit = Converters::MaxNodeDepth;

        ufbx_error error;
        ufbx_scene* rawScene = ufbx_load_file(fileName, &options, &error);
        if (!rawScene)
        {
            outError = "ufbx could not parse the file";
            if (error.description.data && error.description.length > 0)
            {
                outError += AZStd::string::format(": %s", error.description.data);
            }
            return false;
        }

        FbxScenePtr scene(rawScene);
        if (!scene->root_node)
        {
            outError = "The FBX file does not contain a root scene node.";
            return false;
        }

        FbxConverter converter(scene.get(), outScene);
        return converter.Convert(outError);
    }

} // namespace UniversalSceneImporter
