cmake_minimum_required(VERSION 3.20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(wayland-experiments C)

find_library(LibM_PATH m REQUIRED)
add_library(LibM STATIC IMPORTED)
set_target_properties(LibM PROPERTIES IMPORTED_LOCATION ${LibM_PATH})

include(FindPkgConfig)
pkg_check_modules(WaylandClient REQUIRED IMPORTED_TARGET "wayland-client")
pkg_check_modules(WaylandEGL REQUIRED IMPORTED_TARGET "wayland-egl")
pkg_check_modules(LibDRM REQUIRED IMPORTED_TARGET "libdrm")
pkg_check_modules(LibGBM REQUIRED IMPORTED_TARGET "gbm")
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET "egl")
pkg_check_modules(GLESv2 REQUIRED IMPORTED_TARGET "glesv2")
pkg_check_modules(WaylandScanner REQUIRED "wayland-scanner")
pkg_get_variable(WAYLAND_SCANNER "wayland-scanner" "wayland_scanner")

# wayland protocols needed by the experiments
set(PROTOCOLDIR "/usr/share/wayland-protocols/" CACHE STRING "wayland-protocols directory")
set(WLRPROTOCOLDIR "${CMAKE_SOURCE_DIR}/wlr-protocols/" CACHE STRING "wlr-protocols directory")
set(PROTOCOLS
    "stable/xdg-shell/xdg-shell.xml"
    "stable/viewporter/viewporter.xml"
    "staging/fractional-scale/fractional-scale-v1.xml"
    "unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml"
    "unstable/wlr-screencopy-unstable-v1.xml"
    "unstable/wlr-export-dmabuf-unstable-v1.xml"
)

# wayland protocol wrapper generation with wayland-scanner
add_library(protocols STATIC)
target_link_libraries(protocols PRIVATE PkgConfig::WaylandClient)
target_include_directories(protocols INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/proto/include/")

foreach(proto ${PROTOCOLS})
    cmake_path(APPEND PROTOCOLDIR ${proto} OUTPUT_VARIABLE wl-protofile)
    cmake_path(APPEND WLRPROTOCOLDIR ${proto} OUTPUT_VARIABLE wlr-protofile)
    if(EXISTS ${wl-protofile})
        message(STATUS "using ${proto} from wayland-protocols")
        set(protofile ${wl-protofile})
    elseif(EXISTS ${wlr-protofile})
        message(STATUS "using ${proto} from wlr-protocols")
        set(protofile ${wlr-protofile})
    else()
        message(FATAL_ERROR "protocol ${proto} not found")
    endif()

    cmake_path(GET protofile STEM protobase)
    set(protoheader "${CMAKE_CURRENT_BINARY_DIR}/proto/include/${protobase}.h")
    set(protosource "${CMAKE_CURRENT_BINARY_DIR}/proto/src/${protobase}.c")

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/proto/include/")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/proto/src/")
    add_custom_command(
        OUTPUT "${protoheader}"
        MAIN_DEPENDENCY "${protofile}"
        COMMAND ${WAYLAND_SCANNER} client-header "${protofile}" "${protoheader}"
    )
    add_custom_command(
        OUTPUT "${protosource}"
        MAIN_DEPENDENCY "${protofile}"
        COMMAND ${WAYLAND_SCANNER} private-code "${protofile}" "${protosource}"
    )
    add_custom_target(gen-${protobase} DEPENDS "${protoheader}" "${protosource}")

    set_source_files_properties("${protoheader}" PROPERTIES GENERATED 1)
    set_source_files_properties("${protosource}" PROPERTIES GENERATED 1)

    add_dependencies(protocols gen-${protobase})
    target_sources(protocols PRIVATE "${protosource}")
endforeach()

# eample targets
file(GLOB experiments CONFIGURE_DEPENDS *.c)
foreach(experiment ${experiments})
    cmake_path(GET experiment STEM name)
    add_executable(${name} ${experiment})
    target_link_libraries(${name} PRIVATE
        protocols LibM
        PkgConfig::WaylandClient
        PkgConfig::LibDRM PkgConfig::LibGBM
        PkgConfig::WaylandEGL PkgConfig::EGL PkgConfig::GLESv2
    )
endforeach()
