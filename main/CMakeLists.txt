# secrets.h holds WiFi credentials and is never committed; CI (and fresh clones) build from the example.
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT EXISTS "${COMPONENT_DIR}/secrets.h")
    message(WARNING "main/secrets.h missing - building with secrets.h.example (no WiFi credentials)")
    configure_file("${COMPONENT_DIR}/secrets.h.example" "${COMPONENT_DIR}/secrets.h" COPYONLY)
endif()

# c6_fw.bin is M5Stack's ESP32-C6 hosted firmware (recovered from the stock UIFlow2 filesystem); it is
# not redistributed in the repo. Without it the one-time C6 update is compiled out.
set(C6_FW "${COMPONENT_DIR}/c6_fw.bin")
if(NOT EXISTS "${C6_FW}")
    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        message(WARNING "main/c6_fw.bin missing - C6 co-processor update disabled in this build")
    endif()
    file(WRITE "${CMAKE_BINARY_DIR}/c6_fw_placeholder/c6_fw.bin" "")
    set(C6_FW "${CMAKE_BINARY_DIR}/c6_fw_placeholder/c6_fw.bin")
    set(C6_DEFS "NO_C6_FW=1")
endif()

idf_component_register(
    SRCS "main.cpp" "net.cpp" "console.cpp" "launcher.cpp" "theme.cpp" "topbar.cpp" "lv_mem_psram.cpp"
         "radio_app.cpp" "radio_ui.cpp" "stream.cpp" "stations.cpp" "bridge.cpp"
         "settings.cpp" "settings_app.cpp" "update.cpp" "c6_update.cpp" "translate_app.cpp"
         "synth_engine.cpp" "synth_app.cpp" "synth_ui.cpp" "synth_bank.cpp"
         "synth_perf.cpp" "synth_keys.cpp" "synth_play_ui.cpp" "synth_relay.cpp" "screenshot.cpp"
         "fonts/familjen_bold_52.c" "fonts/familjen_medium_24.c" "fonts/familjen_semibold_18.c" "fonts/familjen_medium_14.c" "fonts/jbmono_14.c" "fonts/jbmono_32.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash esp_netif esp_event esp_http_client esp_timer mbedtls lwip esp_driver_usb_serial_jtag esp_https_ota app_update esp_app_format esp_timer msfa
    EMBED_FILES "${C6_FW}"
)
if(C6_DEFS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${C6_DEFS})
endif()

# The LVGL memory backend (lv_mem_psram.cpp) is referenced only from the LVGL library, which
# the linker scans after libmain.a; force the object in so the symbols resolve.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
