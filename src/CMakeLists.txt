add_subdirectory(core)
add_subdirectory(hardware)
add_subdirectory(cli)

add_executable(fidget
    main.cpp
    ui/App.cpp
    ui/Fonts.cpp
    ui/ProjectStage.cpp
    ui/SessionStage.cpp
    ui/Theme.cpp
    ui/WorkflowRail.cpp
)

# The version shown in the status strip comes from the project() call.
target_compile_definitions(fidget PRIVATE FIDGET_VERSION="${PROJECT_VERSION}")

# So sources can include project headers relative to src/, e.g. "ui/App.h".
target_include_directories(fidget PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_package(OpenGL REQUIRED)

# implot links imgui publicly, so imgui and glfw come with it.
target_link_libraries(fidget PRIVATE fidget_hardware implot OpenGL::GL)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fidget PRIVATE -Wall -Wextra -Wpedantic)
endif()
