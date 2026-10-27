set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_arm_gcc_hints)
if(DEFINED ENV{ARM_GCC_PATH})
  file(TO_CMAKE_PATH "$ENV{ARM_GCC_PATH}" _arm_gcc_env_path)
  list(APPEND _arm_gcc_hints "${_arm_gcc_env_path}")
endif()

# STM32CubeIDE bundles the GNU Arm toolchain. This fallback keeps local builds
# convenient while ARM_GCC_PATH remains the portable/CI-friendly override.
if(CMAKE_HOST_WIN32)
  file(GLOB _cubeide_gcc_dirs
    "C:/ST/STM32CubeIDE_*/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin")
  list(APPEND _arm_gcc_hints ${_cubeide_gcc_dirs})
endif()

find_program(ARM_NONE_EABI_GCC arm-none-eabi-gcc HINTS ${_arm_gcc_hints} REQUIRED)
get_filename_component(ARM_NONE_EABI_BIN_DIR "${ARM_NONE_EABI_GCC}" DIRECTORY)

set(CMAKE_C_COMPILER "${ARM_NONE_EABI_GCC}" CACHE FILEPATH "GNU Arm C compiler" FORCE)
set(CMAKE_ASM_COMPILER "${ARM_NONE_EABI_GCC}" CACHE FILEPATH "GNU Arm assembler" FORCE)
set(CMAKE_AR "${ARM_NONE_EABI_BIN_DIR}/arm-none-eabi-ar${CMAKE_EXECUTABLE_SUFFIX}" CACHE FILEPATH "GNU Arm archiver" FORCE)
set(CMAKE_RANLIB "${ARM_NONE_EABI_BIN_DIR}/arm-none-eabi-ranlib${CMAKE_EXECUTABLE_SUFFIX}" CACHE FILEPATH "GNU Arm ranlib" FORCE)
set(CMAKE_OBJCOPY "${ARM_NONE_EABI_BIN_DIR}/arm-none-eabi-objcopy${CMAKE_EXECUTABLE_SUFFIX}" CACHE FILEPATH "GNU Arm objcopy" FORCE)
set(CMAKE_SIZE "${ARM_NONE_EABI_BIN_DIR}/arm-none-eabi-size${CMAKE_EXECUTABLE_SUFFIX}" CACHE FILEPATH "GNU Arm size" FORCE)
