### QuickDNS v2.1.0

Feature release introducing the AI Services Availability Checker dialog for ChatGPT, Claude, Gemini, and Microsoft Copilot. QuickDNS can now independently test every DNS profile using direct RFC 1035 UDP DNS resolution and non-blocking Schannel TLS probing to detect geo-restrictions, HTTP 403 blocks, and network latency without modifying system DNS settings:

- **AI Services Availability Checker Window (`dialog_ai_check`)**: Added dedicated tool accessible via the new "Проверка ИИ" button on the main window. It executes concurrent availability checks across all DNS profiles for ChatGPT, Claude, Gemini, and Copilot, displaying individual status badges, round-trip latencies, geo-block indicators, and a one-click button to apply the best-performing profile immediately.
- **Direct RFC 1035 UDP DNS Resolver (`AiChecker::resolve_domain_via_dns`)**: Implemented low-level DNS client querying UDP port 53 directly against each profile's DNS server IP, parsing A records through arbitrary CNAME chains with compression pointer safety, avoiding any interference with Windows system resolver cache.
- **Schannel TLS & Geo-Restriction Probing (`AiChecker::check_single_service`)**: Engineered non-blocking TCP and Windows Schannel TLS handshake client with Server Name Indication (SNI) to query target endpoints directly at the resolved IP. Detects HTTP status codes and geo-blocking patterns (OpenAI 403/unsupported_country, Claude geo-block, Cloudflare cdn-cgi trace location, Gemini /faq redirect, Copilot reachability).
- **Client Geo-IP Auto-Detection**: Integrated non-blocking lookup of current client public IP and country code via Cloudflare trace endpoint to accurately determine whether SmartDNS proxying is required or bypassing local ISP blocking.
- **Comprehensive Unit Test Suite (`test_ai_checker`)**: Added 24 automated unit tests verifying wireformat query packing, DNS response parsing, CNAME pointer traversal, truncated buffer guards, and multi-profile latency scoring.

**Full Changelog**: https://github.com/szanko02/QuickDNS/compare/v2.0.5...v2.1.0

---

#### Описание изменений (Русский)

- **Окно проверки доступности ИИ-сервисов (`dialog_ai_check`)**: Добавлен специализированный инструмент, вызываемый новой кнопкой «Проверка ИИ» в главном окне. Выполняет параллельное тестирование доступности нейросетей (ChatGPT, Claude, Gemini, Copilot) по каждому DNS-профилю с выводом детальных статусов, времени отклика, определением геоблокировок и кнопкой мгновенного применения выбранного DNS.
- **Прямой RFC 1035 UDP DNS-резолвер (`AiChecker::resolve_domain_via_dns`)**: Реализован встроенный DNS-клиент, отправляющий прямые UDP-запросы на порт 53 целевого DNS-сервера и парсящий A-записи через цепочки CNAME с поддержкой сжатия имен без изменения системных настроек сети.
- **TLS/HTTPS пробинг через Schannel (`AiChecker::check_single_service`)**: Разработан неблокирующий TCP-клиент с поддержкой TLS-рукопожатия Schannel и SNI для отправки HTTP GET запросов напрямую на полученный IP-адрес. Корректно определяет геоблокировки (OpenAI 403 / unsupported_country, блокировки Claude, редиректы Gemini /faq, доступность Copilot).
- **Определение страны подключения клиента**: Интегрировано определение публичного IP и двухбуквенного кода страны через `cdn-cgi/trace` для точного анализа необходимости обхода региональных ограничений.
- **Набор модульных тестов (`test_ai_checker`)**: Добавлены 24 автоматических теста, проверяющие корректность сборки пакетов DNS, разбор ответов с цепочками CNAME и AAAA, защиту от некорректных буферов и алгоритмы агрегации результатов.
