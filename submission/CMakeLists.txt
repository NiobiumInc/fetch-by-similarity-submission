cmake_minimum_required(VERSION 3.14)
project(add_openfhe LANGUAGES CXX)

# --------------------------------------------------------------------
# 1.  Compiler / flags
# --------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
# set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# add_compile_options(-O3 -Wall -Wextra -Wno-unused-parameter)
option( BUILD_STATIC "Set to ON to include static versions of the library" OFF)

# --------------------------------------------------------------------
# 2.  Find the OpenFHE *package*. The harness passes
#     CMAKE_PREFIX_PATH pointing at the OpenFHE installed by the
#     niobium-client Makefile (submission/niobium-client/vendor/lib/openfhe).
# --------------------------------------------------------------------

find_package(OpenFHE CONFIG REQUIRED)
if (OpenFHE_FOUND)
    message(STATUS "FOUND PACKAGE OpenFHE")
    message(STATUS "OpenFHE Version: ${BASE_OPENFHE_VERSION}")
    message(STATUS "OpenFHE installed as shared libraries: ${OpenFHE_SHARED}")
    message(STATUS "OpenFHE include files location: ${OpenFHE_INCLUDE}")
    message(STATUS "OpenFHE lib files location: ${OpenFHE_LIBDIR}")
    message(STATUS "OpenFHE Native Backend size: ${OpenFHE_NATIVE_SIZE}")
else()
    message(FATAL_ERROR "PACKAGE OpenFHE NOT FOUND")
endif ()

set( CMAKE_CXX_FLAGS "${OpenFHE_CXX_FLAGS} -Werror")

# --------------------------------------------------------------------
# 3.  Link libraries
# --------------------------------------------------------------------

include_directories( ${OPENMP_INCLUDES} )
include_directories( ${OpenFHE_INCLUDE} )
include_directories( ${OpenFHE_INCLUDE}/third-party/include )
include_directories( ${OpenFHE_INCLUDE}/core )
include_directories( ${OpenFHE_INCLUDE}/pke )
include_directories( ${OpenFHE_INCLUDE}/binfhe )
### add directories for other OpenFHE modules as needed for your project

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

link_directories( ${OpenFHE_LIBDIR} )
link_directories( ${OPENMP_LIBRARIES} )

# set(CMAKE_SKIP_BUILD_RPATH FALSE)
# set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
# set(CMAKE_BUILD_RPATH "${OpenFHE_LIBDIR}") 
# set(CMAKE_INSTALL_RPATH "${OpenFHE_LIBDIR}") 
# set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
if (NOT APPLE)
    add_link_options(-Wl,--no-as-needed)
endif()

# These should take care of setting the correct LD path without manually setting LD_LIBRARY_PATH. 
# But if a shared library or symbol lookup error occurs, you need to set the LD_LIBRARY_PATH environment 
# to the OpenFHE lib directory: export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:/path/to/OpenFHE/lib

if(BUILD_STATIC)
    set( CMAKE_EXE_LINKER_FLAGS "${OpenFHE_EXE_LINKER_FLAGS} -static")
    link_libraries( ${OpenFHE_STATIC_LIBRARIES} )
    message(STATUS "Build with static libs")
else()
    set( CMAKE_EXE_LINKER_FLAGS ${OpenFHE_EXE_LINKER_FLAGS} )
    link_libraries( ${OpenFHE_SHARED_LIBRARIES} )
endif()

# --------------------------------------------------------------------
# 3a. niobium-fhetch — provides g_replay_mode and other symbols
#     referenced by this OpenFHE fork's headers. The harness builds
#     niobium-client first, which produces libnbfhetch in
#     submission/niobium-client/build/vendor/niobium-fhetch.
# --------------------------------------------------------------------
set(NIOBIUM_CLIENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/niobium-client")
set(NIOBIUM_FHETCH_DIR "${NIOBIUM_CLIENT_DIR}/vendor/niobium-fhetch")
set(NIOBIUM_FHETCH_BUILD_DIR "${NIOBIUM_CLIENT_DIR}/build/vendor/niobium-fhetch")

find_library(NBFHETCH_LIB
    NAMES nbfhetch
    PATHS "${NIOBIUM_FHETCH_BUILD_DIR}"
    NO_DEFAULT_PATH
    REQUIRED)
message(STATUS "Found nbfhetch: ${NBFHETCH_LIB}")

include_directories("${NIOBIUM_FHETCH_DIR}/include")
link_libraries(${NBFHETCH_LIB})

# The bundled OpenFHE was built with WITH_CPROBES=ON, but its exported
# OpenFHE_CXX_FLAGS doesn't propagate -DOPENFHE_CPROBES. Without the
# define, the niobium_auto::on_deserialize_crypto_context hook in
# cryptocontext-ser.h is preprocessed out and the auto-facade never
# activates. libnbfhetch / libniobium_client_autofacade both set this
# PUBLIC on their CMake targets — we have to set it ourselves since
# the submission consumes those as raw .a/.dylib paths via find_library.
add_compile_definitions(OPENFHE_CPROBES)

# --------------------------------------------------------------------
# 3b. niobium_client_autofacade — transparent record/replay for
#     unmodified OpenFHE programs. Linked only into server_encrypted_compute
#     below. Stays dormant unless NIOBIUM_CONFIG (or a .niobium_compiler.yml
#     in CWD / exe dir) points at a YAML config.
# --------------------------------------------------------------------
find_library(NB_AUTOFACADE_LIB
    NAMES niobium_client_autofacade
    PATHS "${NIOBIUM_CLIENT_DIR}/build/src/auto_facade"
    NO_DEFAULT_PATH
    REQUIRED)
message(STATUS "Found niobium_client_autofacade: ${NB_AUTOFACADE_LIB}")

# yaml-cpp is a transitive PRIVATE dep of the autofacade. Reuse the copy
# niobium-client built via FetchContent.
find_library(YAML_CPP_LIB
    NAMES yaml-cpp
    PATHS "${NIOBIUM_CLIENT_DIR}/build/_deps/yaml_cpp-build"
    NO_DEFAULT_PATH
    REQUIRED)
message(STATUS "Found yaml-cpp: ${YAML_CPP_LIB}")

# --------------------------------------------------------------------
# 4.  Each *.cpp file becomes its own executable.
#     The eight stage names are hard-wired by the benchmark contract.
# --------------------------------------------------------------------
 
add_executable( client_key_generation src/running_sums.cpp src/slot_replication.cpp src/client_key_generation.cpp )

add_executable( client_preprocess_dataset src/client_preprocess_dataset.cpp )

add_executable( client_preprocess_query src/client_preprocess_query.cpp )

add_executable( client_encode_encrypt_db src/client_encode_encrypt_db.cpp )

add_executable( client_encode_encrypt_query src/client_encode_encrypt_query.cpp )

add_executable( client_decrypt_decode src/client_decrypt_decode.cpp )

add_executable( client_postprocess src/running_sums.cpp src/client_postprocess.cpp )

add_executable( server_preprocess_dataset src/server_preprocess_dataset.cpp )

add_executable( server_encrypted_compute src/running_sums.cpp src/slot_replication.cpp src/server_encrypted_compute.cpp )

# Drive the explicit cooperative record/replay lifecycle compiled into
# server_encrypted_compute.cpp (init/enable_auto_tagging/is_cache_valid/start/
# probe/stop/replay/result). The auto-facade is still linked below — cooperative
# mode reuses its OpenFHE deserialize hooks to auto-tag inputs/keys.
target_compile_definitions(server_encrypted_compute PRIVATE NIOBIUM_COMPILER)

# Auto-facade: strong niobium_auto::* hook impls live in
# libniobium_client_autofacade and have no direct callers in
# server_encrypted_compute.cpp, so the linker would normally drop them.
# Force-load the whole archive so the strong symbols win over the weak
# stubs in libnbfhetch.
if(APPLE)
    target_link_options(server_encrypted_compute PRIVATE
        "LINKER:-force_load,${NB_AUTOFACADE_LIB}")
else()
    target_link_options(server_encrypted_compute PRIVATE
        "LINKER:--whole-archive,${NB_AUTOFACADE_LIB},--no-whole-archive")
endif()
target_link_libraries(server_encrypted_compute PRIVATE "${YAML_CPP_LIB}")
# yaml-cpp lives next to the niobium-client build tree; add an rpath so
# the dylib is found at runtime without LD_LIBRARY_PATH gymnastics.
set_target_properties(server_encrypted_compute PROPERTIES
    BUILD_RPATH "${NIOBIUM_CLIENT_DIR}/build/_deps/yaml_cpp-build")
