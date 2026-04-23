cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(claude_face)

# On IDF 5.5 + pre-v3 ESP32-P4, the combined .sdata from all linked
# components overflows the RISC-V GP-relative addressing window (±2 KB),
# causing --enable-non-contiguous-regions to silently discard ~19 KB of
# sections at link time. Disabling small-data optimisation forces those
# variables into regular .data, which is unrestricted.
idf_build_set_property(COMPILE_OPTIONS "-msmall-data-limit=0" APPEND)
