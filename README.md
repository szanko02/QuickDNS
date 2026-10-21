<div align="center">

<img src="res/app_logo.png" width="128" height="128" alt="QuickDNS Logo" />

# QuickDNS

**Быстрый и нативный переключатель DNS для Windows.**  
*Fast, lightweight and native Windows DNS switcher with instant presets, dark mode & tray support.*

[![GitHub Release](https://img.shields.io/github/v/release/szanko02/QuickDNS?style=for-the-badge&logo=github&color=0078D4)](https://github.com/szanko02/QuickDNS/releases/latest)
[![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6?style=for-the-badge&logo=windows)](https://github.com/szanko02/QuickDNS)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge&logo=c%2B%2B)](https://github.com/szanko02/QuickDNS)
[![Architecture](https://img.shields.io/badge/Arch-x64%20Native-informational?style=for-the-badge)](https://github.com/szanko02/QuickDNS)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg?style=for-the-badge)](LICENSE)

<br/>

### [Скачать релиз v1.6.1 (Portable ZIP / EXE)](https://github.com/szanko02/QuickDNS/releases/latest)
> Не требует установки. Скачайте архив или файл `QuickDNS.exe` и запустите.

</div>

---

## Назначение

В стандартном интерфейсе Windows для смены DNS необходимо открывать цепочку системных меню: «Параметры» → «Сеть и Интернет» → «Свойства адаптера» → «IP версии 4» → вручную вводить адреса → подтверждать изменения → выполнять очистку кэша через командную строку.

QuickDNS автоматизирует этот процесс:
1. Выберите сетевой адаптер и нужный профиль DNS (Comss, GeoHide, Zapret, Cloudflare и др.).
2. Нажмите **«Применить DNS в 1 клик»** — настройки применятся мгновенно, а кэш DNS будет очищен.

---

## Возможности

* **Смена DNS в один клик:** Мгновенное применение настроек к активному сетевому адаптеру (Ethernet / Wi-Fi) без перезагрузки системы.
* **Метки [SmartDNS]:** Визуальная индикация профилей, автоматически проксирующих трафик для разблокировки зарубежных нейросетей (ChatGPT, Claude, Gemini, Copilot), музыки и стримингов (Spotify, YouTube, Deezer, Netflix), сервисов и рабочих платформ (Canva, Notion, Autodesk, JetBrains).
* **Встроенный интерактивный Каталог SmartDNS:** Отдельное окно с подробными карточками провайдеров, категориями сервисов, поисковым фильтром в реальном времени и кнопками прямого перехода на официальные сайты.
* **Копируемые IP и редактируемый DoH:** Текстовые поля IP-адресов и шаблона DNS over HTTPS доступны для выделения и копирования, а также для ручного редактирования пользовательского шаблона DoH.
* **Переход в параметры сети Windows:** Отдельная кнопка прямого вызова системных настроек сети и DHCP Windows в один клик.
* **Мониторинг задержки и график стабильности:** Встроенный sparkline-график в реальном времени с интервалом обновления 3 секунды и историей пинга за последние 60 секунд.
* **Тестирование скорости всех серверов:** Многопоточный замер времени отклика на пуле популярных доменов с градацией качества и графиками sparkline.
* **Предустановленные профили:** Популярные DNS-серверы для ускорения, защиты и обхода региональных ограничений (Comss.one, GeoHide, Без межаў, Zapret, Malw.link, Cloudflare, Google, Quad9, AdGuard).
* **Добавление собственных профилей:** Диалоговое окно для сохранения пользовательских конфигураций с валидацией IP-адресов и поддержкой DoH-ссылок.
* **Управление профилями:** Удаление пользовательских профилей в один клик.
* **Работа в системном трее:** Сворачивание в область уведомлений, контекстное меню быстрого переключения DNS по правому клику без открытия главного окна.
* **Автоматическая очистка кэша DNS:** Вызов системной функции `DnsFlushResolverCache` после каждой смены конфигурации.
* **Нейтральный интерфейс со сглаживанием:** Сбалансированная цветовая гамма, рендеринг GDI+ с субпиксельным сглаживанием (Anti-Aliasing), выверенные отступы между элементами управления.
* **Переключение темной и светлой темы:** Иконка-переключатель в верхнем углу интерфейса с мгновенной сменой палитры, адаптацией заголовка Windows и сохранением выбора в настройках.
* **Анимация кнопок и курсор-указатель:** Плавное наведение с откликом, тактильная анимация нажатия и курсор в виде указательного пальца при наведении на кнопки.
* **Легковесность и автономность:** C++20 и Win32 API, статическая линковка, размер исполняемого файла ~1.8 МБ, потребление памяти ~8 МБ RAM в фоновом режиме.

---

## Предустановленные профили DNS

| Профиль | Тип | Основной DNS (IPv4) | Дополнительный DNS (IPv4) | DNS over HTTPS (DoH) URL | Официальный сайт | Разблокируемые сервисы |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **GeoHide DNS** | `[SmartDNS]` | `193.233.112.67` | `193.233.112.88` | `https://dns.geohide.ru/dns-query` | [geohide.ru](https://geohide.ru/) | ChatGPT, Claude, Gemini, Grok, Cursor, DeepL, Spotify, Notion, Canva, Autodesk, JetBrains |
| **GeoHide EU (Европа)** | `[SmartDNS]` | `45.13.237.213` | `5.175.161.3` | `https://eu.geohide.ru/dns-query` | [geohide.ru](https://geohide.ru/) | Европейские ноды: ChatGPT, Claude, Gemini, Grok, Deezer, Spotify, Canva, Notion |
| **GeoHide US (США)** | `[SmartDNS]` | `192.255.159.241` | `192.3.233.253` | `https://us.geohide.ru/dns-query` | [geohide.ru](https://geohide.ru/) | Серверы США: ChatGPT, Claude, Gemini, Copilot, Grok, Spotify, Netflix US, Notion |
| **Comss.one DNS** | `[SmartDNS]` | `83.220.169.155` | `212.109.195.93` | `https://dns.comss.one/dns-query` | [dns.comss.one](https://dns.comss.one/) | ChatGPT, Copilot, Claude, Spotify, Canva, Notion, Instagram, Twitter/X, AdGuard |
| **Zapret DNS** | `[SmartDNS]` | `83.217.211.149` | — | `https://dns.zapret.moe/dns-query` | [zapret.moe](https://zapret.moe/) | Discord, YouTube, ChatGPT, Claude, Instagram, Twitter/X, RuTracker |
| **Без межаў DNS** | `[SmartDNS]` | `143.20.64.55` | `31.58.216.201` | `https://dns.bezmezhau.com/dns-query` | [bezmezhau.com](https://bezmezhau.com/) | ChatGPT, YouTube, Discord, Spotify, Twitter/X, Instagram, Reddit, TikTok |
| **Malw.link DNS** | `[SmartDNS]` | `179.254.166.171` | `164.5.224.81` | `https://dns.malw.link/dns-query` | [info.dns.malw.link](https://info.dns.malw.link/) | ChatGPT, Gemini, Spotify, Discord, Instagram, Twitter/X, Notion, Canva |
| **Astracat DNS** | `[SmartDNS]` | `135.106.217.200` | `185.139.69.24` | `https://dns.astracat.ru/dns-query` | [astracat.ru](https://astracat.ru/) | ChatGPT, Claude, Discord, YouTube, Spotify, Twitter/X, Instagram, Notion |
| **Mafioznik DNS** | `[SmartDNS]` | `103.27.157.38` | `103.27.157.100` | `https://dns.mafioznik.xyz/dns-query` | [dns.mafioznik.xyz](https://dns.mafioznik.xyz/) | ChatGPT, Discord, YouTube, Steam Community, Spotify, Instagram, Twitter/X |
| **Shecan DNS** | `[SmartDNS]` | `178.22.122.100` | `185.51.200.2` | `https://free.shecan.ir/dns-query` | [shecan.ir](https://shecan.ir/) | Антисанкционный: Docker Hub, Android Dev, Oracle, NVIDIA, Gradle, Google Services |
| **Control D (ControlID)** | `[SmartDNS]` | `76.76.2.0` | `76.76.10.0` | `https://freedns.controld.com/p0` | [controld.com](https://controld.com/) | Anycast SmartDNS от Windscribe: ChatGPT, Claude, стриминги, защита от фишинга |
| **Akile DNS (AKDNS)** | `[SmartDNS]` | `66.66.66.66` | `108.160.138.51` | `https://dns.akile.ai/dns-query` | [dns.akile.ai](https://dns.akile.ai/) | Разблокировка стримингов: Netflix, Disney+, YouTube Premium, Spotify, TikTok |
| **Cloudflare** | Публичный | `1.1.1.1` | `1.0.0.1` | `https://cloudflare-dns.com/dns-query` | [1.1.1.1](https://1.1.1.1/) | Высокая скорость отклика и повышенная приватность |
| **Google Public DNS** | Публичный | `8.8.8.8` | `8.8.4.4` | `https://dns.google/dns-query` | [dns.google](https://dns.google/) | Общедоступный стабильный глобальный DNS-резолвер |
| **Quad9 DNS** | Безопасность | `9.9.9.9` | `149.112.112.112` | `https://dns.quad9.net/dns-query` | [quad9.net](https://quad9.net/) | Блокировка вредоносных и фишинговых ресурсов |
| **AdGuard DNS** | Фильтрация | `94.140.14.14` | `94.140.15.15` | `https://dns.adguard-dns.com/dns-query` | [adguard-dns.io](https://adguard-dns.io/) | Фильтрация рекламы и счетчиков аналитики |
| **Автоматически (DHCP)** | Системный | *Авто* | *Авто* | — | — | Сброс параметров на стандартный DNS роутера или провайдера |

---

## Инструкция по использованию

1. Скачайте [`QuickDNS.exe`](https://github.com/szanko02/QuickDNS/releases/latest) или архив `QuickDNS-v1.6.1-Portable.zip`.
2. Запустите **`QuickDNS.exe`** (при первом запуске подтвердите стандартный запрос UAC для доступа к сетевым параметрам).
3. Программа автоматически выберет ваш активный адаптер (Wi-Fi или Ethernet).
4. Выберите нужный DNS-профиль из выпадающего списка.
5. Нажмите кнопку **«Применить DNS в 1 клик»** — настройки применятся мгновенно.
6. Для быстрого переключения сверните программу в трей: кликните правой кнопкой мыши по значку возле часов и выберите нужный сервер.

---

## Вопросы и ответы (FAQ)

<details>
<summary><b>Нужно ли устанавливать дополнительные библиотеки (.NET, Visual C++ Redistributable)?</b></summary>
Нет. QuickDNS скомпилирован со статической линковкой C++ Runtime (UCRT). В системе не требуется наличие сторонних библиотек или сред выполнения.
</details>

<details>
<summary><b>Зачем программе требуются права Администратора?</b></summary>
Изменение системных параметров сетевых адаптеров в Windows является привилегированной операцией. Без прав Администратора операционная система блокирует вызовы API настройки сетевых интерфейсов. В исполняемый файл встроен манифест <code>requireAdministrator</code>, поэтому запрос UAC появляется автоматически при запуске.
</details>

<details>
<summary><b>Где сохраняются добавленные профили?</b></summary>
Пользовательские профили сохраняются в системной директории пользователя в формате JSON:<br/>
<code>%APPDATA%\QuickDNS\profiles.json</code><br/>
Файл создается автоматически и синхронизируется в потокобезопасном режиме.
</details>

<details>
<summary><b>Как вернуть настройки DNS по умолчанию?</b></summary>
Выберите профиль <b>«Автоматически (DHCP)»</b> и нажмите «Применить DNS в 1 клик». Программа переведет адаптер в автоматический режим получения DNS от вашего роутера или провайдера.
</details>

---

## Архитектура и стек технологий

* **Язык разработки:** C++20 (Win32 API, STL).
* **Сетевые интерфейсы:** Windows **IP Helper API** (`GetAdaptersAddresses`, `SetInterfaceDnsSettings` для Windows 10 1809+ / Windows 11 с автоматическим fallback на `netsh`).
* **Очистка кэша:** Windows **DNS API** (`DnsFlushResolverCache` из `dnsapi.dll`).
* **Графика и стиль:** Windows **DWM API** (`DwmSetWindowAttribute`), **GDI+** с субпиксельным сглаживанием (`SmoothingModeAntiAlias`), **Common Controls v6**.
* **Хранилище настроек:** JSON в `%APPDATA%\QuickDNS\profiles.json` (потокобезопасный `std::mutex`, кодировка UTF-8 / UTF-16).
* **Система сборки:** CMake 3.20+ и скрипт `build.bat` (LLVM-MinGW / MSVC).

---

## Структура проекта

```
QuickDNS/
├── .github/
│   └── workflows/
│       └── build.yml             # Автоматическая сборка в GitHub Actions
├── include/
│   ├── config_manager.h          # Управление профилями и JSON-персистентность
│   ├── dialog_add_profile.h      # Модальное окно добавления своих DNS-профилей
│   ├── dialog_benchmark.h        # Диалог многопоточного тестирования скорости DNS
│   ├── dialog_smartdns.h         # Каталог SmartDNS и разблокируемых сервисов
│   ├── dns_benchmark.h          # Движок замера пинга серверов
│   ├── dns_manager.h             # Сетевое ядро, Win32 IPHlpApi и сброс кэша
│   ├── encoding_utils.h          # Декодирование UTF-8 <-> UTF-16
│   ├── resource.h                # Идентификаторы ресурсов и контролов
│   ├── ui_main.h                 # Архитектура главного окна и системного трея
│   └── ui_palette.h              # Цветовая палитра темной и светлой тем
├── res/
│   ├── app.manifest              # Манифест UAC requireAdministrator + DPI v2
│   ├── app_icon.ico              # Фирменная иконка приложения (16x16 - 256x256)
│   ├── app_logo.png              # Графический прозрачный логотип для документации
│   ├── default_profiles.json     # Предустановленные профили DNS
│   ├── resource.rc               # Ресурсы главного приложения
│   └── resource_diag.rc          # Ресурсы диагностических тестов
├── src/
│   ├── config_manager.cpp        # Реализация JSON хранилища и встроенных пресетов
│   ├── dialog_add_profile.cpp    # Реализация модального диалога добавления профиля
│   ├── dialog_benchmark.cpp      # Реализация окна тестирования DNS
│   ├── dialog_smartdns.cpp       # Реализация каталога SmartDNS с поиском и карточками
│   ├── dns_benchmark.cpp         # Реализация движка замера скорости
│   ├── dns_manager.cpp           # Системные сетевые вызовы Windows
│   ├── main.cpp                  # Точка входа wWinMain, инициализация GDI+ и COM
│   └── ui_main.cpp               # Графический интерфейс, трей, обработка событий
├── tests/
│   ├── test_benchmark.cpp        # Тесты движка бенчмарка DNS
│   ├── test_config.cpp           # Тесты конфигураций и персистентности
│   └── test_dns.cpp              # Тесты адаптеров и сброса кэша DNS
├── .gitignore
├── build.bat                     # Быстрая сборка проекта
├── CMakeLists.txt                # Скрипт сборки CMake
├── LICENSE                       # Лицензия MIT
├── QuickDNS.exe                  # Готовый исполняемый файл
└── README.md                     # Документация проекта
```

---

## Сборка из исходников

### Вариант 1: Быстрая сборка через `build.bat`
```cmd
git clone https://github.com/szanko02/QuickDNS.git
cd QuickDNS
build.bat
```
*Скрипт скомпилирует ресурсы, сетевое ядро, GUI и создаст готовый `QuickDNS.exe`.*

### Вариант 2: Сборка через CMake (Visual Studio 2022)
```cmd
git clone https://github.com/szanko02/QuickDNS.git
cd QuickDNS
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

---

## Права доступа (UAC)

Смена DNS в операционных системах семейства Windows затрагивает общесистемные параметры сетевых адаптеров. В исполняемый файл встроен манифест `requireAdministrator`, поэтому Windows автоматически запрашивает подтверждение запуска с повышенными правами.

---

## Лицензия

Проект распространяется под лицензией **[MIT](LICENSE)**.  
Автор: **Stanislav (sweety)**.  
Вы можете свободно использовать, модифицировать и распространять его.
