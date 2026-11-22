cmake_minimum_required(VERSION 3.16)

option(TATER_USB_ETHERNET "Build the experimental USB Ethernet prototype" OFF)
if(TATER_USB_ETHERNET)
    if(NOT SDKCONFIG_DEFAULTS)
        set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
    endif()
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.usb_ethernet.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tater_native_satellite)

# ESP-IDF's component manager can refresh managed_components after configure. Make the patch a
# build dependency of the vendor component so local and clean CI builds both retain support for
# Music Assistant's aiosendspin 9.x wire format alongside current Sendspin.
set(SENDSPIN_COMPONENT_DIR "${CMAKE_SOURCE_DIR}/managed_components/sendspin__sendspin-cpp")
set(SENDSPIN_COMPAT_PATCH "${CMAKE_SOURCE_DIR}/patches/sendspin-cpp-0.9-ma-compat.patch")
if(TARGET __idf_sendspin__sendspin-cpp)
    set(SENDSPIN_COMPAT_STAMP "${CMAKE_BINARY_DIR}/tater_sendspin_compat.stamp")
    add_custom_command(
        OUTPUT "${SENDSPIN_COMPAT_STAMP}"
        COMMAND "${CMAKE_COMMAND}"
            -DSENDSPIN_PROJECT_DIR=${CMAKE_SOURCE_DIR}
            -DSENDSPIN_COMPONENT_DIR=${SENDSPIN_COMPONENT_DIR}
            -DSENDSPIN_COMPAT_PATCH=${SENDSPIN_COMPAT_PATCH}
            -DSENDSPIN_COMPAT_STAMP=${SENDSPIN_COMPAT_STAMP}
            -P "${CMAKE_SOURCE_DIR}/cmake/ApplySendspinCompat.cmake"
        DEPENDS
            "${SENDSPIN_COMPAT_PATCH}"
            "${SENDSPIN_COMPONENT_DIR}/.component_hash"
            "${CMAKE_SOURCE_DIR}/cmake/ApplySendspinCompat.cmake"
        COMMENT "Applying Sendspin Music Assistant compatibility patch"
        VERBATIM
    )
    add_custom_target(tater_sendspin_compat DEPENDS "${SENDSPIN_COMPAT_STAMP}")
    add_dependencies(__idf_sendspin__sendspin-cpp tater_sendspin_compat)

    # Make the affected objects depend on the stamp. This guarantees the patch runs before their
    # compile commands and makes a component-manager refresh rebuild them in the same invocation.
    set(SENDSPIN_COMPAT_SOURCES
        "${SENDSPIN_COMPONENT_DIR}/src/client.cpp"
        "${SENDSPIN_COMPONENT_DIR}/src/client_dispatch.cpp"
        "${SENDSPIN_COMPONENT_DIR}/src/connection.cpp"
        "${SENDSPIN_COMPONENT_DIR}/src/connection_manager.cpp"
        "${SENDSPIN_COMPONENT_DIR}/src/noise_handshake.cpp"
        "${SENDSPIN_COMPONENT_DIR}/src/noise_transport.cpp"
        "${SENDSPIN_COMPONENT_DIR}/src/player_role.cpp"
        "${SENDSPIN_COMPONENT_DIR}/src/protocol.cpp"
        "${SENDSPIN_COMPONENT_DIR}/src/sync_task.cpp"
    )
    set_property(
        SOURCE ${SENDSPIN_COMPAT_SOURCES}
        TARGET_DIRECTORY __idf_sendspin__sendspin-cpp
        APPEND PROPERTY OBJECT_DEPENDS "${SENDSPIN_COMPAT_STAMP}"
    )
endif()

# CDC's stream transport does not normally request a short packet after an
# aligned bulk OUT transfer. ECM uses it to delimit Ethernet frames.
if(CONFIG_TATER_USB_ETHERNET AND TARGET __idf_espressif__iot_usbh_cdc)
    set(USB_ETH_COMPONENT_DIR "${CMAKE_SOURCE_DIR}/managed_components/espressif__iot_usbh_cdc")
    set(USB_ETH_PATCH "${CMAKE_SOURCE_DIR}/patches/iot-usbh-cdc-3.1.1-ecm-zlp.patch")
    set(USB_ETH_PATCH_STAMP "${CMAKE_BINARY_DIR}/tater_usb_ethernet_patch.stamp")
    add_custom_command(
        OUTPUT "${USB_ETH_PATCH_STAMP}"
        COMMAND "${CMAKE_COMMAND}"
            -DUSB_ETH_PROJECT_DIR=${CMAKE_SOURCE_DIR}
            -DUSB_ETH_COMPONENT_DIR=${USB_ETH_COMPONENT_DIR}
            -DUSB_ETH_PATCH=${USB_ETH_PATCH}
            -DUSB_ETH_PATCH_STAMP=${USB_ETH_PATCH_STAMP}
            -P "${CMAKE_SOURCE_DIR}/cmake/ApplyUsbEthernetPatch.cmake"
        DEPENDS
            "${USB_ETH_PATCH}"
            "${USB_ETH_COMPONENT_DIR}/.component_hash"
            "${CMAKE_SOURCE_DIR}/cmake/ApplyUsbEthernetPatch.cmake"
        COMMENT "Applying USB Ethernet frame boundary patch"
        VERBATIM
    )
    add_custom_target(tater_usb_ethernet_patch DEPENDS "${USB_ETH_PATCH_STAMP}")
    add_dependencies(__idf_espressif__iot_usbh_cdc tater_usb_ethernet_patch)
    set_property(
        SOURCE "${USB_ETH_COMPONENT_DIR}/iot_usbh_cdc.c"
        TARGET_DIRECTORY __idf_espressif__iot_usbh_cdc
        APPEND PROPERTY OBJECT_DEPENDS "${USB_ETH_PATCH_STAMP}"
    )
endif()

# Custom ECM and STA must recognize their own mDNS probes on a shared LAN.
if(CONFIG_TATER_USB_ETHERNET AND TARGET __idf_espressif__mdns)
    set(USB_MDNS_COMPONENT_DIR "${CMAKE_SOURCE_DIR}/managed_components/espressif__mdns")
    set(USB_MDNS_PATCH "${CMAKE_SOURCE_DIR}/patches/mdns-1.14.0-usb-ethernet-pair.patch")
    set(USB_MDNS_PATCH_STAMP "${CMAKE_BINARY_DIR}/tater_usb_mdns_patch.stamp")
    add_custom_command(
        OUTPUT "${USB_MDNS_PATCH_STAMP}"
        COMMAND "${CMAKE_COMMAND}"
            -DUSB_ETH_PROJECT_DIR=${CMAKE_SOURCE_DIR}
            -DUSB_ETH_COMPONENT_DIR=${USB_MDNS_COMPONENT_DIR}
            -DUSB_ETH_PATCH=${USB_MDNS_PATCH}
            -DUSB_ETH_PATCH_STAMP=${USB_MDNS_PATCH_STAMP}
            -P "${CMAKE_SOURCE_DIR}/cmake/ApplyUsbEthernetPatch.cmake"
        DEPENDS
            "${USB_MDNS_PATCH}"
            "${USB_MDNS_COMPONENT_DIR}/.component_hash"
            "${CMAKE_SOURCE_DIR}/cmake/ApplyUsbEthernetPatch.cmake"
        COMMENT "Applying USB Ethernet mDNS interface pairing patch"
        VERBATIM
    )
    add_custom_target(tater_usb_mdns_patch DEPENDS "${USB_MDNS_PATCH_STAMP}")
    add_dependencies(__idf_espressif__mdns tater_usb_mdns_patch)
    set_property(
        SOURCE "${USB_MDNS_COMPONENT_DIR}/mdns_netif.c"
        TARGET_DIRECTORY __idf_espressif__mdns
        APPEND PROPERTY OBJECT_DEPENDS "${USB_MDNS_PATCH_STAMP}"
    )
endif()

# Keep SDK changes in this build directory; never patch the shared framework.
if(CONFIG_TATER_USB_ETHERNET AND TARGET __idf_lwip)
    include("${CMAKE_SOURCE_DIR}/cmake/UsbEthernetIpv4Route.cmake")
endif()
