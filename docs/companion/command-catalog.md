# Command Catalog v1 — командная поверхность компаньона

Компаньон переиспользует существующие команды `SerialCli` дословно (хост шлёт их в `REQ`) плюс несколько новых `companion`-глаголов для стриминга и передачи файлов.

**Легенда транспорта:**
- 🟢 **BLE-safe** — дискретная команда, короткий ответ → нормально по BLE и USB.
- 🟡 **BLE-ok с оговоркой** — может дать много вывода/событий; по BLE работает, но с учётом потолка ~2–24 КБ/с.
- 🔴 **USB-предпочтительно** — высокочастотный live/большой объём; по BLE — только «захват в файл → выгрузка».

> Глаголы и привязка к файлам сверены с `grep addCommand` по `src/core/serial_commands/`. Точный синтаксис аргументов — в соответствующем `*_commands.cpp`. Алиасы указаны в скобках (SimpleCLI: `a,b` или `pre/fix`).

---

## Существующие команды (по категориям)

Регистрация — `src/core/serial_commands/cli.cpp:36-64`.

### status — `status_commands.cpp` 🟢
`status` (сводное состояние: батарея, RF-модуль и т.д.), `gpsmon` 🔴 (live). Основа дашборда.

### util — `util_commands.cpp` 🟢/🟡
`uptime`, `date`, `i2c` (скан шины) 🟡, `free` (память), `info`(`!`,`device_info`), `help`(`?`,`halp`), `optionsJSON` 🟡, `display`, `nav`(`navigate`,`navigation`), `options`(`option`), `loader`.
> `factory_reset` — в **settings**; `run_from_file`/`run_from_buffer` — в **badusb**/**interpreter** (не здесь).

### power — `power_commands.cpp` 🟢
top-level: `poweroff`, `reboot`, `sleep`; композит `power` с под-командами `off`, `reboot`, `sleep`. ⚠️ разрывают сессию (ожидаемо).

### settings — `settings_commands.cpp` 🟢
`factory_reset` ⚠️ (top-level), `set`(`settings`) — get/set настроек устройства. Основа «Settings» в UI; кандидат для двусторонней синхронизации конфигом.

### wifi — `wifi_commands.cpp` 🟡/🔴
`webui`, `wifi` (вкл/скан/connect — см. источник) 🟡, `arp` 🟡, `listen` 🟡, `sniffer` 🔴 (высокочастотный live → по BLE захват в файл).
> `responder` — закомментирован/TODO (`wifi_commands.cpp:129`), пока недоступен.

### rf (sub-GHz / CC1101) — `rf_commands.cpp` 🟡/🔴
`rx` 🔴 (live приём), `tx`, `scan` 🟡, `tx_from_file`, `tx_from_buffer`; плюс single-arg `RfSend`. По BLE: `tx_from_file`/`tx_from_buffer`/`scan` ок; live `rx`/спектр — захват в `.sub` → выгрузка.
> `tx_raw` — в **ir**; `jam_sweep` — в **nrf**.

### ir — `ir_commands.cpp` 🟡
`rx` 🟡, `tx`, `tx_raw`, `tx_from_file`, `tx_from_buffer`; плюс single-arg `IRSend`.
> Нет глагола `scan`; `type_from_file` — в **crypto**.

### nrf (NRF24) — `nrf_commands.cpp` 🟡/🔴
`scan` 🟡, `jam_sweep` ⚠️🔴. (Только эти два глагола.)

### gpio — `gpio_commands.cpp` 🟢
set/read/режимы пинов (см. источник).

### crypto — `crypto_commands.cpp` 🟢
top-level: `decrypt`, `encrypt`; под композитом `crypto`: `decrypt_from_file`, `encrypt_to_file`, `type_from_file`.
> `md5`/`crc32` — в **storage**; `hex` — в **screen** (цвет темы), не криптопримитив.

### storage — `storage_commands.cpp` 🟢/🟡
top-level: `ls`(`dir`), `cat`(`type`) 🟡, `md5`, `crc32`, `rm`(`del`), `md`(`mkdir`), `rmdir`; под композитом `storage`: `list`, `read` 🟡, `write`, `remove`, `rename`, `copy`, `mkdir`, `rmdir`, `stat`, `md5`, `crc32`, `free`. База файловых операций; крупные `read`/`cat` лучше через `companion file get` (chunk+sha256).

### gps — `gps_commands.cpp` 🟢/🟡
`gps_source`, `gps_baud`, `gps_rate`, `gps_system`, `gps_nmea`, `gps_muteant`, `gps_reset`, `gps_save`, `gps_info`, `gps_log`, `gps_status`, `gps_sats`, `gps_satapp`, `gps_web`. (`gpsmon` 🔴 live — в **status**.)

### sound — `sound_commands.cpp` 🟢 *(если `HAS_NS4168_SPKR`/`BUZZ_PIN`)*
тон/воспроизведение (см. источник).

### screen — `screen_commands.cpp` 🟢 *(если `HAS_SCREEN`)*
`clock`; под композитом `screen`: `brightness`(`bright`); под `color`: `rgb`, `hex` (цвет темы).

### badusb — `badusb_commands.cpp` 🟢 *(если `USB_as_HID`)*
под композитом `badusb`: `run_from_file`, `run_from_buffer` (запуск HID-нагрузки).

### interpreter (JS) — `interpreter_commands.cpp` 🟡 *(если не `LITE_VERSION`)*
под композитом `js`: `run_from_file`, `run_from_buffer`. ⚠️ интерпретатор **убивает serial-таск** (`main.cpp:528`) → во время его работы компаньон недоступен (см. [`firmware.md`](firmware.md#учесть-интерпретатор-убивает-serial-таск)).

---

## Новые `companion`-глаголы (добавляем)

Регистрируются через `createCompanionCommands(&_cli)` (новый файл), синтаксис — единый стиль SimpleCLI.

| Глагол | Назначение | Транспорт |
|---|---|---|
| `HELLO proto=N token=…` | рукопожатие+авторизация, выдаёт fw/board/mtu/caps | 🟢 |
| `companion caps` | повтор списка возможностей | 🟢 |
| `companion busy` | текущий владелец радио/занятость (для арбитража) | 🟢 |
| `companion file get <path> [range=a-b]` | выгрузка файла: chunk+base64+sha256 | 🟡 |
| `companion file put <path> size=… sha256=…` | загрузка файла (stop-and-wait `ACK`) | 🟡 |
| `companion stream start <kind>` | старт потоковой задачи, события `EVT … ` | 🟡/🔴 |
| `companion stream stop <id>` | остановка потоковой задачи | 🟢 |
| `companion wifi deauth bssid=… [sta=…] [ch=N] [count=N]` | инжект deauth (форсит рукопожатие) | 🔴 |
| `companion audio tx path=… [freq=MHz] [dev=kHz] [rate=Hz] [osr=N] [reps=N]` | аналоговая FM-передача голоса/аудио через CC1101 (sigma-delta → 2-FSK); PCM грузится через `companion file put` | 🔴 реализовано |
| `companion audio rx freq=MHz [wait=s] [secs=s] [rssi=dBm] [rate=Hz] [hold=ms] [path=…]` | запись по несущей: ждёт carrier (GDO2 carrier-sense), пишет демод-поток GDO0, сохраняет в файл для офлайн-декода на хосте | 🔴 реализовано (не валидировано на железе) |

### `companion audio tx` / `audio rx` — аналоговое радио на CC1101

У CC1101 нет аналогового FM-тракта, поэтому голос синтезируется программно:
- **TX:** хост (`host/audio_tx.py`) переводит TTS/аудио в 8-бит mono PCM (полоса 300–3400 Гц + AGC), грузит файл, прошивка оверсэмплит в 1-битный sigma-delta-поток и манипулирует GDO0 в 2-FSK — приёмник аналоговой FM (рация) интегрирует плотность бит обратно в звук. Узкополосная девиация ~4 кГц. **Важно:** библиотечные `setMHZ`/`setPA` на общей с дисплеем шине SPI нестабильны — частота пишется с verify+retry и fallback на прямую запись FREQ, мощность PATABLE и калибровка SCAL форсируются напрямую.
- **RX:** живой приём невозможен; вместо него запись по несущей. Детектор несущей вынесен на пин GDO2 (carrier-sense GPIO), чтобы циклы не делали SPI (иначе гонка с дисплеем → `xTaskPriorityDisinherit`). Сырой 1-битный захват → хост (`audio_tx.record`/`decode_file`): НЧ-фильтр + децимация → WAV. Захват и декод **разделены** — сырьё хранится в `host/captures/*.bin`+`.json`, передекодируется офлайн с другими параметрами.
- **TTS-движки:** RHVoice (русский, голоса elena/aleksandr/…), espeak-ng (en/de/fr/…). Выбор языка/голоса: дропдаун в GUI, `:voices`/`:voice` в TUI, `voice=auto` детектит кириллицу→ru.

### Допустимые `<kind>` для `companion stream start`

| kind | под капотом | событие | статус |
|---|---|---|---|
| `telemetry` | ms / free heap, 1 Гц | `EVT <id> tick seq=.. ms=.. heap=..` | 🟢 реализовано |
| `wifi` | async `WiFi.scanNetworks(true)` | `EVT <id> wifi seq=.. count=N` + `EVT <id> wifi net ch=.. rssi=.. enc=.. bssid=.. ssid=..` | 🟢 реализовано (SPI-safe) |
| `nrf` | NRF24 RPD-свип (80 ch) | `EVT <id> nrf seq=.. channels=80 active_n=.. peak_ch=.. peak=.. active=ch:hits,..` | 🟢 реализовано (⚠️ общий SPI с дисплеем — держать экран idle) |
| `rf [start stop]` | CC1101 RSSI-свип sub-GHz (40 бинов, по умолч. 433.0–434.8) | `EVT <id> rf seq=.. f0=.. f1=.. step=.. n=40 peak_f=.. peak=.. floor=.. rssi=v0,v1,..` | 🟢 реализовано (⚠️ общий SPI с дисплеем — держать экран idle) |

Доп.: `interval=<ms>` в аргументах меняет частоту выдачи (200–10000), напр. `companion stream start telemetry interval=500`.

Неизвестный kind → `ERR <id> 3`; nrf без чипа → `ERR <id> 4`. Формат полей и `companion busy` — в [`protocol.md`](protocol.md#43-старт-стоп-потоковой-задачи).

---

## Приоритет v1 (по решению)

Все группы в v1, в таком порядке вертикальных срезов:

1. **status + wifi scan** — доказать протокол end-to-end (Phase 1-2).
2. **передача файлов захватов** (`companion file get/put`, storage) — снять лимит BLE (Phase 3).
3. **RF / sub-GHz** (`rf scan`, `tx_from_file`, захват в `.sub`) (Phase 3).
4. **settings + nrf/ir/badusb** — полнота поверхности (Phase 3-4).

Далее — host-compute обвязка над выгруженными захватами (Phase 5, см. [`host.md`](host.md#host-compute-augmentation)).
