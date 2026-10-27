# Сборка GNU Arm через CMake

Сборка CMake существует параллельно проекту Keil и использует тот же набор
исходников. Она создаёт прошивку для STM32F746NGHx с аппаратным FPU и сохраняет
специальное размещение буферов SD и Ethernet из `MDK-ARM/CAN_sniffer.sct`.

## Требования

- CMake 3.22 или новее;
- Ninja;
- GNU Arm Embedded Toolchain (`arm-none-eabi-gcc`);
- STM32CubeProgrammer — только для цели `flash`.

Если GNU Arm Toolchain не находится через `PATH`, задайте каталог `bin`:

```powershell
$env:ARM_GCC_PATH = 'C:\path\to\arm-gnu-toolchain\bin'
```

На Windows toolchain-файл также автоматически ищет компилятор, поставляемый
вместе с STM32CubeIDE в `C:\ST\STM32CubeIDE_*`.

## Сборка Release

```powershell
cmake -S . -B build/gcc-release -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build/gcc-release --parallel
```

Результаты находятся в `build/gcc-release`:

- `CAN_sniffer.elf` — ELF с символами;
- `CAN_sniffer.hex` — образ для программатора;
- `CAN_sniffer.bin` — бинарный образ от адреса `0x08000000`;
- `CAN_sniffer.map` — карта памяти.

## Прошивка через ST-Link

Подключите плату и выполните:

```powershell
cmake --build build/gcc-release --target flash
```

Эквивалентная команда STM32CubeProgrammer:

```powershell
STM32_Programmer_CLI -c port=SWD `
  -w build/gcc-release/CAN_sniffer.hex -v -rst
```

Перед проверкой CAN/Ethernet рекомендуется сначала убедиться, что после сброса
запустился дисплей, а затем проверить DHCP/статический IP, REST status endpoint,
WebSocket-поток CAN, запись на SD-карту и обмен CANopen SDO.
