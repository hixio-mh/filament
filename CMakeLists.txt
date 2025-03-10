# ==================================================================================================
# CMake
# ==================================================================================================
cmake_minimum_required(VERSION 3.10)

# ==================================================================================================
# Project declaration
# ==================================================================================================
project(TNT)

# ==================================================================================================
# Options
# ==================================================================================================
option(FILAMENT_ENABLE_JAVA "Compile Java projects, requires a JDK and the JAVA_HOME env var" ON)

option(FILAMENT_USE_EXTERNAL_GLES3 "Experimental: Compile Filament against OpenGL ES 3" OFF)

option(FILAMENT_USE_SWIFTSHADER "Experimental: Compile Filament against SwiftShader" OFF)

option(FILAMENT_ENABLE_LTO "Enable link-time optimizations if supported by the compiler" OFF)

option(FILAMENT_SKIP_SAMPLES "Don't build samples" OFF)

set(FILAMENT_PER_RENDER_PASS_ARENA_SIZE_IN_MB "2" CACHE STRING
    "Per render pass arena size. Must be roughly 1 MB larger than FILAMENT_PER_FRAME_COMMANDS_SIZE_IN_MB, default 2."
)

set(FILAMENT_PER_FRAME_COMMANDS_SIZE_IN_MB "1" CACHE STRING
    "Size of the high-level draw commands buffer. Rule of thumb, 1 MB less than FILAMENT_PER_RENDER_PASS_ARENA_SIZE_IN_MB, default 1."
)

set(FILAMENT_MIN_COMMAND_BUFFERS_SIZE_IN_MB "1" CACHE STRING
    "Size of the command-stream buffer. As a rule of thumb use the same value as FILAMENT_PER_FRRAME_COMMANDS_SIZE_IN_MB, default 1."
)

set(FILAMENT_OPENGL_HANDLE_ARENA_SIZE_IN_MB "2" CACHE STRING
    "Size of the OpenGL handle arena, default 2."
)

# ==================================================================================================
# CMake policies
# ==================================================================================================
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.12")
    cmake_policy(SET CMP0074 NEW)
endif()

# ==================================================================================================
# Support for ccache
# ==================================================================================================
find_program(CCACHE_PROGRAM ccache)
if (CCACHE_PROGRAM)
    set(C_LAUNCHER   "${CCACHE_PROGRAM}")
    set(CXX_LAUNCHER "${CCACHE_PROGRAM}")

    configure_file(build/launch-c.in   launch-c)
    configure_file(build/launch-cxx.in launch-cxx)

    execute_process(COMMAND chmod a+rx
        "${CMAKE_BINARY_DIR}/launch-c"
        "${CMAKE_BINARY_DIR}/launch-cxx"
    )

    if (CMAKE_GENERATOR STREQUAL "Xcode")
        set(CMAKE_XCODE_ATTRIBUTE_CC         "${CMAKE_BINARY_DIR}/launch-c")
        set(CMAKE_XCODE_ATTRIBUTE_CXX        "${CMAKE_BINARY_DIR}/launch-cxx")
        set(CMAKE_XCODE_ATTRIBUTE_LD         "${CMAKE_BINARY_DIR}/launch-c")
        set(CMAKE_XCODE_ATTRIBUTE_LDPLUSPLUS "${CMAKE_BINARY_DIR}/launch-cxx")
    else()
        set(CMAKE_C_COMPILER_LAUNCHER        "${CMAKE_BINARY_DIR}/launch-c")
        set(CMAKE_CXX_COMPILER_LAUNCHER      "${CMAKE_BINARY_DIR}/launch-cxx")
    endif()
endif()

# ==================================================================================================
# Support Vim and Visual Studio Code by generating compile_commands.json
# ==================================================================================================
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ==================================================================================================
# OS specific
# ==================================================================================================
if (UNIX AND NOT APPLE AND NOT ANDROID AND NOT WEBGL)
    set(LINUX TRUE)
endif()

if (ANDROID OR WEBGL OR IOS OR FILAMENT_USE_SWIFTSHADER)
    set(IS_MOBILE_TARGET TRUE)
endif()

if (NOT ANDROID AND NOT WEBGL AND NOT IOS)
    set(IS_HOST_PLATFORM TRUE)
endif()

if (IOS)
    # Remove the headerpad_max_install_names linker flag on iOS. It causes warnings when linking
    # executables with bitcode.
    string(REPLACE "-Wl,-headerpad_max_install_names" "" CMAKE_C_LINK_FLAGS ${CMAKE_C_LINK_FLAGS})
    string(REPLACE "-Wl,-headerpad_max_install_names" "" CMAKE_CXX_LINK_FLAGS ${CMAKE_CXX_LINK_FLAGS})
    string(REPLACE "-Wl,-headerpad_max_install_names" "" CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS ${CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS})
    string(REPLACE "-Wl,-headerpad_max_install_names" "" CMAKE_SHARED_LIBRARY_CREATE_CXX_FLAGS ${CMAKE_SHARED_LIBRARY_CREATE_CXX_FLAGS})
endif()

if (WIN32)
    # Link statically against c/c++ lib to avoid missing redistriburable such as
    # "VCRUNTIME140.dll not found. Try reinstalling the app.", but give users
    # a choice to opt for the shared runtime if they want.
    option(USE_STATIC_CRT "Link against the static runtime libraries." ON)

    # On Windows we need to instruct cmake to generate the .def in order to get the .lib required
    # when linking against dlls. CL.EXE will not generate .lib without .def file (or without pragma
    # __declspec(dllexport) in front of each functions).
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

    if (${USE_STATIC_CRT})
        set(CRT_FLAGS_RELEASE "/MT")
        set(CRT_FLAGS_DEBUG "/MTd")
    else()
        set(CRT_FLAGS_RELEASE "/MD")
        set(CRT_FLAGS_DEBUG "/MDd")
    endif()

    # TODO: Figure out why pdb generation messes with incremental compilaton.
    # IN RELEASE_WITH_DEBUG_INFO, generate debug info in .obj, no in pdb.
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${CRT_FLAGS_RELEASE} /Z7")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} ${CRT_FLAGS_RELEASE} /Z7")

    # In RELEASE, also generate PDBs.
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${CRT_FLAGS_RELEASE} /Zi")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} ${CRT_FLAGS_RELEASE} /Zi")

    # In DEBUG, avoid generating a PDB file which seems to mess with incremental compilation.
    # Instead generate debug info directly inside obj files.
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${CRT_FLAGS_DEBUG} /Z7")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} ${CRT_FLAGS_DEBUG} /Z7")

    # Special settings when building on CI.
    if (${FILAMENT_WINDOWS_CI_BUILD})
        set(LinkerFlags
            CMAKE_SHARED_LINKER_FLAGS_DEBUG
            CMAKE_EXE_LINKER_FLAGS_DEBUG
            CMAKE_MODULE_LINKER_FLAGS_DEBUG
            )
        foreach(LinkerFlag ${LinkerFlags})
            # The /debug flag outputs .pdb files, which we don't need on CI.
            string(REPLACE "/debug" "" ${LinkerFlag} ${${LinkerFlag}})

            # The /INCREMENTAL flag outputs .ilk files for incremental linking. These are huge, and
            # we don't need them on CI.
            string(REPLACE "/INCREMENTAL" "/INCREMENTAL:NO" ${LinkerFlag} ${${LinkerFlag}})
        endforeach()
    endif()
endif()

# ==================================================================================================
#  Paths
# ==================================================================================================
# Where our external libs are
set(EXTERNAL ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# Where our libraries are
set(LIBRARIES ${CMAKE_CURRENT_SOURCE_DIR}/libs)

# Where our filament code is
set(FILAMENT ${CMAKE_CURRENT_SOURCE_DIR})

# Where our tools are
set(TOOLS ${CMAKE_CURRENT_SOURCE_DIR}/tools)

# ==================================================================================================
# Compiler check
# ==================================================================================================
set(MIN_CLANG_VERSION "6.0")

if (CMAKE_C_COMPILER_ID MATCHES "Clang")
    if (CMAKE_C_COMPILER_VERSION VERSION_LESS MIN_CLANG_VERSION)
        message(FATAL_ERROR "Detected C compiler Clang ${CMAKE_C_COMPILER_VERSION} < ${MIN_CLANG_VERSION}")
    endif()
elseif (NOT MSVC)
    message(FATAL_ERROR "Detected C compiler ${CMAKE_C_COMPILER_ID} is unsupported")
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS MIN_CLANG_VERSION)
        message(FATAL_ERROR "Detected CXX compiler Clang ${CMAKE_CXX_COMPILER_VERSION} < ${MIN_CLANG_VERSION}")
    endif()
elseif (NOT MSVC)
    message(FATAL_ERROR "Detected CXX compiler ${CMAKE_CXX_COMPILER_ID} is unsupported")
endif()

# Detect use of the clang-cl.exe frontend, which does not support all of clangs normal options
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    if ("${CMAKE_CXX_SIMULATE_ID}" STREQUAL "MSVC")
        message(FATAL_ERROR "Building with Clang on Windows is no longer supported. Use MSVC 2019 instead.")
    endif()
endif()

# ==================================================================================================
# Link time optimizations (LTO)
# ==================================================================================================
if (FILAMENT_ENABLE_LTO)
    include(CheckIPOSupported)

    check_ipo_supported(RESULT IPO_SUPPORT)

    if (IPO_SUPPORT)
        message(STATUS "LTO support is enabled")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# ==================================================================================================
# General compiler flags
# ==================================================================================================
set(CXX_STANDARD "-std=c++14")
if (WIN32)
    set(CXX_STANDARD "/std:c++14")
endif()

if (MSVC)
    set(CXX_STANDARD "/std:c++latest")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CXX_STANDARD} /W0 /Zc:__cplusplus")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CXX_STANDARD} -fstrict-aliasing -Wno-unknown-pragmas -Wno-unused-function")
endif()

if (FILAMENT_USE_EXTERNAL_GLES3)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DUSE_EXTERNAL_GLES3")
endif()

if (FILAMENT_USE_SWIFTSHADER)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DSWIFTSHADER")
    set(EGL TRUE)
endif()

if (WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_USE_MATH_DEFINES=1")
endif()

if (LINUX)
    option(USE_STATIC_LIBCXX "Link against the static runtime libraries." ON)
    if (${USE_STATIC_LIBCXX}) 
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
        link_libraries("-static-libgcc -static-libstdc++")
        link_libraries(libc++.a)
        link_libraries(libc++abi.a)
    endif()

    # Only linux, clang doesn't want to use a shared library that is not PIC.
    # /usr/bin/ld: ../bluegl/libbluegl.a(BlueGL.cpp.o): relocation R_X86_64_32S
    # against `.bss' can not be used when making a shared object; recompile with -fPIC
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

endif()

if (CYGWIN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-rtti")
endif()

if (MSVC)
    # Since the "secure" replacements that MSVC suggests are not portable, disable
    # the deprecation warnings. Also disable warnings about use of POSIX functions (i.e. "unlink").
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE")
endif()

# Add colors to ninja builds
if (UNIX AND CMAKE_GENERATOR STREQUAL "Ninja")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fcolor-diagnostics")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcolor-diagnostics")
endif()

# ==================================================================================================
# Release compiler flags
# ==================================================================================================
if (NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fomit-frame-pointer")

    # These aren't compatible with -fembed-bitcode (and seem to have no effect on Apple platforms anyway)
    if (NOT IOS)
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffunction-sections -fdata-sections")
    endif()
endif()

# On Android RELEASE builds, we disable exceptions and RTTI to save some space (about 75 KiB
# saved by -fno-exception and 10 KiB saved by -fno-rtti).
if (ANDROID OR WEBGL)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-rtti")
endif()

# With WebGL, we disable RTTI even for debug builds because we pass emscripten::val back and forth
# between C++ and JavaScript in order to efficiently access typed arrays, which are unbound.
# NOTE: This is not documented in emscripten so we should consider a different approach.
if (WEBGL)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-rtti")
endif()

# ==================================================================================================
# Debug compiler flags
# ==================================================================================================
# ASAN is deactivated for now because:
#  -fsanitize=undefined causes extremely long link times
#  -fsanitize=address causes a crash with assimp, which we can't explain for now
#set(EXTRA_SANITIZE_OPTIONS "-fsanitize=undefined -fsanitize=address")
if (NOT MSVC AND NOT WEBGL)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fstack-protector")
endif()
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${EXTRA_SANITIZE_OPTIONS}")

# Disable the stack check for macOS to workaround a known issue in clang 11.0.0.
# See: https://forums.developer.apple.com/thread/121887
if (APPLE)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}  -fno-stack-check")
endif()

# ==================================================================================================
# Linker flags
# ==================================================================================================
# Strip unused sections
if (NOT WEBGL)
    set(GC_SECTIONS "-Wl,--gc-sections")
endif()
set(B_SYMBOLIC_FUNCTIONS "-Wl,-Bsymbolic-functions")

if (APPLE)
    set(GC_SECTIONS "-Wl,-dead_strip")
    set(B_SYMBOLIC_FUNCTIONS "")

    # tell ranlib to ignore empty compilation units
    set(CMAKE_C_ARCHIVE_FINISH   "<CMAKE_RANLIB> -no_warning_for_no_symbols <TARGET>")
    set(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols <TARGET>")
    # prevents ar from invoking ranlib, let CMake do it
    set(CMAKE_C_ARCHIVE_CREATE   "<CMAKE_AR> qc -S <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qc -S <TARGET> <LINK_FLAGS> <OBJECTS>")
endif()

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${GC_SECTIONS}")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${GC_SECTIONS} ${B_SYMBOLIC_FUNCTIONS}")

if (WEBGL)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s USE_WEBGL2=1")
endif()

# ==================================================================================================
# Project flags
# ==================================================================================================
# Debug modes only
if (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(TNT_DEV true)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DTNT_DEV")
endif()

# By default, build with Vulkan support on desktop platforms, although clients must request to use
# it at run time.
if (WIN32 OR WEBGL OR IOS OR FILAMENT_USE_SWIFTSHADER)
    option(FILAMENT_SUPPORTS_VULKAN "Include the Vulkan backend" OFF)
else()
    option(FILAMENT_SUPPORTS_VULKAN "Include the Vulkan backend" ON)
endif()
if (FILAMENT_SUPPORTS_VULKAN)
    add_definitions(-DFILAMENT_DRIVER_SUPPORTS_VULKAN)
endif()

# Build with Metal support on non-WebGL Apple platforms.
if (APPLE AND NOT WEBGL)
    option(FILAMENT_SUPPORTS_METAL "Include the Metal backend" ON)
else()
    option(FILAMENT_SUPPORTS_METAL "Include the Metal backend" OFF)
endif()
if (FILAMENT_SUPPORTS_METAL)
    add_definitions(-DFILAMENT_SUPPORTS_METAL)
endif()

# Building filamat increases build times and isn't required for web, so turn it off by default.
if (NOT WEBGL)
    option(FILAMENT_BUILD_FILAMAT "Build filamat and JNI buildings" ON)
else()
    option(FILAMENT_BUILD_FILAMAT "Build filamat and JNI buildings" OFF)
endif()

# By default, link in matdbg for Desktop + Debug only since it pulls in filamat and a web server.
if (CMAKE_BUILD_TYPE STREQUAL "Debug" AND IS_HOST_PLATFORM)
    option(FILAMENT_ENABLE_MATDBG "Enable the material debugger" ON)
else()
    option(FILAMENT_ENABLE_MATDBG "Enable the material debugger" OFF)
endif()

# ==================================================================================================
# Material compilation flags
# ==================================================================================================

# Target system.
if (IS_MOBILE_TARGET)
    set(MATC_TARGET mobile)
else()
    set(MATC_TARGET desktop)
endif()

set(MATC_API_FLAGS )

# TODO: Add a flag to build Filament without support for OpenGL.
set(MATC_API_FLAGS ${MATC_API_FLAGS} -a opengl)

if (FILAMENT_SUPPORTS_VULKAN)
    set(MATC_API_FLAGS ${MATC_API_FLAGS} -a vulkan)
endif()
if (FILAMENT_SUPPORTS_METAL)
    set(MATC_API_FLAGS ${MATC_API_FLAGS} -a metal)
endif()

# Only optimize materials in Release mode (so error message lines match the source code)
if (NOT CMAKE_BUILD_TYPE MATCHES Release)
    set(MATC_OPT_FLAGS -g)
endif()

set(MATC_BASE_FLAGS ${MATC_API_FLAGS} -p ${MATC_TARGET} ${MATC_OPT_FLAGS})

# ==================================================================================================
# Distribution
# ==================================================================================================
# choose where to put the objects in the dist folder
if (NOT DIST_ARCH)
    set(DIST_ARCH x86_64)
endif()
if (NOT DIST_DIR)
    set(DIST_DIR "${DIST_ARCH}")
endif()

# ==================================================================================================
# Functions
# ==================================================================================================
## The MSVC compiler has a limitation on literal string length which is reached when all the
## licenses are concatenated together into a large string... so split them into multiple strings.
function(list_licenses OUTPUT MODULES)
    set(STR_OPENER "R\"FILAMENT__(")
    set(STR_CLOSER ")FILAMENT__\"")
    set(CONTENT)
    set(_MODULES ${MODULES} ${ARGN})
    foreach(module ${_MODULES})
        set(license_path "../../third_party/${module}/LICENSE")
        get_filename_component(fullname "${license_path}" ABSOLUTE)
        string(APPEND CONTENT "${STR_OPENER}License and copyrights for ${module}:\n${STR_CLOSER},\n")
        file(READ ${license_path} license_long)
        string(REPLACE "\n" "${STR_CLOSER},\n${STR_OPENER}" license ${license_long})
        string(APPEND CONTENT ${STR_OPENER}${license}\n${STR_CLOSER},)
        string(APPEND CONTENT "\n\n")
    endforeach()
    configure_file(${FILAMENT}/build/licenses.inc.in ${OUTPUT})
endfunction(list_licenses)

set(COMBINE_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/build/linux/combine-static-libs.sh")
if (WIN32)
    set(COMBINE_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/build/windows/combine-static-libs.bat")
    set(CMAKE_AR "lib.exe")
endif()

# Add a custom command to TARGET that combines the static libraries in DEPS into a single archive.
function(combine_static_libs TARGET OUTPUT DEPS)
    # Loop through the dependent libraries and query their location on disk.
    set(DEPS_FILES )
    foreach(DEPENDENCY ${DEPS})
        list(APPEND DEPS_FILES "$<TARGET_FILE:${DEPENDENCY}>")
    endforeach()

    add_custom_command(
        TARGET ${TARGET} POST_BUILD
        COMMAND "${COMBINE_SCRIPT}" "${CMAKE_AR}" "${OUTPUT}" ${DEPS_FILES}
        COMMENT "Combining ${target} dependencies into single shared library"
        VERBATIM
    )
endfunction()

# ==================================================================================================
# Configuration for CMAKE_CROSSCOMPILING.
# ==================================================================================================
if (WEBGL)
    set(IMPORT_EXECUTABLES ${FILAMENT}/${IMPORT_EXECUTABLES_DIR}/ImportExecutables-Release.cmake)
else()
    set(IMPORT_EXECUTABLES ${FILAMENT}/${IMPORT_EXECUTABLES_DIR}/ImportExecutables-${CMAKE_BUILD_TYPE}.cmake)
endif()

# ==================================================================================================
# Try to find Vulkan if the SDK is installed, otherwise fall back to the bundled version.
# This needs to stay in our top-level CMakeLists because it sets up variables that are used by the
# "bluevk" and "samples" targets.
# ==================================================================================================

if (FILAMENT_SUPPORTS_VULKAN AND APPLE)
    find_library(Vulkan_LIBRARY NAMES vulkan HINTS "$ENV{VULKAN_SDK}/lib" "$ENV{VULKAN_SDK}/macOS/lib")
    if (Vulkan_LIBRARY)
        set(Vulkan_FOUND ON)
        message(STATUS "Found Vulkan library in SDK: ${Vulkan_LIBRARY}.")
        add_definitions(-DFILAMENT_VKLIBRARY_PATH=\"${Vulkan_LIBRARY}\")
    endif()
endif()

if (FILAMENT_USE_SWIFTSHADER)
    find_library(SWIFTSHADER_EGL NAMES EGL HINTS "$ENV{SWIFTSHADER_LD_LIBRARY_PATH}")
    find_library(SWIFTSHADER_GLES NAMES GLESv3 GLESv2 HINTS "$ENV{SWIFTSHADER_LD_LIBRARY_PATH}")
    message(STATUS "Found SwiftShader EGL library in: ${SWIFTSHADER_EGL}.")
    message(STATUS "Found SwiftShader GLES library in: ${SWIFTSHADER_GLES}.")
endif()

# ==================================================================================================
# Common Functions
# ==================================================================================================

# Sets the following variables: RESGEN_HEADER, RESGEN_SOURCE, RESGEN_FLAGS, RESGEN_SOURCE_FLAGS,
# and RESGEN_OUTPUTS. Please pass in an ARCHIVE_NAME that is unique to your project, otherwise the
# incbin directive will happily consume a blob from the wrong project without warnings or errors.
# Also be sure to include the ASM language in the CMake "project" directive for your project.
function(get_resgen_vars ARCHIVE_DIR ARCHIVE_NAME)
    set(OUTPUTS
        ${ARCHIVE_DIR}/${ARCHIVE_NAME}.bin
        ${ARCHIVE_DIR}/${ARCHIVE_NAME}.S
        ${ARCHIVE_DIR}/${ARCHIVE_NAME}.apple.S
        ${ARCHIVE_DIR}/${ARCHIVE_NAME}.h
    )
    if (IOS)
        set(ASM_ARCH_FLAG "-arch ${DIST_ARCH}")
    endif()
    if (APPLE)
        set(ASM_SUFFIX ".apple")
    endif()
    set(RESGEN_HEADER "${ARCHIVE_DIR}/${ARCHIVE_NAME}.h" PARENT_SCOPE)
    # Visual Studio makes it difficult to use assembly without using MASM. MASM doesn't support
    # the equivalent of .incbin, so on Windows we'll just tell resgen to output a C file.
    if (WEBGL OR WIN32 OR ANDROID_ON_WINDOWS)
        set(RESGEN_OUTPUTS "${OUTPUTS};${ARCHIVE_DIR}/${ARCHIVE_NAME}.c" PARENT_SCOPE)
        set(RESGEN_FLAGS -qcx ${ARCHIVE_DIR} -p ${ARCHIVE_NAME} PARENT_SCOPE)
        set(RESGEN_SOURCE "${ARCHIVE_DIR}/${ARCHIVE_NAME}.c" PARENT_SCOPE)
    else()
        set(RESGEN_OUTPUTS "${OUTPUTS}" PARENT_SCOPE)
        set(RESGEN_FLAGS -qx ${ARCHIVE_DIR} -p ${ARCHIVE_NAME} PARENT_SCOPE)
        set(RESGEN_SOURCE "${ARCHIVE_DIR}/${ARCHIVE_NAME}${ASM_SUFFIX}.S" PARENT_SCOPE)
        set(RESGEN_SOURCE_FLAGS "-I${ARCHIVE_DIR} ${ASM_ARCH_FLAG}" PARENT_SCOPE)
    endif()
endfunction()

# ==================================================================================================
# Sub-projects
# ==================================================================================================

# Common to all platforms
add_subdirectory(${EXTERNAL}/libgtest/tnt)
add_subdirectory(${LIBRARIES}/camutils)
add_subdirectory(${LIBRARIES}/filabridge)
add_subdirectory(${LIBRARIES}/filaflat)
add_subdirectory(${LIBRARIES}/filameshio)
add_subdirectory(${LIBRARIES}/geometry)
add_subdirectory(${LIBRARIES}/gltfio)
add_subdirectory(${LIBRARIES}/ibl)
add_subdirectory(${LIBRARIES}/image)
add_subdirectory(${LIBRARIES}/math)
add_subdirectory(${LIBRARIES}/mathio)
add_subdirectory(${LIBRARIES}/utils)
add_subdirectory(${FILAMENT}/filament)
add_subdirectory(${FILAMENT}/shaders)
add_subdirectory(${EXTERNAL}/hat-trie/tnt)
add_subdirectory(${EXTERNAL}/robin-map/tnt)
add_subdirectory(${EXTERNAL}/smol-v/tnt)
add_subdirectory(${EXTERNAL}/benchmark/tnt)
add_subdirectory(${EXTERNAL}/meshoptimizer)
add_subdirectory(${EXTERNAL}/cgltf/tnt)
add_subdirectory(${EXTERNAL}/draco/tnt)
add_subdirectory(${EXTERNAL}/stb/tnt)
add_subdirectory(${EXTERNAL}/getopt)

if (FILAMENT_BUILD_FILAMAT OR IS_HOST_PLATFORM)
    # spirv-tools must come before filamat, as filamat relies on the presence of the
    # spirv-tools_SOURCE_DIR variable.
    add_subdirectory(${EXTERNAL}/spirv-tools)
    add_subdirectory(${EXTERNAL}/glslang/tnt)
    add_subdirectory(${EXTERNAL}/spirv-cross/tnt)
    add_subdirectory(${LIBRARIES}/filamat)

    # the material debugger requires filamat
    if (FILAMENT_ENABLE_MATDBG OR IS_HOST_PLATFORM)
        add_subdirectory(${EXTERNAL}/civetweb/tnt)
        add_subdirectory(${LIBRARIES}/matdbg)
    endif()
endif()

if (FILAMENT_SUPPORTS_VULKAN)
    add_subdirectory(${LIBRARIES}/bluevk)
    add_subdirectory(${EXTERNAL}/vkmemalloc/tnt)
endif()

if (APPLE)
    add_subdirectory(${EXTERNAL}/moltenvk/tnt)
endif()

set(FILAMENT_SAMPLES_BINARY_DIR ${PROJECT_BINARY_DIR}/samples)

if (WEBGL)
    add_subdirectory(web/filament-js)
    add_subdirectory(web/samples)
    add_subdirectory(${EXTERNAL}/imgui/tnt)
endif()

if (IS_HOST_PLATFORM)
    add_subdirectory(${LIBRARIES}/bluegl)
    add_subdirectory(${LIBRARIES}/filamentapp)
    add_subdirectory(${LIBRARIES}/filagui)
    add_subdirectory(${LIBRARIES}/imageio)

    add_subdirectory(${FILAMENT}/java/filamat)
    add_subdirectory(${FILAMENT}/java/filament)
    add_subdirectory(${FILAMENT}/java/gltfio)

    add_subdirectory(${FILAMENT}/samples)

    add_subdirectory(${EXTERNAL}/astcenc/tnt)
    add_subdirectory(${EXTERNAL}/etc2comp)
    add_subdirectory(${EXTERNAL}/imgui/tnt)
    add_subdirectory(${EXTERNAL}/libassimp/tnt)
    add_subdirectory(${EXTERNAL}/libpng/tnt)
    add_subdirectory(${EXTERNAL}/libsdl2/tnt)
    add_subdirectory(${EXTERNAL}/libz/tnt)
    add_subdirectory(${EXTERNAL}/tinyexr/tnt)

    add_subdirectory(${TOOLS}/cmgen)
    add_subdirectory(${TOOLS}/cso-lut)
    add_subdirectory(${TOOLS}/filamesh)
    add_subdirectory(${TOOLS}/glslminifier)
    add_subdirectory(${TOOLS}/matc)
    add_subdirectory(${TOOLS}/matinfo)
    add_subdirectory(${TOOLS}/mipgen)
    add_subdirectory(${TOOLS}/normal-blending)
    add_subdirectory(${TOOLS}/resgen)
    add_subdirectory(${TOOLS}/roughness-prefilter)
    add_subdirectory(${TOOLS}/specular-color)
endif()

if (FILAMENT_USE_SWIFTSHADER)
    add_subdirectory(${EXTERNAL}/swiftshader/tnt)
endif()

# Generate exported executables for cross-compiled builds (Android, WebGL, and iOS)
if (NOT CMAKE_CROSSCOMPILING)
    export(TARGETS matc cmgen filamesh mipgen resgen glslminifier FILE ${IMPORT_EXECUTABLES})
endif()
