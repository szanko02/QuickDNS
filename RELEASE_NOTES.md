### QuickDNS v2.0.3

Maintenance and hardening release delivering code review fixes, JSON update payload parsing via nlohmann/json, enhanced thread-safety across DNS management routines, WinHTTP redirect restrictions, and UI resource protections:

- **Benchmark Stability Verdict Fix**: Fixed jitter comparison in `BenchmarkDialog::update_verdict` (`it.jitter < bestJitter` instead of `bestStableIdx`), ensuring the most stable DNS server is accurately identified.
- **Static DNS Registry Fallback**: Tracked IPv4 and IPv6 registry fallback success in `DnsManager::set_static_dns` and required both protocols to succeed (`!v4_ok || !v6_ok`) to prevent false-positive status reports on partial configuration failure.
- **JSON Update Parser**: Replaced manual string slicing in `AppUpdater::check_github_update` with `nlohmann::json::parse`, properly unescaping changelog `\r\n` sequences and avoiding 400-char URL truncation.
- **Thread-Safe Error Reporting**: Protected `DnsManager::last_error_` across UI and worker threads using `std::mutex`.
- **SmartDNS URL & Redirect Hardening**: Validated `http://` and `https://` schemes before `ShellExecuteW` in `SmartDnsDialog::on_open_website` and enforced `WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP` in `SmartDnsVerifier`.
- **UI Resource & Concurrency Guards**: Added atomic concurrency guard `m_isVerifyingSmartDns` for tray SmartDNS checks, `GlobalFree` cleanup on clipboard lock/set failures, and `GetModuleFileNameW` truncation checks.

**Full Changelog**: https://github.com/szanko02/QuickDNS/compare/v2.0.2...v2.0.3

---

#### Описание изменений (Русский)

- **Исправление вердикта стабильности бенчмарка**: Исправлено сравнение джиттера в `BenchmarkDialog::update_verdict` (`it.jitter < bestJitter` вместо `bestStableIdx`), гарантируя точный выбор наиболее стабильного DNS-сервера.
- **Fallback статического DNS в реестр**: Отслеживание успешности fallback-записи IPv4 и IPv6 в реестр в `DnsManager::set_static_dns` с требованием успешности обоих протоколов (`!v4_ok || !v6_ok`).
- **JSON-парсер обновлений**: Замена ручного парсинга строк в `AppUpdater::check_github_update` на `nlohmann::json::parse` с корректным разэкранированием переводов строк `\r\n` и устранением лимита обрезки URL в 400 символов.
- **Потокобезопасная обработка ошибок**: Защита `DnsManager::last_error_` между потоками UI и фоновыми воркерами с помощью `std::mutex`.
- **Безопасность ссылок SmartDNS и редиректов WinHTTP**: Валидация протоколов `http://` и `https://` перед вызовом `ShellExecuteW` в `SmartDnsDialog::on_open_website` и принудительный запрет небезопасных редиректов HTTPS-to-HTTP (`WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP`) в `SmartDnsVerifier`.
- **Защита ресурсов UI и параллельного доступа**: Атомарный флаг `m_isVerifyingSmartDns` для предотвращения повторных проверок SmartDNS из трея, освобождение памяти `GlobalFree` при ошибках буфера обмена и проверка переполнения буфера в `GetModuleFileNameW`.
