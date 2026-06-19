cmake_minimum_required(VERSION 3.30)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(kinema VERSION 1.0.0)

# macOS: we handle dylib bundling and rpath manually in a post-build script,
# so disable CMake's automatic install_name_tool rpath rewriting.
if(APPLE)
    set(CMAKE_SKIP_INSTALL_RPATH TRUE)
endif()

set(KINEMA_ASSETS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets)

# Geni Engine
# Prevent third-party sub-libraries from installing their own files into the package
set(INSTALL_LIBS OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_FILES OFF CACHE BOOL "" FORCE)
set(INSTALL_EXTRA_LIBS OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/external/geni)

# OpenCV
find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio highgui)

# ImGui via FetchContent
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.0
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

# GLFW
find_package(glfw3 CONFIG REQUIRED)

# GLEW
find_package(GLEW REQUIRED)

set(SRC_FILES
    src/main.cpp
    src/Application.cpp
    src/modules/MarkerDetection.cpp
    src/modules/MarkerConfigIO.cpp
    src/modules/HSVMarkerDetector.cpp
    src/modules/RGBRatioMarkerDetector.cpp
    src/modules/MarkerAssigner.cpp
    src/modules/MarkerStabilizer.cpp
    src/modules/MotionRecorder.cpp
    src/modules/SkeletonDriver.cpp
    src/modules/UDPReceiver.cpp
    src/modules/VideoExporter.cpp
    src/modules/GlbExporter.cpp
    src/ui/UIManager.cpp
    ${IMGUI_SOURCES}
)

add_executable(${PROJECT_NAME} ${SRC_FILES})

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "Kinema"
)

# macOS: Build as .app bundle so CPack can package a real app
if(APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.kinema.app"
        MACOSX_BUNDLE_BUNDLE_NAME "Kinema"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "icon.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
    )
endif()

# Windows: embed icon into the executable via .rc file
if(WIN32)
    set(KINEMA_RC "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kinema.rc")
    if(EXISTS "${KINEMA_RC}")
        target_sources(${PROJECT_NAME} PRIVATE ${KINEMA_RC})
    endif()
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    src/
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${GLFW_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
    external/geni/thirdparty/stb
    external/geni/thirdparty/cgltf
    external/geni/include
    external/pfd
)

target_link_libraries(${PROJECT_NAME}
    GeniEngine
    glfw
    GLEW::GLEW
    ${OpenCV_LIBS}
)

# Copy assets next to executable in build tree
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${KINEMA_ASSETS_DIR}
    $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets
)

# ── Bundle Runtime Dependencies (post-build, before install) ──────────
if(APPLE)
    # Copy dylibs into .app's Frameworks/ and fix install_names
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -D "_EXE=$<TARGET_FILE:${PROJECT_NAME}>"
            -D "_BUNDLE=$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fixup_bundle.cmake"
        COMMENT "Bundling dylib dependencies into .app bundle..."
    )
endif()

if(WIN32)
    # Copy runtime DLLs next to the exe
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -D "_EXE=$<TARGET_FILE:${PROJECT_NAME}>"
            -D "_OUTPUT_DIR=$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_dlls.cmake"
        COMMENT "Collecting runtime DLL dependencies..."
    )
endif()

# ── Install Rules ──────────────────────────────────────────────────
if(APPLE)
    # Copy the already self-contained .app bundle from the build tree.
    # We use install(DIRECTORY) instead of install(TARGETS BUNDLE) so that
    # CMake does NOT run install_name_tool on the binary (which would break
    # the code signature our fixup script already created).
    install(DIRECTORY ${CMAKE_BINARY_DIR}/Kinema.app
        DESTINATION .
        USE_SOURCE_PERMISSIONS
        PATTERN "*.dSYM" EXCLUDE
    )
    # Copy icon into Resources (must also be in build tree for CMake to find it)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.icns
        DESTINATION ${CMAKE_BINARY_DIR}/Kinema.app/Contents/Resources)
else()
    # Windows / Linux: flat layout — exe + DLLs + assets/ in the same folder
    install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION .
    )
    install(DIRECTORY ${KINEMA_ASSETS_DIR}/
        DESTINATION assets
    )
    # DLLs are already next to the exe (copied by POST_BUILD above)
    install(DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>/
        DESTINATION .
        FILES_MATCHING
        PATTERN "*.dll"
    )
endif()

# ── Packaging (CPack) ──────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "Kinema")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Kinema")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Real-time motion capture and 3D skeleton driving application")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Kinema")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "Kinema")
    set(CPACK_DMG_FORMAT "UDZO")
    set(CPACK_DMG_DS_STORE "")
elseif(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "Kinema")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_NSIS_CREATE_ICONS_EXTRA "
        CreateShortCut '$SMPROGRAMS\\\\Kinema.lnk' '$INSTDIR\\\\Kinema.exe'
        CreateShortCut '$DESKTOP\\\\Kinema.lnk' '$INSTDIR\\\\Kinema.exe'
    ")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA "
        Delete '$SMPROGRAMS\\\\Kinema.lnk'
        Delete '$DESKTOP\\\\Kinema.lnk'
    ")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
