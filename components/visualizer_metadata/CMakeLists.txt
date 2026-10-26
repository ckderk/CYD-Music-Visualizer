idf_component_register(
    SRCS "visualizer_metadata.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
)

# Same pattern as components/display/display_st7789.c: reach main's RTSP
# event headers (rtsp_events.h, METADATA_STRING_MAX) by adding the include
# dir directly rather than listing `main` in REQUIRES (ESP-IDF resolves the
# symbols at final link anyway).
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${PROJECT_DIR}/main/rtsp"
)
