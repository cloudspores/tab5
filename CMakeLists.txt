cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_radio)

# The mdns component has a benign strncpy truncation warning that our warnings-as-errors build rejects.
idf_component_get_property(mdns_lib espressif__mdns COMPONENT_LIB)
if(mdns_lib)
    target_compile_options(${mdns_lib} PRIVATE -Wno-error=stringop-truncation)
endif()
