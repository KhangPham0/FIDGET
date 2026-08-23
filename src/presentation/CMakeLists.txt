add_library(fidget_gui_presentation STATIC
    GuiPresentation.cpp
)

target_include_directories(fidget_gui_presentation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(fidget_gui_presentation PUBLIC fidget_core)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fidget_gui_presentation PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()
