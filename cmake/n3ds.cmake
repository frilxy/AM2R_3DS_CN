# Nintendo 3DS (devkitARM + libctru + citro2d/citro3d). Included from CMakeLists.txt when PLATFORM=n3ds.
#
# The program is per game (title, IDs, SD folder come from the N3DS_* settings below; tools/n3ds/build.sh has the
# AM2R profile) and installs as <N3DS_OUTPUT_NAME>.cia; a .3dsx of the same program is built for quick tests.
# The game's files stay on the SD card in sdmc:/3ds/<N3DS_SD_FOLDER>/ and do not change between builds.

option(N3DS_ENABLE_LTO "Enable link-time optimization for the 3DS target" ON)
option(N3DS_ENABLE_AUDIO "NDSP audio (sound_bank.bin + streamed BCWAV music from n3ds-preprocess)" ON)
set(N3DS_APP_NAME "Butterscotch" CACHE STRING "3DS title name (SMDH / CIA)")
set(N3DS_APP_DESCRIPTION "GameMaker runner" CACHE STRING "3DS title description (SMDH)")
set(N3DS_APP_AUTHOR "Butterscotch" CACHE STRING "3DS title author (SMDH)")
set(N3DS_OUTPUT_NAME "butterscotch" CACHE STRING "Output base name (<name>.cia, <name>.3dsx)")
set(N3DS_SD_FOLDER "butterscotch" CACHE STRING "Game folder on the SD card: sdmc:/3ds/<folder>/")
set(N3DS_UNIQUE_ID "0xB5C07" CACHE STRING "CIA unique ID (homebrew range 0x00300-0xF7FFF); title ID 00040000<id>00")
set(N3DS_PRODUCT_CODE "CTR-P-BSCH" CACHE STRING "CIA product code")
set(N3DS_ASSET_DIR "" CACHE PATH "Optional icon.png (48x48), banner.png (256x128) + banner.wav (default: res/n3ds/<N3DS_SD_FOLDER>)")
# The default follows N3DS_SD_FOLDER (a cached default would keep the folder of the first configure).
if(N3DS_ASSET_DIR STREQUAL "" OR N3DS_ASSET_DIR STREQUAL "${CMAKE_SOURCE_DIR}/res/n3ds/butterscotch")
    set(N3DS_ASSET_DIR "${CMAKE_SOURCE_DIR}/res/n3ds/${N3DS_SD_FOLDER}")
endif()
set(N3DS_SCREEN_MODE "wide" CACHE STRING "Default top-screen mode: wide (widescreen hack), stretch or pillarbox")
set(N3DS_TOOLS_DIR "/root/kfx/3ds-prefix/bin" CACHE PATH "Where makerom and bannertool are")

add_compile_definitions(PLATFORM_N3DS)
string(TOUPPER "${N3DS_SCREEN_MODE}" _n3ds_screen_mode)
target_compile_definitions(butterscotch PRIVATE N3DS_DEFAULT_SCREEN_MODE=N3DS_SCREEN_${_n3ds_screen_mode})
target_compile_definitions(butterscotch PRIVATE N3DS_SD_DIR="sdmc:/3ds/${N3DS_SD_FOLDER}/")
# Revision of the SD card data this build expects (tools/n3ds/make_sd.sh writes it to the card). Raise it when a
# build needs regenerated card data (e.g. a new gfx/ format); an older card then gets a clear message at startup.
file(STRINGS "${CMAKE_SOURCE_DIR}/tools/n3ds/sd_data_rev.txt" N3DS_SD_DATA_REV REGEX "^[0-9]+$" LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/n3ds/sd_data_rev.txt")
target_compile_definitions(butterscotch PRIVATE N3DS_SD_DATA_REV=${N3DS_SD_DATA_REV})

if(NOT N3DS_ENABLE_AUDIO)
    get_target_property(_n3ds_sources butterscotch SOURCES)
    list(REMOVE_ITEM _n3ds_sources "${CMAKE_CURRENT_SOURCE_DIR}/src/n3ds/n3ds_audio_system.c")
    set_target_properties(butterscotch PROPERTIES SOURCES "${_n3ds_sources}")
else()
    target_compile_definitions(butterscotch PRIVATE N3DS_ENABLE_AUDIO)
endif()

foreach(_opt ENABLE_VM_GML_PROFILER ENABLE_VM_TRACING ENABLE_VM_OPCODE_PROFILER ENABLE_VM_STUB_LOGS)
    option(${_opt} "" OFF)
    if(${_opt})
        add_compile_definitions(${_opt})
    endif()
endforeach()

option(N3DS_KEYBOARD_MIRROR "D-pad, Start and Select also press the keyboard's arrows, Enter and Escape" ON)
if(N3DS_KEYBOARD_MIRROR)
    target_compile_definitions(butterscotch PRIVATE N3DS_KEYBOARD_MIRROR)
endif()

option(N3DS_FRAME_PROFILER "Per-room frame profile in log.txt: zones, GPU time, GML (also turn on ENABLE_VM_GML_PROFILER)" OFF)
if(N3DS_FRAME_PROFILER)
    target_compile_definitions(butterscotch PRIVATE N3DS_FRAME_PROFILER)
endif()
option(N3DS_NO_TILE_CULL "Draw every room tile, no culling (debug)" OFF)
if(N3DS_NO_TILE_CULL)
    target_compile_definitions(butterscotch PRIVATE N3DS_NO_TILE_CULL)
endif()
option(N3DS_POISON_MALLOC "Fill new heap memory with a pattern, like reused memory on hardware (debug)" OFF)
if(N3DS_POISON_MALLOC)
    target_compile_definitions(butterscotch PRIVATE N3DS_POISON_MALLOC)
    target_link_options(butterscotch PRIVATE -Wl,--wrap=malloc -Wl,--wrap=realloc)
endif()
option(N3DS_DIAG_PATTERN "Draw renderer test squares (debug)" OFF)
if(N3DS_DIAG_PATTERN)
    target_compile_definitions(butterscotch PRIVATE N3DS_DIAG_PATTERN)
endif()

target_include_directories(butterscotch PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/vendor/stb/ds
    ${CMAKE_SOURCE_DIR}/vendor/stb/image
)

target_compile_options(butterscotch PRIVATE -ffunction-sections -fdata-sections)
target_link_options(butterscotch PRIVATE -Wl,--gc-sections)
if(N3DS_ENABLE_LTO)
    target_compile_options(butterscotch PRIVATE -flto)
    target_link_options(butterscotch PRIVATE -flto)
endif()

target_link_libraries(butterscotch PRIVATE bzip2 stb_ds sha1 stb_vorbis citro2d citro3d ctru m)

# romfs: build information only (the game's files live on the SD card).
set(N3DS_ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/romfs")
file(MAKE_DIRECTORY "${N3DS_ROMFS_DIR}")
file(WRITE "${N3DS_ROMFS_DIR}/build.txt" "${N3DS_APP_NAME} on Butterscotch ${BUTTERSCOTCH_COMMIT_HASH}\n")

set(N3DS_SMDH_FILE "${CMAKE_CURRENT_BINARY_DIR}/${N3DS_OUTPUT_NAME}.smdh")
set(_n3ds_icon_args "")
if(EXISTS "${N3DS_ASSET_DIR}/icon.png")
    set(_n3ds_icon_args ICON "${N3DS_ASSET_DIR}/icon.png")
endif()
ctr_generate_smdh(OUTPUT "${N3DS_SMDH_FILE}"
    NAME "${N3DS_APP_NAME}"
    DESCRIPTION "${N3DS_APP_DESCRIPTION}"
    AUTHOR "${N3DS_APP_AUTHOR}"
    ${_n3ds_icon_args}
)
ctr_create_3dsx(butterscotch_3dsx TARGET butterscotch OUTPUT ${N3DS_OUTPUT_NAME}.3dsx SMDH "${N3DS_SMDH_FILE}" ROMFS "${N3DS_ROMFS_DIR}")

# Installable title. As a title it gets its own memory mode (124 MB, 804 MHz, L2 cache on New 3DS: res/n3ds/app.rsf);
# every build has the same title ID, so installing a new .cia replaces the old one and keeps the SD data and saves.
find_program(N3DS_MAKEROM makerom HINTS "${N3DS_TOOLS_DIR}")
# Title version: the commit count, so every build is newer than the last. The HOME Menu keeps a title's icon (SMDH) in
# a cache it refreshes only when the installed version changes; with every build at version 0, a new icon never showed.
set(N3DS_TITLE_VERSION 0)
execute_process(COMMAND git rev-list --count HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _n3ds_commit_count OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _n3ds_git_result)
if(_n3ds_git_result EQUAL 0 AND _n3ds_commit_count MATCHES "^[0-9]+$")
    math(EXPR N3DS_TITLE_VERSION "${_n3ds_commit_count} % 65536")
endif()
message(STATUS "n3ds: title version ${N3DS_TITLE_VERSION}")
find_program(N3DS_BANNERTOOL bannertool HINTS "${N3DS_TOOLS_DIR}")
if(N3DS_MAKEROM)
    set(_n3ds_banner_args "")
    set(_n3ds_banner_deps "")
    if(N3DS_BANNERTOOL AND EXISTS "${N3DS_ASSET_DIR}/banner.png" AND EXISTS "${N3DS_ASSET_DIR}/banner.wav")
        add_custom_command(OUTPUT ${N3DS_OUTPUT_NAME}.bnr
            COMMAND "${N3DS_BANNERTOOL}" makebanner -i "${N3DS_ASSET_DIR}/banner.png" -a "${N3DS_ASSET_DIR}/banner.wav" -o ${N3DS_OUTPUT_NAME}.bnr
            DEPENDS "${N3DS_ASSET_DIR}/banner.png" "${N3DS_ASSET_DIR}/banner.wav"
            VERBATIM)
        set(_n3ds_banner_args -banner ${N3DS_OUTPUT_NAME}.bnr)
        set(_n3ds_banner_deps ${N3DS_OUTPUT_NAME}.bnr)
    endif()
    add_custom_command(OUTPUT ${N3DS_OUTPUT_NAME}.cia
        COMMAND "${N3DS_MAKEROM}" -f cia -o ${N3DS_OUTPUT_NAME}.cia -target t -exefslogo
                -elf "$<TARGET_FILE:butterscotch>"
                -rsf "${CMAKE_SOURCE_DIR}/res/n3ds/app.rsf"
                -icon "${N3DS_SMDH_FILE}" ${_n3ds_banner_args} -ver ${N3DS_TITLE_VERSION}
                "-DAPP_TITLE=${N3DS_APP_NAME}" "-DAPP_PRODUCT_CODE=${N3DS_PRODUCT_CODE}"
                "-DAPP_UNIQUE_ID=${N3DS_UNIQUE_ID}" "-DAPP_ROMFS=${N3DS_ROMFS_DIR}"
        DEPENDS butterscotch butterscotch_3dsx ${_n3ds_banner_deps} "${CMAKE_SOURCE_DIR}/res/n3ds/app.rsf"
        VERBATIM)
    add_custom_target(butterscotch_cia ALL DEPENDS ${N3DS_OUTPUT_NAME}.cia)
else()
    message(STATUS "n3ds: makerom not found in ${N3DS_TOOLS_DIR}; no .cia (only the .3dsx)")
endif()
