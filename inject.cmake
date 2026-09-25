#
# Attaches hopperscan to an existing lab37 build without modifying the lab37
# tree. Pointed at by -DCMAKE_PROJECT_Lab37_INCLUDE, which CMake includes right
# after project(Lab37) -- too early for lab37's own targets, so the real work is
# deferred to the end of the root scope. Deferred arguments re-expand at call
# time, hence the scoped variable rather than CMAKE_CURRENT_LIST_DIR.
#
set(HOPPERSCAN_PROJECT_DIR
    "${CMAKE_CURRENT_LIST_DIR}")

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL include "${HOPPERSCAN_PROJECT_DIR}/hopperscan.cmake")
