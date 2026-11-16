### QuickDNS v2.2.1

This Windows x64 release packages the routing engine in QuickDNS.exe and adds optional split DNS for ChatGPT during an application routing session.

- **Split DNS:** ChatGPT domains use the DNS server of the second adapter. Other names use the primary adapter's DNS. The policy applies to the Windows resolver for the lifetime of the TUN session.
- **Portable routing:** The EXE embeds the verified sing-box 1.14.4 engine. Select one EXE or MSIX package per session. A separate browser executable can be selected for ChatGPT on the web.
- **Reliable restart:** Each session uses a new Wintun adapter name, avoiding startup hangs when a previous device is still pending removal.
- **Verification:** Windows live tests exercised two active adapters, selected application routing, DNS selection, and disconnect and recovery. The README documents the two-adapter setup and the limits of TUN routing.

Application routing is experimental. Traffic outside the TUN and app-specific encrypted DNS can follow other paths. Windows resumes normal routing when the engine stops.

**Full Changelog**: https://github.com/szanko02/QuickDNS/compare/v2.2.0...v2.2.1

---

#### Описание изменений (Русский)

- **Раздельный DNS:** домены ChatGPT используют DNS второго адаптера, остальные имена — DNS основного. Правило действует на системный DNS Windows во время сеанса.
- **Portable:** sing-box 1.14.4 встроен в QuickDNS.exe. Можно выбрать одно приложение или отдельный браузер за сеанс.
- **Повторный запуск:** каждый сеанс получает новое имя адаптера Wintun. Устранено зависание при повторном запуске со старым именем.
- **Проверка:** на Windows проверены две активные сети, выбор DNS, отключение второго адаптера и восстановление. Порядок настройки описан в README.
