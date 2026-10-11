# QuickDNS ⚡

[![Download Ready Binary](https://img.shields.io/badge/Download-QuickDNS%20v1.0.0-blue?style=for-the-badge&logo=windows)](https://github.com/szanko02/QuickDNS/releases/latest)
[![GitHub release](https://img.shields.io/github/v/release/szanko02/QuickDNS?style=for-the-badge)](https://github.com/szanko02/QuickDNS/releases/latest)

> 🚀 **[Скачать готовую версию QuickDNS v1.0.0 (Portable ZIP / EXE)](https://github.com/szanko02/QuickDNS/releases/latest)**  
> Приложение не требует сборки или установки: скачайте, запустите и переключайте DNS в 1 клик!

**QuickDNS** — легковесное нативное приложение для Windows (C++20) с графическим интерфейсом для мгновенного переключения DNS-серверов в один клик.

## 🌟 Основные возможности

* **Переключение в 1 клик:** мгновенная смена системного DNS без перезагрузки адаптера или открытия системных параметров Windows.
* **Встроенные пресеты:**
  * **Comss.one DNS** — защита от интернет-цензуры, блокировок и навязчивой рекламы (`92.223.109.31`, `91.107.150.82`).
  * **GeoHide DNS** — быстрый обход региональных и географических ограничений (`193.187.91.189`, `193.187.91.190`).
  * **Без межаў DNS** — свободный независимый DNS-сервер (`185.162.9.228`, `185.162.9.229`).
  * **Cloudflare (1.1.1.1)** — максимальная скорость и приватность (`1.1.1.1`, `1.0.0.1`).
  * **Google Public DNS** — эталонная надежность от Google (`8.8.8.8`, `8.8.4.4`).
  * **Quad9 DNS** — блокировка вредоносного ПО и фишинга (`9.9.9.9`, `149.112.112.112`).
  * **Автоматически (DHCP)** — мгновенный возврат к стандартным DNS вашего роутера / провайдера.
* **Пользовательские профили:** поддержка добавления собственных DNS-серверов через удобный JSON-конфиг (`%APPDATA%\QuickDNS\profiles.json`).
* **Интеграция с системным треем:**
  * Сворачивание приложения в область уведомлений Windows.
  * Контекстное меню в трее для переключения пресетов прямо из панели задач без открытия окна.
  * Всплывающие уведомления Windows при успешной смене DNS.
* **Автоматический сброс кэша:** системный вызов `DnsFlushResolverCache` (`ipconfig /flushdns`) сразу после изменения адресов.
* **Современный нативный интерфейс:**
  * Поддержка темной темы Windows 10/11 (Immersive Dark Mode).
  * Скругленные углы окна (DWM Window Corner Preference).
  * Четкая типографика Segoe UI Variable и поддержка Per-Monitor V2 DPI (высокое разрешение экранов).

---

## 🛠 Стек технологий

* **Язык:** Modern C++20.
* **Система сборки:** CMake 3.20+.
* **Компилятор:** MSVC (Visual Studio 2022) / Clang-cl.
* **Системные Windows API:**
  * `IP Helper API` (`GetAdaptersAddresses`, `SetInterfaceDnsSettings`).
  * `DNS API` (`DnsFlushResolverCache`).
  * `DWM API` (`DwmSetWindowAttribute`).
  * `Common Controls v6` (`InitCommonControlsEx`).
* **Конфигурация:** JSON (`profiles.json`).

---

## 📁 Структура проекта

```
QuickDNS/
├── .github/
│   └── workflows/
│       └── build.yml             # GitHub Actions CI для автоматической сборки
├── include/
│   ├── config_manager.h          # Заголовочный файл управления профилями
│   ├── dns_manager.h             # Заголовочный файл сетевого ядра DNS
│   ├── resource.h                # Идентификаторы ресурсов и контролов
│   └── ui_main.h                 # Заголовочный файл современного GUI и трея
├── res/
│   ├── app.manifest              # UAC requireAdministrator + DPI v2 + ComCtl32 v6
│   ├── default_profiles.json     # Предустановленные профили DNS
│   └── resource.rc               # Ресурсы приложения и метаданные
├── src/
│   ├── config_manager.cpp        # Реализация JSON хранилища и профилей
│   ├── dns_manager.cpp           # Реализация системных сетевых вызовов Windows
│   ├── main.cpp                  # Точка входа WinMain, инициализация COM
│   └── ui_main.cpp               # Реализация графического интерфейса
├── tests/
│   ├── test_config.cpp           # Модульные тесты конфигураций
│   └── test_dns.cpp              # Модульные тесты сетевого ядра
├── .gitignore
├── CMakeLists.txt
└── README.md
```

---

## 🚀 Сборка проекта

### Требования:
* Windows 10 (1809+) или Windows 11.
* Visual Studio 2022 (с установленным компонентом *«Разработка классических приложений на C++»*) или компилятор Clang.
* CMake 3.20+.

### Инструкция по сборке:

```cmd
git clone https://github.com/your-username/QuickDNS.git
cd QuickDNS

mkdir build
cd build

cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

Собранный исполняемый файл будет находиться в `build/Release/QuickDNS.exe`.

---

## 🔒 Права доступа (UAC)

Изменение настроек сетевых интерфейсов в Windows требует административных привилегий. Приложение содержит встроенный манифест (`requireAdministrator`), поэтому при запуске Windows автоматически запросит подтверждение прав.
