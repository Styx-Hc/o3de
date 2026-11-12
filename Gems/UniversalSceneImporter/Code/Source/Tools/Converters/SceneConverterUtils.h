/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Math/Matrix3x4.h>
#include <AzCore/Math/Vector2.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/Vector4.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/string.h>
#include <AzCore/std/string/string_view.h>

#include "../Scene/UniversalScene.h"

namespace UniversalSceneImporter::Converters
{
    //! Default sampling rate used when baking animations into keyframes, in seconds per frame.
    constexpr double DefaultAnimationTimeStep = 1.0 / 30.0;
    //! Upper bound for the number of baked keyframes per animation.
    constexpr size_t MaxAnimationFrames = 30000;
    //! Upper bound for the number of scene nodes accepted from a single file.
    constexpr size_t MaxNodeCount = 1000000;
    //! Maximum node hierarchy depth accepted from a single file.
    constexpr int MaxNodeDepth = 512;

    //! Converts a C string to an AZStd::string, tolerating null pointers.
    AZStd::string ToAzString(const char* value);

    //! Returns the lower case extension of a path or file name, without the leading dot.
    AZStd::string ExtractExtension(AZStd::string_view pathOrName);

    //! Makes a name safe to use as a scene graph node name. Dots are the scene graph separator and
    //! '%' is reserved for printf style trace messages, so both are replaced. Empty names get a
    //! fallback based on the provided prefix.
    AZStd::string SanitizeNodeName(AZStd::string_view name, const char* fallbackPrefix = "BlendShape");

    //! Identifies common image file formats from their header bytes. Returns a lower case
    //! extension without a dot, or an empty string if the format is not recognized.
    AZStd::string SniffImageExtension(const AZ::u8* data, size_t size);

    //! Matrix that converts glTF's Y-up right handed space into O3DE's Z-up right handed space.
    const AZ::Matrix3x4& GetGltfAxisConversionMatrix();

    //! Converts a position or direction from glTF space to O3DE space.
    AZ::Vector3 ConvertGltfVector(const AZ::Vector3& value);

    //! Converts a local transform from glTF space to O3DE space.
    AZ::Matrix3x4 ConvertGltfTransform(const AZ::Matrix3x4& transform);

    //! Computes area weighted smooth normals for a triangle list.
    void GenerateSmoothNormals(
        const AZStd::vector<AZ::Vector3>& positions,
        const AZStd::vector<AZ::u32>& indices,
        AZStd::vector<AZ::Vector3>& outNormals);

    //! Computes per-vertex tangents (xyz + handedness in w) from positions, normals,
    //! UV0 and a triangle list. Vertices without a valid tangent get a stable fallback.
    void GenerateTangents(
        const AZStd::vector<AZ::Vector3>& positions,
        const AZStd::vector<AZ::Vector3>& normals,
        const AZStd::vector<AZ::Vector2>& uvs,
        const AZStd::vector<AZ::u32>& indices,
        AZStd::vector<AZ::Vector4>& outTangents);

    //! Returns true when the 3x3 part of the matrix has a non zero determinant.
    bool IsInvertible(const AZ::Matrix3x4& matrix);
    AZ::Matrix3x4 InvertAffine(const AZ::Matrix3x4& matrix);

    //! Ensures a skinned primitive has tangent and bitangent streams. Skinned meshes require both
    //! streams downstream, so when the source has none they are generated from UVs, or from the
    //! normals when no UVs are available.
    void EnsureTangentStreams(MeshPrimitive& primitive);

    //! Appends the triangles of a triangle strip to an index buffer.
    void AppendTriangleStrip(const AZStd::vector<AZ::u32>& strip, AZStd::vector<AZ::u32>& indices);

    //! Appends the triangles of a triangle fan to an index buffer.
    void AppendTriangleFan(const AZStd::vector<AZ::u32>& fan, AZStd::vector<AZ::u32>& indices);

    //! Marks every ancestor of each bone as a bone so the skeleton hierarchy stays continuous.
    //! The scene root (node 0) is never marked.
    void MarkBoneAncestors(AZStd::vector<Node>& nodes);

} // namespace UniversalSceneImporter::Converters
