// FIDGET: a standalone tuner for Mesytec MDPP digitizers behind an MVLC
// controller.
//
// Phase 1 scaffold: open a window with a working ImGui context. The
// application shell (theme, fonts, docking layout) arrives in phase 2.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

namespace {

void GlfwErrorCallback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

} // namespace

int main(int argc, char** argv)
{
    // Smoke test mode: "fidget --frames N" renders N frames and exits, so a
    // build can be verified without a person closing the window.
    long frameLimit = -1;
    if (argc == 3 && std::strcmp(argv[1], "--frames") == 0)
    {
        frameLimit = std::strtol(argv[2], nullptr, 10);
    }

    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit())
    {
        return 1;
    }

    // OpenGL 3.2 core profile: the newest version macOS still supports.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(1280, 800, "FIDGET", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    long frame = 0;
    while (!glfwWindowShouldClose(window))
    {
        if (frameLimit >= 0 && frame++ >= frameLimit)
        {
            break;
        }
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("FIDGET");
        ImGui::TextUnformatted(
            "Phase 1 scaffold. The application shell arrives in phase 2.");
        ImGui::End();

        ImGui::Render();
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        // The window background color from the theme, 0x131316.
        glClearColor(0.075f, 0.075f, 0.086f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
