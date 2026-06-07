# Firmware — дизайн компаньон-режима

Всё ниже привязано к реальным файлам форка. Где внутренности не доказуемы из исходника на момент написания — помечено **ASSUMPTION** (проверить в коде при реализации).

Принцип: **аддитивно и минимально.** Не переписываем диспетчер и команды; добавляем тонкий слой и пару охранников. При выключенном флаге поведение прошивки = текущее.

---

## 1. Что переиспользуем как есть

| Что | Где | Зачем |
|---|---|---|
| `SerialCli` / SimpleCLI диспетчер + все `createXxxCommands()` | `src/core/serial_commands/cli.cpp:36-64` | 80+ команд становятся командной поверхностью компаньона бесплатно |
| Абстракция `SerialDevice` | `include/SerialDevice.h` | интерфейс, который декорируем для фрейминга |
| `BLESerialService` (NimBLE, сервис `4371ec0b-…`, char `d555ed97-…`) | `src/modules/ble_api/services/BLESerialService.{h,cpp}` | готовый BLE-транспорт, MTU-callback, advertising |
| `BLE_API` setup/teardown + `serialDevice` swap | `src/modules/ble_api/ble_api.cpp:22-54` | включение BLE-транспорта |
| `enableBLEAPI()` + пункт меню | `src/core/settings.cpp:1637`, `src/core/settings.h:109` | пользовательский on/off |
| Очередь команд `cmdQueue`/`rspQueue` + `parseSerialCommand()` | `src/core/serialcmds.cpp:8-35` | программная инъекция команд без деструктивного `readStringUntil` |
| FreeRTOS serial-таск (prio 2, core 1) | цикл `_serialCmdsTaskLoop` `src/core/serialcmds.cpp:56-62`, создание `startSerialCommandsHandlerTask` `:64-86`, запуск `src/main.cpp:509` | уже крутится параллельно UI — туда и встроимся |
| `BruceConfig` (JSON `/bruce.conf`) | `src/core/config.h:20`, `config.cpp` | добавить флаг + токен по существующему паттерну |

---

## 2. Что добавляем

Новый модуль: `src/core/companion/companion.{h,cpp}` + декоратор `FramingSerialDevice`.

### 2.1. `FramingSerialDevice` — декоратор фрейминга (ядро)

Реализует `SerialDevice` (тот же интерфейс, `include/SerialDevice.h`) и **оборачивает** реальный транспорт (`BLESerialService` или `USBserial`). Это позволяет добавить кадрирование **не трогая ни одну из 80+ команд** — они продолжают звать `serialDevice->println(...)`, а декоратор оборачивает вывод в `RSP <id>`.

```cpp
// эскиз, не финальный код
class FramingSerialDevice : public SerialDevice {
    SerialDevice *inner;        // реальный BLESerialService / USBserial
    uint32_t curReqId = 0;      // id текущего обрабатываемого REQ
    bool framing = false;       // включён ли framed-режим (после HELLO)
public:
    void beginRequest(uint32_t id) { curReqId = id; }
    void endRequest(int code);    // шлёт inner: "END <id> <code>\r\n"
    void emitEvent(const String &payload, uint32_t id = 0); // "EVT <id> ..."
    void emitError(uint32_t id, int code, const String &msg);

    // SerialDevice: каждую строку оборачиваем как "RSP <id> <line>"
    size_t println(const String &s) override {
        if (framing) return inner->println("RSP " + String(curReqId) + " " + s);
        return inner->println(s);
    }
    // print() аккумулирует до '\n', затем флашит как один RSP (см. ниже про print без перевода строки)
    // available()/readStringUntil() — проксируются в inner
    // ...
};
```

Тонкость с `print()` (без `\n`): часть команд печатают по кускам через `print()`. Декоратор буферизует частичный вывод и флашит как `RSP` по приходу `\n` или по завершению запроса. Это локальная логика декоратора, тела команд не меняются.

### 2.2. Установка декоратора

Когда компаньон-режим включён, `serialDevice` указывает на `FramingSerialDevice`, который оборачивает выбранный транспорт:

- **BLE-сессия:** `framing.inner = &serial_service` (вместо прямого `serialDevice = &serial_service` в `ble_api.cpp:32`).
- **USB-сессия:** `framing.inner = &USBserial`.

Так как разбор команд идёт **только** в serial-таске (однопоточно для парсинга), отдельной синхронизации `serialDevice` не требуется — переключение/использование происходит в одном таске.

### 2.3. Companion-обработчик кадров (неблокирующий путь)

В `handleSerialCommands()` (`src/core/serialcmds.cpp:37-54`) добавляем ветку: если компаньон-режим включён — вызываем `handleCompanionFrame()` **вместо** legacy-блока (строки 47-53). Критично: **не вызываем `backToMenu()`**.

```cpp
// эскиз внутри handleSerialCommands(), companion-ветка
if (companionEnabled) {
    if (!serialDevice->available()) return;
    String line = serialDevice->readStringUntil('\n');     // один кадр = один BLE-write
    companion.onFrame(line);   // парсит TYPE/ID, проверяет HELLO/auth,
                               // ставит framing.beginRequest(id),
                               // serialCli.parse(payload),
                               // framing.endRequest(code).
    return;                    // <-- НЕТ backToMenu(): UI не трогаем
}
// ... иначе legacy-путь как сейчас (строки 47-53) ...
```

Это и есть решение жёсткого ограничения №2 из [`README`](README.md): `backToMenu()` (`utils.cpp:25`) ставит `returnToMenu=true`, который рвёт UI-циклы (`utils.cpp:17`). В компаньон-пути его просто нет → автономный экран продолжает жить.

---

## 3. Модель конкурентности (минимальный риск)

**Рекомендация: переиспользовать существующий serial-таск, не плодить второй.**

- Serial-таск уже работает параллельно UI (`_serialCmdsTaskLoop`, prio 2, core 1, цикл 10 мс — `serialcmds.cpp:56-62`). Companion-кадры обрабатываются в нём же по неблокирующему пути.
- **Почему не отдельный таск:** дисплей, SD, NRF24, W5500 и CC1101 висят на **общей SPI-шине** (MOSI/MISO/SCK), а PN532/PMIC/fuel-gauge — на **общей I2C**. Второй несинхронизированный таск, дёргающий радио, конкурировал бы за шину. Переиспользование одного таска снимает этот класс гонок.
- **Находка (проверено grep по `src/`): глобального SPI-мьютекса в прошивке НЕТ.** Существуют только локальные/доменные блокировки: `ir_tx_mutex` (`modules/ir/TV-B-Gone.cpp:139`), `handshakeMutex`/`fileMutex` в WiFi-сниффере (`modules/wifi/sniffer.cpp`), мьютекс в `audio.cpp`, и `portMUX` для WiFi-критсекции (`wifi/wifi_common.cpp:17`). **Общая SPI-шина (CC1101/SD/дисплей/NRF) защищена только тем, что радио-операции идут последовательно в одном контексте (UI/loop).** Это и есть неявный инвариант, который компаньон не должен нарушать.
- **Следствие:** поскольку нет шинного мьютекса, который защитил бы конкурентный доступ, **busy-арбитраж (§4) обязателен, а не опционален** — компаньон не имеет права лезть на радио/SPI, пока модальная фича активна.

### Учесть: интерпретатор убивает serial-таск

В `loop()` при входе в JS-интерпретатор serial-таск **удаляется** и потом пересоздаётся (`src/main.cpp:528,535`). Значит во время работы интерпретатора компаньон недоступен. Поведение: при активном интерпретаторе BLE-нотификации не обслуживаются; на стороне хоста это видно как `TIMEOUT`. Документировать как ожидаемое; при желании — `companion`-глагол для запуска/остановки интерпретатора с корректной координацией (later).

---

## 4. Арбитраж ресурсов (BUSY)

Модальные фичи (RF-spam, снифферы и т.п.) крутят собственный блокирующий `while(...)` в UI-таске (loop). Serial-таск при этом продолжает работать (преемптивный FreeRTOS, тот же приоритет → тайм-слайсинг), поэтому компаньон-команда **может прийти во время модальной фичи** и начать конкурировать за радио/шину.

**v1:** companion-команды, которым нужен эксклюзивный ресурс (CC1101/NRF/WiFi-radio), проверяют флаг занятости и возвращают `ERR <id> 2 BUSY`, а не лезут на шину.

- **Находка: готового busy-флага в прошивке НЕТ** — его нужно ВВЕСТИ. Предлагаемый минимум: лёгкий «owner»-таг в `globals` —
  ```cpp
  enum RadioOwner { OWNER_NONE, OWNER_UI, OWNER_COMPANION };
  volatile RadioOwner radioOwner = OWNER_NONE;   // кто держит радио/SPI
  ```
- **Кто должен выставлять/снимать таг (модальные фичи, которые надо обернуть на входе/выходе):**
  - RF: `rx`/`tx`/`scan` лупы (`modules/rf/`), live-приём CC1101;
  - NRF24: `scan`/`jam_sweep` (`modules/NRF24/`);
  - WiFi: `sniffer` (`modules/wifi/sniffer.cpp`), деаут/атаки;
  - IR: `rx` (`modules/ir/`).
  В v1 достаточно обернуть фичи из приоритета v1 (rf/wifi/nrf); остальные — по мере включения в каталог.
- **Проверка в companion-пути:** перед radio-командой — `if (radioOwner != OWNER_NONE && radioOwner != OWNER_COMPANION) -> ERR BUSY`. Команды, не трогающие радио/SPI-эксклюзив (status, free, storage-чтение и т.п.), выполняются всегда.
- `companion busy` возвращает текущего владельца (см. [`protocol.md`](protocol.md#4-примеры-обмена)).

---

## 5. Конфиг: флаг и токен

В `BruceConfig` (`src/core/config.h:20`, секция Misc ~строка 94) добавить по существующему паттерну:

```cpp
// Companion
bool companionEnabled = false;   // framed non-modal режим (по умолчанию выкл => поведение как сейчас)
String companionToken = "";      // общий секрет для HELLO (пусто => авторизация требует установки токена)
```

- Сериализация/десериализация — рядом с прочими полями в `config.cpp` (паттерн `toJson`/`fromJson`).
- Пункт меню: расширить существующий BLE-API toggle (`enableBLEAPI()`, `settings.cpp:1637`) под-опцией «Companion mode» и генерацией/показом токена (по аналогии с `webUI`-кредами в `config.h:69`).
- **Дефолт выключен** ⇒ при невключённом флаге `handleSerialCommands()` идёт по legacy-ветке, прошивка байт-в-байт как сейчас.

---

## 6. Поверхность capabilities (`caps` в HELLO)

`caps` выводятся из тех же `#ifdef`, что гейтят регистрацию команд в `cli.cpp:36-64`:

| cap | условие компиляции (cli.cpp) | всегда? |
|---|---|---|
| crypto, gpio, ir, nrf, power, rf, settings, storage, status, gps, util, wifi | — | да (строки 39-50) |
| badusb | `#ifdef USB_as_HID` (52) | зависит |
| js | `#ifndef LITE_VERSION` (55) | зависит |
| screen | `#ifdef HAS_SCREEN` (58) | зависит |
| sound | `#if defined(HAS_NS4168_SPKR) || defined(BUZZ_PIN)` (61) | зависит |

`companion.cpp` собирает строку `caps=` теми же `#ifdef`. Хост-UI предлагает только то, что есть на устройстве.

---

## 7. Точки правки (сводка)

| Файл | Правка |
|---|---|
| `src/core/companion/companion.{h,cpp}` | **новый** — обработчик кадров, HELLO/auth, caps, busy-арбитраж, file get/put, stream start/stop |
| `src/core/companion/FramingSerialDevice.{h,cpp}` | **новый** — декоратор `SerialDevice` для кадрирования вывода |
| `src/core/serialcmds.cpp:37-54` | ветка companion в `handleSerialCommands()` (без `backToMenu()`) |
| `src/modules/ble_api/ble_api.cpp:32` | при companion-режиме оборачивать `serial_service` во `FramingSerialDevice` |
| `src/core/config.h` (~94), `src/core/config.cpp` | поля `companionEnabled`, `companionToken` + (de)сериализация |
| `src/core/settings.cpp:1637` | под-опция меню «Companion mode» + токен |
| `src/core/serial_commands/cli.cpp` | (опц.) `createCompanionCommands(&_cli)` для новых `companion`-глаголов |

---

## 8. Известные узкие места

1. **Потолок пропускной способности:** `vTaskDelay(pdMS_TO_TICKS(10))` после каждого `notify()` (`BLESerialService.cpp:44,50,65,98`) → ~100 нотификаций/с. Это главный лимит device→host. **Phase 6:** заменить на credit/ack-схему.
2. **Баг `vprintf`:** `sprintf(str, fmt, args)` (`BLESerialService.cpp:62`) передаёт `va_list` как vararg — формат сломан; плюс буфер фиксирован `BUFFER_SIZE=128` (`.h:7`), а `size` считается из `vsnprintf` (возможен выход за буфер). Фрейминг **не должен** полагаться на `printf`-путь; использовать `println(String)`/`write`. При случае — починить (`vsnprintf(str, sizeof(str), fmt, args)`).
3. **`available()` деструктивен** (`newValue` сбрасывается при чтении) и `readStringUntil` отдаёт всё значение характеристики (`BLESerialService.cpp:34-77`): **строго один кадр на один BLE-write** на стороне хоста. Несколько кадров в одном write будут потеряны/слиты.
4. **MTU:** по умолчанию 23 (≈20 payload). Согласование есть (`onMTUChange`→`update_mtu`, `ble_api.cpp:16,40`), но хост обязан инициировать MTU exchange (btleplug/BlueZ). Размер чанков считать от фактического MTU.
5. **`serialDevice` единый указатель:** BLE и USB не одновременно. Хост выбирает один транспорт на сессию (это и так заложено в [`host.md`](host.md)).

---

## 9. Чек-лист «не сломать автономность»

- [ ] При `companionEnabled=false` путь обработки идентичен текущему (legacy CLI + `backToMenu`).
- [ ] В companion-пути **нет** вызова `backToMenu()` → текущий экран UI не перерисовывается принудительно.
- [ ] Companion-команда к занятому радио возвращает `BUSY`, а не лезет на шину.
- [ ] BLE advertising/обычные BLE-фичи прошивки не регрессируют.
- [ ] Сборка под `lilygo-t-embed-cc1101` проходит; размер прошивки в партиции (см. `custom_16Mb.csv`/`custom_8Mb.csv`).
- [ ] Ручной приёмочный тест: при подключённом хосте навигация по меню на устройстве работает как обычно.
