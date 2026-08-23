add_subdirectory(core)
add_subdirectory(presentation)
add_subdirectory(hardware)
add_subdirectory(cli)
add_subdirectory(bridge)

if(NOT FIDGET_BUILD_GUI)
    return()
endif()

find_package(OpenGL REQUIRED)

add_library(fidget_ui STATIC
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
target_include_directories(fidget_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# implot links imgui publicly, so imgui and glfw come with it.
target_link_libraries(fidget_ui
    PUBLIC
        fidget_core
        implot
    PRIVATE
        fidget_gui_presentation
        nfd
        OpenGL::GL
)

add_executable(fidget main.cpp)
target_link_libraries(fidget PRIVATE
    fidget_ui
    fidget_hardware
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fidget_ui PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(fidget PRIVATE -Wall -Wextra -Wpedantic)
endif()
