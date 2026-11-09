### QuickDNS v2.0.2

Security and architecture release introducing cryptographic update verification, native IPv6 DNS configuration, strict input validation, and thread-safety hardening:

- **Cryptographic Update Verification**: Enforces SHA-256 checksum verification of downloaded updates using native Windows Cryptography API: Next Generation (BCrypt) before execution. Update downloads are strictly routed over HTTPS via WinHTTP with TLS validation.
- **SmartDNS GitHub Rate-Limit Resiliency**: Automatic fallback to direct HTTP HEAD redirect resolution on GitHub Releases when shared SmartDNS proxy endpoints encounter GitHub API rate limits (HTTP 403).
- **DoH Command Injection Protection**: Strict URL template validation restricting DNS-over-HTTPS templates to valid HTTPS protocols and authorized character sets, preventing command and argument injection.
- **Static IPv6 DNS Configuration**: Full support for IPv6 DNS address configuration alongside IPv4 via the modern Windows `SetInterfaceDnsSettings` API with cascading fallback to `netsh`.
- **Robust Multi-Threading Architecture**: Eliminated detached background threads and race conditions during rapid adapter switching; background DNS ping and verification tasks are centrally tracked and joined on application exit.
- **Atomic Configuration Storage**: Upgraded configuration persistence to `nlohmann/json` with transactional atomic replacement (`ReplaceFileW`), migration to `%APPDATA%\QuickDNS\profiles.json`, and automated corrupted-state recovery.
- **Core Engine & In-Process Cache Flushing**: In-process DNS cache flushing via Windows `dnsapi.dll` (`DnsFlushResolverCache`) replacing legacy external process invocation, paired with thread-safe rotating file logging at `%LOCALAPPDATA%\QuickDNS\quickdns.log`.
- **Zero-Dependency Portable Distribution**: Statically linked Universal C Runtime (UCRT) and C++ standard library for true standalone operation without external Visual C++ Redistributable dependencies.

**Full Changelog**: https://github.com/szanko02/QuickDNS/compare/v2.0.1...v2.0.2

---

#### Описание изменений (Русский)

- **Безопасность DoH и обновлений**: Строгая валидация URL-шаблонов DoH для защиты от внедрения команд, загрузка обновлений исключительно по HTTPS через WinHTTP с валидацией TLS и сверкой контрольной суммы SHA-256 (Windows BCrypt API).
- **Устойчивость обновления к rate limit**: Автоматический fallback на прямой HEAD-запрос GitHub при ограничениях API (HTTP 403 на общих SmartDNS-прокси).
- **Поддержка статического IPv6 DNS**: Настройка адресов IPv6 DNS параллельно с IPv4 через современный Windows `SetInterfaceDnsSettings` API с каскадным fallback на `netsh`.
- **Надежная многопоточность**: Устранены detached-потоки и гонки данных при переключении адаптеров, фоновые сетевые операции управляются централизованно.
- **Атомарное хранилище конфигураций**: Переход на nlohmann/json с атомарной перезаписью файлов (`ReplaceFileW`), хранением в `%APPDATA%\QuickDNS\profiles.json` и автоматическим восстановлением поврежденных файлов.
- **Оптимизация сетевого ядра и логирование**: Сброс кэша DNS через in-process `dnsapi.dll`, устранение избыточных вызовов внешних процессов, потокобезопасное журналирование в `%LOCALAPPDATA%\QuickDNS\quickdns.log` с ротацией.
- **Полная автономность (Portable)**: Статическая линковка UCRT и C++ runtime — запуск без внешних зависимостей.
