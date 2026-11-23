# Project setup
project (ValiDDK)
enable_language (C)

# Nasm is only used for x86 and x86_64 architectures
if (VALI_ARCH STREQUAL "i386" OR VALI_ARCH STREQUAL "amd64")
    enable_language(ASM_NASM)
    set (CMAKE_ASM_NASM_OBJECT_FORMAT ${NASM_DEFAULT_FORMAT})
endif ()

# The gracht client stubs that are built into libddk. Each name is a protocol
# under protocols/, and its generated header and source are found in the
# protocols build directory.
set (PROTOCOL_CLIENTS
    sys_session
    sys_storage
    sys_file
    sys_mount
    sys_process
    sys_library
    sys_socket
    sys_device
    sys_usb
    ctt_driver
    ctt_storage
    ctt_usbhub
    ctt_usbhost
    ctt_input
)

set (GENERATED_SOURCES "")
foreach (PROTOCOL_CLIENT ${PROTOCOL_CLIENTS})
    list (APPEND GENERATED_SOURCES
        ${CMAKE_BINARY_DIR}/protocols/${PROTOCOL_CLIENT}_service_client.h
        ${CMAKE_BINARY_DIR}/protocols/${PROTOCOL_CLIENT}_service_client.c
    )
endforeach ()
set_source_files_properties ( ${GENERATED_SOURCES} PROPERTIES GENERATED TRUE )

if (VALI_ARCH STREQUAL "i386" OR VALI_ARCH STREQUAL "amd64")
    set(ASM_SOURCES arch/${VALI_ARCH}/_io.s)
    set_source_files_properties(${ASM_SOURCES} PROPERTIES LANGUAGE ASM_NASM)
endif ()

add_library(libddk
    ${GENERATED_SOURCES}
    ${ASM_SOURCES}

    device/clock.c
    device/controller.c
    device/device.c
    device/ethernet.c
    device/firmware_node.c
    device/resources.c
    device/supply.c

    dma/attachment.c
    dma/buffer.c
    dma/chunk.c
    dma/context.c
    dma/fence.c
    dma/mapping.c
    dma/region.c
    dma/resource.c

    firmware/acpi.c
    firmware/firmware.c

    initrd/filter.c
    initrd/ramdisk.c

    interrupts/bus.c
    interrupts/interrupt.c
    interrupts/pci.c

    io/deviceio.c
    io/volatile.c

    services/service.c
    services/storage.c
    services/usb.c

    system/capabilities.c
    system/debug.c
    system/log.c
    system/memory.c
    system/video.c

    # Code from other projects, kept as it was written.
    third_party/aplib/depacks.c
    third_party/bget/bytepool.c

    utils/bufferpool.c
    utils/crc32.c
)
add_dependencies(libddk service_clients contract_clients)
target_include_directories(libddk
    PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include;${CMAKE_CURRENT_SOURCE_DIR}/../libusb/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_PREFIX}/include>"
)

# avoid using target_link_libraries for the RT libraries as it could result
# in cyclic dependencies, and most are static libraries and do not actually really
# depend on each other
target_include_directories(libddk PRIVATE
    ../../tools/rd/libvafs/include
    ../libgracht/include
    ../libds/include
    ../libos/include
    ../libc/include
)

install(TARGETS libddk
    EXPORT ddk_targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY ../../protocols/ DESTINATION share/vali/protocols)
