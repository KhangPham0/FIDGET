add_executable(fidget main.cpp)

# So sources can include project headers relative to src/, e.g. "ui/App.h".
target_include_directories(fidget PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_package(OpenGL REQUIRED)

# implot links imgui publicly, so imgui and glfw come with it.
target_link_libraries(fidget PRIVATE implot OpenGL::GL)

if(APPLE)
    # The frame clear uses classic OpenGL calls, which macOS marks
    # deprecated but still supports.
    target_compile_definitions(fidget PRIVATE GL_SILENCE_DEPRECATION)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fidget PRIVATE -Wall -Wextra -Wpedantic)
endif()
