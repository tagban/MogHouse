# The C ABI shim MogHouse.Core P/Invokes into.
#
# Built as part of the renderer configure rather than on its own, because the
# only thing it wraps is the renderer, and building it separately would mean
# finding Dawn and SDL twice.
add_library(moghouse_interop SHARED
    src/moghouse_interop.cpp
)

target_include_directories(moghouse_interop PUBLIC include)

# moghouse-render is the renderer as a library - see renderer/viewer.h. Linking
# it here is what makes one application possible: the client loads this rather
# than talking to a second executable.
target_link_libraries(moghouse_interop PRIVATE moghouse-render)

if(MSVC)
    # Dawn and SDL arrive as DLLs; put them beside the shim so a .NET host that
    # loads it finds them without the working directory having to be right.
    add_custom_command(TARGET moghouse_interop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:moghouse_interop> $<TARGET_FILE_DIR:moghouse_interop>
        COMMAND_EXPAND_LISTS
    )
endif()
