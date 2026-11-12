idf_component_register(SRCS "clipboard.c" "desktop.c" "text.c" "log_buffer.c" "jiggler.c" "hid_parser.c" "usb_policy.c" "router.c" "wire.c" "controls.c" "keyboard_leds.c" "delivery.c" "ack.c" "diagnostics_model.c" "management.c" "slave_history.c" "slave_snapshot.c" "user_preferences.c" "radio.c"
                       INCLUDE_DIRS "include" "../../../config")
target_sources(${COMPONENT_LIB} PRIVATE "firmware_cache.c" "cached_update.c" "receiver_recovery.c")
target_sources(${COMPONENT_LIB} PRIVATE "key_trace.c" "stream.c")
target_sources(${COMPONENT_LIB} PRIVATE "desktop_routes.c")
