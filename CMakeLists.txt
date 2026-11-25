cmake_minimum_required(VERSION 3.16)
project(cubspielecom_libretro C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(cubspielecom_libretro SHARED
    src/sm8521.c
    src/lcd.c
    src/audio.c
    src/zip_bios.c
    src/libretro.c
)

set_target_properties(cubspielecom_libretro PROPERTIES
    PREFIX ""
    OUTPUT_NAME "cubspielecom_libretro"
)

target_include_directories(cubspielecom_libretro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(NOT MSVC)
    target_compile_options(cubspielecom_libretro PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(cubspielecom_libretro PRIVATE m)
endif()

if(MINGW)
    target_link_options(cubspielecom_libretro PRIVATE -static-libgcc)
endif()

option(BUILD_SMOKE_TEST "Build standalone native smoke test" ON)
if(BUILD_SMOKE_TEST)
    add_executable(smoke_test
        tests/smoke_test.c
        src/sm8521.c
        src/lcd.c
        src/audio.c
    )
    target_include_directories(smoke_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(NOT MSVC)
        target_link_libraries(smoke_test PRIVATE m)
    endif()
endif()
