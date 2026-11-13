cmake_minimum_required(VERSION 3.21)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

project(y-bot-ozz-animation-cglm-opengl-sdl3-cpp-desktop CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenGL REQUIRED)

if(WIN32)
    set(SDL3_DIR "C:/libs/sdl3-3.4.16-mingw/lib/cmake/SDL3")
    set(SDL3_image_DIR "C:/libs/sdl3_image-3.4.6-mingw/lib/cmake/SDL3_image")
else()
    set(SDL3_DIR "$ENV{HOME}/libs/sdl3-3.4.16-ubuntu/lib/cmake/SDL3")
    set(SDL3_image_DIR "$ENV{HOME}/libs/sdl3_image-3.4.6-ubuntu/lib/cmake/SDL3_image")
endif()

find_package(SDL3 REQUIRED CONFIG)
find_package(SDL3_image REQUIRED CONFIG)

if(WIN32)
    set(CGLM_INCLUDE_DIR "C:/libs/cglm-0.9.6/include")
    set(CGLTF_INCLUDE_DIR "C:/libs/cgltf-1.15")
    set(GLAD_INCLUDE_DIR "C:/libs/glad-0.1.36/opengl-3.3/include")
    set(GLAD_SOURCE "C:/libs/glad-0.1.36/opengl-3.3/src/glad.c")
    set(OZZ_INCLUDE_DIR "C:/libs/ozz-animation-0.17.0-mingw/include")
else()
    set(CGLM_INCLUDE_DIR "$ENV{HOME}/libs/cglm-0.9.6/include")
    set(CGLTF_INCLUDE_DIR "$ENV{HOME}/libs/cgltf-1.15")
    set(GLAD_INCLUDE_DIR "$ENV{HOME}/libs/glad-0.1.36/opengl-3.3/include")
    set(GLAD_SOURCE "$ENV{HOME}/libs/glad-0.1.36/opengl-3.3/src/glad.c")
    set(OZZ_INCLUDE_DIR "$ENV{HOME}/libs/ozz-animation-0.17.0-ubuntu/include")
endif()

add_executable(app
    src/main.cpp
    ${GLAD_SOURCE}
)

target_include_directories(app PRIVATE 
    ${CGLM_INCLUDE_DIR}
    ${CGLTF_INCLUDE_DIR}
    ${GLAD_INCLUDE_DIR}
    ${OZZ_INCLUDE_DIR}
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    add_custom_command(
        TARGET app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:app>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets"
                "$<TARGET_FILE_DIR:app>/assets"
        COMMENT "Copying assets directory"
    )
endif()

target_link_options(app PRIVATE -static-libgcc -static-libstdc++)

if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_link_options(app PRIVATE -s)
endif()

if(WIN32)
    target_link_options(app PRIVATE -static)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_link_options(app PRIVATE -mconsole)
    else()
        target_compile_options(app PRIVATE -Os)
        target_link_options(app PRIVATE -mwindows)
    endif()

    target_link_libraries(app PRIVATE 
        SDL3_image::SDL3_image-static
        SDL3::SDL3-static
        OpenGL::GL
        C:/libs/ozz-animation-0.17.0-mingw/lib/libozz_animation_offline_r.a
        C:/libs/ozz-animation-0.17.0-mingw/lib/libozz_animation_r.a
        C:/libs/ozz-animation-0.17.0-mingw/lib/libozz_geometry_r.a
        C:/libs/ozz-animation-0.17.0-mingw/lib/libozz_options_r.a
        C:/libs/ozz-animation-0.17.0-mingw/lib/libozz_base_r.a
    )
else()
    target_link_libraries(app PRIVATE 
        SDL3_image::SDL3_image-static
        SDL3::SDL3-static
        OpenGL::GL
        $ENV{HOME}/libs/ozz-animation-0.17.0-ubuntu/lib/libozz_animation_offline_r.a
        $ENV{HOME}/libs/ozz-animation-0.17.0-ubuntu/lib/libozz_animation_r.a
        $ENV{HOME}/libs/ozz-animation-0.17.0-ubuntu/lib/libozz_geometry_r.a
        $ENV{HOME}/libs/ozz-animation-0.17.0-ubuntu/lib/libozz_options_r.a
        $ENV{HOME}/libs/ozz-animation-0.17.0-ubuntu/lib/libozz_base_r.a
    )
endif()
