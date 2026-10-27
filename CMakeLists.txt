cmake_minimum_required(VERSION 3.22)

project(CAN_sniffer LANGUAGES C ASM)

include(cmake/sources.cmake)

add_executable(CAN_sniffer
  ${CAN_SNIFFER_SOURCES}
  startup/startup_stm32f746xx_gcc.S
  cmake/syscalls_gcc.c
)

set_target_properties(CAN_sniffer PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS YES
  SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/linker/STM32F746NGHx_FLASH.ld"
)

target_compile_definitions(CAN_sniffer PRIVATE
  USE_HAL_DRIVER
  STM32F746xx
)

target_include_directories(CAN_sniffer PRIVATE
  Core/Inc
  Drivers/STM32F7xx_HAL_Driver/Inc
  Drivers/STM32F7xx_HAL_Driver/Inc/Legacy
  Middlewares/Third_Party/FreeRTOS/Source/include
  Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS
  Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM7/r0p1
  Drivers/CMSIS/Device/ST/STM32F7xx/Include
  Drivers/CMSIS/Include
  Middlewares/Third_Party/lvgl
  Middlewares/Third_Party/lvgl/include
  Middlewares/Third_Party/FatFs/src
  Middlewares/Third_Party/LwIP/src/include
  Middlewares/Third_Party/LwIP/system
  Drivers/BSP/STM32746G-Discovery
  Drivers/BSP/Components/lan8742
  Drivers/BSP/Components/n25q128a
)

set(MCU_FLAGS
  -mcpu=cortex-m7
  -mthumb
  -mfpu=fpv5-sp-d16
  -mfloat-abi=hard
)

target_compile_options(CAN_sniffer PRIVATE
  ${MCU_FLAGS}
  -ffunction-sections
  -fdata-sections
  -fno-common
  -Wall
  -Wextra
  $<$<CONFIG:Debug>:-Og;-g3>
  $<$<CONFIG:Release>:-O2;-g1>
  $<$<CONFIG:RelWithDebInfo>:-O2;-g3>
  $<$<CONFIG:MinSizeRel>:-Os;-g1>
)

target_link_options(CAN_sniffer PRIVATE
  ${MCU_FLAGS}
  -T${CMAKE_CURRENT_SOURCE_DIR}/linker/STM32F746NGHx_FLASH.ld
  # Full newlib printf is required: the API and logger format uint64_t values
  # with %llu.  nano.specs prints those fields incorrectly and corrupts JSON.
  --specs=nosys.specs
  -Wl,--gc-sections
  -Wl,--cref
  -Wl,--print-memory-usage
  -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/CAN_sniffer.map
  -Wl,-u,_printf_float
)

target_link_libraries(CAN_sniffer PRIVATE m)

add_custom_command(TARGET CAN_sniffer POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O ihex
          $<TARGET_FILE:CAN_sniffer>
          ${CMAKE_CURRENT_BINARY_DIR}/CAN_sniffer.hex
  COMMAND ${CMAKE_OBJCOPY} -O binary
          $<TARGET_FILE:CAN_sniffer>
          ${CMAKE_CURRENT_BINARY_DIR}/CAN_sniffer.bin
  COMMAND ${CMAKE_SIZE} --format=berkeley $<TARGET_FILE:CAN_sniffer>
  COMMENT "Generating HEX/BIN images and printing image size"
)

if(CMAKE_HOST_WIN32)
  set(_standalone_programmer
    "C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe")
  file(GLOB _cubeide_programmers
    "C:/ST/STM32CubeIDE_*/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.*/tools/bin/STM32_Programmer_CLI.exe")
endif()

if(EXISTS "${_standalone_programmer}")
  set(STM32_PROGRAMMER_CLI "${_standalone_programmer}")
elseif(_cubeide_programmers)
  list(GET _cubeide_programmers 0 STM32_PROGRAMMER_CLI)
else()
  find_program(STM32_PROGRAMMER_CLI
    NAMES STM32_Programmer_CLI STM32_Programmer_CLI.exe
    NO_CMAKE_FIND_ROOT_PATH)
endif()

if(STM32_PROGRAMMER_CLI)
  add_custom_target(flash
    COMMAND ${STM32_PROGRAMMER_CLI}
            -c port=SWD
            -w ${CMAKE_CURRENT_BINARY_DIR}/CAN_sniffer.hex
            -v
            -rst
    DEPENDS CAN_sniffer
    USES_TERMINAL
    COMMENT "Flashing CAN_sniffer.hex over ST-Link/SWD"
  )
endif()
