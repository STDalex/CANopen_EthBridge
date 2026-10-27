# CANopen Ethernet Bridge для STM32F746G-DISCO

Прошивка превращает отладочную плату STM32F746G-DISCO в автономный
CAN/CANopen-анализатор, регистратор и управляемый CAN–Ethernet Bridge. Устройство
показывает состояние шины на встроенном дисплее, сохраняет трафик на microSD и
предоставляет REST/WebSocket API для внешних приложений диагностики.

Проект рассчитан на Classic CAN и CANopen. По умолчанию контроллер работает в
безопасном режиме `silent`: принимает кадры, но не передаёт данные и не формирует
ACK. Активная передача включается только явно.

## Основные возможности

- автоматическое определение скорости CAN: 10, 20, 50, 100, 125, 250, 500,
  800 и 1000 кбит/с;
- мониторинг Classic CAN с временными метками и оценкой загрузки шины;
- анализ CANopen-узлов, NMT, PDO, EMCY и expedited SDO;
- чтение и запись expedited SDO через экран и REST API;
- поток CAN-кадров через WebSocket;
- управляемый CAN–Ethernet Bridge с одной клиентской сессией;
- запись полного или отфильтрованного трафика на microSD;
- сенсорный интерфейс на дисплее STM32F746G-DISCO;
- настройка Ethernet, CAN и регистратора через REST API;
- две системы сборки: Keil MDK-ARM и CMake с GNU Arm Embedded Toolchain.

Подробное описание Bridge-протокола находится в
[README_BRIDGE.md](README_BRIDGE.md), интерактивная документация API — в
[doc/CAN_BRIDGE_API.html](doc/CAN_BRIDGE_API.html).

## Подключение TJA1040 к STM32F746G-DISCO

На STM32F746G-DISCO нет встроенного CAN-трансивера. Логические сигналы CAN1
выведены на разъём CN7, поэтому между платой и линиями `CAN_H`/`CAN_L` необходим
внешний TJA1040 или совместимый высокоскоростной CAN-трансивер.

| STM32F746G-DISCO | TJA1040 | Назначение |
|---|---:|---|
| CN7 D14, PB9, CAN1_TX | 1, TXD | Передача от STM32 к трансиверу |
| CN7 D15, PB8, CAN1_RX | 4, RXD | Приём от трансивера в STM32 |
| 5V | 3, VCC | Питание трансивера 5 В |
| GND | 2, GND | Общая земля платы, трансивера и CAN-устройства |
| GND | 8, STB | Низкий уровень включает normal mode |
| — | 7, CANH | Подключение к `CAN_H` шины |
| — | 6, CANL | Подключение к `CAN_L` шины |
| — | 5, SPLIT | Можно оставить неподключённым или использовать со split-терминацией |

Рядом с TJA1040 установите керамический конденсатор 100 нФ между `VCC` и `GND`.
Шина должна иметь терминаторы 120 Ом между `CAN_H` и `CAN_L` на двух физических
концах. Не ставьте отдельный терминатор на каждом узле.

TJA1040 питается от 5 В — питание 3,3 В для него не подходит. Вход TXD распознаёт
уровень 3,3 В STM32, а PB8 используется как 5-вольт-толерантный цифровой вход.
Не подключайте `CAN_H` или `CAN_L` непосредственно к выводам микроконтроллера.

Распиновка сверена с официальными документами
[STM32F746G-DISCO UM1907](https://www.st.com/resource/en/user_manual/dm00190424.pdf),
[STM32F745/746 datasheet](https://www.st.com/resource/en/datasheet/stm32f746vg.pdf)
и [TJA1040 datasheet](https://www.nxp.com/docs/en/data-sheet/TJA1040.pdf).

Простейшая схема соединения:

```text
STM32F746G-DISCO                TJA1040                   CAN-шина

CN7 D14 / PB9 / CAN1_TX  --->  TXD  (1)
CN7 D15 / PB8 / CAN1_RX  <---  RXD  (4)
5V                       --->  VCC  (3) -- 100 нФ -- GND
GND                      ----- GND  (2) ------------- CAN_GND
GND                      --->  STB  (8)
                              CANH (7) ------------- CAN_H
                              CANL (6) ------------- CAN_L
```

Для пассивного мониторинга достаточно режима `silent`, но такой узел не
формирует ACK. Если передатчик один на шине, для нормального обмена и проверки
CANopen следует включить active mode либо подключить другой активный CAN-узел.

## Сборка GNU Arm через CMake

CMake-сборка использует те же исходники, что и проект Keil, и создаёт прошивку
для STM32F746NGHx с аппаратным FPU. Linker script сохраняет размещение специальных
буферов SD, Ethernet и внешней SDRAM.

### Требования

- CMake 3.22 или новее;
- Ninja;
- GNU Arm Embedded Toolchain (`arm-none-eabi-gcc`);
- STM32CubeProgrammer для прошивки через ST-Link.

На Windows toolchain-файл автоматически ищет GNU Arm и Ninja из установленной
STM32CubeIDE. Для отдельной toolchain можно явно задать каталог `bin`:

```powershell
$env:ARM_GCC_PATH = 'C:\path\to\arm-gnu-toolchain\bin'
```

### Конфигурация и сборка Release

Команды для Windows PowerShell:

```powershell
cmake -S . -B build/gcc-release -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build/gcc-release --parallel
```

Результаты находятся в `build/gcc-release`:

- `CAN_sniffer.elf` — ELF с отладочными символами;
- `CAN_sniffer.hex` — образ для программатора;
- `CAN_sniffer.bin` — бинарный образ от адреса `0x08000000`;
- `CAN_sniffer.map` — карта памяти.

### Прошивка через ST-Link

Подключите разъём ST-LINK USB на STM32F746G-DISCO и выполните:

```powershell
cmake --build build/gcc-release --target flash
```

Цель `flash` использует STM32CubeProgrammer, записывает HEX, проверяет содержимое
Flash и перезапускает микроконтроллер.

Дополнительные пояснения приведены в [BUILD_CMAKE.md](BUILD_CMAKE.md). Проект
Keil сохранён в каталоге `MDK-ARM` и может использоваться параллельно.

## Первый запуск

1. Подключите TJA1040 и проверьте общую землю и терминацию CAN.
2. Подключите Ethernet и питание/ST-Link USB.
3. Соберите и прошейте проект.
4. Узнайте адрес устройства по DHCP или задайте статический адрес на экране.
5. Откройте `http://<IP-адрес>/` либо проверьте API:

```powershell
curl.exe http://<IP-адрес>/api/v1/state
```

В PowerShell следует запускать именно `curl.exe`: команда `curl` в старых
версиях Windows PowerShell может быть псевдонимом `Invoke-WebRequest`.

## Ограничения

- поддерживается Classic CAN, но не CAN FD;
- Bridge v1 обслуживает одного WebSocket-клиента;
- передача через API ограничена expedited CANopen SDO;
- segmented и block SDO не поддерживаются;
- в API v1 нет авторизации и TLS, поэтому устройство следует использовать
  только в доверенной локальной сети.
