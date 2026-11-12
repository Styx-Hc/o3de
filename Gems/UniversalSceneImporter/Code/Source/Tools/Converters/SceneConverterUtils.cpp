/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "SceneConverterUtils.h"

#include <AzCore/Math/MathUtils.h>
#include <AzCore/Math/Vector2.h>
#include <AzCore/Math/Vector4.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/string/conversions.h>

namespace UniversalSceneImporter::Converters
{
    namespace
    {
        bool MatchesSignature(const AZ::u8* data, size_t size, const AZ::u8* signature, size_t signatureSize)
        {
            return size >= signatureSize && AZStd::equal(data, data + signatureSize, signature);
        }
    } // namespace

    AZStd::string ToAzString(const char* value)
    {
        return value ? AZStd::string(value) : AZStd::string();
    }

    AZStd::string ExtractExtension(AZStd::string_view pathOrName)
    {
        const size_t dot = pathOrName.find_last_of('.');
        if (dot == AZStd::string_view::npos || dot + 1 >= pathOrName.size())
        {
            return AZStd::string();
        }
        AZStd::string extension(pathOrName.substr(dot + 1));
        AZStd::to_lower(extension.begin(), extension.end());
        return extension;
    }

    AZStd::string SanitizeNodeName(AZStd::string_view name, const char* fallbackPrefix)
    {
        AZStd::string result;
        result.reserve(name.size());
        for (char character : name)
        {
            if (character == '.' || character == '%')
            {
                result.push_back('_');
            }
            else
            {
                result.push_back(character);
            }
        }

        // Trim surrounding whitespace so names do not start or end with spaces.
        size_t begin = 0;
        size_t end = result.size();
        while (begin < end && (result[begin] == ' ' || result[begin] == '\t'))
        {
            ++begin;
        }
        while (end > begin && (result[end - 1] == ' ' || result[end - 1] == '\t'))
        {
            --end;
        }
        result = result.substr(begin, end - begin);

        if (result.empty())
        {
            result = fallbackPrefix ? fallbackPrefix : "Node";
        }
        return result;
    }

    AZStd::string SniffImageExtension(const AZ::u8* data, size_t size)
    {
        if (!data || size < 4)
        {
            return AZStd::string();
        }

        static const AZ::u8 png[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        if (MatchesSignature(data, size, png, sizeof(png)))
        {
            return "png";
        }
        static const AZ::u8 jpeg[] = { 0xFF, 0xD8, 0xFF };
        if (MatchesSignature(data, size, jpeg, sizeof(jpeg)))
        {
            return "jpg";
        }
        static const AZ::u8 dds[] = { 'D', 'D', 'S', ' ' };
        if (MatchesSignature(data, size, dds, sizeof(dds)))
        {
            return "dds";
        }
        static const AZ::u8 bmp[] = { 'B', 'M' };
        if (MatchesSignature(data, size, bmp, sizeof(bmp)))
        {
            return "bmp";
        }
        static const AZ::u8 gif[] = { 'G', 'I', 'F', '8' };
        if (MatchesSignature(data, size, gif, sizeof(gif)))
        {
            return "gif";
        }
        static const AZ::u8 webpRiff[] = { 'R', 'I', 'F', 'F' };
        if (MatchesSignature(data, size, webpRiff, sizeof(webpRiff)) && size >= 12 && data[8] == 'W' && data[9] == 'E' &&
            data[10] == 'B' && data[11] == 'P')
        {
            return "webp";
        }
        static const AZ::u8 exr[] = { 0x76, 0x2F, 0x31, 0x01 };
        if (MatchesSignature(data, size, exr, sizeof(exr)))
        {
            return "exr";
        }
        static const AZ::u8 ktx2[] = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
        if (MatchesSignature(data, size, ktx2, sizeof(ktx2)))
        {
            return "ktx2";
        }
        static const AZ::u8 hdr[] = { '#', '?', 'R', 'A', 'D', 'I', 'A', 'N', 'C', 'E' };
        if (MatchesSignature(data, size, hdr, sizeof(hdr)))
        {
            return "hdr";
        }
        return AZStd::string();
    }

    const AZ::Matrix3x4& GetGltfAxisConversionMatrix()
    {
        // Columns: (-1, 0, 0), (0, 0, 1), (0, 1, 0)
        // Maps glTF (x, y, z) to O3DE (-x, z, y): Y-up -> Z-up while preserving handedness.
        static const float columnMajor[12] = { -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        static const AZ::Matrix3x4 conversion = AZ::Matrix3x4::CreateFromColumnMajorFloat12(columnMajor);
        return conversion;
    }

    AZ::Vector3 ConvertGltfVector(const AZ::Vector3& value)
    {
        return GetGltfAxisConversionMatrix().TransformVector(value);
    }

    AZ::Matrix3x4 ConvertGltfTransform(const AZ::Matrix3x4& transform)
    {
        const AZ::Matrix3x4& conversion = GetGltfAxisConversionMatrix();
        return conversion * transform * conversion.GetTranspose();
    }

    void GenerateSmoothNormals(
        const AZStd::vector<AZ::Vector3>& positions, const AZStd::vector<AZ::u32>& indices, AZStd::vector<AZ::Vector3>& outNormals)
    {
        outNormals.assign(positions.size(), AZ::Vector3::CreateZero());
        if (positions.empty())
        {
            return;
        }

        auto accumulateTriangle = [&positions, &outNormals](AZ::u32 i0, AZ::u32 i1, AZ::u32 i2)
        {
            if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size())
            {
                return;
            }
            const AZ::Vector3 edge1 = positions[i1] - positions[i0];
            const AZ::Vector3 edge2 = positions[i2] - positions[i0];
            const AZ::Vector3 faceNormal = edge1.Cross(edge2); // Area weighted; normalization happens later.
            if (faceNormal.IsZero())
            {
                return;
            }
            outNormals[i0] += faceNormal;
            outNormals[i1] += faceNormal;
            outNormals[i2] += faceNormal;
        };

        if (indices.empty())
        {
            for (size_t index = 0; index + 2 < positions.size(); index += 3)
            {
                accumulateTriangle(static_cast<AZ::u32>(index), static_cast<AZ::u32>(index + 1), static_cast<AZ::u32>(index + 2));
            }
        }
        else
        {
            for (size_t index = 0; index + 2 < indices.size(); index += 3)
            {
                accumulateTriangle(indices[index], indices[index + 1], indices[index + 2]);
            }
        }

        for (AZ::Vector3& normal : outNormals)
        {
            if (!normal.IsZero())
            {
                normal.NormalizeSafe();
            }
            else
            {
                normal = AZ::Vector3::CreateAxisZ();
            }
        }
    }

    void GenerateTangents(
        const AZStd::vector<AZ::Vector3>& positions,
        const AZStd::vector<AZ::Vector3>& normals,
        const AZStd::vector<AZ::Vector2>& uvs,
        const AZStd::vector<AZ::u32>& indices,
        AZStd::vector<AZ::Vector4>& outTangents)
    {
        const size_t vertexCount = positions.size();
        outTangents.assign(vertexCount, AZ::Vector4(1.0f, 0.0f, 0.0f, 1.0f));
        if (vertexCount == 0 || normals.size() != vertexCount || uvs.size() != vertexCount)
        {
            return;
        }

        AZStd::vector<AZ::Vector3> tangentSum(vertexCount, AZ::Vector3::CreateZero());
        AZStd::vector<AZ::Vector3> bitangentSum(vertexCount, AZ::Vector3::CreateZero());

        auto accumulateTriangle = [&](AZ::u32 i0, AZ::u32 i1, AZ::u32 i2)
        {
            if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
            {
                return;
            }
            const AZ::Vector3 edge1 = positions[i1] - positions[i0];
            const AZ::Vector3 edge2 = positions[i2] - positions[i0];
            const AZ::Vector2 deltaUV1 = uvs[i1] - uvs[i0];
            const AZ::Vector2 deltaUV2 = uvs[i2] - uvs[i0];
            const float determinant = deltaUV1.GetX() * deltaUV2.GetY() - deltaUV2.GetX() * deltaUV1.GetY();
            if (AZ::IsClose(determinant, 0.0f))
            {
                return;
            }
            const float r = 1.0f / determinant;
            const AZ::Vector3 tangent = (edge1 * deltaUV2.GetY() - edge2 * deltaUV1.GetY()) * r;
            const AZ::Vector3 bitangent = (edge2 * deltaUV1.GetX() - edge1 * deltaUV2.GetX()) * r;
            tangentSum[i0] += tangent;
            tangentSum[i1] += tangent;
            tangentSum[i2] += tangent;
            bitangentSum[i0] += bitangent;
            bitangentSum[i1] += bitangent;
            bitangentSum[i2] += bitangent;
        };

        if (indices.empty())
        {
            for (size_t index = 0; index + 2 < vertexCount; index += 3)
            {
                accumulateTriangle(static_cast<AZ::u32>(index), static_cast<AZ::u32>(index + 1), static_cast<AZ::u32>(index + 2));
            }
        }
        else
        {
            for (size_t index = 0; index + 2 < indices.size(); index += 3)
            {
                accumulateTriangle(indices[index], indices[index + 1], indices[index + 2]);
            }
        }

        for (size_t index = 0; index < vertexCount; ++index)
        {
            const AZ::Vector3 normal = normals[index];
            AZ::Vector3 tangent = tangentSum[index];
            if (tangent.IsZero())
            {
                // Pick a stable tangent perpendicular to the normal.
                const AZ::Vector3 reference = AZ::IsClose(AZ::Abs(normal.GetZ()), 1.0f) ? AZ::Vector3::CreateAxisY() : AZ::Vector3::CreateAxisZ();
                tangent = reference.Cross(normal);
            }
            tangent = (tangent - normal * normal.Dot(tangent)).GetNormalizedSafe();
            if (tangent.IsZero())
            {
                tangent = AZ::Vector3::CreateAxisX();
            }

            const float handedness = (normal.Cross(tangent).Dot(bitangentSum[index]) < 0.0f) ? -1.0f : 1.0f;
            outTangents[index] = AZ::Vector4(tangent.GetX(), tangent.GetY(), tangent.GetZ(), handedness);
        }
    }

    void EnsureTangentStreams(MeshPrimitive& primitive)
    {
        const size_t vertexCount = primitive.m_positions.size();
        if (vertexCount == 0)
        {
            return;
        }

        if (primitive.m_normals.size() != vertexCount)
        {
            GenerateSmoothNormals(primitive.m_positions, primitive.m_indices, primitive.m_normals);
        }

        if (primitive.m_tangents.size() != vertexCount)
        {
            if (!primitive.m_uvSets[0].empty())
            {
                GenerateTangents(
                    primitive.m_positions, primitive.m_normals, primitive.m_uvSets[0], primitive.m_indices, primitive.m_tangents);
            }
            else
            {
                // No UVs: build an orthonormal basis from the normal so downstream skinning has a
                // complete tangent frame instead of inserting dummy values.
                primitive.m_tangents.resize(vertexCount);
                for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
                {
                    const AZ::Vector3 normal = primitive.m_normals[vertexIndex];
                    const AZ::Vector3 reference =
                        AZ::Abs(normal.GetZ()) > 0.9f ? AZ::Vector3::CreateAxisY() : AZ::Vector3::CreateAxisZ();
                    AZ::Vector3 tangent = reference.Cross(normal).GetNormalizedSafe();
                    if (tangent.IsZero())
                    {
                        tangent = AZ::Vector3::CreateAxisX();
                    }
                    primitive.m_tangents[vertexIndex] = AZ::Vector4(tangent.GetX(), tangent.GetY(), tangent.GetZ(), 1.0f);
                }
            }
        }

        if (primitive.m_bitangents.size() != vertexCount)
        {
            primitive.m_bitangents.resize(vertexCount);
            for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
            {
                const AZ::Vector4& tangent = primitive.m_tangents[vertexIndex];
                const AZ::Vector3 tangentVector(tangent.GetX(), tangent.GetY(), tangent.GetZ());
                primitive.m_bitangents[vertexIndex] = primitive.m_normals[vertexIndex].Cross(tangentVector) * tangent.GetW();
            }
        }
    }

    AZ::Matrix3x4 InvertAffine(const AZ::Matrix3x4& matrix)
    {
        const AZ::Vector3 scale(matrix.GetBasisX().GetLength(), matrix.GetBasisY().GetLength(), matrix.GetBasisZ().GetLength());
        AZ::Matrix3x4 normalized = matrix;
        normalized.SetBasisX(matrix.GetBasisX() / scale.GetX());
        normalized.SetBasisY(matrix.GetBasisY() / scale.GetY());
        normalized.SetBasisZ(matrix.GetBasisZ() / scale.GetZ());
        normalized.SetTranslation(AZ::Vector3::CreateZero());
        AZ::Matrix3x4 inverse = normalized.GetInverseFull();
        inverse.SetRow(0, inverse.GetRow(0) / scale.GetX());
        inverse.SetRow(1, inverse.GetRow(1) / scale.GetY());
        inverse.SetRow(2, inverse.GetRow(2) / scale.GetZ());
        inverse.SetTranslation(-inverse.TransformVector(matrix.GetTranslation()));
        return inverse;
    }

    bool IsInvertible(const AZ::Matrix3x4& matrix)
    {
        const AZ::Vector3 column0 = matrix.GetColumn(0);
        const AZ::Vector3 column1 = matrix.GetColumn(1);
        const AZ::Vector3 column2 = matrix.GetColumn(2);
        const float length0 = column0.GetLength();
        const float length1 = column1.GetLength();
        const float length2 = column2.GetLength();
        if (length0 <= 0.0f || length1 <= 0.0f || length2 <= 0.0f)
        {
            return false;
        }
        const float determinant = (column0 / length0).Dot((column1 / length1).Cross(column2 / length2));
        return AZ::Abs(determinant) > 1e-8f;
    }

    void AppendTriangleStrip(const AZStd::vector<AZ::u32>& strip, AZStd::vector<AZ::u32>& indices)
    {
        if (strip.size() < 3)
        {
            return;
        }
        for (size_t index = 0; index + 2 < strip.size(); ++index)
        {
            const AZ::u32 i0 = strip[index];
            const AZ::u32 i1 = strip[index + 1];
            const AZ::u32 i2 = strip[index + 2];
            if (i0 == i1 || i1 == i2 || i0 == i2)
            {
                continue;
            }
            // Every second triangle in a strip has reversed winding.
            if ((index % 2) == 0)
            {
                indices.push_back(i0);
                indices.push_back(i1);
                indices.push_back(i2);
            }
            else
            {
                indices.push_back(i1);
                indices.push_back(i0);
                indices.push_back(i2);
            }
        }
    }

    void AppendTriangleFan(const AZStd::vector<AZ::u32>& fan, AZStd::vector<AZ::u32>& indices)
    {
        if (fan.size() < 3)
        {
            return;
        }
        for (size_t index = 1; index + 1 < fan.size(); ++index)
        {
            const AZ::u32 i0 = fan[0];
            const AZ::u32 i1 = fan[index];
            const AZ::u32 i2 = fan[index + 1];
            if (i0 == i1 || i1 == i2 || i0 == i2)
            {
                continue;
            }
            indices.push_back(i0);
            indices.push_back(i1);
            indices.push_back(i2);
        }
    }

    void MarkBoneAncestors(AZStd::vector<Node>& nodes)
    {
        // Any ancestor of a joint is part of the skeleton. The scene root (node 0) is not marked.
        for (size_t index = 0; index < nodes.size(); ++index)
        {
            if (!nodes[index].m_isBone)
            {
                continue;
            }
            int parent = nodes[index].m_parent;
            while (parent > 0)
            {
                nodes[parent].m_isBone = true;
                parent = nodes[parent].m_parent;
            }
        }
    }

} // namespace UniversalSceneImporter::Converters
