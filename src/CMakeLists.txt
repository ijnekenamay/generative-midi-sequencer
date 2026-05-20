add_executable(generative_midi_sequencer
    main.cpp
    engine/euclidean.cpp
    engine/note_generator.cpp
    engine/midi_handler.cpp
    engine/track.cpp
    engine/storage_manager.cpp
    ui/input_manager.cpp
    ui/display_controller.cpp
)

# Link standard Pico SDK libraries and LVGL
target_link_libraries(generative_midi_sequencer
    pico_stdlib
    pico_multicore      # Required for dual core
    hardware_uart       # Required for MIDI OUT (GP0/GP1)
    hardware_spi        # Required for Display (SPI0)
    hardware_timer      # Required for Clock / timing
    hardware_pio        # Required for hardware/pio.h
    pico_rand           # Required for pico/rand.h
    lvgl::lvgl
)

# Provide path to lv_conf.h
target_include_directories(generative_midi_sequencer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Enable USB stdio (for printf debugging) and disable UART stdio (GP0/GP1 is dedicated to MIDI)
pico_enable_stdio_usb(generative_midi_sequencer 1)
pico_enable_stdio_uart(generative_midi_sequencer 0)

# Add extra outputs (hex, bin, uf2 for flashing)
pico_add_extra_outputs(generative_midi_sequencer)

