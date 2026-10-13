if(NOT EXISTS "${SENDSPIN_PROJECT_DIR}" OR NOT EXISTS "${SENDSPIN_COMPONENT_DIR}"
   OR NOT EXISTS "${SENDSPIN_COMPAT_PATCH}")
    message(FATAL_ERROR "Sendspin component or compatibility patch is missing")
endif()

file(RELATIVE_PATH SENDSPIN_COMPONENT_RELATIVE
    "${SENDSPIN_PROJECT_DIR}" "${SENDSPIN_COMPONENT_DIR}")

execute_process(
    COMMAND git apply --check "--directory=${SENDSPIN_COMPONENT_RELATIVE}"
        "${SENDSPIN_COMPAT_PATCH}"
    WORKING_DIRECTORY "${SENDSPIN_PROJECT_DIR}"
    RESULT_VARIABLE PATCH_CHECK
    OUTPUT_QUIET
    ERROR_QUIET
)

if(PATCH_CHECK EQUAL 0)
    execute_process(
        COMMAND git apply "--directory=${SENDSPIN_COMPONENT_RELATIVE}"
            "${SENDSPIN_COMPAT_PATCH}"
        WORKING_DIRECTORY "${SENDSPIN_PROJECT_DIR}"
        RESULT_VARIABLE PATCH_RESULT
    )
    if(NOT PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to apply Sendspin Music Assistant compatibility patch")
    endif()
    message(STATUS "Applied Sendspin Music Assistant compatibility patch")
else()
    execute_process(
        COMMAND git apply --reverse --check "--directory=${SENDSPIN_COMPONENT_RELATIVE}"
            "${SENDSPIN_COMPAT_PATCH}"
        WORKING_DIRECTORY "${SENDSPIN_PROJECT_DIR}"
        RESULT_VARIABLE PATCH_ALREADY_APPLIED
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT PATCH_ALREADY_APPLIED EQUAL 0)
        message(FATAL_ERROR "Sendspin compatibility patch does not match the resolved component")
    endif()
    message(STATUS "Sendspin Music Assistant compatibility patch already applied")
endif()

if(NOT SENDSPIN_COMPAT_STAMP)
    message(FATAL_ERROR "Sendspin compatibility stamp path is missing")
endif()
file(TOUCH "${SENDSPIN_COMPAT_STAMP}")
