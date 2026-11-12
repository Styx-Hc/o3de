/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "GltfSceneConverter.h"

#include <AzCore/Debug/Trace.h>
#include <AzCore/Math/MathUtils.h>
#include <AzCore/Math/Quaternion.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/sort.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/containers/unordered_set.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/limits.h>
#include <AzCore/std/string/conversions.h>
#include <SceneAPI/SceneCore/Utilities/Reporting.h>

#include "SceneConverterUtils.h"
#include "cgltf.h"

#include <cmath>
#include <cstring>
#include <memory>

namespace UniversalSceneImporter
{
    namespace
    {

        struct CgltfDataDeleter
        {
            void operator()(cgltf_data* data) const
            {
                if (data)
                {
                    cgltf_free(data);
                }
            }
        };

        using CgltfDataPtr = std::unique_ptr<cgltf_data, CgltfDataDeleter>;

        const cgltf_attribute* FindAttribute(
            const cgltf_attribute* attributes, cgltf_size attributeCount, cgltf_attribute_type type, cgltf_int index)
        {
            for (cgltf_size attributeIndex = 0; attributeIndex < attributeCount; ++attributeIndex)
            {
                if (attributes[attributeIndex].type == type && attributes[attributeIndex].index == index)
                {
                    return &attributes[attributeIndex];
                }
            }
            return nullptr;
        }

        AZStd::string DecodeUri(AZStd::string_view uri)
        {
            AZStd::string result;
            result.reserve(uri.size());
            for (size_t index = 0; index < uri.size(); ++index)
            {
                if (uri[index] == '%' && index + 2 < uri.size())
                {
                    auto hexValue = [](char value) -> int
                    {
                        if (value >= '0' && value <= '9')
                        {
                            return value - '0';
                        }
                        if (value >= 'a' && value <= 'f')
                        {
                            return 10 + value - 'a';
                        }
                        if (value >= 'A' && value <= 'F')
                        {
                            return 10 + value - 'A';
                        }
                        return -1;
                    };
                    const int high = hexValue(uri[index + 1]);
                    const int low = hexValue(uri[index + 2]);
                    if (high >= 0 && low >= 0)
                    {
                        result.push_back(static_cast<char>((high << 4) | low));
                        index += 2;
                        continue;
                    }
                }
                result.push_back(uri[index]);
            }
            return result;
        }

        bool DecodeBase64(AZStd::string_view input, AZStd::vector<AZ::u8>& out)
        {
            auto decode = [](char value) -> int
            {
                if (value >= 'A' && value <= 'Z')
                {
                    return value - 'A';
                }
                if (value >= 'a' && value <= 'z')
                {
                    return 26 + value - 'a';
                }
                if (value >= '0' && value <= '9')
                {
                    return 52 + value - '0';
                }
                if (value == '+' || value == '-')
                {
                    return 62;
                }
                if (value == '/' || value == '_')
                {
                    return 63;
                }
                return -1;
            };

            out.clear();
            out.reserve(input.size() * 3 / 4);
            uint32_t accumulator = 0;
            int bits = 0;
            for (char character : input)
            {
                if (character == '=' || character == '\r' || character == '\n' || character == ' ' || character == '\t')
                {
                    continue;
                }
                const int value = decode(character);
                if (value < 0)
                {
                    out.clear();
                    return false;
                }
                accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    out.push_back(static_cast<AZ::u8>((accumulator >> bits) & 0xFF));
                }
            }
            return true;
        }

        bool DecodeDataUri(
            AZStd::string_view uri, AZStd::vector<AZ::u8>& outData, AZStd::string& outExtension)
        {
            outData.clear();
            outExtension.clear();
            if (!uri.starts_with("data:"))
            {
                return false;
            }

            const size_t comma = uri.find(',');
            if (comma == AZStd::string_view::npos)
            {
                return false;
            }

            const AZStd::string_view header = uri.substr(5, comma - 5);
            const AZStd::string_view payload = uri.substr(comma + 1);

            const size_t mimeEnd = header.find(';');
            AZStd::string mime(header.substr(0, mimeEnd == AZStd::string_view::npos ? header.size() : mimeEnd));
            AZStd::to_lower(mime.begin(), mime.end());
            if (mime == "image/png")
            {
                outExtension = "png";
            }
            else if (mime == "image/jpeg")
            {
                outExtension = "jpg";
            }
            else if (mime == "image/webp")
            {
                outExtension = "webp";
            }
            else if (mime == "image/ktx2")
            {
                outExtension = "ktx2";
            }
            else if (mime == "image/vnd-ms.dds" || mime == "image/dds")
            {
                outExtension = "dds";
            }

            if (header.find(";base64") != AZStd::string_view::npos)
            {
                if (!DecodeBase64(payload, outData))
                {
                    return false;
                }
            }
            else
            {
                // Percent-encoded text payload; rare for images but legal.
                const AZStd::string decoded = DecodeUri(payload);
                outData.assign(decoded.begin(), decoded.end());
            }

            if (outExtension.empty())
            {
                outExtension = Converters::SniffImageExtension(outData.data(), outData.size());
            }
            return !outData.empty();
        }

        AZ::Matrix3x4 NodeLocalMatrix(const cgltf_node* node)
        {
            if (node->has_matrix)
            {
                return AZ::Matrix3x4::CreateFromColumnMajorFloat16(node->matrix);
            }

            const AZ::Vector3 translation = node->has_translation
                ? AZ::Vector3(node->translation[0], node->translation[1], node->translation[2])
                : AZ::Vector3::CreateZero();
            const AZ::Quaternion rotation = node->has_rotation
                ? AZ::Quaternion(node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]).GetNormalized()
                : AZ::Quaternion::CreateIdentity();
            const AZ::Vector3 scale = node->has_scale ? AZ::Vector3(node->scale[0], node->scale[1], node->scale[2])
                                                       : AZ::Vector3::CreateOne();

            AZ::Matrix3x4 matrix = AZ::Matrix3x4::CreateFromQuaternionAndTranslation(rotation, translation);
            matrix.MultiplyByScale(scale);
            return matrix;
        }

        struct NodeTransform
        {
            AZ::Vector3 m_translation = AZ::Vector3::CreateZero();
            AZ::Quaternion m_rotation = AZ::Quaternion::CreateIdentity();
            AZ::Vector3 m_scale = AZ::Vector3::CreateOne();

            AZ::Matrix3x4 ToMatrix() const
            {
                AZ::Matrix3x4 matrix = AZ::Matrix3x4::CreateFromQuaternionAndTranslation(m_rotation, m_translation);
                matrix.MultiplyByScale(m_scale);
                return matrix;
            }
        };

        NodeTransform GetBaseTransform(const cgltf_node* node)
        {
            NodeTransform result;
            if (node->has_matrix)
            {
                const AZ::Matrix3x4 matrix = AZ::Matrix3x4::CreateFromColumnMajorFloat16(node->matrix);
                result.m_translation = matrix.GetTranslation();
                result.m_rotation = AZ::Quaternion::CreateFromMatrix3x4(matrix).GetNormalized();
                AZ::Matrix3x4 scaleMatrix = matrix;
                result.m_scale = scaleMatrix.ExtractScale();
                return result;
            }

            if (node->has_translation)
            {
                result.m_translation = AZ::Vector3(node->translation[0], node->translation[1], node->translation[2]);
            }
            if (node->has_rotation)
            {
                result.m_rotation =
                    AZ::Quaternion(node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]).GetNormalized();
            }
            if (node->has_scale)
            {
                result.m_scale = AZ::Vector3(node->scale[0], node->scale[1], node->scale[2]);
            }
            return result;
        }

        bool ReadAccessorFloats(const cgltf_accessor* accessor, size_t components, AZStd::vector<float>& out)
        {
            out.clear();
            if (!accessor || components == 0)
            {
                return false;
            }

            const size_t requiredCount = static_cast<size_t>(accessor->count) * components;
            if (requiredCount == 0)
            {
                return true;
            }

            if (accessor->is_sparse)
            {
                const size_t accessorComponents = AZStd::max<size_t>(1, cgltf_num_components(accessor->type));
                const cgltf_size required = cgltf_accessor_unpack_floats(accessor, nullptr, 0);
                const size_t expected = static_cast<size_t>(accessor->count) * accessorComponents;
                if (required != expected)
                {
                    return false;
                }
                AZStd::vector<float> unpacked(required);
                if (cgltf_accessor_unpack_floats(accessor, unpacked.data(), required) != required)
                {
                    return false;
                }
                out.assign(requiredCount, 0.0f);
                const size_t copiedComponents = AZStd::min(components, accessorComponents);
                for (cgltf_size element = 0; element < accessor->count; ++element)
                {
                    for (size_t component = 0; component < copiedComponents; ++component)
                    {
                        out[element * components + component] = unpacked[element * accessorComponents + component];
                    }
                }
                return true;
            }

            out.resize(requiredCount);
            for (cgltf_size element = 0; element < accessor->count; ++element)
            {
                float values[16] = {};
                if (!cgltf_accessor_read_float(accessor, element, values, components))
                {
                    out.clear();
                    return false;
                }
                std::memcpy(out.data() + element * components, values, sizeof(float) * components);
            }
            return true;
        }

        bool ReadAccessorUints(const cgltf_accessor* accessor, size_t components, AZStd::vector<AZ::u32>& out)
        {
            out.clear();
            if (!accessor || components == 0)
            {
                return false;
            }
            if (accessor->is_sparse)
            {
                AZStd::vector<float> floats;
                if (!ReadAccessorFloats(accessor, components, floats))
                {
                    return false;
                }
                out.resize(floats.size());
                for (size_t index = 0; index < floats.size(); ++index)
                {
                    out[index] = static_cast<AZ::u32>(AZ::GetMax(0.0f, floats[index]) + 0.5f);
                }
                return true;
            }

            const size_t requiredCount = static_cast<size_t>(accessor->count) * components;
            out.resize(requiredCount);
            if (requiredCount == 0)
            {
                return true;
            }
            if (components == 1)
            {
                for (cgltf_size element = 0; element < accessor->count; ++element)
                {
                    out[element] = static_cast<AZ::u32>(cgltf_accessor_read_index(accessor, element));
                }
                return true;
            }

            for (cgltf_size element = 0; element < accessor->count; ++element)
            {
                cgltf_uint values[4] = {};
                if (!cgltf_accessor_read_uint(accessor, element, values, components))
                {
                    out.clear();
                    return false;
                }
                for (size_t component = 0; component < components; ++component)
                {
                    out[element * components + component] = values[component];
                }
            }
            return true;
        }

        bool ReadIndices(const cgltf_accessor* accessor, AZStd::vector<AZ::u32>& out)
        {
            out.clear();
            if (!accessor)
            {
                return false;
            }
            if (!accessor->is_sparse)
            {
                out.resize(accessor->count);
                if (accessor->count == 0)
                {
                    return true;
                }
                return cgltf_accessor_unpack_indices(accessor, out.data(), sizeof(AZ::u32), accessor->count) == accessor->count;
            }
            return ReadAccessorUints(accessor, 1, out);
        }

        class GltfConverter
        {
        public:
            GltfConverter(cgltf_data* data, Scene& output)
                : m_data(data)
                , m_scene(output)
            {
            }

            bool Convert(AZStd::string& outError);

        private:
            int AddNode(cgltf_node* node, int parentIndex, int depth = 0);
            int GetOrBuildMesh(cgltf_mesh* mesh, cgltf_skin* skin);
            bool BuildMesh(cgltf_mesh* mesh, cgltf_skin* skin, Mesh& outMesh);
            bool BuildPrimitive(
                cgltf_mesh* mesh,
                cgltf_primitive* primitive,
                cgltf_skin* skin,
                MeshPrimitive& outPrimitive);
            int GetMaterialIndex(cgltf_material* material);
            void ConvertMaterial(cgltf_material* material, Material& outMaterial);
            Texture MakeTexture(const cgltf_texture_view& view) const;
            Texture MakeTextureFromImage(cgltf_image* image) const;
            void MarkBoneHierarchy();
            void BuildSkins();
            void BuildAnimations();

            AZStd::string GetMorphTargetName(cgltf_mesh* mesh, size_t index) const;

            cgltf_data* m_data = nullptr;
            Scene& m_scene;
            AZStd::unordered_map<const cgltf_node*, int> m_nodeIndices;
            AZStd::unordered_map<const cgltf_material*, int> m_materialIndices;
            AZStd::unordered_map<const cgltf_mesh*, int> m_meshIndices;
            AZStd::unordered_map<const cgltf_skin*, int> m_skinIndices;
            AZStd::vector<cgltf_node*> m_cgltfNodes;
            bool m_hitNodeLimit = false;
        };

        int GltfConverter::AddNode(cgltf_node* node, int parentIndex, int depth)
        {
            if (!node || m_hitNodeLimit)
            {
                return -1;
            }
            if (depth > Converters::MaxNodeDepth)
            {
                AZ_Error(
                    "UniversalSceneImporter", false,
                    "glTF scene '%s' contains a node hierarchy deeper than %d levels; the subtree was skipped.",
                    m_scene.GetSourcePath().c_str(), Converters::MaxNodeDepth);
                return -1;
            }
            if (m_scene.GetNodes().size() >= Converters::MaxNodeCount)
            {
                m_hitNodeLimit = true;
                AZ_Error(
                    "UniversalSceneImporter", false, "glTF scene '%s' has more than %zu nodes and will not be imported.",
                    m_scene.GetSourcePath().c_str(), Converters::MaxNodeCount);
                return -1;
            }

            const auto existing = m_nodeIndices.find(node);
            if (existing != m_nodeIndices.end())
            {
                return existing->second;
            }

            const int nodeIndex = static_cast<int>(m_scene.GetNodes().size());
            m_scene.GetNodes().emplace_back();
            m_cgltfNodes.push_back(node);
            m_nodeIndices.emplace(node, nodeIndex);

            Node& outputNode = m_scene.GetNodes().back();
            outputNode.m_parent = parentIndex;
            outputNode.m_localTransform = Converters::ConvertGltfTransform(NodeLocalMatrix(node));

            // The glTF specification says the transform of a node that contains a skinned mesh is
            // ignored; only the joints and inverse bind matrices position the mesh. Honoring the
            // transform would offset the skinned mesh twice.
            if (node->skin && node->mesh)
            {
                if (node->children_count == 0)
                {
                    outputNode.m_localTransform = AZ::Matrix3x4::CreateIdentity();
                }
                else
                {
                    AZ_Warning(
                        "UniversalSceneImporter", false,
                        "Skinned mesh node '%s' has children. glTF ignores the transform of a skinned mesh node; "
                        "the transform is dropped so the mesh binds correctly, which may leave its children misplaced.",
                        Converters::ToAzString(node->name).c_str());
                }
            }

            AZStd::string name = Converters::ToAzString(node->name);
            if (name.empty())
            {
                name = parentIndex < 0 ? "RootNode" : AZStd::string::format("Node_%zu", m_scene.GetNodes().size() - 1);
            }
            outputNode.m_name = AZStd::move(name);

            for (cgltf_size childIndex = 0; childIndex < node->children_count; ++childIndex)
            {
                const int childNodeIndex = AddNode(node->children[childIndex], nodeIndex, depth + 1);
                if (childNodeIndex >= 0)
                {
                    m_scene.GetNodes()[nodeIndex].m_children.push_back(childNodeIndex);
                }
            }

            return nodeIndex;
        }

        AZStd::string GltfConverter::GetMorphTargetName(cgltf_mesh* mesh, size_t index) const
        {
            if (mesh && index < mesh->target_names_count && mesh->target_names[index] && mesh->target_names[index][0] != '\0')
            {
                return Converters::SanitizeNodeName(mesh->target_names[index], "MorphTarget");
            }
            return AZStd::string::format("MorphTarget_%zu", index);
        }

        Texture GltfConverter::MakeTextureFromImage(cgltf_image* image) const
        {
            Texture result;
            if (!image)
            {
                return result;
            }

            result.m_name = Converters::ToAzString(image->name);
            const AZStd::string mimeType = Converters::ToAzString(image->mime_type);
            if (mimeType == "image/png")
            {
                result.m_extension = "png";
            }
            else if (mimeType == "image/jpeg")
            {
                result.m_extension = "jpg";
            }
            else if (mimeType == "image/ktx2")
            {
                result.m_extension = "ktx2";
            }
            else if (mimeType == "image/webp")
            {
                result.m_extension = "webp";
            }

            if (image->buffer_view)
            {
                const AZ::u8* data = cgltf_buffer_view_data(image->buffer_view);
                if (data && image->buffer_view->size > 0)
                {
                    result.m_hasEmbeddedData = true;
                    result.m_embeddedData.assign(data, data + image->buffer_view->size);
                    if (result.m_extension.empty())
                    {
                        result.m_extension = Converters::SniffImageExtension(result.m_embeddedData.data(), result.m_embeddedData.size());
                    }
                }
            }
            else if (image->uri)
            {
                const AZStd::string_view uri = image->uri;
                if (uri.starts_with("data:"))
                {
                    AZStd::string dataUriExtension;
                    if (DecodeDataUri(uri, result.m_embeddedData, dataUriExtension))
                    {
                        result.m_hasEmbeddedData = true;
                        result.m_uri = "embedded";
                        if (result.m_extension.empty())
                        {
                            result.m_extension = dataUriExtension;
                        }
                    }
                }
                else
                {
                    result.m_uri = DecodeUri(uri);
                }
            }

            if (result.m_extension.empty())
            {
                result.m_extension = Converters::ExtractExtension(result.m_uri);
            }
            if (result.m_extension.empty())
            {
                result.m_extension = Converters::ExtractExtension(result.m_name);
            }
            if (result.m_extension.empty())
            {
                result.m_extension = "png";
            }
            if (result.m_name.empty())
            {
                result.m_name = Converters::ExtractExtension(result.m_uri).empty()
                    ? "Texture"
                    : AZStd::string::format("Texture.%s", result.m_extension.c_str());
            }

            return result;
        }

        Texture GltfConverter::MakeTexture(const cgltf_texture_view& view) const
        {
            if (!view.texture)
            {
                return Texture();
            }

            cgltf_image* image = view.texture->image;
            if (view.texture->has_basisu && view.texture->basisu_image)
            {
                image = view.texture->basisu_image;
            }
            else if (view.texture->has_webp && view.texture->webp_image)
            {
                image = view.texture->webp_image;
            }
            return MakeTextureFromImage(image);
        }

        void GltfConverter::ConvertMaterial(cgltf_material* material, Material& outMaterial)
        {
            outMaterial.m_name = Converters::ToAzString(material->name);
            if (outMaterial.m_name.empty())
            {
                outMaterial.m_name = "Material";
            }

            if (material->has_pbr_metallic_roughness)
            {
                const cgltf_pbr_metallic_roughness& pbr = material->pbr_metallic_roughness;
                outMaterial.m_baseColor = AZ::Vector3(pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2]);
                outMaterial.m_opacity = AZ::GetClamp(pbr.base_color_factor[3], 0.0f, 1.0f);
                outMaterial.m_metallic = AZ::GetClamp(pbr.metallic_factor, 0.0f, 1.0f);
                outMaterial.m_roughness = AZ::GetClamp(pbr.roughness_factor, 0.0f, 1.0f);
                outMaterial.m_diffuseColor = outMaterial.m_baseColor;

                outMaterial.GetTexture(MaterialTexture::BaseColor) = MakeTexture(pbr.base_color_texture);
                outMaterial.GetTexture(MaterialTexture::Diffuse) = outMaterial.GetTexture(MaterialTexture::BaseColor);
                // glTF packs metallic in the blue channel and roughness in the green channel of the
                // same texture. Point both texture slots at it so material conversion can pick channels.
                outMaterial.GetTexture(MaterialTexture::Metallic) = MakeTexture(pbr.metallic_roughness_texture);
                outMaterial.GetTexture(MaterialTexture::Roughness) = outMaterial.GetTexture(MaterialTexture::Metallic);
            }

            if (material->has_pbr_specular_glossiness)
            {
                const cgltf_pbr_specular_glossiness& specGloss = material->pbr_specular_glossiness;
                outMaterial.m_baseColor =
                    AZ::Vector3(specGloss.diffuse_factor[0], specGloss.diffuse_factor[1], specGloss.diffuse_factor[2]);
                outMaterial.m_diffuseColor = outMaterial.m_baseColor;
                outMaterial.m_opacity = AZ::GetClamp(specGloss.diffuse_factor[3], 0.0f, 1.0f);
                outMaterial.m_specularColor =
                    AZ::Vector3(specGloss.specular_factor[0], specGloss.specular_factor[1], specGloss.specular_factor[2]);
                outMaterial.m_roughness = AZ::GetClamp(1.0f - specGloss.glossiness_factor, 0.0f, 1.0f);
                outMaterial.GetTexture(MaterialTexture::BaseColor) = MakeTexture(specGloss.diffuse_texture);
                outMaterial.GetTexture(MaterialTexture::Diffuse) = outMaterial.GetTexture(MaterialTexture::BaseColor);
                outMaterial.GetTexture(MaterialTexture::Specular) = MakeTexture(specGloss.specular_glossiness_texture);
            }

            outMaterial.m_emissiveColor =
                AZ::Vector3(material->emissive_factor[0], material->emissive_factor[1], material->emissive_factor[2]);
            if (material->has_emissive_strength)
            {
                outMaterial.m_emissiveIntensity = material->emissive_strength.emissive_strength;
            }
            outMaterial.GetTexture(MaterialTexture::Emissive) = MakeTexture(material->emissive_texture);
            outMaterial.GetTexture(MaterialTexture::Normal) = MakeTexture(material->normal_texture);
            outMaterial.GetTexture(MaterialTexture::AmbientOcclusion) = MakeTexture(material->occlusion_texture);

            switch (material->alpha_mode)
            {
            case cgltf_alpha_mode_mask:
                outMaterial.m_alphaMasked = true;
                outMaterial.m_alphaCutoff = material->alpha_cutoff;
                break;
            case cgltf_alpha_mode_blend:
                outMaterial.m_opacity = AZStd::min(outMaterial.m_opacity, 1.0f);
                break;
            default:
                outMaterial.m_opacity = 1.0f;
                break;
            }

            if (material->unlit)
            {
                outMaterial.m_shininess = 0.0f;
            }
        }

        int GltfConverter::GetMaterialIndex(cgltf_material* material)
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

        bool GltfConverter::BuildPrimitive(
            cgltf_mesh* mesh,
            cgltf_primitive* primitive,
            cgltf_skin* skin,
            MeshPrimitive& outPrimitive)
        {
            if (primitive->type != cgltf_primitive_type_triangles && primitive->type != cgltf_primitive_type_triangle_strip &&
                primitive->type != cgltf_primitive_type_triangle_fan)
            {
                AZ_Warning(
                    "UniversalSceneImporter", false,
                    "Mesh '%s' contains a primitive of type %d which is not a triangle list, strip or fan; it will be skipped.",
                    Converters::ToAzString(mesh->name).c_str(), static_cast<int>(primitive->type));
                return false;
            }

            if (primitive->has_draco_mesh_compression)
            {
                AZ_Error(
                    "UniversalSceneImporter", false,
                    "Mesh '%s' uses Draco mesh compression which is not supported. Re-export the file without Draco compression.",
                    Converters::ToAzString(mesh->name).c_str());
                return false;
            }

            const cgltf_attribute* positionAttribute =
                FindAttribute(primitive->attributes, primitive->attributes_count, cgltf_attribute_type_position, 0);
            if (!positionAttribute || !positionAttribute->data)
            {
                AZ_Warning(
                    "UniversalSceneImporter", false, "Mesh '%s' contains a primitive without positions; it will be skipped.",
                    Converters::ToAzString(mesh->name).c_str());
                return false;
            }

            // EXT_meshopt_compression can be applied to any accessor of the primitive, including
            // the index buffer, so check every accessor that will be read.
            auto isMeshoptCompressed = [](const cgltf_accessor* accessor)
            {
                return accessor && accessor->buffer_view && accessor->buffer_view->has_meshopt_compression;
            };
            bool hasMeshoptCompression = isMeshoptCompressed(positionAttribute->data) ||
                isMeshoptCompressed(primitive->indices);
            for (cgltf_size attributeIndex = 0; attributeIndex < primitive->attributes_count && !hasMeshoptCompression;
                 ++attributeIndex)
            {
                hasMeshoptCompression = isMeshoptCompressed(primitive->attributes[attributeIndex].data);
            }
            for (cgltf_size targetIndex = 0; targetIndex < primitive->targets_count && !hasMeshoptCompression; ++targetIndex)
            {
                const cgltf_morph_target& target = primitive->targets[targetIndex];
                for (cgltf_size attributeIndex = 0; attributeIndex < target.attributes_count; ++attributeIndex)
                {
                    if (isMeshoptCompressed(target.attributes[attributeIndex].data))
                    {
                        hasMeshoptCompression = true;
                        break;
                    }
                }
            }
            if (hasMeshoptCompression)
            {
                AZ_Error(
                    "UniversalSceneImporter", false,
                    "Mesh '%s' uses EXT_meshopt_compression which is not supported. Re-export the file without meshopt compression.",
                    Converters::ToAzString(mesh->name).c_str());
                return false;
            }

            const cgltf_size vertexCount = positionAttribute->data->count;
            if (vertexCount == 0)
            {
                return false;
            }

            AZStd::vector<float> positions;
            if (!ReadAccessorFloats(positionAttribute->data, 3, positions))
            {
                AZ_Error(
                    "UniversalSceneImporter", false, "Failed to read the positions of mesh '%s'.",
                    Converters::ToAzString(mesh->name).c_str());
                return false;
            }

            outPrimitive.m_positions.resize(vertexCount);
            for (cgltf_size vertex = 0; vertex < vertexCount; ++vertex)
            {
                outPrimitive.m_positions[vertex] = Converters::ConvertGltfVector(
                    AZ::Vector3(positions[vertex * 3 + 0], positions[vertex * 3 + 1], positions[vertex * 3 + 2]));
            }

            const cgltf_attribute* normalAttribute =
                FindAttribute(primitive->attributes, primitive->attributes_count, cgltf_attribute_type_normal, 0);
            if (normalAttribute && normalAttribute->data && normalAttribute->data->count == vertexCount)
            {
                AZStd::vector<float> normals;
                if (ReadAccessorFloats(normalAttribute->data, 3, normals))
                {
                    outPrimitive.m_normals.resize(vertexCount);
                    for (cgltf_size vertex = 0; vertex < vertexCount; ++vertex)
                    {
                        AZ::Vector3 normal = Converters::ConvertGltfVector(
                            AZ::Vector3(normals[vertex * 3 + 0], normals[vertex * 3 + 1], normals[vertex * 3 + 2]));
                        normal.NormalizeSafe();
                        outPrimitive.m_normals[vertex] = normal;
                    }
                }
            }

            for (size_t set = 0; set < MaxUvSets; ++set)
            {
                const cgltf_attribute* uvAttribute = FindAttribute(
                    primitive->attributes, primitive->attributes_count, cgltf_attribute_type_texcoord, static_cast<cgltf_int>(set));
                if (!uvAttribute || !uvAttribute->data || uvAttribute->data->count != vertexCount)
                {
                    continue;
                }
                AZStd::vector<float> uvs;
                if (!ReadAccessorFloats(uvAttribute->data, 2, uvs))
                {
                    continue;
                }
                outPrimitive.m_uvSets[set].resize(vertexCount);
                for (cgltf_size vertex = 0; vertex < vertexCount; ++vertex)
                {
                    // glTF UVs already use a top-left origin, matching O3DE, so no V flip is needed.
                    outPrimitive.m_uvSets[set][vertex] = AZ::Vector2(uvs[vertex * 2 + 0], uvs[vertex * 2 + 1]);
                }
            }

            for (size_t set = 0; set < MaxColorSets; ++set)
            {
                const cgltf_attribute* colorAttribute = FindAttribute(
                    primitive->attributes, primitive->attributes_count, cgltf_attribute_type_color, static_cast<cgltf_int>(set));
                if (!colorAttribute || !colorAttribute->data || colorAttribute->data->count != vertexCount)
                {
                    continue;
                }
                AZStd::vector<float> colors;
                if (!ReadAccessorFloats(colorAttribute->data, 4, colors))
                {
                    continue;
                }
                outPrimitive.m_colorSets[set].resize(vertexCount);
                for (cgltf_size vertex = 0; vertex < vertexCount; ++vertex)
                {
                    const float alpha = colorAttribute->data->type == cgltf_type_vec4 ? colors[vertex * 4 + 3] : 1.0f;
                    outPrimitive.m_colorSets[set][vertex] =
                        AZ::Vector4(colors[vertex * 4 + 0], colors[vertex * 4 + 1], colors[vertex * 4 + 2], alpha);
                }
            }

            const cgltf_attribute* tangentAttribute =
                FindAttribute(primitive->attributes, primitive->attributes_count, cgltf_attribute_type_tangent, 0);
            if (tangentAttribute && tangentAttribute->data && tangentAttribute->data->count == vertexCount &&
                tangentAttribute->data->type == cgltf_type_vec4)
            {
                AZStd::vector<float> tangents;
                if (ReadAccessorFloats(tangentAttribute->data, 4, tangents))
                {
                    outPrimitive.m_tangents.resize(vertexCount);
                    for (cgltf_size vertex = 0; vertex < vertexCount; ++vertex)
                    {
                        const AZ::Vector3 tangent = Converters::ConvertGltfVector(
                            AZ::Vector3(tangents[vertex * 4 + 0], tangents[vertex * 4 + 1], tangents[vertex * 4 + 2]));
                        outPrimitive.m_tangents[vertex] =
                            AZ::Vector4(tangent.GetX(), tangent.GetY(), tangent.GetZ(), tangents[vertex * 4 + 3] < 0.0f ? -1.0f : 1.0f);
                    }
                }
            }

            // Indices. Non indexed primitives are expanded to a sequential triangle list.
            if (primitive->indices)
            {
                if (!ReadIndices(primitive->indices, outPrimitive.m_indices))
                {
                    AZ_Error(
                        "UniversalSceneImporter", false, "Failed to read the indices of mesh '%s'.",
                        Converters::ToAzString(mesh->name).c_str());
                    return false;
                }
            }
            else
            {
                outPrimitive.m_indices.resize(vertexCount);
                for (cgltf_size vertex = 0; vertex < vertexCount; ++vertex)
                {
                    outPrimitive.m_indices[vertex] = static_cast<AZ::u32>(vertex);
                }
            }

            if (primitive->type == cgltf_primitive_type_triangle_strip)
            {
                AZStd::vector<AZ::u32> strip = AZStd::move(outPrimitive.m_indices);
                outPrimitive.m_indices.clear();
                Converters::AppendTriangleStrip(strip, outPrimitive.m_indices);
            }
            else if (primitive->type == cgltf_primitive_type_triangle_fan)
            {
                AZStd::vector<AZ::u32> fan = AZStd::move(outPrimitive.m_indices);
                outPrimitive.m_indices.clear();
                Converters::AppendTriangleFan(fan, outPrimitive.m_indices);
            }
            outPrimitive.m_indices.resize(outPrimitive.m_indices.size() - (outPrimitive.m_indices.size() % 3));

            if (outPrimitive.m_normals.empty())
            {
                // glTF does not require normals. Generate smooth normals so the mesh is renderable.
                AZ_Warning(
                    "UniversalSceneImporter", false, "Mesh '%s' has no normals; smooth normals were generated.",
                    Converters::ToAzString(mesh->name).c_str());
                Converters::GenerateSmoothNormals(
                    outPrimitive.m_positions, outPrimitive.m_indices, outPrimitive.m_normals);
            }

            if (outPrimitive.m_tangents.empty() && !outPrimitive.m_uvSets[0].empty())
            {
                Converters::GenerateTangents(
                    outPrimitive.m_positions, outPrimitive.m_normals, outPrimitive.m_uvSets[0], outPrimitive.m_indices,
                    outPrimitive.m_tangents);
            }
            if (!outPrimitive.m_tangents.empty())
            {
                outPrimitive.m_bitangents.resize(vertexCount);
                for (cgltf_size vertex = 0; vertex < vertexCount; ++vertex)
                {
                    const AZ::Vector3 normal = vertex < outPrimitive.m_normals.size() ? outPrimitive.m_normals[vertex]
                                                                                     : AZ::Vector3::CreateAxisZ();
                    const AZ::Vector4& tangent = outPrimitive.m_tangents[vertex];
                    const AZ::Vector3 bitangent =
                        normal.Cross(AZ::Vector3(tangent.GetX(), tangent.GetY(), tangent.GetZ())) * tangent.GetW();
                    outPrimitive.m_bitangents[vertex] = bitangent;
                }
            }

            outPrimitive.m_material = primitive->material ? GetMaterialIndex(primitive->material) : -1;

            // Morph targets. Empty targets keep their slot so target indices stay aligned
            // with the morph animation channels.
            outPrimitive.m_morphTargets.resize(primitive->targets_count);
            for (cgltf_size targetIndex = 0; targetIndex < primitive->targets_count; ++targetIndex)
            {
                MorphTarget& target = outPrimitive.m_morphTargets[targetIndex];
                target.m_name = GetMorphTargetName(mesh, targetIndex);
                target.m_positionDeltas.assign(vertexCount, AZ::Vector3::CreateZero());
                target.m_normalDeltas.assign(vertexCount, AZ::Vector3::CreateZero());

                const cgltf_morph_target& sourceTarget = primitive->targets[targetIndex];
                auto readDelta = [&](cgltf_attribute_type type, size_t components, AZStd::vector<AZ::Vector3>& out)
                {
                    const cgltf_attribute* attribute = FindAttribute(sourceTarget.attributes, sourceTarget.attributes_count, type, 0);
                    if (!attribute || !attribute->data || attribute->data->count != vertexCount)
                    {
                        return;
                    }
                    AZStd::vector<float> values;
                    if (!ReadAccessorFloats(attribute->data, components, values))
                    {
                        return;
                    }
                    for (cgltf_size vertex = 0; vertex < vertexCount; ++vertex)
                    {
                        out[vertex] = Converters::ConvertGltfVector(
                            AZ::Vector3(values[vertex * components + 0], values[vertex * components + 1], values[vertex * components + 2]));
                    }
                };

                readDelta(cgltf_attribute_type_position, 3, target.m_positionDeltas);
                readDelta(cgltf_attribute_type_normal, 3, target.m_normalDeltas);
            }

            if (skin && primitive->attributes)
            {
                bool hasSkinAttributes = false;
                AZStd::vector<AZStd::vector<SkinInfluence>> influences(vertexCount);
                for (int set = 0; set < 4; ++set)
                {
                    const cgltf_attribute* jointsAttribute = FindAttribute(
                        primitive->attributes, primitive->attributes_count, cgltf_attribute_type_joints, set);
                    const cgltf_attribute* weightsAttribute = FindAttribute(
                        primitive->attributes, primitive->attributes_count, cgltf_attribute_type_weights, set);
                    if (!jointsAttribute || !weightsAttribute || !jointsAttribute->data || !weightsAttribute->data)
                    {
                        continue;
                    }
                    if (jointsAttribute->data->count != vertexCount || weightsAttribute->data->count != vertexCount)
                    {
                        continue;
                    }
                    hasSkinAttributes = true;

                    AZStd::vector<AZ::u32> joints;
                    AZStd::vector<float> weights;
                    if (!ReadAccessorUints(jointsAttribute->data, 4, joints) ||
                        !ReadAccessorFloats(weightsAttribute->data, 4, weights))
                    {
                        continue;
                    }

                    for (cgltf_size vertex = 0; vertex < vertexCount; ++vertex)
                    {
                        for (size_t influenceIndex = 0; influenceIndex < 4; ++influenceIndex)
                        {
                            const AZ::u32 jointSlot = joints[vertex * 4 + influenceIndex];
                            const float weight = weights[vertex * 4 + influenceIndex];
                            if (weight <= 0.0f || jointSlot >= skin->joints_count)
                            {
                                continue;
                            }
                            const auto jointNode = m_nodeIndices.find(skin->joints[jointSlot]);
                            if (jointNode == m_nodeIndices.end())
                            {
                                continue;
                            }
                            influences[vertex].push_back({ static_cast<AZ::u32>(jointNode->second), weight });
                        }
                    }
                }

                if (hasSkinAttributes)
                {
                    outPrimitive.m_skinInfluences.resize(vertexCount);
                    for (cgltf_size vertex = 0; vertex < vertexCount; ++vertex)
                    {
                        AZStd::vector<SkinInfluence>& vertexInfluences = influences[vertex];
                        if (vertexInfluences.size() > MaxSkinInfluences)
                        {
                            AZStd::sort(
                                vertexInfluences.begin(), vertexInfluences.end(),
                                [](const SkinInfluence& lhs, const SkinInfluence& rhs) { return lhs.m_weight > rhs.m_weight; });
                            vertexInfluences.resize(MaxSkinInfluences);
                        }
                        float totalWeight = 0.0f;
                        for (const SkinInfluence& influence : vertexInfluences)
                        {
                            totalWeight += influence.m_weight;
                        }
                        if (totalWeight > 0.0f)
                        {
                            for (SkinInfluence& influence : vertexInfluences)
                            {
                                influence.m_weight /= totalWeight;
                            }
                        }
                        outPrimitive.m_skinInfluences[vertex] = AZStd::move(vertexInfluences);
                    }
                }
            }

            // Skinned meshes need a complete tangent frame downstream. Generate one when the
            // source file does not provide tangents.
            if (skin && !outPrimitive.m_skinInfluences.empty())
            {
                Converters::EnsureTangentStreams(outPrimitive);
            }

            return !outPrimitive.m_positions.empty() && !outPrimitive.m_indices.empty();
        }

        bool GltfConverter::BuildMesh(cgltf_mesh* mesh, cgltf_skin* skin, Mesh& outMesh)
        {
            outMesh.m_name = Converters::ToAzString(mesh->name);
            if (outMesh.m_name.empty())
            {
                outMesh.m_name = "Mesh";
            }

            for (cgltf_size primitiveIndex = 0; primitiveIndex < mesh->primitives_count; ++primitiveIndex)
            {
                cgltf_primitive* primitive = &mesh->primitives[primitiveIndex];
                outMesh.m_primitives.emplace_back();
                if (!BuildPrimitive(mesh, primitive, skin, outMesh.m_primitives.back()))
                {
                    outMesh.m_primitives.pop_back();
                }
            }

            return !outMesh.m_primitives.empty();
        }

        int GltfConverter::GetOrBuildMesh(cgltf_mesh* mesh, cgltf_skin* skin)
        {
            if (!mesh)
            {
                return -1;
            }

            // Only skinned meshes depend on the skin, and skinning data is baked into the primitive
            // data, so unskinned meshes can be shared between instances.
            if (!skin)
            {
                const auto existing = m_meshIndices.find(mesh);
                if (existing != m_meshIndices.end())
                {
                    return existing->second;
                }
            }

            Mesh outputMesh;
            if (!BuildMesh(mesh, skin, outputMesh))
            {
                return -1;
            }

            const int meshIndex = static_cast<int>(m_scene.GetMeshes().size());
            m_scene.GetMeshes().push_back(AZStd::move(outputMesh));
            if (!skin)
            {
                m_meshIndices.emplace(mesh, meshIndex);
            }
            return meshIndex;
        }

        void GltfConverter::MarkBoneHierarchy()
        {
            for (cgltf_size skinIndex = 0; skinIndex < m_data->skins_count; ++skinIndex)
            {
                cgltf_skin* skin = &m_data->skins[skinIndex];
                for (cgltf_size jointIndex = 0; jointIndex < skin->joints_count; ++jointIndex)
                {
                    const auto found = m_nodeIndices.find(skin->joints[jointIndex]);
                    if (found != m_nodeIndices.end())
                    {
                        m_scene.GetNodes()[found->second].m_isBone = true;
                    }
                }
            }

            Converters::MarkBoneAncestors(m_scene.GetNodes());
        }

        void GltfConverter::BuildSkins()
        {
            for (size_t nodeIndex = 0; nodeIndex < m_scene.GetNodes().size(); ++nodeIndex)
            {
                cgltf_node* node = m_cgltfNodes[nodeIndex];
                if (!node || !node->skin)
                {
                    continue;
                }

                const auto existing = m_skinIndices.find(node->skin);
                if (existing != m_skinIndices.end())
                {
                    m_scene.GetNodes()[nodeIndex].m_skin = existing->second;
                    continue;
                }

                Skin skin;
                skin.m_joints.reserve(node->skin->joints_count);
                for (cgltf_size jointIndex = 0; jointIndex < node->skin->joints_count; ++jointIndex)
                {
                    const auto found = m_nodeIndices.find(node->skin->joints[jointIndex]);
                    if (found != m_nodeIndices.end())
                    {
                        skin.m_joints.push_back(found->second);
                    }
                    else
                    {
                        AZ_Warning(
                            "UniversalSceneImporter", false,
                            "Skin joint '%s' is not part of the imported scene; its influences will be dropped.",
                            Converters::ToAzString(node->skin->joints[jointIndex] ? node->skin->joints[jointIndex]->name : nullptr)
                                .c_str());
                    }
                }

                if (node->skin->inverse_bind_matrices)
                {
                    AZStd::vector<float> matrices;
                    if (ReadAccessorFloats(node->skin->inverse_bind_matrices, 16, matrices))
                    {
                        const size_t matrixCount = matrices.size() / 16;
                        skin.m_inverseBindMatrices.resize(matrixCount);
                        for (size_t matrixIndex = 0; matrixIndex < matrixCount; ++matrixIndex)
                        {
                            skin.m_inverseBindMatrices[matrixIndex] =
                                AZ::Matrix3x4::CreateFromColumnMajorFloat16(matrices.data() + matrixIndex * 16);
                        }
                    }
                }

                const int skinIndex = static_cast<int>(m_scene.GetSkins().size());
                m_scene.GetSkins().push_back(AZStd::move(skin));
                m_skinIndices.emplace(node->skin, skinIndex);
                m_scene.GetNodes()[nodeIndex].m_skin = skinIndex;
            }
        }

        struct AnimationSample
        {
            AZStd::vector<float> m_times;
            AZStd::vector<float> m_values;
            cgltf_interpolation_type m_interpolation = cgltf_interpolation_type_linear;
            //! Number of values stored per key (for example 3 for translation, 4 for rotation,
            //! or the number of morph targets for weights animation).
            size_t m_valuesPerKey = 1;

            bool IsValid() const
            {
                return !m_times.empty() && !m_values.empty();
            }
        };

        bool ReadAnimationSample(
            const cgltf_animation_sampler* sampler,
            size_t valuesPerKey,
            size_t accessorComponents,
            AnimationSample& outSample)
        {
            if (!sampler || !sampler->input || !sampler->output || valuesPerKey == 0 || accessorComponents == 0)
            {
                return false;
            }
            outSample.m_interpolation = sampler->interpolation;
            outSample.m_valuesPerKey = valuesPerKey;
            if (!ReadAccessorFloats(sampler->input, 1, outSample.m_times))
            {
                return false;
            }
            if (!ReadAccessorFloats(sampler->output, accessorComponents, outSample.m_values))
            {
                return false;
            }

            // The output must provide at least one value per key (and three per key for cubic
            // spline interpolated channels).
            const size_t valuesPerKeyWithTangents = valuesPerKey * (sampler->interpolation == cgltf_interpolation_type_cubic_spline ? 3 : 1);
            if (outSample.m_times.empty() || outSample.m_values.size() < outSample.m_times.size() * valuesPerKeyWithTangents)
            {
                return false;
            }
            return true;
        }

        struct AnimationSampleScratch
        {
            AZStd::vector<float> m_result;
            AZStd::vector<float> m_value0;
            AZStd::vector<float> m_value1;
            AZStd::vector<float> m_tangent0;
            AZStd::vector<float> m_tangent1;
        };

        template<typename SampleValueFn>
        void SampleAnimationChannel(
            const AnimationSample& sample, float time, AnimationSampleScratch& scratch, SampleValueFn&& sampleValue)
        {
            if (!sample.IsValid())
            {
                return;
            }

            const AZStd::vector<float>& times = sample.m_times;
            size_t key = 0;
            if (time <= times.front())
            {
                key = 0;
            }
            else if (time >= times.back())
            {
                key = times.size() - 1;
            }
            else
            {
                key = static_cast<size_t>(AZStd::upper_bound(times.begin(), times.end(), time) - times.begin()) - 1;
            }
            const size_t nextKey = AZStd::min(key + 1, times.size() - 1);
            const float t0 = times[key];
            const float t1 = times[nextKey];
            const float alpha = (t1 > t0) ? AZ::GetClamp((time - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;

            const bool cubic = sample.m_interpolation == cgltf_interpolation_type_cubic_spline;
            const bool step = sample.m_interpolation == cgltf_interpolation_type_step;
            const size_t valuesPerKey = sample.m_valuesPerKey;
            const size_t stride = valuesPerKey * (cubic ? 3 : 1);
            // Layout for cubic spline keys is (in-tangent, value, out-tangent); other
            // interpolations store the value directly.
            const size_t valueOffset = cubic ? valuesPerKey : 0;

            auto readBlock = [&](size_t keyIndex, size_t offset, float* out)
            {
                const size_t base = keyIndex * stride + offset;
                for (size_t valueIndex = 0; valueIndex < valuesPerKey; ++valueIndex)
                {
                    out[valueIndex] = (base + valueIndex < sample.m_values.size()) ? sample.m_values[base + valueIndex] : 0.0f;
                }
            };

            AZStd::vector<float>& result = scratch.m_result;
            AZStd::vector<float>& value0 = scratch.m_value0;
            AZStd::vector<float>& value1 = scratch.m_value1;
            result.assign(valuesPerKey, 0.0f);
            value0.assign(valuesPerKey, 0.0f);
            value1.assign(valuesPerKey, 0.0f);
            readBlock(key, valueOffset, value0.data());
            readBlock(nextKey, valueOffset, value1.data());

            if (step || nextKey == key)
            {
                result = value0;
            }
            else if (!cubic)
            {
                for (size_t valueIndex = 0; valueIndex < valuesPerKey; ++valueIndex)
                {
                    result[valueIndex] = AZ::Lerp(value0[valueIndex], value1[valueIndex], alpha);
                }
            }
            else
            {
                AZStd::vector<float>& tangent0 = scratch.m_tangent0;
                AZStd::vector<float>& tangent1 = scratch.m_tangent1;
                tangent0.assign(valuesPerKey, 0.0f);
                tangent1.assign(valuesPerKey, 0.0f);
                readBlock(key, valuesPerKey * 2, tangent0.data());
                readBlock(nextKey, 0, tangent1.data());
                const float delta = t1 - t0;
                const float alpha2 = alpha * alpha;
                const float alpha3 = alpha2 * alpha;
                const float a = 2.0f * alpha3 - 3.0f * alpha2 + 1.0f;
                const float b = alpha3 - 2.0f * alpha2 + alpha;
                const float c = -2.0f * alpha3 + 3.0f * alpha2;
                const float d = alpha3 - alpha2;
                for (size_t valueIndex = 0; valueIndex < valuesPerKey; ++valueIndex)
                {
                    result[valueIndex] = a * value0[valueIndex] + b * delta * tangent0[valueIndex] + c * value1[valueIndex] +
                        d * delta * tangent1[valueIndex];
                }
            }

            sampleValue(result.data());
        }

        void GltfConverter::BuildAnimations()
        {
            for (cgltf_size animationIndex = 0; animationIndex < m_data->animations_count; ++animationIndex)
            {
                cgltf_animation* sourceAnimation = &m_data->animations[animationIndex];
                if (!sourceAnimation || sourceAnimation->channels_count == 0)
                {
                    continue;
                }

                float startTime = AZStd::numeric_limits<float>::max();
                float endTime = -AZStd::numeric_limits<float>::max();
                for (cgltf_size samplerIndex = 0; samplerIndex < sourceAnimation->samplers_count; ++samplerIndex)
                {
                    const cgltf_animation_sampler& sampler = sourceAnimation->samplers[samplerIndex];
                    AZStd::vector<float> times;
                    if (sampler.input && ReadAccessorFloats(sampler.input, 1, times) && !times.empty())
                    {
                        startTime = AZStd::min(startTime, times.front());
                        endTime = AZStd::max(endTime, times.back());
                    }
                }
                if (!(endTime > startTime))
                {
                    AZ_Warning(
                        "UniversalSceneImporter", false, "Animation '%s' has a non positive duration and will be skipped.",
                        Converters::ToAzString(sourceAnimation->name).c_str());
                    continue;
                }

                const float duration = endTime - startTime;
                const float timeStep = static_cast<float>(Converters::DefaultAnimationTimeStep);
                size_t frameCount = static_cast<size_t>(std::ceil(duration / timeStep)) + 1;
                frameCount = AZ::GetClamp<size_t>(frameCount, 2, Converters::MaxAnimationFrames);

                Animation animation;
                animation.m_name = Converters::ToAzString(sourceAnimation->name);
                if (animation.m_name.empty())
                {
                    animation.m_name = AZStd::string::format("Animation_%zu", animationIndex);
                }
                animation.m_timeStep = timeStep;

                // Group translation/rotation/scale channels by their target node.
                struct NodeChannels
                {
                    cgltf_node* m_node = nullptr;
                    const cgltf_animation_sampler* m_translation = nullptr;
                    const cgltf_animation_sampler* m_rotation = nullptr;
                    const cgltf_animation_sampler* m_scale = nullptr;
                };
                AZStd::vector<NodeChannels> nodeChannels;
                auto findOrAddNodeChannels = [&nodeChannels](cgltf_node* node) -> NodeChannels&
                {
                    for (NodeChannels& channels : nodeChannels)
                    {
                        if (channels.m_node == node)
                        {
                            return channels;
                        }
                    }
                    nodeChannels.emplace_back();
                    nodeChannels.back().m_node = node;
                    return nodeChannels.back();
                };

                for (cgltf_size channelIndex = 0; channelIndex < sourceAnimation->channels_count; ++channelIndex)
                {
                    cgltf_animation_channel* channel = &sourceAnimation->channels[channelIndex];
                    if (!channel->target_node || !channel->sampler)
                    {
                        continue;
                    }

                    if (channel->target_path == cgltf_animation_path_type_weights)
                    {
                        // Morph animation is handled per mesh below.
                        continue;
                    }

                    NodeChannels& channels = findOrAddNodeChannels(channel->target_node);
                    switch (channel->target_path)
                    {
                    case cgltf_animation_path_type_translation:
                        channels.m_translation = channel->sampler;
                        break;
                    case cgltf_animation_path_type_rotation:
                        channels.m_rotation = channel->sampler;
                        break;
                    case cgltf_animation_path_type_scale:
                        channels.m_scale = channel->sampler;
                        break;
                    default:
                        break;
                    }
                }

                for (const NodeChannels& channels : nodeChannels)
                {
                    const auto found = m_nodeIndices.find(channels.m_node);
                    if (found == m_nodeIndices.end())
                    {
                        continue;
                    }
                    const int nodeIndex = found->second;
                    if (!m_scene.GetNodes()[nodeIndex].m_isBone)
                    {
                        continue;
                    }

                    NodeAnimation nodeAnimation;
                    nodeAnimation.m_node = nodeIndex;
                    nodeAnimation.m_keyFrames.reserve(frameCount);

                    const NodeTransform baseTransform = GetBaseTransform(channels.m_node);
                    AnimationSample translationSample;
                    AnimationSample rotationSample;
                    AnimationSample scaleSample;
                    ReadAnimationSample(channels.m_translation, 3, 3, translationSample);
                    ReadAnimationSample(channels.m_rotation, 4, 4, rotationSample);
                    ReadAnimationSample(channels.m_scale, 3, 3, scaleSample);

                    AnimationSampleScratch scratch;
                    for (size_t frame = 0; frame < frameCount; ++frame)
                    {
                        const float time = startTime + static_cast<float>(frame) * timeStep;
                        NodeTransform transform = baseTransform;

                        if (translationSample.IsValid())
                        {
                            SampleAnimationChannel(
                                translationSample, time, scratch,
                                [&transform](const float* value)
                                {
                                    transform.m_translation = AZ::Vector3(value[0], value[1], value[2]);
                                });
                        }
                        if (rotationSample.IsValid())
                        {
                            SampleAnimationChannel(
                                rotationSample, time, scratch,
                                [&transform](const float* value)
                                {
                                    transform.m_rotation =
                                        AZ::Quaternion(value[0], value[1], value[2], value[3]).GetNormalized();
                                });
                        }
                        if (scaleSample.IsValid())
                        {
                            SampleAnimationChannel(
                                scaleSample, time, scratch,
                                [&transform](const float* value)
                                {
                                    transform.m_scale = AZ::Vector3(value[0], value[1], value[2]);
                                });
                        }

                        nodeAnimation.m_keyFrames.push_back(Converters::ConvertGltfTransform(transform.ToMatrix()));
                    }
                    animation.m_nodeAnimations.push_back(AZStd::move(nodeAnimation));
                }

                for (cgltf_size channelIndex = 0; channelIndex < sourceAnimation->channels_count; ++channelIndex)
                {
                    cgltf_animation_channel* channel = &sourceAnimation->channels[channelIndex];
                    if (!channel->target_node || !channel->sampler || channel->target_path != cgltf_animation_path_type_weights)
                    {
                        continue;
                    }
                    const auto found = m_nodeIndices.find(channel->target_node);
                    if (found == m_nodeIndices.end())
                    {
                        continue;
                    }
                    const int nodeIndex = found->second;
                    const int meshIndex = m_scene.GetNodes()[nodeIndex].m_mesh;
                    const Mesh* outputMesh = m_scene.GetMesh(meshIndex);
                    if (!outputMesh || outputMesh->m_primitives.empty())
                    {
                        continue;
                    }

                    cgltf_mesh* sourceMesh = channel->target_node->mesh;
                    if (!sourceMesh)
                    {
                        continue;
                    }
                    const size_t targetCount = sourceMesh->target_names_count;
                    if (targetCount == 0)
                    {
                        continue;
                    }

                    AnimationSample weightsSample;
                    if (!ReadAnimationSample(channel->sampler, targetCount, 1, weightsSample))
                    {
                        continue;
                    }

                    for (size_t targetIndex = 0; targetIndex < targetCount; ++targetIndex)
                    {
                        MorphAnimation morphAnimation;
                        morphAnimation.m_node = nodeIndex;
                        morphAnimation.m_mesh = meshIndex;
                        morphAnimation.m_morphTarget = static_cast<int>(targetIndex);
                        morphAnimation.m_blendShapeName = GetMorphTargetName(sourceMesh, targetIndex);
                        morphAnimation.m_keyFrames.reserve(frameCount);
                        AnimationSampleScratch scratch;
                        for (size_t frame = 0; frame < frameCount; ++frame)
                        {
                            const float time = startTime + static_cast<float>(frame) * timeStep;
                            float sampledWeight = 0.0f;
                            SampleAnimationChannel(
                                weightsSample, time, scratch,
                                [&sampledWeight, targetIndex](const float* value)
                                {
                                    sampledWeight = value[targetIndex];
                                });
                            morphAnimation.m_keyFrames.push_back(sampledWeight);
                        }
                        animation.m_morphAnimations.push_back(AZStd::move(morphAnimation));
                    }
                }

                if (!animation.m_nodeAnimations.empty() || !animation.m_morphAnimations.empty())
                {
                    m_scene.GetAnimations().push_back(AZStd::move(animation));
                }
            }
        }

        bool GltfConverter::Convert(AZStd::string& outError)
        {
            if (m_data->scenes_count == 0)
            {
                AZ_Warning(
                    "UniversalSceneImporter", false, "glTF scene '%s' does not define any scenes; importing all root nodes.",
                    m_scene.GetSourcePath().c_str());
            }

            cgltf_scene* sourceScene = m_data->scene;
            if (!sourceScene && m_data->scenes_count > 0)
            {
                sourceScene = &m_data->scenes[0];
                AZ_Warning(
                    "UniversalSceneImporter", false,
                    "glTF scene '%s' does not define a default scene; scene 0 will be imported.", m_scene.GetSourcePath().c_str());
            }
            if (m_data->scenes_count > 1)
            {
                AZ_Warning(
                    "UniversalSceneImporter", false,
                    "glTF file '%s' contains %zu scenes; only the default scene is imported.", m_scene.GetSourcePath().c_str(),
                    m_data->scenes_count);
            }

            // Build the root. If the scene has a single root node, use it directly so the imported
            // hierarchy matches the file. Otherwise create a synthetic root that contains the roots.
            cgltf_node* singleRoot = nullptr;
            AZStd::vector<cgltf_node*> roots;
            if (sourceScene)
            {
                for (cgltf_size index = 0; index < sourceScene->nodes_count; ++index)
                {
                    if (sourceScene->nodes[index])
                    {
                        roots.push_back(sourceScene->nodes[index]);
                    }
                }
            }
            else if (m_data->scenes_count == 0)
            {
                for (cgltf_size index = 0; index < m_data->nodes_count; ++index)
                {
                    if (m_data->nodes[index].parent == nullptr)
                    {
                        roots.push_back(&m_data->nodes[index]);
                    }
                }
            }

            if (roots.empty())
            {
                outError = "The glTF file does not contain any scene nodes.";
                return false;
            }
            if (roots.size() == 1)
            {
                singleRoot = roots[0];
            }

            if (singleRoot)
            {
                if (AddNode(singleRoot, -1) != 0)
                {
                    outError = "Failed to build the glTF node hierarchy.";
                    return false;
                }
            }
            else
            {
                m_scene.GetNodes().emplace_back();
                m_cgltfNodes.push_back(nullptr);
                Node& root = m_scene.GetNodes().back();
                root.m_name = "RootNode";
                root.m_parent = -1;
                root.m_isRoot = true;
                for (cgltf_node* rootNode : roots)
                {
                    const int childIndex = AddNode(rootNode, 0);
                    if (childIndex >= 0)
                    {
                        m_scene.GetNodes()[0].m_children.push_back(childIndex);
                    }
                }
            }

            if (m_hitNodeLimit)
            {
                outError = "The glTF scene is too large to import.";
                return false;
            }

            MarkBoneHierarchy();
            BuildSkins();

            for (size_t nodeIndex = 0; nodeIndex < m_scene.GetNodes().size(); ++nodeIndex)
            {
                cgltf_node* node = m_cgltfNodes[nodeIndex];
                if (!node)
                {
                    continue;
                }
                const int meshIndex = GetOrBuildMesh(node->mesh, node->skin);
                m_scene.GetNodes()[nodeIndex].m_mesh = meshIndex;
                if (meshIndex >= 0 && node->skin)
                {
                    // Skinned mesh: glTF skins vertices in world space directly (the mesh
                    // node's transform is ignored for skinned primitives), so the vertices stay
                    // in mesh-local space and the model builder's single world bake must cancel
                    // the ancestor transforms. The override makes the node's graph world
                    // identity; the skin's authored inverse binds (kept verbatim by
                    // BuildSkins) carry the bind placement. Without the override the skin mesh
                    // importer baked the full node world and transformed ancestors ended up
                    // applied twice, deforming the mesh the moment anything animated.
                    Node& meshNode = m_scene.GetNodes()[nodeIndex];
                    const AZ::Matrix3x4 ancestorWorld = m_scene.GetNodeWorldTransform(meshNode.m_parent);
                    meshNode.m_hasLocalOverride = true;
                    meshNode.m_localOverride = Converters::IsInvertible(ancestorWorld)
                        ? ancestorWorld.GetInverseFull()
                        : AZ::Matrix3x4::CreateIdentity();
                }
                if (meshIndex < 0 && node->mesh)
                {
                    AZ_Warning(
                        "UniversalSceneImporter", false, "Node '%s' references mesh '%s' which could not be imported.",
                        m_scene.GetNodes()[nodeIndex].m_name.c_str(), Converters::ToAzString(node->mesh->name).c_str());
                }
            }

            BuildAnimations();
            return true;
        }

    } // namespace

    bool ConvertGltfScene(const char* fileName, Scene& outScene, AZStd::string& outError)
    {
        cgltf_options options = {};
        cgltf_data* rawData = nullptr;
        cgltf_result result = cgltf_parse_file(&options, fileName, &rawData);
        if (result != cgltf_result_success || !rawData)
        {
            outError = AZStd::string::format("cgltf could not parse the file (result %d).", static_cast<int>(result));
            return false;
        }

        CgltfDataPtr data(rawData);
        result = cgltf_load_buffers(&options, data.get(), fileName);
        if (result != cgltf_result_success)
        {
            outError = AZStd::string::format(
                "One or more buffers referenced by the glTF file could not be loaded (result %d). "
                "If the file uses external .bin files, make sure they are next to the scene file and processed by the asset processor.",
                static_cast<int>(result));
            return false;
        }

        // cgltf_load_buffers can succeed while leaving a buffer without data (for example a buffer
        // without a uri that is not backed by a GLB BIN chunk). Accessor reads would then hit a
        // null buffer, so reject the file with an actionable message.
        for (cgltf_size bufferIndex = 0; bufferIndex < data->buffers_count; ++bufferIndex)
        {
            const cgltf_buffer& buffer = data->buffers[bufferIndex];
            if (buffer.size > 0 && !buffer.data)
            {
                outError = AZStd::string::format(
                    "glTF buffer %zu ('%s') has no data. If the file was exported as .gltf, make sure the "
                    "referenced .bin file is next to it.",
                    bufferIndex, buffer.name ? buffer.name : "unnamed");
                return false;
            }
        }

        const cgltf_result validationResult = cgltf_validate(data.get());
        if (validationResult != cgltf_result_success)
        {
            // Some exporters produce files that are not strictly valid but still loadable. cgltf's
            // accessors bounds check reads, so warn and continue rather than rejecting the asset.
            AZ_Warning(
                "UniversalSceneImporter", false,
                "glTF file '%s' failed strict validation (result %d). Import will continue, the result may be incomplete.",
                fileName, static_cast<int>(validationResult));
        }

        GltfConverter converter(data.get(), outScene);
        return converter.Convert(outError);
    }

} // namespace UniversalSceneImporter


