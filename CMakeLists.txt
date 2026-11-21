cmake_minimum_required(VERSION 3.10)

project(tsl_hat_trie VERSION 0.7.1)
include(GNUInstallDirs)

add_library(tsl_hat_trie INTERFACE)
# Use tsl::hat_trie as target, more consistent with other libraries conventions (Boost, Qt, ...)
add_library(tsl::hat_trie ALIAS tsl_hat_trie)

target_include_directories(tsl_hat_trie INTERFACE
                           "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                           "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

list(APPEND headers "${CMAKE_CURRENT_SOURCE_DIR}/include/tsl/htrie_children_policies.h"
                    "${CMAKE_CURRENT_SOURCE_DIR}/include/tsl/htrie_hash.h"
                    "${CMAKE_CURRENT_SOURCE_DIR}/include/tsl/htrie_map.h"
                    "${CMAKE_CURRENT_SOURCE_DIR}/include/tsl/htrie_set.h")
target_sources(tsl_hat_trie INTERFACE "$<BUILD_INTERFACE:${headers}>")

target_compile_features(tsl_hat_trie INTERFACE cxx_std_11)

# The exported target must be called tsl::hat_trie (NAMESPACE tsl:: + EXPORT_NAME),
# not tsl::tsl_hat_trie, to match the alias above.
set_target_properties(tsl_hat_trie PROPERTIES EXPORT_NAME hat_trie)


## array-hash dependency
# 1. If the git submodule is checked out (third_party/array-hash), use it.
# 2. Otherwise look for an installed tsl-array-hash package (Nix, distro
#    packages, a parent project, ...).
set(TSL_HAT_TRIE_ARRAY_HASH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/array-hash")
if(EXISTS "${TSL_HAT_TRIE_ARRAY_HASH_DIR}/include/tsl/array_map.h")
    set(TSL_HAT_TRIE_USE_SYSTEM_ARRAY_HASH FALSE)
    target_include_directories(tsl_hat_trie INTERFACE
                               "$<BUILD_INTERFACE:${TSL_HAT_TRIE_ARRAY_HASH_DIR}/include>")
else()
    set(TSL_HAT_TRIE_USE_SYSTEM_ARRAY_HASH TRUE)
    find_package(tsl-array-hash CONFIG QUIET)
    if(NOT TARGET tsl::array_hash)
        message(FATAL_ERROR
            "array-hash was not found. Either run: git submodule update --init --recursive "
            "or install tsl-array-hash and make it visible to CMake (CMAKE_PREFIX_PATH).")
    endif()
    target_link_libraries(tsl_hat_trie INTERFACE tsl::array_hash)
endif()


include(CMakePackageConfigHelpers)

## Install include directory
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/tsl"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# When array-hash comes from the submodule, ship its headers too so that the
# installed package is self-contained. With an external tsl-array-hash the
# package config depends on it through find_dependency() instead.
if(NOT TSL_HAT_TRIE_USE_SYSTEM_ARRAY_HASH)
    install(DIRECTORY "${TSL_HAT_TRIE_ARRAY_HASH_DIR}/include/tsl"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()


## Create and install tsl-hat-trieConfig.cmake
configure_package_config_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/tsl-hat-trieConfig.cmake.in"
                              "${CMAKE_CURRENT_BINARY_DIR}/tsl-hat-trieConfig.cmake"
                              INSTALL_DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/cmake/tsl-hat-trie")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/tsl-hat-trieConfig.cmake"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/cmake/tsl-hat-trie")


## Create and install tsl-hat-trieTargets.cmake
install(TARGETS tsl_hat_trie
        EXPORT tsl-hat-trieTargets)

install(EXPORT tsl-hat-trieTargets
        NAMESPACE tsl::
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/cmake/tsl-hat-trie")


## Create and install tsl-hat-trieConfigVersion.cmake
# tsl-hat-trie is header-only and does not depend on the architecture.
# Remove CMAKE_SIZEOF_VOID_P from tsl-hat-trieConfigVersion.cmake so that a
# tsl-hat-trieConfig.cmake generated for a 64 bit target can be used for 32 bit
# targets and vice versa.
set(CMAKE_SIZEOF_VOID_P_BACKUP ${CMAKE_SIZEOF_VOID_P})
unset(CMAKE_SIZEOF_VOID_P)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/tsl-hat-trieConfigVersion.cmake"
                                 COMPATIBILITY SameMajorVersion)
set(CMAKE_SIZEOF_VOID_P ${CMAKE_SIZEOF_VOID_P_BACKUP})

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/tsl-hat-trieConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/cmake/tsl-hat-trie")


OPTION(BUILD_TESTS OFF)
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
