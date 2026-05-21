add_executable(generative_midi_sequencer
    main.cpp
    usb_descriptors.c
    engine/euclidean.cpp
    engine/note_generator.cpp
    engine/midi_handler.cpp
    engine/track.cpp
    engine/storage_manager.cpp
    ui/input_manager.cpp
    ui/display_controller.cpp
)

# Make sure TinyUSB can find project-specific tusb_config.h in this directory
target_include_directories(generative_midi_sequencer PUBLIC ${CMAKE_CURRENT_LIST_DIR})

# Link standard Pico SDK libraries (no LVGL - using custom Dirty Rect renderer)
target_link_libraries(generative_midi_sequencer
    pico_stdlib
    pico_multicore      # Required for dual core
    hardware_uart       # Required for MIDI OUT (GP0/GP1)
    hardware_spi        # Required for Display (SPI1)
    hardware_timer      # Required for Clock / timing
    hardware_pio        # Required for hardware/pio.h
    pico_rand           # Required for pico/rand.h
    tinyusb_device      # TinyUSB device stack (for USB MIDI)
    tinyusb_board
)

# Enable USB stdio (for printf debugging) and disable UART stdio (GP0/GP1 is dedicated to MIDI)
pico_enable_stdio_usb(generative_midi_sequencer 0)
pico_enable_stdio_uart(generative_midi_sequencer 1)

# Ensure TinyUSB builds the MIDI device class
target_compile_definitions(generative_midi_sequencer PRIVATE
    CFG_TUD_MIDI=1
)

# Add extra outputs (hex, bin, uf2 for flashing)
pico_add_extra_outputs(generative_midi_sequencer)
