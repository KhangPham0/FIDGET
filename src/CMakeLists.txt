add_subdirectory(core)
add_subdirectory(presentation)
add_subdirectory(hardware)
add_subdirectory(cli)
add_subdirectory(bridge)

if(NOT FIDGET_BUILD_GUI)
    return()
endif()

find_package(OpenGL REQUIRED)

add_executable(fidget_font_embed EXCLUDE_FROM_ALL
    ${PROJECT_SOURCE_DIR}/vendor/imgui/misc/fonts/binary_to_compressed_c.cpp
)

set(FIDGET_GENERATED_FONT_DIRECTORY
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ui/fonts
)

function(fidget_embed_font input_name symbol output_name)
    set(output_path ${FIDGET_GENERATED_FONT_DIRECTORY}/${output_name})
    add_custom_command(
        OUTPUT ${output_path}
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${FIDGET_GENERATED_FONT_DIRECTORY}
        COMMAND ${CMAKE_COMMAND}
            -DFIDGET_FONT_EMBED_TOOL=$<TARGET_FILE:fidget_font_embed>
            -DFIDGET_FONT_INPUT_DIRECTORY=${PROJECT_SOURCE_DIR}/vendor/imgui/misc/fonts
            -DFIDGET_FONT_INPUT_NAME=${input_name}
            -DFIDGET_FONT_SYMBOL=${symbol}
            -DFIDGET_FONT_OUTPUT=${output_path}
            -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFont.cmake
        DEPENDS
            fidget_font_embed
            ${PROJECT_SOURCE_DIR}/vendor/imgui/misc/fonts/${input_name}
            ${PROJECT_SOURCE_DIR}/cmake/EmbedFont.cmake
        VERBATIM
    )
    set(FIDGET_GENERATED_FONT_HEADERS
        ${FIDGET_GENERATED_FONT_HEADERS}
        ${output_path}
        PARENT_SCOPE
    )
endfunction()

fidget_embed_font(Karla-Regular.ttf KarlaRegular KarlaRegular.h)
fidget_embed_font(Roboto-Medium.ttf RobotoMedium RobotoMedium.h)
fidget_embed_font(Cousine-Regular.ttf CousineRegular CousineRegular.h)

add_custom_target(fidget_embedded_fonts
    DEPENDS ${FIDGET_GENERATED_FONT_HEADERS}
)

add_library(fidget_ui STATIC
    ui/AcquisitionStage.cpp
    ui/App.cpp
    ui/ConfigurationStage.cpp
    ui/Fonts.cpp
    ui/GuiShell.cpp
    ui/ProfileStage.cpp
    ui/ProjectStage.cpp
    ui/SessionStage.cpp
    ui/StartupAuditStage.cpp
    ui/StartupStage.cpp
    ui/Theme.cpp
    ui/UiDialogs.cpp
    ui/WorkflowRail.cpp
)

add_dependencies(fidget_ui fidget_embedded_fonts)

# So sources can include project headers relative to src/, e.g. "ui/App.h".
target_include_directories(fidget_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(fidget_ui PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

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
