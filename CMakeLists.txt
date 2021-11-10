cmake_minimum_required(VERSION 3.20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(wayland-experiments C)

# wayland protocols needed by the experiments
set(PROTOCOLDIR "/usr/share/wayland-protocols/" CACHE STRING "wayland-protocols directory")
set(PROTOCOLS
    "stable/xdg-shell/xdg-shell.xml"
)

# wayland protocol wrapper generation with wayland-scanner
add_library(protocols STATIC)
target_include_directories(protocols INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/proto/include/")
foreach(proto ${PROTOCOLS})
    cmake_path(APPEND PROTOCOLDIR ${proto} OUTPUT_VARIABLE protofile)
    cmake_path(GET protofile STEM protobase)
    set(protoheader "${CMAKE_CURRENT_BINARY_DIR}/proto/include/${protobase}.h")
    set(protosource "${CMAKE_CURRENT_BINARY_DIR}/proto/src/${protobase}.c")

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/proto/include/")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/proto/src/")
    add_custom_command(
        OUTPUT "${protoheader}"
        MAIN_DEPENDENCY "${protofile}"
        COMMAND wayland-scanner client-header "${protofile}" "${protoheader}"
    )
    add_custom_command(
        OUTPUT "${protosource}"
        MAIN_DEPENDENCY "${protofile}"
        COMMAND wayland-scanner private-code "${protofile}" "${protosource}"
    )
    add_custom_target(gen-${protobase} DEPENDS "${protoheader}" "${protosource}")

    set_source_files_properties("${protoheader}" PROPERTIES GENERATED 1)
    set_source_files_properties("${protosource}" PROPERTIES GENERATED 1)

    add_dependencies(protocols gen-${protobase})
    target_sources(protocols PRIVATE "${protosource}")
endforeach()

# eample targets
file(GLOB experiments *.c)
foreach(experiment ${experiments})
    cmake_path(GET experiment STEM name)
    add_executable(${name} ${experiment})
    target_link_libraries(${name} PRIVATE wayland-client protocols)
endforeach()
