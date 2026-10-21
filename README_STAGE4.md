# CAN sniffer — этап 4, задачи 1–4: Ethernet, API, Live и загрузка логов

## Ethernet

- STM32F746 Ethernet MAC работает через LAN8742A/RMII в режиме 10/100 Мбит/с;
- LwIP 2.1.2 запускается с `NO_SYS=0`, FreeRTOS и Netconn API;
- при включённом DHCP устройство ждёт адрес заданное время, затем применяет
  сохранённый статический IPv4; при отключённом DHCP статический адрес
  применяется сразу;
- адрес, маска и шлюз валидируются до сохранения;
- параметры хранятся не на microSD, а в последних двух 4-КиБ секторах QSPI
  N25Q128A; записи чередуются и содержат version, generation и CRC;
- при повреждении или отсутствии обеих записей применяются безопасные значения:
  DHCP, fallback `192.168.7.10/24`, gateway `192.168.7.1`, HTTP port 80;
- LwIP heap, Ethernet DMA descriptors и zero-copy RX pool размещены в
  выделенной некэшируемой области внутренней SRAM.

Сетевые параметры доступны на локальном экране `Settings → Network settings`.

## REST API `/api/v1`

| Метод | Путь | Назначение |
|---|---|---|
| GET | `/api/v1/state` | состояние устройства, Ethernet, CAN и microSD |
| GET, PUT | `/api/v1/config/network` | сетевой профиль |
| GET, PUT | `/api/v1/config/can` | CAN bitrate и режим отображения SDO |
| GET, PUT | `/api/v1/config/logger` | режим logger и список выбранных нод (этап 5) |
| POST | `/api/v1/logger/start`, `/api/v1/logger/stop` | запуск и остановка записи с текущими настройками (этап 5) |
| GET | `/api/v1/nodes?offset=0&limit=32` | постраничный список нод |
| GET | `/api/v1/statistics` | счётчики CAN, анализатора и logger |
| GET | `/api/v1/snapshots/frames?offset=0&limit=32` | снимок последних CAN-кадров |
| GET | `/api/v1/snapshots/nodes/{id}/events?limit=32` | события ноды |
| GET | `/api/v1/snapshots/nodes/{id}/objects?limit=32` | объекты SDO ноды |
| GET | `/api/v1/logs` | завершённые `.CSL` через `storageTask` |
| GET | `/api/v1/logs/{session}/{file}` | скачать завершённый `.CSL` |
| GET Upgrade | `/api/v1/stream` | Live WebSocket с бинарными CAN-кадрами |

Ответы REST — UTF-8 JSON с `Transfer-Encoding: chunked`. HTTP worker
последовательно обслуживает короткие запросы, а единственный Live WebSocket
после Upgrade передаётся отдельной статической задаче. Поэтому открытый stream
не блокирует REST; приём CAN, analyzer и logger от сети не зависят. Несколько
одновременных stream-клиентов относятся к следующей задаче.

## Live WebSocket `/api/v1/stream`

Клиент подключается к `ws://<ip>/api/v1/stream`. Сначала приходит текстовый
`hello`, затем бинарные пакеты. Все многобайтовые поля бинарного протокола имеют
little-endian порядок.

Заголовок пакета, 16 байт:

| Смещение | Тип | Поле |
|---:|---|---|
| 0 | `char[4]` | magic `CSWS` |
| 4 | `uint8` | версия протокола, сейчас `1` |
| 5 | `uint8` | flags, bit 0 означает пропуск кадров |
| 6 | `uint16` | количество записей |
| 8 | `uint32` | sequence первой записи |
| 12 | `uint32` | количество кадров, пропущенных перед пакетом |

Каждая запись занимает 24 байта: `uint64 timestamp_us`, `uint32 can_id`,
`uint8 flags` (`bit0=extended`, `bit1=remote`), `uint8 dlc`, два резервных
байта и `uint8 data[8]`. Время — непрерывный 64-битный счётчик микросекунд от
запуска контроллера. Пакет содержит не более 60 кадров и вместе с заголовком
WebSocket помещается в один TCP MSS.

Общий Live ring содержит 8192 последних кадра в SDRAM. Новый клиент начинает
с текущего кадра. Если клиент отстаёт более чем на ёмкость ring, сервер
перемещает его курсор к старейшему доступному кадру и сообщает точное число
пропущенных кадров в `lost_count`. Счётчики доступны в
`GET /api/v1/statistics` в объекте `websocket`.

## Скачивание завершённых логов

`GET /api/v1/logs` возвращает для каждого файла поле `download_url`, например
`/api/v1/logs/S000032/L000001.CSL`. Этот URL отдаёт файл с `Content-Length`,
`Content-Disposition: attachment` и `Accept-Ranges: bytes`. Поддерживается один
диапазон `Range: bytes=start-end`, включая открытый конец `bytes=start-`, поэтому
браузер или другой клиент может продолжить прерванную загрузку. `HEAD` для того
же URL возвращает метаданные без тела файла. HTTP-заголовки принимаются в
выделенный 16-КиБ буфер SDRAM; неполный и слишком большой запросы различаются
ответами `408` и `431`.

Сетевой код не обращается к FatFs напрямую. Открытие, позиционирование и чтение
выполняет единственный `storageTask`; данные передаются сети порциями по 16 КиБ
через буфер в SDRAM. Если в очереди есть блоки текущей записи CAN, они
обрабатываются раньше запроса скачивания. Загружать можно только завершённые
файлы с точным путём `Sdddddd/Ldddddd.CSL`; `.TMP`, метаданные и произвольные
пути недоступны.

Пример настройки DHCP со статическим fallback:

```json
{
  "dhcp_enabled": true,
  "static_ip": "192.168.7.10",
  "netmask": "255.255.255.0",
  "gateway": "192.168.7.1",
  "dhcp_timeout_ms": 12000
}
```

## Сборка

Target `CAN_sniffer` собирается Arm Compiler 6.19. Scatter-файл
`MDK-ARM/CAN_sniffer.sct` резервирует внутреннюю SRAM для Ethernet/LwIP.
