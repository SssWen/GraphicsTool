set(sources
    vk_common.cpp
    vk_common.h
    vk_next_chains.cpp
    vk_core.cpp
    vk_core.h
    vk_counters.cpp
    vk_debug.h
    vk_debug.cpp
    vk_postvs.cpp
    vk_shader_feedback.cpp
    vk_overlay.cpp
    vk_msaa_buffer_conv.cpp
    vk_outputwindow.cpp
    vk_rendermesh.cpp
    vk_rendertexture.cpp
    vk_rendertext.h
    vk_rendertext.cpp
    vk_shader_cache.h
    vk_shader_cache.cpp
    vk_dispatchtables.cpp
    vk_dispatchtables.h
    vk_dispatch_defs.h
    vk_hookset_defs.h
    vk_image_states.cpp
    vk_info.cpp
    vk_info.h
    vk_initstate.cpp
    vk_manager.cpp
    vk_manager.h
    vk_memory.cpp
    vk_pixelhistory.cpp
    vk_replay.cpp
    vk_replay.h
    vk_resources.cpp
    vk_resources.h
    vk_shaderdebug.cpp
    vk_state.cpp
    vk_state.h
    vk_serialise.cpp
    vk_stringise.cpp
    vk_layer.cpp
    vk_acceleration_structure.h
    vk_acceleration_structure.cpp
    imagestate_tests.cpp
    imgrefs_tests.cpp
    official/vk_layer.h
    official/vk_platform.h
    official/vulkan.h
    official/vulkan_android.h
    official/vulkan_core.h
    official/vulkan_ios.h
    official/vulkan_macos.h
    official/vulkan_fuchsia.h
    official/vulkan_vi.h
    official/vulkan_wayland.h
    official/vulkan_win32.h
    official/vulkan_xcb.h
    official/vulkan_xlib.h
    official/vulkan_xlib_xrandr.h
    wrappers/vk_cmd_funcs.cpp
    wrappers/vk_descriptor_funcs.cpp
    wrappers/vk_device_funcs.cpp
    wrappers/vk_draw_funcs.cpp
    wrappers/vk_dynamic_funcs.cpp
    wrappers/vk_get_funcs.cpp
    wrappers/vk_misc_funcs.cpp
    wrappers/vk_queue_funcs.cpp
    wrappers/vk_resource_funcs.cpp
    wrappers/vk_shader_funcs.cpp
    wrappers/vk_sync_funcs.cpp
    wrappers/vk_wsi_funcs.cpp)

set(definitions ${RDOC_DEFINITIONS})

set(VULKAN_LAYER_FOLDER_DEFAULT /etc/vulkan/implicit_layer.d)

if(NOT ANDROID AND NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT DEFINED VULKAN_LAYER_FOLDER)
    message(WARNING "*** CMAKE_INSTALL_PREFIX has been customised to ${CMAKE_INSTALL_PREFIX}, but VULKAN_LAYER_FOLDER is not customised and defaults to ${VULKAN_LAYER_FOLDER_DEFAULT}. This may not do what you expect, e.g. installing to a non-root location ***")
endif()

# This must be specified separately because it needs to go in /etc regardless of what the install
# prefix is, since the loader only looks in a set location (and /usr/share is reserved for distribution
# packages). For people who want to 'make install' to another folder, perhaps for preparing a package,
# they can set this variable to make sure it stays local
set(VULKAN_LAYER_FOLDER ${VULKAN_LAYER_FOLDER_DEFAULT} CACHE PATH "Path to install the vulkan layer file")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_property(SOURCE vk_shaderdebug.cpp
        APPEND_STRING PROPERTY COMPILE_FLAGS " -Wno-shadow -Wno-shorten-64-to-32")
endif()

if(ANDROID)
    list(APPEND sources vk_posix.cpp vk_android.cpp vk_layer_android.cpp)
    list(APPEND definitions PRIVATE -DVK_USE_PLATFORM_ANDROID_KHR)
elseif(APPLE)
    list(APPEND sources vk_posix.cpp vk_apple.cpp vk_apple_helpers.mm)

    add_definitions(-DVK_USE_PLATFORM_MACOS_MVK -DVK_USE_PLATFORM_METAL_EXT)
elseif(UNIX)
    list(APPEND sources vk_posix.cpp vk_linux.cpp)

    if(ENABLE_XLIB)
        add_definitions(-DVK_USE_PLATFORM_XLIB_KHR -DVK_USE_PLATFORM_XLIB_XRANDR_EXT)
    endif()

    if(ENABLE_XCB)
        add_definitions(-DVK_USE_PLATFORM_XCB_KHR)
    endif()

    if(ENABLE_UNSUPPORTED_EXPERIMENTAL_POSSIBLY_BROKEN_WAYLAND)
        add_definitions(-DVK_USE_PLATFORM_WAYLAND_KHR)
    endif()
    
    add_definitions(-DVK_USE_PLATFORM_WIN32_KHR)

    set(VULKAN_LAYER_MODULE_PATH "${CMAKE_INSTALL_PREFIX}/lib${LIB_SUFFIX}/${LIB_SUBFOLDER_TRAIL_SLASH}lib${RDOC_BASE_NAME}.so")
    set(VULKAN_ENABLE_VAR "ENABLE_VULKAN_${RDOC_BASE_NAME_UPPER}_CAPTURE")

    set(json_in ${CMAKE_CURRENT_SOURCE_DIR}/renderdoc.json)
    set(json_out ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${RDOC_BASE_NAME}_capture${VULKAN_JSON_SUFFIX}.json)
    
    configure_file(${json_in} ${json_out})

    install (FILES ${json_out} DESTINATION ${VULKAN_LAYER_FOLDER})
endif()

add_library(rdoc_vulkan OBJECT ${sources})
target_compile_definitions(rdoc_vulkan ${definitions})
target_include_directories(rdoc_vulkan ${RDOC_INCLUDES})
