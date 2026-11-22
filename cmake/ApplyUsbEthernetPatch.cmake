if(NOT EXISTS "${USB_ETH_PROJECT_DIR}" OR NOT EXISTS "${USB_ETH_COMPONENT_DIR}"
   OR NOT EXISTS "${USB_ETH_PATCH}")
    message(FATAL_ERROR "USB Ethernet component or compatibility patch is missing")
endif()

file(RELATIVE_PATH USB_ETH_COMPONENT_RELATIVE
    "${USB_ETH_PROJECT_DIR}" "${USB_ETH_COMPONENT_DIR}")

execute_process(
    COMMAND git apply --check "--directory=${USB_ETH_COMPONENT_RELATIVE}"
        "${USB_ETH_PATCH}"
    WORKING_DIRECTORY "${USB_ETH_PROJECT_DIR}"
    RESULT_VARIABLE PATCH_CHECK
    OUTPUT_QUIET
    ERROR_QUIET
)

if(PATCH_CHECK EQUAL 0)
    execute_process(
        COMMAND git apply "--directory=${USB_ETH_COMPONENT_RELATIVE}"
            "${USB_ETH_PATCH}"
        WORKING_DIRECTORY "${USB_ETH_PROJECT_DIR}"
        RESULT_VARIABLE PATCH_RESULT
    )
    if(NOT PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to apply USB Ethernet compatibility patch")
    endif()
    message(STATUS "Applied USB Ethernet compatibility patch")
else()
    execute_process(
        COMMAND git apply --reverse --check "--directory=${USB_ETH_COMPONENT_RELATIVE}"
            "${USB_ETH_PATCH}"
        WORKING_DIRECTORY "${USB_ETH_PROJECT_DIR}"
        RESULT_VARIABLE PATCH_ALREADY_APPLIED
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT PATCH_ALREADY_APPLIED EQUAL 0)
        message(FATAL_ERROR "USB Ethernet patch does not match the resolved component")
    endif()
    message(STATUS "USB Ethernet compatibility patch already applied")
endif()

if(NOT USB_ETH_PATCH_STAMP)
    message(FATAL_ERROR "USB Ethernet stamp path is missing")
endif()
file(TOUCH "${USB_ETH_PATCH_STAMP}")
