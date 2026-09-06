cmake_minimum_required(VERSION 3.16)
project(wallpiper-kde-portal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH
    ${ECM_MODULE_PATH}
    ${ECM_FIND_MODULE_DIR}
    ${CMAKE_MODULE_PATH}
)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Qml Quick)
find_package(EGL REQUIRED)
find_package(Vulkan REQUIRED)
find_package(X11 REQUIRED COMPONENTS Xrandr)

qt_standard_project_setup(REQUIRES 6.7)

qt_add_qml_module(wallpiper_capture_plugin
    URI dev.wallpiper.capture
    VERSION 1.0
    PLUGIN_TARGET wallpiper_capture_plugin
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/dev/wallpiper/capture

    SOURCES
        include/protocol.h
        include/capture_socket.h
        include/ctl_listener.h
        include/capture_coordinator.h
        include/egl_dmabuf_importer.h
        include/vulkan_dmabuf_importer.h
        include/capture_item.h
        include/x11_output_lookup.h

        src/protocol.cc
        src/capture_socket.cc
        src/ctl_listener.cc
        src/capture_coordinator.cc
        src/egl_dmabuf_importer.cc
        src/vulkan_dmabuf_importer.cc
        src/capture_item.cc
        src/x11_output_lookup.cc
)

target_include_directories(wallpiper_capture_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)

target_link_libraries(wallpiper_capture_plugin PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    EGL::EGL
    Vulkan::Vulkan
    ${X11_LIBRARIES}
    ${X11_Xrandr_LIB}
)

target_include_directories(wallpiper_capture_plugin PRIVATE
    ${X11_INCLUDE_DIR}
)

target_compile_options(wallpiper_capture_plugin PRIVATE
    -Os -ffunction-sections -fdata-sections
)
target_link_options(wallpiper_capture_plugin PRIVATE
    -Wl,--gc-sections -s
)

qt6_query_qml_module(wallpiper_capture_plugin QMLDIR wallpiper_capture_qmldir)

install(TARGETS wallpiper_capture_plugin
    LIBRARY DESTINATION "${QT6_INSTALL_QML}/dev/wallpiper/capture"
    RUNTIME DESTINATION "${QT6_INSTALL_QML}/dev/wallpiper/capture"
)

install(
    FILES ${wallpiper_capture_qmldir}
    DESTINATION "${QT6_INSTALL_QML}/dev/wallpiper/capture"
)
