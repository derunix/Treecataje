# Host — архитектура Linux-приложения

Цель: **общее ядро** (транспорт + кодек протокола + сессия + шина событий) на **Rust**, с **Python-биндингами (pyo3)**, и два тонких фронта на Python — **TUI** и **GUI**. Сборка под **uConsole (aarch64)** и **x86_64**.

Выбор стека (зафиксировано): **Rust-ядро + Python-фронты**.
- Rust даёт один статически слинкованный, быстрый, надёжно типизированный кодек/транспорт, который тривиально кросс-компилируется под aarch64 и x86, и не тащит интерпретатор в горячий путь BLE.
- Python-фронты дают скорость разработки UI и богатую экосистему для **host-compute augmentation** (numpy/scipy/matplotlib, обвязки к hashcat/aircrack и т.п.).
- Мост — pyo3/maturin: ядро публикуется как нативный Python-модуль (`companion_core`).

---

## 1. Слои

```
   ┌──────────┐   ┌──────────┐   ┌─────────────────────┐
   │   TUI    │   │   GUI    │   │   MCP server        │   ← потребители (Python)
   │(Textual) │   │(PySide6) │   │ (Claude управляет/  │
   │          │   │          │   │  читает устройство) │
   └────┬─────┘   └────┬─────┘   └──────────┬──────────┘
        └──────────────┼────────────────────┘
                 ┌──────▼─────────────┐
                 │  companion_core    │   ← pyo3-модуль (Rust); ранний фолбэк — companion_proto (pure Python)
                 │  • Transport (BLE/USB)  trait + 2 impl
                 │  • Protocol codec (frame ⇄ struct)
                 │  • Session/state (caps, reqid, inflight)
                 │  • Event bus (EVT fan-out)
                 │  • File transfer (chunk/base64/sha256)
                 └─────────┬──────────┘
            ┌──────────────┴───────────────┐
      BLE (btleplug/BlueZ)            USB-CDC (serialport)
```

Три потребителя — **TUI**, **GUI** и **MCP-сервер** — все поверх одного ядра. MCP добавлен по требованию: он даёт Claude инструменты «читать данные с устройства / управлять им», и это тот же паттерн «тонкий потребитель над общим ядром».

Оба фронта — «глупые» рендереры одного и того же async-API ядра:
`core.request("wifi scan").events()` / `core.request("wifi scan").result()`.

---

## 2. Rust-ядро (`companion_core`)

Крейты-кандидаты:

| Подсистема | Крейт | Заметки |
|---|---|---|
| async runtime | `tokio` | единый рантайм |
| BLE central | `btleplug` | central-only (наша роль), обёртка над BlueZ через D-Bus |
| USB-CDC | `tokio-serial` / `serialport` | проводной транспорт и среда отладки |
| base64 | `base64` | чанки файлов |
| sha256 | `sha2` | целостность файлов |
| Python-мост | `pyo3` + `maturin` | публикация как `companion_core` |

### Транспорт-trait

```rust
#[async_trait]
trait Transport {
    async fn connect(&mut self) -> Result<()>;
    async fn send_frame(&self, bytes: &[u8]) -> Result<()>; // один кадр = один write
    fn recv_frames(&self) -> impl Stream<Item = Frame>;     // нотификации/строки
    async fn negotiated_mtu(&self) -> u16;
}
```

Две реализации: `BleTransport` (GATT central; находит `Bruc`/сервис `4371ec0b-…`, подписывается на нотификации char `d555ed97-…`, шлёт write-with-response) и `UsbTransport` (CDC, line-based). Реальность «один `serialDevice`» в прошивке ложится идеально: хост выбирает один транспорт на сессию.

### Кодек протокола

Чистый, без I/O, юнит-тестируемый: `encode(Frame)->bytes`, `decode(line)->Frame` для `REQ/RSP/END/ERR/EVT/ACK`; сборка чанков base64; сверка sha256. Это «правда» протокола из [`protocol.md`](protocol.md), один раз и в типах.

### Сессия/состояние

- аллокатор request id;
- карта in-flight `id -> oneshot/stream` (ответ-future + поток событий по id);
- набор `caps` из `HELLO`; версия протокола; машина состояний подключения;
- авторизация: подстановка `token` в `HELLO`.

### Шина событий

Broadcast `EVT`-кадров → TUI и GUI подписываются одинаково. Долгие запросы отдают поток событий по своему id (`stream start/stop`, `file get` чанки).

### Python-поверхность (pyo3)

```python
import companion_core as cc
dev = await cc.connect_ble(name="Bruc", token=TOKEN)      # или cc.connect_usb("/dev/ttyACM0", token=...)
hello = dev.hello()                                         # fw, board, mtu, caps[]
async for line in dev.request("wifi scan"):                # RSP-строки до END
    ...
job = dev.stream_start("beacon")
async for evt in job.events():                             # EVT pkt ...
    ...
job.stop()
await dev.file_get("/sd/captures/x.sub", "./x.sub")        # chunk+sha256, проверка целостности
```

---

## 3. Фронты (Python)

| Фронт | Стек | Состав |
|---|---|---|
| **TUI** | `Textual` (+ `rich`) | менеджер подключений (BLE/USB), capability-driven меню, таблицы сканов, live event-log, файловый браузер/трансфер, raw-command консоль |
| **GUI** | `PySide6`/Qt | дашборд устройства, визуализации сканов, потоковый вид (high-rate — только USB), файловый менеджер, панель host-compute задач |

Оба используют **один** `companion_core`. Никакой логики протокола во фронтах — только рендер и ввод.

### MCP-сервер (Claude как потребитель)

Третий потребитель ядра — **MCP-сервер** (Python, MCP SDK / FastMCP), который даёт Claude инструменты для чтения и управления устройством. Бэкенд — `companion_core` (на раннем этапе — `companion_proto` напрямую).

Экспортируемые инструменты (v1):

| Tool | Действие |
|---|---|
| `device_connect` | выбрать транспорт (usb `/dev/ttyACM1` или ble) и сделать `HELLO` (токен) |
| `device_status` | `status` устройства (батарея/радио/SD/WiFi) |
| `device_run` | выполнить любую CLI-команду через `REQ`, вернуть `RSP`-строки + код `END` |
| `device_caps` | список возможностей из `HELLO` |
| `device_file_list` / `device_file_get` / `device_file_put` | файловые операции (chunk+sha256) |
| `device_stream_start` / `device_stream_stop` | потоковые задачи (Phase 3+) |
| `device_busy` | владелец радио/занятость |

Регистрация — в `.mcp.json` проекта (или user-настройки). После добавления инструменты появляются у Claude после переподключения. Сборка — рано (сразу после Phase 1), чтобы дальнейший автономный цикл шёл через MCP-инструменты к устройству. Подробности безопасности (тот же токен) — [`security.md`](security.md).

---

## 4. Host-compute augmentation

Отдельный Python-слой `companion_compute` поверх ядра — переносит тяжёлую обработку с ESP32 на хост (uConsole/x86). Шаблон всегда один: **устройство добывает → выгрузка файла/событий → хост считает**.

| Домен | На устройстве | На хосте (`companion_compute`) |
|---|---|---|
| **WiFi** | захват handshake/PMKID, beacon-сниф | разбор `.pcap`/`.hccapx`, прогон hashcat/aircrack по словарям, карта эфира, дедуп AP/клиентов |
| **Sub-GHz (CC1101)** | запись `.sub`/сырого захвата | декодирование протоколов, поиск rolling-code/повторов, спектр/таймлайны, подготовка реплея |
| **NRF24** | сниф payload'ов | разбор/профилирование устройств, фаззинг-кандидаты |
| **IR / RFID / NFC** | дамп | нормализация, конвертация форматов, библиотека сигнатур |
| **GPS / wardrive** | NMEA + привязка | агрегирование, экспорт в Wigle/Kismet, карты |
| **Общее** | — | локальная БД захватов (SQLite), теги/гео, отчёты/экспорт, скриптинг пайплайнов |

Архитектурно: `companion_compute` подписывается на события ядра и работает с выгруженными файлами; результаты кладёт в БД и отдаёт фронтам для визуализации. Внешние инструменты (hashcat, aircrack-ng, rtl_433-подобные декодеры) — через subprocess-обёртки, опционально.

> Лимит BLE (~2–24 КБ/с) делает «захват→выгрузка→счёт» **правильной** моделью: высокочастотный live по BLE невозможен, но выгрузка готового захвата и тяжёлый офлайн-анализ на хосте — именно то, что снимает ограничение устройства.

---

## 5. Платформы и упаковка

| Цель | Заметки |
|---|---|
| **uConsole (aarch64)** | CM4 — встроенный BLE 5.0 (без донгла). **CM5** может требовать патчей ядра/прошивки BT — проверить рано (Phase 2). BlueZ + D-Bus. |
| **x86_64** | dev-машина; тот же стек |

Сборка:
- ядро: `maturin build --release` под каждую арку → wheel `companion_core`;
- фронты: обычные Python-пакеты; для дистрибуции — venv или PyInstaller/AppImage на арку (PySide6-wheel под aarch64 — самое узкое место упаковки, проверить отдельно);
- кросс-компиляция Rust: `cross`/`cargo --target aarch64-unknown-linux-gnu`.

---

## 6. Тестируемость

- **кодек** — чистые юнит-тесты в Rust (round-trip кадров, сборка чанков, sha256);
- **транспорт** — мок-Transport (in-memory) для интеграционных тестов сессии без железа;
- **фронты** — тесты на мок-устройстве (записанные обмены);
- **на железе** — приёмочные тесты по фазам (см. [`roadmap.md`](roadmap.md)).
