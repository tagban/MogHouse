cmake_minimum_required(VERSION 3.28)
project(portjeuno-renderer CXX)

# Objective-C++ only exists on Apple, and enabling it unconditionally makes the
# configure fail everywhere else.
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    main.cpp
    ${PJ_SURFACE_SOURCE}
)

target_link_libraries(portjeuno-renderer PRIVATE dawn::webgpu_dawn SDL3::SDL3)

if(APPLE)
    target_link_libraries(portjeuno-renderer PRIVATE "-framework QuartzCore" "-framework Cocoa")
endif()

if(MSVC)
    add_custom_command(TARGET portjeuno-renderer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:portjeuno-renderer> $<TARGET_FILE_DIR:portjeuno-renderer>
        COMMAND_EXPAND_LISTS
    )
endif()
