cmake_minimum_required(VERSION 3.13)

# Pinned dependencies fetched by scripts/setup.sh (see deps.lock).
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(PICO_SDK_PATH "${PROJECT_ROOT}/deps/pico-sdk")
set(PICO_TINYUSB_PATH "${PROJECT_ROOT}/deps/tinyusb")
set(PICO_PIO_USB_PATH "${PROJECT_ROOT}/deps/Pico-PIO-USB")
set(PICO_BOARD pico)
set(PICO_PLATFORM rp2040)
if(NOT DEFINED picotool_DIR AND EXISTS "${PROJECT_ROOT}/.tools/picotool/picotool")
    set(picotool_DIR "${PROJECT_ROOT}/.tools/picotool/picotool")
endif()

include(pico_sdk_import.cmake)

# Clock and voltage: identical to jfedor2/wheel-adapter r2026-11-07, whose pinned
# Pico-PIO-USB/TinyUSB forks are used here: 192 MHz (multiple of 48 MHz for
# Pico-PIO-USB), core at 1.15 V, flash clock divider 4.
add_compile_definitions(
    SYS_CLK_HZ=192000000
    PLL_SYS_VCO_FREQ_HZ=1536000000
    PLL_SYS_POSTDIV1=4
    PLL_SYS_POSTDIV2=2
    SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST=1
    SYS_CLK_VREG_VOLTAGE_MIN=VREG_VOLTAGE_1_15
    PICO_FLASH_SPI_CLKDIV=4
    # GP0/GP1 are host port A: keep the SDK's default UART away from them.
    PICO_DEFAULT_UART_TX_PIN=16
    PICO_DEFAULT_UART_RX_PIN=17
)

project(ps5_wheel_adapter C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

set(ADAPTER_SOURCES
    src/main.c
    src/usb_descriptors.c
    src/auth.c
    src/crc32.c
    src/devices.c
    src/diag.c
    src/g29.c
    src/hid_desc.c
    src/input_map.c
)

function(add_adapter_variant TARGET BENCH)
    add_executable(${TARGET} ${ADAPTER_SOURCES})
    target_include_directories(${TARGET} PRIVATE src config)
    target_compile_definitions(${TARGET} PRIVATE ADAPTER_BENCH=${BENCH})
    target_compile_options(${TARGET} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(${TARGET}
        pico_stdlib
        hardware_flash
        hardware_watchdog
        tinyusb_device
        tinyusb_host
        tinyusb_pico_pio_usb
    )
    # Whole image runs from SRAM: flash log writes cannot stall USB interrupts.
    pico_set_binary_type(${TARGET} copy_to_ram)
    pico_enable_stdio_uart(${TARGET} 0)
    pico_enable_stdio_usb(${TARGET} 0)
    pico_add_extra_outputs(${TARGET})
endfunction()

add_adapter_variant(ps5_adapter_console 0)  # for the PS5
add_adapter_variant(ps5_adapter_bench 1)    # PC bench only: adds a CDC diagnostics port
