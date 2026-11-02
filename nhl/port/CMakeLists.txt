cmake_minimum_required(VERSION 3.20)
project(NHLLegacy
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "NHL Legacy Edition (BLUS31540) recompiled with ps3recomp"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
# NHL #156: profile-guided optimisation, set before the runtime subdirectory so it
# covers the runtime library too. NHL_PGO_GEN=ON = instrumented build (counters;
# boot_main.cpp dumps them every 30 s to LLVM_PROFILE_FILE, since a killed run never
# reaches the exit-time write). NHL_PGO_USE=<merged .profdata> = optimised build.
option(NHL_PGO_GEN "Instrumented build for profile-guided optimisation" OFF)
set(NHL_PGO_USE "" CACHE FILEPATH "Merged .profdata for a profile-guided build")
if(NHL_PGO_GEN)
    add_compile_options(-fprofile-instr-generate)
elseif(NHL_PGO_USE)
    add_compile_options("-fprofile-instr-use=${NHL_PGO_USE}"
                        -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date
                        -Wno-profile-instr-missing)
endif()

add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    # The generator discovers handlers from library definitions. Rebuild when
    # an existing module changes, and reconfigure when a new module is added.
    file(GLOB_RECURSE HLE_NID_SOURCES CONFIGURE_DEPENDS
        "${PS3RECOMP_DIR}/libs/*.c")
    # A removed input cannot make the output older than a remaining dependency.
    # Track the list itself so deletions also remove stale handler references.
    set(HLE_NID_SOURCE_LIST "${CMAKE_CURRENT_BINARY_DIR}/hle_nid_sources.txt")
    file(GENERATE OUTPUT "${HLE_NID_SOURCE_LIST}" CONTENT "${HLE_NID_SOURCES}\n")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${HLE_NID_SOURCES} "${HLE_NID_SOURCE_LIST}"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(PORT_SPU_INPUTS "" CACHE STRING
    "Input files to scan for all embedded SPU ELF programs (semicolon-separated decrypted ELF/PRX/data paths)")
set(SPU_SOURCES "")
if(PORT_ENABLE_SPU AND PORT_SPU_INPUTS)
    include("${PS3RECOMP_DIR}/cmake/PS3RecompSPU.cmake")
    ps3recomp_generate_spu("${CMAKE_CURRENT_BINARY_DIR}/generated_spu" ${PORT_SPU_INPUTS})
    list(APPEND SPU_SOURCES ${SPU_GENERATED_SOURCES}
         "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
elseif(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

add_executable(${PROJECT_NAME}
    boot_main.cpp   # ps3recomp's instrumented harness (watchdog, PS3_DEBUG console, sampler)
    nhl_hle.cpp     # NHL: port-specific HLE handlers
    nhl_config.cpp  # NHL: nhl_settings.ini + built-in run defaults (#221)
    nhl_vp6_bridge.cpp  # NHL: host-decode (ffmpeg) bridge for the VP6 movies
    nhl_edge.c      # NHL: Edge Geometry job recon (#268)
    nhl_edge_native.c  # NHL: native Edge decompress + skin + compress (#292)
    stubs.cpp
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
if(SPU_SOURCES)
    # #137 bisect: NHL_SPU_SCALAR_INT=ON builds the lifted SPU code with the scalar _ref
    # integer helpers (SPU_SCALAR_INT) instead of the x86 SIMD forms.
    option(NHL_SPU_SCALAR_INT "Lifted SPU code uses the scalar integer helpers" OFF)
    set(_spu_defs "SPU_USE_DISPATCH_MT;SPU_LS_PROBES=0")   # #71: no per-access LS debug probes
    if(NHL_SPU_SCALAR_INT)
        list(APPEND _spu_defs "SPU_SCALAR_INT")
    endif()
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS "${_spu_defs}")
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # NHL: AVX2 (with SSE4.1) for the lifted SPU code. Without a target flag
        # the build is baseline x86-64 (SSE2), which compiles out the SSE4.1
        # paths in spu_helpers.h (shufb became a scalar 16-byte loop) and leaves
        # nothing to vectorize with. The game's SPU VP6 decode took ~65 ms a
        # frame. -ffp-contract=off: separate SPU fm/fa must not fuse into FMAs.
        # NHL_SPU_ARCH=OFF reverts to the baseline target.
        # NHL_SPU_AVX512 (#70, needs an AVX-512 CPU, e.g. Zen 4/5): the SPU float ops become one
        # native {rz-sae} instruction each (bit-exact, exact fallback). OFF = the AVX2 build.
        # #137: no stack-protector cookies in the lifted SPU code (/GS- / -fno-stack-protector):
        # every lifted function with a stack temporary paid a cookie store + check per call.
        option(NHL_SPU_ARCH "Build the lifted SPU code for AVX2" ON)
        option(NHL_SPU_AVX512 "Build the lifted SPU code for AVX-512" ON)
        if(MSVC)
            # /Zc:tlsGuards-: same as the PPU code (#261), g_spu_trampoline_fn is constant-initialised
            set(_spu_opts_avx512 "-O2;/arch:AVX512;-ffp-contract=off;/GS-;/Zc:tlsGuards-")
            set(_spu_opts_avx2 "-O2;/arch:AVX2;-ffp-contract=off;/GS-;/Zc:tlsGuards-")
        else()
            set(_spu_opts_avx512 "-O2;-march=x86-64-v4;-ffp-contract=off;-fno-stack-protector")
            set(_spu_opts_avx2 "-O2;-march=x86-64-v3;-ffp-contract=off;-fno-stack-protector")
        endif()
        if(NHL_SPU_ARCH AND NHL_SPU_AVX512)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "${_spu_opts_avx512}")
        elseif(NHL_SPU_ARCH)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "${_spu_opts_avx2}")
        else()
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
        endif()
        # #257 NHL_SPU_DUAL: one exe for every CPU. The AVX-512 build above dies with an
        # illegal instruction on CPUs without AVX-512 (Steam Deck / Zen 2, most Intel
        # desktop chips), so the lifted SPU code is also built for AVX2 through the
        # renaming wrappers in spu_avx2/ (tools/nhl_spu_dual.py), and nhl_spu_isa.c picks
        # the set by CPUID at startup (NHL_SPU_ISA=avx2 forces the AVX2 one). The two
        # registration TUs and the dispatcher run before that check, so they are AVX2.
        option(NHL_SPU_DUAL "Also build the lifted SPU code for AVX2 and pick by CPU at startup" ON)
        if(NHL_SPU_DUAL AND NHL_SPU_ARCH AND NHL_SPU_AVX512 AND EXISTS "${SPU_DIR}/spu_workloads.c")
            file(GLOB SPU_AVX2_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/spu_avx2/*.c")
            if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/spu_avx2/spu_workloads_avx2.c")
                message(FATAL_ERROR "NHL_SPU_DUAL: run python -I tools/nhl_spu_dual.py first (or -DNHL_SPU_DUAL=OFF)")
            endif()
            list(LENGTH SPU_AVX2_SOURCES _n)
            message(STATUS "Lifted SPU AVX2: ${_n} wrapper(s), picked by CPU at startup")
            target_sources(${PROJECT_NAME} PRIVATE ${SPU_AVX2_SOURCES} nhl_spu_isa.c)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES
                COMPILE_DEFINITIONS "${_spu_defs};NHL_SPU_VARIANT=512")
            # #286: the AVX2 copy uses RPCS3-Approximate-style float (no result check / fix)
            option(NHL_SPU_FAST_FLOAT "AVX2 SPU copy: approximate float like RPCS3 (no exact fix-ups)" ON)
            if(NHL_SPU_FAST_FLOAT)
                set(_spu_fast 1)
            else()
                set(_spu_fast 0)
            endif()
            set_source_files_properties(${SPU_AVX2_SOURCES} PROPERTIES
                INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
                COMPILE_DEFINITIONS "${_spu_defs};NHL_SPU_VARIANT=2;NHL_SPU_FAST_FLOAT=${_spu_fast}"
                COMPILE_OPTIONS "${_spu_opts_avx2}")
            set_source_files_properties("${SPU_DIR}/spu_workloads.c"
                "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c"
                PROPERTIES COMPILE_OPTIONS "${_spu_opts_avx2}")
        endif()
    endif()
endif()

if(MSVC)
    set_source_files_properties(nhl_edge_native.c PROPERTIES COMPILE_OPTIONS "-O2;/arch:AVX2;/GS-")   # #292: intrinsics, AVX2 is the minimum CPU
else()
    set_source_files_properties(nhl_edge_native.c PROPERTIES COMPILE_OPTIONS "-O2;-march=x86-64-v3;-fno-stack-protector")
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
        # IID_IDXGIFactory6 and the other DXGI/D3D12 interface GUIDs; the
        # backend's #pragma comment(lib) list does not include it.
        dxguid
    )
    # NHL: a link map, so watchdog/sampler RVAs in runtime code can be named.
    target_link_options(${PROJECT_NAME} PRIVATE "/MAP:${CMAKE_BINARY_DIR}/NHLLegacy.map")
    # NHL #283: hot functions first, packed together (tools/nhl_order.py from Deck perf samples),
    # so the hot code fits fewer cache lines and pages. Empty = linker default layout.
    set(NHL_ORDER_FILE "" CACHE FILEPATH "lld-link /order file (hot functions first)")
    if(NHL_ORDER_FILE)
        target_link_options(${PROJECT_NAME} PRIVATE "/order:@${NHL_ORDER_FILE}")
        set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS "${NHL_ORDER_FILE}")
    endif()
    if(NHL_PGO_GEN)   # #156: the profile runtime (the clang driver would add it; lld-link does not)
        get_filename_component(_tcbin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        file(GLOB _prof_rt "${_tcbin}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
        if(NOT _prof_rt)
            message(FATAL_ERROR "NHL_PGO_GEN: clang_rt.profile-x86_64.lib not found under ${_tcbin}/../lib/clang")
        endif()
        list(GET _prof_rt 0 _prof_rt)
        target_link_libraries(${PROJECT_NAME} PRIVATE "${_prof_rt}")
        target_link_options(${PROJECT_NAME} PRIVATE "/INCLUDE:__llvm_profile_runtime")
        target_compile_definitions(${PROJECT_NAME} PRIVATE NHL_PGO_GEN=1)
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
        # The lift spells PPC arithmetic as signed C arithmetic, which must wrap;
        # without this, clang exploits overflow UB (e.g. folds "x*x < 0" to false).
        -fwrapv
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
#
# NHL_PPU_FMA (#146): the lifted PPU code for FMA3. Baseline x86-64 has no FMA
# instruction, so every fmaf() -- one per lane in vmx_vmaddfp/vnmsubfp, ~120k
# lifted lines -- was a call into ucrt's software fma (vmx_vmaddfp ~9% of the
# render thread). Hardware FMA rounds exactly like it. -ffp-contract=off keeps
# separate PPU multiplies and adds unfused once FMA exists (clang-cl needs the
# /clang: spelling). Needs Haswell / Zen 1 or newer (Steam Deck: yes).
option(NHL_PPU_FMA "Build the lifted PPU code for FMA3" ON)   # default since play206 (user OK)
# [#261] NHL_PPU_V3: the lifted PPU code for x86-64-v3 (MOVBE folds the guest byte swap into
# the load/store, BMI2/LZCNT for rotate-masks and cntlzw). No CPU is lost: the exe already
# needs AVX2 (the SPU code has only AVX-512 and AVX2 sets). /Zc:tlsGuards-: every
# DRAIN_TRAMPOLINE read of the thread-local g_trampoline_fn carried an on-demand TLS-init
# check (~7% of the lifted instructions); it is constant-initialised (= nullptr) and the
# exe's TLS callback runs any dynamic TLS init at thread start, so the guard is never needed.
option(NHL_PPU_V3 "Build the lifted PPU code for x86-64-v3" ON)
if(MSVC)
    set(_recomp_opts "/w;/bigobj;/Zc:tlsGuards-")
    if(NHL_PPU_FMA)
        list(APPEND _recomp_opts "/clang:-mfma" "/clang:-ffp-contract=off")
    endif()
    if(NHL_PPU_V3)
        list(APPEND _recomp_opts "/clang:-march=x86-64-v3")
    endif()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${_recomp_opts}")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
