### QuickDNS v2.0.5

Feature release adding the dedicated `Без межаў DNS (РФ)` preset for Russian users, multi-IP pool (`ip_pool`) persistence and migration across all profiles, an interactive IP pool selector on the main window, and measured latency pool pickers inside the SmartDNS catalog:

- **Bezmezhau DNS (RF) Preset**: Added dedicated built-in profile `Без межаў DNS (РФ)` (`185.236.10.189`, `31.58.216.201`, DoH `https://sync.bezmezhau.com/dns-query`) and automatic `sync.bezmezhau.com` hostname resolution in `SmartDnsVerifier`.
- **Multi-IP Pool Architecture (`ip_pool`)**: Extended `DnsProfile` and JSON config persistence with `ip_pool` arrays across all SmartDNS and standard presets, automatically migrating existing `profiles.json` files and merging newly verified alive IPs into the pool.
- **Main Window IP Pool Picker**: Added `Пул IP ▾` button and popup selector on the main window IP row to switch Primary/Secondary DNS from the profile's pool, configure single-IP mode, or run one-click latency auto-selection.
- **SmartDNS Catalog IP Pool Picker**: Added pool count badges (`[Пул: N IP]`) on catalog cards and a `Пул IP ▾` picker showing measured ping latencies (`ms`) for every pool address.

**Full Changelog**: https://github.com/szanko02/QuickDNS/compare/v2.0.4...v2.0.5

---

#### Описание изменений (Русский)

- **Профиль Без межаў DNS (РФ)**: Добавлен выделенный встроенный пресет `Без межаў DNS (РФ)` (`185.236.10.189`, `31.58.216.201`, DoH `https://sync.bezmezhau.com/dns-query`) и автоматическое разрешение хоста `sync.bezmezhau.com` в `SmartDnsVerifier`.
- **Архитектура пула IP-адресов (`ip_pool`)**: Структура `DnsProfile` и JSON-конфигурация дополнены массивом `ip_pool` для всех SmartDNS и базовых профилей с автоматической миграцией существующих `profiles.json` и сохранением проверенных рабочих IP.
- **Выбор IP из пула в главном окне**: В строке IP-адресов главного окна добавлена кнопка `Пул IP ▾` с меню быстрого переключения основного и дополнительного DNS из пула, режима одного IP и автоподбора пары с минимальным пингом.
- **Пул IP в каталоге SmartDNS**: На карточки каталога добавлены индикаторы размера пула (`[Пул: N IP]`) и меню `Пул IP ▾` с отображением измеренной задержки (`мс`) для каждого адреса в пуле.
