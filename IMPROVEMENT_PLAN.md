# Bruce Firmware - План функциональных улучшений

## Обзор текущего состояния

**Платформа:** LilyGo T-Embed CC1101
**Текущий размер прошивки:** 4.3 MB (92.1% Flash)
**RAM использование:** 40.3%
**Найдено TODO/FIXME:** 81+
**Модулей:** 99 файлов

---

## Приоритет 1: КРИТИЧЕСКИЕ УЛУЧШЕНИЯ (1-2 недели)

### 1.1 NRF24 - Завершение функционала

**Проблема:** MouseJack и KeyboardJack не работают, UART режим не реализован
**Файлы:** `src/modules/NRF24/nrf_hijack.cpp`, `nrf_common.cpp`

**Задачи:**
- [ ] Реализовать корректный протокол MouseJack (Logitech Unifying)
- [ ] Добавить поддержку Microsoft Wireless Mouse/Keyboard
- [ ] Исправить payload injection для беспроводных клавиатур
- [ ] Добавить режим UART для внешнего NRF24 модуля
- [ ] Создать библиотеку известных уязвимых устройств

**Оценка сложности:** Высокая
**Зависимости:** Нужны тестовые устройства

### 1.2 RF - Rolling Code Attack (Rolljam)

**Проблема:** Атака на rolling codes не реализована
**Файлы:** `src/modules/rf/rf_send.cpp`, новый файл `rf_rolljam.cpp`

**Задачи:**
- [ ] Реализовать захват rolling code с одновременным глушением
- [ ] Добавить анализатор Keeloq протокола
- [ ] Поддержка Nice FLO-R, Came TOP, Faac protocols
- [ ] UI для выбора метода атаки
- [ ] Сохранение захваченных кодов с timestamp

**Оценка сложности:** Очень высокая
**Зависимости:** Два CC1101 или быстрое переключение RX/TX

### 1.3 WiFi - Улучшение захвата Handshake

**Проблема:** Захват WPA handshake нестабилен
**Файлы:** `src/modules/wifi/wifi_atks.cpp`, `sniffer.cpp`

**Задачи:**
- [ ] Добавить индикатор качества захваченного handshake
- [ ] Автоматическое определение PMKID
- [ ] Экспорт в формат hashcat/aircrack-ng
- [ ] Параллельный deauth + capture
- [ ] Фильтрация по BSSID для уменьшения шума

**Оценка сложности:** Средняя

---

## Приоритет 2: ВАЖНЫЕ УЛУЧШЕНИЯ (2-4 недели)

### 2.1 BLE - Расширенные атаки

**Текущее состояние:** Только BLE spam реализован
**Файлы:** `src/modules/ble/`

**Задачи:**
- [ ] GATT enumeration - полный анализ BLE сервисов
- [ ] BLE sniffing с декодированием протоколов
- [ ] Атака на BLE pairing (MITM)
- [ ] Поддержка BLE Mesh атак
- [ ] Фаззинг BLE характеристик
- [ ] База данных известных BLE устройств (IoT)

**Оценка сложности:** Высокая

### 2.2 RF - Spectrum Analyzer Pro

**Текущее состояние:** Базовая визуализация
**Файлы:** `src/modules/rf/rf_spectrum.cpp`, `rf_waterfall.cpp`

**Задачи:**
- [ ] Waterfall display с историей
- [ ] Автоматическое определение активных частот
- [ ] Измерение мощности сигнала (dBm калибровка)
- [ ] Маркеры пиков и полосы пропускания
- [ ] Экспорт данных в CSV
- [ ] Сравнение спектра "до/после"

**Оценка сложности:** Средняя

### 2.3 IR - Универсальный пульт

**Текущее состояние:** TV-B-Gone + запись/воспроизведение
**Файлы:** `src/modules/ir/`

**Задачи:**
- [ ] Обучение новым кнопкам с автоопределением протокола
- [ ] Создание кастомных пультов с UI
- [ ] База данных IR кодов с поиском (IRDB integration)
- [ ] Макросы - последовательность команд
- [ ] IR repeater режим
- [ ] Анализ неизвестных протоколов

**Оценка сложности:** Средняя

### 2.4 RFID - Улучшения эмуляции

**Текущее состояние:** Чтение/запись работает, эмуляция ограничена
**Файлы:** `src/modules/rfid/`

**Задачи:**
- [ ] Полная эмуляция MIFARE Classic (все сектора)
- [ ] Атака Nested/Hardnested для извлечения ключей
- [ ] Поддержка MIFARE DESFire (базовая)
- [ ] Автоматическое определение типа карты
- [ ] Клонирование iClass/HID
- [ ] База данных стандартных ключей (расширенная)

**Оценка сложности:** Высокая

---

## Приоритет 3: УЛУЧШЕНИЯ UX (2-3 недели)

### 3.1 Unified Settings System

**Проблема:** Настройки разбросаны по модулям
**Файлы:** `src/core/settings.cpp`, `config.cpp`

**Задачи:**
- [ ] Единый формат конфигурации (JSON)
- [ ] Профили настроек (быстрое переключение)
- [ ] Экспорт/импорт настроек
- [ ] Сброс к заводским настройкам по модулям
- [ ] Синхронизация настроек через WiFi

### 3.2 Logging & Reporting

**Проблема:** Нет структурированного логирования
**Файлы:** Новый модуль `src/core/logging/`

**Задачи:**
- [ ] Централизованный logger с уровнями (DEBUG/INFO/WARN/ERROR)
- [ ] Запись в файл с ротацией
- [ ] Генерация отчетов об атаках (HTML/JSON)
- [ ] Timeline событий с timestamps
- [ ] Экспорт логов через WiFi/Serial

### 3.3 UI Improvements

**Проблема:** Непоследовательный UI, дублирование кода
**Файлы:** `src/core/display.cpp`, `scrollableTextArea.h`

**Задачи:**
- [ ] Создать UI component library
- [ ] Единый стиль для всех progress bars
- [ ] Графики в реальном времени (универсальный компонент)
- [ ] Анимации переходов между экранами
- [ ] Темы оформления (сохранение в конфиг)
- [ ] Accessibility - размер шрифта, контраст

---

## Приоритет 4: НОВЫЕ ФУНКЦИИ (1-2 месяца)

### 4.1 JavaScript Automation

**Текущее состояние:** Интерпретатор есть, API неполный
**Файлы:** `src/modules/bjs_interpreter/`

**Задачи:**
- [ ] Завершить API для всех модулей:
  - [ ] WiFi API (scan, attack, sniff)
  - [ ] BLE API (scan, connect, spam)
  - [ ] RF API (send, receive, jam)
  - [ ] IR API (send, receive, learn)
  - [ ] GPIO API (read, write, PWM)
  - [ ] Display API (draw, print, clear)
- [ ] Event system (onPacketReceived, onButtonPress)
- [ ] Scheduler для периодических задач
- [ ] Script marketplace (загрузка из GitHub)
- [ ] Дебаггер с breakpoints

### 4.2 Multi-Attack Chains

**Новый функционал**
**Файлы:** Новый модуль `src/modules/attack_chain/`

**Задачи:**
- [ ] Визуальный редактор цепочек атак
- [ ] Условные переходы (если handshake захвачен → deauth stop)
- [ ] Параллельное выполнение (WiFi + BLE одновременно)
- [ ] Шаблоны атак (Evil Twin + Captive Portal комбо)
- [ ] Импорт/экспорт цепочек

### 4.3 Remote Control

**Новый функционал**
**Файлы:** Новый модуль `src/modules/remote/`

**Задачи:**
- [ ] Web UI для удаленного управления
- [ ] REST API для всех функций
- [ ] WebSocket для real-time данных
- [ ] Мобильное приложение (React Native)
- [ ] Telegram bot интеграция
- [ ] MQTT для IoT интеграции

### 4.4 Packet Crafting UI

**Новый функционал**
**Файлы:** Новый модуль `src/modules/packet_craft/`

**Задачи:**
- [ ] Визуальный редактор пакетов (WiFi/BLE/RF)
- [ ] Hex editor с подсветкой полей
- [ ] Шаблоны пакетов для разных протоколов
- [ ] Fuzzing с параметрами
- [ ] Replay с модификацией

---

## Приоритет 5: ОПТИМИЗАЦИЯ (Постоянно)

### 5.1 Memory Management

**Задачи:**
- [ ] Аудит всех malloc/new на утечки
- [ ] Использование PSRAM для больших буферов
- [ ] Object pooling для частых аллокаций
- [ ] Stack usage monitoring

### 5.2 Performance

**Задачи:**
- [ ] Профилирование горячих путей
- [ ] DMA для SPI операций где возможно
- [ ] Оптимизация display refresh (dirty rectangles)
- [ ] Background tasks для тяжелых операций

### 5.3 Code Quality

**Задачи:**
- [ ] Устранение дублирования кода (DRY)
- [ ] Единый coding style (clang-format)
- [ ] Static analysis (cppcheck integration)
- [ ] Unit tests для критических функций
- [ ] Documentation (Doxygen)

---

## Hardware-Specific Improvements (T-Embed CC1101)

### CC1101 Advanced Features

**Текущее использование:** ~60%
**Файлы:** `src/modules/rf/rf_utils.cpp`

**Задачи:**
- [ ] Adaptive Frequency Control (AFC) - улучшение приёма
- [ ] Variable output power (UI контроль)
- [ ] RX bandwidth adjustment
- [ ] FIFO management для burst TX
- [ ] Wake-on-Radio для мониторинга
- [ ] Address filtering в hardware
- [ ] CRC bypass для raw analysis

### Display Optimization

**Текущее состояние:** Частые полные перерисовки
**Файлы:** `boards/lilygo-t-embed-cc1101/interface.cpp`

**Задачи:**
- [ ] Partial refresh where possible
- [ ] DMA для SPI display
- [ ] Frame buffer в PSRAM
- [ ] Sprite system для анимаций

---

## Метрики успеха

| Метрика | Текущее | Цель |
|---------|---------|------|
| TODO comments | 81+ | <20 |
| Code duplication | Высокое | Низкое |
| Test coverage | 0% | >30% |
| Documentation | Минимальная | Полная API doc |
| Memory leaks | Неизвестно | 0 |
| Crash rate | Неизвестно | <1% |
| Feature completion | ~70% | 95% |

---

## Ресурсы и ссылки

- [Flipper Zero Firmware](https://github.com/flipperdevices/flipperzero-firmware) - референс
- [CC1101 Datasheet](https://www.ti.com/lit/ds/symlink/cc1101.pdf)
- [NRF24 MouseJack](https://github.com/BastilleResearch/mousejack)
- [IRDB Database](https://github.com/probonopd/irdb)
- [Proxmark3 RFID](https://github.com/RfidResearchGroup/proxmark3)

---

*Документ создан: 2026-01-20*
*Последнее обновление: 2026-01-20*
