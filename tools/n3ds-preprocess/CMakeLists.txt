cmake_minimum_required(VERSION 3.21)
project(n3ds_preprocess C CXX)

# Host tool: converts a data.win's textures (Tex3DS .t3x, written by t3x.c) and audio (BCWAV) into the 3DS asset
# layout. It needs no other tools, so it also builds for the browser (Emscripten).

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

set(BS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(n3ds-preprocess
    main.c
    stb_impl.c
    t3x.c
    etc1.cpp
    ${BS_ROOT}/vendor/rg_etc1/rg_etc1.cpp
    ${BS_ROOT}/src/binary_reader.c
    ${BS_ROOT}/src/data_win.c
    ${BS_ROOT}/src/log.c
    ${BS_ROOT}/src/image/image_decoder.c
    ${BS_ROOT}/vendor/bzip2/bz_internal_error.c
    ${BS_ROOT}/vendor/bzip2/bzlib.c
    ${BS_ROOT}/vendor/bzip2/crctable.c
    ${BS_ROOT}/vendor/bzip2/decompress.c
    ${BS_ROOT}/vendor/bzip2/huffman.c
    ${BS_ROOT}/vendor/bzip2/randtable.c
)

target_include_directories(n3ds-preprocess PRIVATE
    ${BS_ROOT}/src
    ${BS_ROOT}/src/image
    ${BS_ROOT}/vendor
    ${BS_ROOT}/vendor/bzip2
    ${BS_ROOT}/vendor/stb/ds
    ${BS_ROOT}/vendor/stb/image
)

target_compile_definitions(n3ds-preprocess PRIVATE
    ENABLE_WAD14 ENABLE_WAD16 ENABLE_WAD17
    N3DS_PREPROCESS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    N3DS_PREPROCESS_TEXTURE_OVERRIDE_DIR="${BS_ROOT}/resources/n3ds/textureOverrides"
)
set(CMAKE_CXX_STANDARD 11)
set_source_files_properties(${BS_ROOT}/vendor/bzip2/bzlib.c ${BS_ROOT}/vendor/bzip2/decompress.c ${BS_ROOT}/vendor/bzip2/huffman.c
    ${BS_ROOT}/vendor/bzip2/crctable.c ${BS_ROOT}/vendor/bzip2/randtable.c ${BS_ROOT}/vendor/bzip2/bz_internal_error.c ${BS_ROOT}/vendor/rg_etc1/rg_etc1.cpp
    PROPERTIES COMPILE_OPTIONS "-w")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(n3ds-preprocess PRIVATE m)
endif()

# Same textures as tex3ds, compared byte for byte: tools/n3ds-preprocess/t3x_cli.c takes tex3ds's command line.
add_executable(t3x-cli EXCLUDE_FROM_ALL t3x_cli.c t3x.c etc1.cpp stb_impl_image.c ${BS_ROOT}/vendor/rg_etc1/rg_etc1.cpp)
target_include_directories(t3x-cli PRIVATE ${BS_ROOT}/vendor)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(t3x-cli PRIVATE m)
endif()
