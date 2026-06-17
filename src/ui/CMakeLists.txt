# Desktop GUI (Dear ImGui + ImPlot + GLFW/OpenGL3). Built only when
# ARGUS_BUILD_GUI is ON. Dependencies are fetched and compiled from source.
include(FetchContent)

# GLFW
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
# Build both backends on Linux so the app runs natively on Wayland and X11. The
# Wayland backend needs wayland-scanner + wayland-protocols + libxkbcommon at build
# time (see packaging/ and the README for the dev packages).
if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(glfw)

# Dear ImGui (no CMake of its own; we compile the sources we need).
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

# ImPlot (waveform / envelope plots).
FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG v0.16
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(implot)

set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMPLOT_DIR ${implot_SOURCE_DIR})

add_executable(argus-gui
    App.cpp
    Texture.cpp
    Player.cpp
    Settings.cpp
    ${CMAKE_SOURCE_DIR}/third_party/tinyfiledialogs/tinyfiledialogs.c
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
)

if(APPLE)
    target_sources(argus-gui PRIVATE MacMenu.mm)
elseif(WIN32)
    # Embed the application icon into the .exe and give it a friendly name.
    target_sources(argus-gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources/argus.rc)
    set_target_properties(argus-gui PROPERTIES OUTPUT_NAME "Argus" WIN32_EXECUTABLE TRUE)
    # WIN32_EXECUTABLE uses the WINDOWS subsystem (no console window), but our entry
    # point is standard main(); route the WINDOWS-subsystem startup through it.
    if(MSVC)
        target_link_options(argus-gui PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()

target_include_directories(argus-gui PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMPLOT_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/tinyfiledialogs
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${CMAKE_SOURCE_DIR}/third_party/miniaudio
)

target_link_libraries(argus-gui PRIVATE argus_core glfw)

find_package(Threads REQUIRED)
target_link_libraries(argus-gui PRIVATE Threads::Threads)

if(APPLE)
    target_compile_definitions(argus-gui PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(argus-gui PRIVATE
        "-framework OpenGL" "-framework Cocoa" "-framework IOKit" "-framework CoreVideo"
        # miniaudio (CoreAudio) backend.
        "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
elseif(WIN32)
    target_link_libraries(argus-gui PRIVATE opengl32)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(argus-gui PRIVATE OpenGL::GL ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# Bundled resources (font) + macOS .app bundle / icon.
# ---------------------------------------------------------------------------
set(GUI_RESOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/JetBrainsMono-Regular.ttf
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/JetBrainsMono-Bold.ttf
)

if(APPLE)
    set(MACOSX_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/argus.icns)
    set_source_files_properties(${MACOSX_ICON} ${GUI_RESOURCES} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
    target_sources(argus-gui PRIVATE ${MACOSX_ICON} ${GUI_RESOURCES})
    set_target_properties(argus-gui PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "Argus"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "Argus"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.donquinleone.argus"
        MACOSX_BUNDLE_ICON_FILE "argus.icns"
        MACOSX_BUNDLE_BUNDLE_VERSION "${ARGUS_VERSION_FULL}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${ARGUS_VERSION}"
    )
else()
    # On Windows/Linux, copy the font next to the binary so loadFont() finds it.
    add_custom_command(TARGET argus-gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:argus-gui>/resources
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GUI_RESOURCES}
                $<TARGET_FILE_DIR:argus-gui>/resources
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Linux install rules (used by the .deb/.rpm/PKGBUILD/AppImage packaging). The
# GUI finds its bundled font via an executable-relative path at runtime.
# ---------------------------------------------------------------------------
if(NOT APPLE AND NOT WIN32)
    include(GNUInstallDirs)
    install(TARGETS argus-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES ${GUI_RESOURCES} DESTINATION ${CMAKE_INSTALL_DATADIR}/argus/resources)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/argus.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    # 512px icon (linuxdeploy/AppImage rejects larger); install to the themed
    # hicolor path and the legacy pixmaps path.
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/resources/argus_512.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps RENAME argus.png)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/resources/argus_512.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps RENAME argus.png)
endif()
