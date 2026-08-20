add_subdirectory(core)
add_subdirectory(hardware)
add_subdirectory(cli)

if(NOT FIDGET_BUILD_GUI)
    return()
endif()

add_executable(fidget
    main.cpp
    ui/AcquisitionStage.cpp
    ui/App.cpp
    ui/ConfigurationStage.cpp
    ui/Fonts.cpp
    ui/ProfileStage.cpp
    ui/ProjectStage.cpp
    ui/SessionStage.cpp
    ui/StartupAuditStage.cpp
    ui/StartupStage.cpp
    ui/Theme.cpp
    ui/UiDialogs.cpp
    ui/WorkflowRail.cpp
)

# So sources can include project headers relative to src/, e.g. "ui/App.h".
target_include_directories(fidget PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_package(OpenGL REQUIRED)

# implot links imgui publicly, so imgui and glfw come with it.
target_link_libraries(fidget PRIVATE
    fidget_hardware
    implot
    nfd
    OpenGL::GL
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fidget PRIVATE -Wall -Wextra -Wpedantic)
endif()
