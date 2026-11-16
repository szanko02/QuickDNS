### QuickDNS v2.2.0

This release adds experimental controls for routing an application's connections through a selected network adapter and for managing Windows routes to IPv4/IPv6 subnets. Existing DNS profiles, the SmartDNS catalog, benchmarks, and the AI availability checker remain available.

- **Application routing:** Select an EXE or MSIX package, a primary network, and a separate network for the selected application. QuickDNS runs an externally supplied sing-box instance with a TUN interface; one selected rule and session are supported at a time.
- **Subnet routes:** Add and remove session-scoped Windows routes for IPv4 and IPv6 CIDR prefixes with a chosen adapter and optional gateway. Routes normally clear when QuickDNS exits; after an abnormal termination they can remain until Windows restarts.
- **Routing settings:** Save application rules separately from DNS profiles, inspect available adapters, and select an installed MSIX package or executable.
- **Packaging and documentation:** The portable package contains QuickDNS and documentation. sing-box is a separate download and is not included in the ZIP.

Application routing is experimental. Only recognized TCP/UDP connections entering the TUN are split. Adapter DNS addresses are informational; system DNS and traffic outside the TUN can use normal Windows routes. A fail-closed kill switch is not provided. Automated tests do not establish end-to-end behavior on two live networks.

**Full Changelog**: https://github.com/szanko02/QuickDNS/compare/v2.1.0...v2.2.0

---

#### Описание изменений (Русский)

- **Маршрутизация приложений:** экспериментальный выбор сети для EXE или пакета MSIX с помощью отдельно установленного sing-box. Одновременно поддерживается один сеанс и одно выбранное правило.
- **Маршруты по подсетям:** создание и удаление маршрутов Windows по IPv4/IPv6 CIDR с выбором адаптера и, при необходимости, шлюза. При аварийном завершении маршрут может сохраниться до перезагрузки Windows.
- **Настройки и документация:** правила приложений сохраняются отдельно от DNS-профилей; в README описаны настройка и ограничения. sing-box в архив QuickDNS не входит.

Маршрутизация разделяет распознанные TCP/UDP-соединения, попавшие в TUN. Системный DNS и трафик вне TUN могут использовать обычные маршруты Windows. Гарантированная блокировка при отказе выбранной сети не реализована; автоматические тесты не подтверждают работу на двух реальных сетях.
