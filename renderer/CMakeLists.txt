cmake_minimum_required(VERSION 3.28)
project(portjeuno-renderer CXX)

# Objective-C++ only exists on Apple, and enabling it unconditionally makes the
# configure fail everywhere else.
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dawn's exported target lists Threads::Threads in its link interface but its
# config never looks for it, so find_package(Dawn) fails unless we do this
# first.
find_package(Threads REQUIRED)

find_package(Dawn REQUIRED)
find_package(SDL3 REQUIRED)

if(WIN32)
    set(PJ_SURFACE_SOURCE surface_win32.cpp)
elseif(APPLE)
    set(PJ_SURFACE_SOURCE surface_metal.mm)
else()
    set(PJ_SURFACE_SOURCE surface_linux.cpp)
endif()

add_executable(portjeuno-renderer
    character.cpp
    collision.cpp
    main.cpp
    scene.cpp
    zonemesh.cpp
    ${PJ_SURFACE_SOURCE}
)

target_link_libraries(portjeuno-renderer PRIVATE dawn::webgpu_dawn SDL3::SDL3 ffxi-dat)

if(APPLE)
    target_link_libraries(portjeuno-renderer PRIVATE "-framework QuartzCore" "-framework Cocoa")
    # surface_metal.mm uses __bridge casts, which only exist under ARC. Without
    # this the file does not compile.
    set_source_files_properties(surface_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

if(MSVC)
    add_custom_command(TARGET portjeuno-renderer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:portjeuno-renderer> $<TARGET_FILE_DIR:portjeuno-renderer>
        COMMAND_EXPAND_LISTS
    )
endif()

# The FFXI asset readers, and a CLI that reports what they see. The CLI needs
# neither Dawn nor SDL, so it builds fast and can be checked against the python
# in tools/ that was validated across every zone in ROM/1.
add_library(ffxi-dat STATIC
    ffxi/dat.cpp
    ffxi/filetable.cpp
    ffxi/lighting.cpp
    ffxi/look.cpp
    ffxi/mmb.cpp
    ffxi/mo2.cpp
    ffxi/mzb.cpp
    ffxi/os2.cpp
    ffxi/skeleton.cpp
    ffxi/texture.cpp
)
target_include_directories(ffxi-dat PUBLIC ffxi)

add_executable(ffxi-datdump ffxi/datdump_main.cpp)
target_link_libraries(ffxi-datdump PRIVATE ffxi-dat)

# Skinning a character needs no GPU, so it gets its own CLI and can be checked
# the same way the zone readers were.
add_executable(ffxi-chardump chardump_main.cpp character.cpp)
target_include_directories(ffxi-chardump PRIVATE .)
target_link_libraries(ffxi-chardump PRIVATE ffxi-dat)

# Probing the ground query needs no GPU either, and finding out whether it
# answers from inside a render loop costs a rebuild per guess.
add_executable(ffxi-collisiondump collisiondump_main.cpp collision.cpp)
target_include_directories(ffxi-collisiondump PRIVATE .)
target_link_libraries(ffxi-collisiondump PRIVATE ffxi-dat)
