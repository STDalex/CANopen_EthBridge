# CanSniff Web

Компактное standalone SPA для встроенного HTTP API устройства. Исходники лежат
в `Web/src`, воспроизводимая сборка — в `Web/dist`.

```powershell
pnpm install
pnpm build
pnpm preview
```

`dist/qspi-web.bin` — готовый образ раздела Web Assets внешней QSPI Flash. Он
прошивается с физического смещения `0x000000` (memory-mapped адрес
`0x90000000`). Раздел ограничен 1 МиБ; два последних 4-КиБ subsector QSPI
по-прежнему принадлежат настройкам устройства.

Образ содержит таблицу ресурсов и только gzip-представления. `index.html`
получает `Cache-Control: no-cache`, а файлы с hash в имени —
`Cache-Control: public, max-age=31536000, immutable`. Для каждого ресурса сервер
также отдаёт `ETag`.
