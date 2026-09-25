add_executable(hopperscan "${HOPPERSCAN_PROJECT_DIR}/src/hopperscan.cc")

target_link_libraries(
    hopperscan
    PRIVATE lab37::assembly_line_messages
            lab37::canopen_messages
            ark::core
            ark::logging
            ark::serialization
            fmt::fmt)
