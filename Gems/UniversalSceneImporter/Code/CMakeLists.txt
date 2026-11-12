#
# Copyright (c) Contributors to the Open 3D Engine Project.
# For complete copyright and license terms please see the LICENSE at the root of this distribution.
#
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
#

# The gem is a tool-only gem: it provides SceneAPI importers that run in the Asset Processor,
# the Asset Builder and the Editor, but it does not ship runtime code.
o3de_pal_dir(pal_dir ${CMAKE_CURRENT_LIST_DIR}/Platform/${PAL_PLATFORM_NAME} "${gem_restricted_path}" "${gem_path}" "${gem_parent_relative_path}")

include(${pal_dir}/PAL_${PAL_PLATFORM_NAME_LOWERCASE}.cmake)

if(NOT PAL_TRAIT_BUILD_HOST_TOOLS)
    return()
endif()

################################################################################
# Vendored single file libraries
################################################################################
# ufbx (public domain / MIT-0) and cgltf (MIT) are vendored so the gem has no
# package dependency. The targets are prefixed with the gem name so they cannot
# collide with the standalone ufbxImporter/cgltfImporter gems if those are also
# installed in the same project.
add_library(UniversalSceneImporter.External.ufbx STATIC
    Source/External/ufbx.h
    Source/External/ufbx.c
)
target_include_directories(UniversalSceneImporter.External.ufbx INTERFACE Source/External)
# Third party code is not built with O3DE's warning-as-error settings.
target_compile_options(UniversalSceneImporter.External.ufbx PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W0>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>
)

add_library(UniversalSceneImporter.External.cgltf STATIC
    Source/External/cgltf.h
    Source/External/cgltf.cc
)
target_include_directories(UniversalSceneImporter.External.cgltf INTERFACE Source/External)
target_compile_options(UniversalSceneImporter.External.cgltf PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W0>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>
)

################################################################################
# Editor static library that contains all of the importer code
################################################################################
ly_add_target(
    NAME ${gem_name}.Editor.Static STATIC
    NAMESPACE Gem
    FILES_CMAKE
        universalsceneimporter_editor_static_files.cmake
    INCLUDE_DIRECTORIES
        PRIVATE
            Source
            Source/External
        PUBLIC
            .
    BUILD_DEPENDENCIES
        PUBLIC
            AZ::AzCore
            AZ::AzFramework
            AZ::AzToolsFramework
            AZ::SceneCore
            AZ::SceneData
            AZ::SceneBuilder.Static
            AZ::SDKWrapper
        PRIVATE
            UniversalSceneImporter.External.cgltf
            UniversalSceneImporter.External.ufbx
)

################################################################################
# Editor / builder module that registers the components
################################################################################
ly_add_target(
    NAME ${gem_name}.Editor GEM_MODULE
    NAMESPACE Gem
    FILES_CMAKE
        universalsceneimporter_editor_files.cmake
    INCLUDE_DIRECTORIES
        PRIVATE
            Source
            Source/Tools
        PUBLIC
            .
    BUILD_DEPENDENCIES
        PRIVATE
            Gem::${gem_name}.Editor.Static
    RUNTIME_DEPENDENCIES
        AZ::SceneCore
        AZ::SceneData
        AZ::SceneBuilder
)

# Keep the module class name in sync with the gem name so the gem can be renamed safely.
ly_add_source_properties(
    SOURCES
        Source/UniversalSceneImporterModule.cpp
    PROPERTY COMPILE_DEFINITIONS
        VALUES
            O3DE_GEM_NAME=${gem_name}
            O3DE_GEM_VERSION=${gem_version}
)

# The gem is loaded by the Asset Processor, the Asset Builder and the Editor.
ly_create_alias(NAME ${gem_name}.Builders NAMESPACE Gem TARGETS Gem::${gem_name}.Editor)
ly_create_alias(NAME ${gem_name}.Tools    NAMESPACE Gem TARGETS Gem::${gem_name}.Editor)

################################################################################
# Tests
################################################################################
if(PAL_TRAIT_BUILD_TESTS_SUPPORTED)
    if(PAL_TRAIT_UNIVERSALSCENEIMPORTER_EDITOR_TEST_SUPPORTED)
        ly_add_target(
            NAME ${gem_name}.Editor.Tests ${PAL_TRAIT_TEST_TARGET_TYPE}
            NAMESPACE Gem
            FILES_CMAKE
                universalsceneimporter_editor_tests_files.cmake
            INCLUDE_DIRECTORIES
                PRIVATE
                    Tests
                    Source
                    Source/External
            BUILD_DEPENDENCIES
                PRIVATE
                    AZ::AzTest
                    AZ::AzToolsFramework
                    AZ::SceneCore
                    AZ::SceneData
                    AZ::SceneBuilder.Static
                    Gem::${gem_name}.Editor.Static
                    UniversalSceneImporter.External.cgltf
                    UniversalSceneImporter.External.ufbx
        )

        ly_add_googletest(
            NAME Gem::${gem_name}.Editor.Tests
        )
    endif()
endif()
