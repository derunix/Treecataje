# Companion — план улучшений (host + firmware)

Живой roadmap поверх уже сделанного (Phases 0–6 + словари + recon + live-телеметрия).
Статус-легенда: 🟢 сделано · 🟡 частично · ⬜ TODO. Оценка усилий: S (часы) ·
M (1 день) · L (несколько дней). Приоритет: P0 (важно) … P3 (nice-to-have).

---

## 0. Ответы на открытые вопросы

### Можно ли работать по USB и BLE одновременно?
**Сейчас — нет.** В прошивке один глобальный `serialDevice` (`main.cpp:23`).
`enableBLEAPI()` переключает его на BLE-serial (`ble_api.cpp:39`), `end()` — назад
на USB. `handleSerialCommands` читает только активный `serialDevice`, а
`companion::emit()` пишет только в него. Поэтому при включённом BLE USB-консоль/MCP
немеют до `companion ble off`.

**Сделать одновременную работу можно** (Phase 7 ниже), это умеренный рефактор ядра:
1. В `handleSerialCommands` опрашивать **оба** входа: `USBserial` и BLE-serial
   (BLE приходит через callback `BLESerialCallbacks` — буферизовать в очередь).
2. Ввести «транспорт ответа» на кадр: `companion::handleLine(cli, line, replyDev)`
   и `emit(frame, replyDev)` вместо глобального `serialDevice`. Ответ уходит туда,
   откуда пришёл `REQ`.
3. Стрим привязать к транспорту-владельцу (`g_streamReplyDev`), `tick()` шлёт EVT
   в него.
4. Generic-CLI dispatch уже временно подменяет `serialDevice` на `g_framing` —
   обернуть так, чтобы `g_framing` оборачивал нужный реальный транспорт.

Стоимость/риски: BLE-API ест ~66 КБ heap (включён постоянно — RAM-бюджет тугой);
удвоение поверхности атаки (закрывается токеном, Phase 6 уже есть); аккуратность с
конкурентным SPI (дисплей) при стримах. Выгода: управление радио по BLE **без**
потери USB-консоли/MCP; одновременная отладка; «телефон рулит радио, ноут пишет лог».
**Оценка: L, приоритет P1.**

### Пункт меню вкл/выкл BLE
**Уже есть**: `ConfigMenu.cpp:192` → `"Toggle BLE API"` → `enableBLEAPI()`.
Улучшения (Phase 7-UX, S): отдельный пункт **«Companion BLE: on/off»** с отображением
текущего состояния (✓/✗), счётчиком свободного heap и предупреждением про RAM; после
рефактора (выше) включение BLE **перестанет глушить USB**. Также — пункт «Companion:
enable/disable» (флаг `companionEnabled`) и показ/QR токена.

---

## 1. Firmware roadmap

### 1.1 Транспорт и параллелизм
- 🟢 **Одновременный USB+BLE** (Sprint A): `serialDevice` больше не перехватывается;
  `serialcmds.cpp` опрашивает USB + `bleApiSerial`; per-frame reply-device в
  `companion::handleLine(..., reply)`/`emit()`; стрим помнит транспорт-владельца.
- ⬜ **P2 / M — Раздельные сессии auth** на каждый транспорт (сейчас один `g_authed`;
  при двух транспортах нужен `authed[USB]`, `authed[BLE]`).
- ⬜ **P2 / S — Больший BLE MTU по факту**: запросили `setMTU(517)`, но передача файлов
  всё ещё чанками ≤192 — проверить реальный negotiated MTU и поднять размер чанка.
- ⬜ **P3 / M — BLE bonding / шифрование канала** (NimBLE `setSecurityAuth(bond,mitm,sc)`,
  passkey на экране) — конфиденциальность + anti-MITM поверх токена.

### 1.2 Безопасность (Phase 6 расширение)
- 🟢 challenge-response токен, BLE-замок, сброс при дисконнекте, персист.
- 🟢 **Анти-brute** (Sprint A): после 5 неудачных AUTH — блокировка на 30 с
  (`ERR 7 AUTH locked retry_ms=…`). ЗАМЕТКА: lockout глобальный — потенциальный DoS
  легитимной сессии; сделать per-transport позже.
- ⬜ **P2 / S — Меню/QR токена на устройстве** (как webUI-креды): показать/сгенерировать.
- ⬜ **P3 / M — Уровни доступа**: радио-TX (`rf tx`, `nrf jam`, `ir tx`) только после
  «расширенной» авторизации/подтверждения на устройстве.

### 1.3 Радио-функции (стримы и захваты)
- 🟢 стримы telemetry/wifi/nrf/rf, `interval=`.
- 🟢 **Захват в файл на устройстве** (Sprint B): `companion capture start <kind> [interval=]
  [path=]` пишет sweeps на SD (`/BruceCapture/<kind>-<ms>.txt`, формат host save_stream:
  `# kind:` + EVT-payload построчно). `capture stop` отдаёт path/bytes/samples/sha256;
  `capture status`; **переживает дисконнект хоста** (resetAuth не рвёт захват, только
  отцепляет progress-EVT транспорт). Хост: `Companion.capture()/capture_fetch()`, MCP
  `device_capture`, тест `phase9_capture_test.py`. Снимает «live по BLE медленно».
- 🟢 **WiFi handshake-захват в pcap** (non-modal): `companion capture start handshake
  [ch=N] [bssid=MAC]` — promiscuous-режим, rx-callback только копирует beacon/EAPOL-кадры
  в очередь, `tick()` пишет libpcap (DLT 105) на SD. Канал-хоп или пин, BSSID-фильтр.
  `companion wifi deauth bssid=MAC [sta=..] [ch=..] [count=..]` — инъекция deauth
  (esp_wifi_80211_tx + глобальный sanity-check bypass). Полный цикл оркеструется на
  хосте (`wifi_attack.run_attack`): find→deauth→capture→crack→brute. ⚠ только для
  авторизованного тестирования своих сетей.
- ⬜ **P2 / M — nrf addr-сниффер в стрим** (адреса/каналы, не только RPD).
- ⬜ **P2 / S — rf RAW-rx стрим** (декодированные кадры / сырые тайминги по событию GDO0).
- ⬜ **P3 / S — параметры стрима** (rf шаг/число бинов, nrf диапазон каналов).

### 1.4 UX устройства / меню
- 🟢 **«Companion BLE: ON/OFF» пункт меню** с состоянием (Sprint A): ConfigMenu →
  Advanced, динамический label через `isBLEAPIEnabled()`; включение больше не глушит USB.
- ⬜ **P2 / S — «Companion enable/disable»** пункт + индикатор активной сессии (иконка
  «host connected» в статус-баре).
- ⬜ **P3 / S — арбитраж радио с UI**: companion уважает `g_radioOwner` и UI тоже
  (сейчас флаг есть, UI его не проверяет → возможна гонка SPI).

### 1.5 Надёжность
- ⬜ **P1 / S — стабильность USB-CDC** (в этой сессии частые ре-энумерации): проверить
  watchdog/таймауты USB-стека, не зависит ли от кабеля/питания; добавить heartbeat.
- ⬜ **P2 / S — фрагментация EVT** для BLE (длинные rf/wifi кадры > MTU обрезаются в
  notify) — резать на под-кадры или поднять MTU (см. 1.1).
- ⬜ **P3 / S — flash usage**: ~93%+, следить; вынести опц. модули за флаги сборки.

---

## 2. Host roadmap

### 2.1 GUI (PySide6)
- 🟢 Functions/Console/Files(браузер)/Stream/Analyze/Dictionaries, история, авто-рефреш,
  live-heap, recon, capture save/load.
- ⬜ **P1 / M — Живой стрим в реальном времени**: worker отдаёт события инкрементально
  (сейчас collect-then-return) → live-обновление спектра/таблицы во время скана.
- ⬜ **P2 / S — Избранное / быстрые действия** (закреплённые команды и IR-сигналы).
- ⬜ **P2 / S — Экспорт**: отчёты/captures в .md/.csv (частично есть Save report/stream).
- ⬜ **P2 / M — Спектр-графика**: рисованный спектр/водопад (rf/nrf) вместо ASCII (QPainter).
- ⬜ **P3 / S — Тема/масштаб** под экран uConsole (компактный режим).

### 2.2 TUI (Textual)
- 🟢 дерево функций + словари, USB/BLE.
- ⬜ **P2 / S — файловый браузер** (как в GUI).
- ⬜ **P2 / S — панель стримов/спарклайны** в TUI.
- ⬜ **P3 / S — история команд** в консоли TUI.

### 2.3 MCP
- 🟢 23 инструмента (connect/run/файлы/анализ/стрим/recon/словари/токен/capture/wpa).
- 🟢 **`device_capture`** (Sprint B): захват в файл на устройстве → fetch + verify + анализ.
- 🟢 **`wpa_crack`/`device_handshakes`/`device_crack_handshake`**: WPA-крек на хосте.
- 🟢 **`device_deauth`/`device_wifi_attack`/`list_crackers`**: deauth + полный цикл.
- 🟢 **Реальные крекеры** (`crackers.py`): GUI/TUI/MCP/оркестрация используют
  **aircrack-ng** (приоритет, стабилен на CPU) / hashcat (если есть рабочий OpenCL —
  на uConsole PoCL падает) с автодетектом, fallback на pure-Python. Автопоиск словарей
  (`/usr/share/wordlists`, `captures/`, `dictionaries/wordlists/`; rockyou тянется из
  Kali-докера). Live-прогресс (k/s), отмена, экспорт `.hc22000` для GPU. GUI: выбор
  инструмента+словаря+brute в Analyze; TUI: `:crack`/`:attack`/`:wordlists`;
  `phase11_crackers_test.py`. Замер на cortex-a72: aircrack ~408/с (потолок CPU).
- ⬜ **P3 / S — ресурсы MCP**: отдавать последние captures/отчёты как ресурсы.
- 🟢 Правило: держать MCP в синхроне с протоколом (соблюдается).

### 2.4 Host-compute / анализ
- 🟢 wifi(AP+OUI)/nrf/rf/telemetry/nrf_scan/battery/pcap анализаторы.
- 🟢 **WPA/WPA2-handshake + PMKID крекинг** на хосте (`wpa_crack.py`): чистый Python
  парсер pcap (DLT 105/radiotap) → извлечение 4-way handshake/PMKID → словарный
  перебор (PBKDF2→PTK→MIC, key-version 1/2/3 = MD5/SHA1/AES-CMAC). Валидирован
  против опубликованных IEEE 802.11i PMK-векторов + forge/parse/crack round-trip
  (`phase10_wpa_test.py`). MCP: `wpa_crack`/`device_handshakes`/`device_crack_handshake`
  (fetch HS_*.pcap из `/BrucePCAP/handshakes` устройства + крек). GUI: «Crack WPA»
  в Analyze; TUI: `:crack`/`:crackdev`. Словарь `dictionaries/wordlists/common.txt`.
  Захват handshake'ов — существующим сниффером прошивки (modal UI); non-modal
  `companion capture start handshake` — следующий firmware-шаг (см. 1.3).
- ⬜ **P2 / S — sub-GHz декодер протоколов** (RcSwitch/Princeton) из rf-захвата.
- ⬜ **P3 / S — гео/время-корреляция** wifi+gps (карта точек).

### 2.5 Словари / базы
- 🟢 IR (Flipper-формат + импорт IRDB), MIFARE-ключи, OUI-вендоры.
- ⬜ **P2 / S — реально импортировать Flipper-IRDB** (склонировать, `--import-ir`).
- ⬜ **P2 / S — sub-GHz база** (частоты/протоколы, известные коды — с дисклеймером).
- ⬜ **P3 / S — NRF24 vendor-префиксы**, BLE OUI, default-creds для EvilPortal.

### 2.6 Упаковка / архитектура
- ⬜ **P2 / M — единый пакет** `pip install` + console-scripts (`companion-gui/tui`).
- ⬜ **P3 / L — Rust-ядро** (заявленный стек): транспорт+кодек на Rust + pyo3, фронты
  на Python. Сейчас ядро — `companion_proto.py` (доказан протокол). Нужен Rust toolchain.

---

## 3. Сквозное
- ⬜ **P1 / M — авто-тесты в CI**: офлайн-юниты (catalog/dicts/compute/парсеры) гонять
  без железа; HW-тесты — опционально по тегу.
- ⬜ **P2 / S — `/code-review` + `/simplify`** по `gui.py` (~1000 строк): дедуп, разбить
  на модули (worker / tabs).
- ⬜ **P2 / S — обновить шапку README/roadmap** (устарела: «Phase 0–5»).
- ⬜ **P3 / S — скриншоты/GIF** GUI/TUI в доках.

---

## 4. Рекомендуемая последовательность

**Спринт A — параллельность + UX (требует прошивки, P1):**
1. USB+BLE одновременно (1.1) + per-request reply-device.
2. Пункт меню «Companion BLE on/off» с состоянием (1.4) — теперь без потери USB.
3. Анти-brute AUTH (1.2) — мелочь, едет тем же билдом.
4. На хосте: раздельная индикация транспортов, обновить доки.

**Спринт B — захват и live (P1/P2):**
5. `companion capture start` в файл (1.3) + `device_capture` MCP.
6. Инкрементальный live-стрим в GUI (2.1) + рисованный спектр.
7. WPA-handshake/декодеры на хосте (2.4).

**Спринт C — полировка (P2/P3):**
8. CI офлайн-тесты (3) + рефактор `gui.py`.
9. Упаковка pip; импорт полного IRDB/OUI; sub-GHz база.
10. (позже) Rust-ядро; NimBLE bonding.

**Быстрые победы (можно сразу, host-only, без прошивки):** избранное (2.1),
TUI файловый браузер (2.2), импорт IRDB/OUI (2.5), экспорт (2.1), обновить доки (3),
CI офлайн-тесты (3).

**Big rocks (нужна стабильная прошивка/USB):** одновременный USB+BLE (1.1),
захват в файл (1.3), bonding (1.1), уровни доступа (1.2).
