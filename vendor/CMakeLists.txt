# Third-party libraries, vendored as git submodules.

if(FIDGET_BUILD_GUI)
    # GLFW: window creation and input handling.
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(glfw)

    # Dear ImGui (docking branch): the GUI library.
    # ImGui ships no CMakeLists, so the target is defined here.
    add_library(imgui STATIC
        imgui/imgui.cpp
        imgui/imgui_demo.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/backends/imgui_impl_glfw.cpp
        imgui/backends/imgui_impl_opengl3.cpp
        imgui/misc/cpp/imgui_stdlib.cpp
    )
    target_include_directories(imgui PUBLIC imgui imgui/backends imgui/misc/cpp)
    target_link_libraries(imgui PUBLIC glfw)

    # ImPlot: plotting library built on ImGui.
    add_library(implot STATIC
        implot/implot.cpp
        implot/implot_items.cpp
        implot/implot_demo.cpp
    )
    target_include_directories(implot PUBLIC implot)
    target_link_libraries(implot PUBLIC imgui)
endif()

# doctest: the test framework. Only the tests link it.
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(doctest)
