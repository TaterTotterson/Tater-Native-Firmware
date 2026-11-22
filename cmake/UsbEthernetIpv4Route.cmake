# Replace only this target's IPv4 source with a patched build-local copy.
get_target_property(USB_LWIP_SOURCE_DIR __idf_lwip SOURCE_DIR)
set(USB_LWIP_ORIGINAL "${USB_LWIP_SOURCE_DIR}/lwip/src/core/ipv4/ip4.c")
set(USB_LWIP_PATCH "${CMAKE_SOURCE_DIR}/patches/esp-idf-5.5.4-usb-ethernet-route.patch")
set(USB_LWIP_COPY_DIR "${CMAKE_BINARY_DIR}/tater_usb_lwip")
file(MAKE_DIRECTORY "${USB_LWIP_COPY_DIR}")
configure_file("${USB_LWIP_ORIGINAL}" "${USB_LWIP_COPY_DIR}/ip4.c" COPYONLY)
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DUSB_ETH_PROJECT_DIR=${CMAKE_BINARY_DIR}
        -DUSB_ETH_COMPONENT_DIR=${USB_LWIP_COPY_DIR}
        -DUSB_ETH_PATCH=${USB_LWIP_PATCH}
        -DUSB_ETH_PATCH_STAMP=${USB_LWIP_COPY_DIR}/patch.stamp
        -P "${CMAKE_SOURCE_DIR}/cmake/ApplyUsbEthernetPatch.cmake"
    RESULT_VARIABLE USB_LWIP_PATCH_RESULT
)
if(NOT USB_LWIP_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Unable to prepare the USB Ethernet IPv4 route policy")
endif()
get_target_property(USB_LWIP_SOURCES __idf_lwip SOURCES)
set(USB_LWIP_ROUTE_SOURCES "${USB_LWIP_SOURCES}")
list(FILTER USB_LWIP_ROUTE_SOURCES INCLUDE REGEX "(^|/)lwip/src/core/ipv4/ip4[.]c$")
list(LENGTH USB_LWIP_ROUTE_SOURCES USB_LWIP_ROUTE_SOURCE_COUNT)
if(NOT USB_LWIP_ROUTE_SOURCE_COUNT EQUAL 1)
    message(FATAL_ERROR "Expected exactly one SDK IPv4 route source")
endif()
list(FILTER USB_LWIP_SOURCES EXCLUDE REGEX "(^|/)lwip/src/core/ipv4/ip4[.]c$")
set_property(TARGET __idf_lwip PROPERTY SOURCES
    ${USB_LWIP_SOURCES} "${USB_LWIP_COPY_DIR}/ip4.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${USB_LWIP_ORIGINAL}" "${USB_LWIP_PATCH}"
    "${CMAKE_SOURCE_DIR}/cmake/ApplyUsbEthernetPatch.cmake")
