idf_component_register(
    SRCS "visualizer_feed.c" "visualizer_render.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp-dsp
)
