cmake_minimum_required(VERSION 3.16)

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
