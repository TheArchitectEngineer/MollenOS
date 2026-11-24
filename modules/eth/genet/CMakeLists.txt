# vali genet ethernet driver

set(GENERATED_SOURCES
    ${CMAKE_BINARY_DIR}/protocols/ctt_driver_service_server.c
)
set_source_files_properties(${GENERATED_SOURCES} PROPERTIES GENERATED TRUE)

add_module_target(genet "" ${GENERATED_SOURCES}
    main.c
    controller.c
    diagnostics.c
    interrupt.c
    link.c
    mac.c
    mdio.c
    queues.c
    registers.c
    service.c
    thresholds.c
    transfer.c
    wrapper.c
)
add_dependencies(genet contract_servers service_clients)
target_link_libraries(genet PRIVATE eth-common)

# Interrupt thresholds. The defaults follow Linux, see thresholds.h for what
# each value does. A value the hardware cannot use stops the build.
set(VALI_GENET_TX_DONE_FRAMES 10 CACHE STRING "Frames sent before the GENET TX ring raises its done interrupt cause")
set(VALI_GENET_RX_DONE_FRAMES 1 CACHE STRING "Frames received before the GENET RX ring raises its done interrupt cause")
set(VALI_GENET_RX_TIMEOUT_US 50 CACHE STRING "Microseconds a received frame may wait for the RX frame count")
target_compile_definitions(genet PRIVATE
    GENET_DEFAULT_TX_DONE_FRAMES=${VALI_GENET_TX_DONE_FRAMES}U
    GENET_DEFAULT_RX_DONE_FRAMES=${VALI_GENET_RX_DONE_FRAMES}U
    GENET_DEFAULT_RX_TIMEOUT_US=${VALI_GENET_RX_TIMEOUT_US}U
)

deploy_file_to_initrd(genet "modules" genet.yaml)
