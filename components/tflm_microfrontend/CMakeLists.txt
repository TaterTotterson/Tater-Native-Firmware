file(GLOB TFLM_MICROFRONTEND_SRCS "src/*.c")

idf_component_register(
    SRCS ${TFLM_MICROFRONTEND_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
)
