### QuickDNS v2.2.4

This preview from the routing branch reorganizes the routing dialog and sends external web traffic through the selected secondary network while the session is active.

- **Routing dialog** — Choose the primary and secondary networks at the top. Add, remove, and save application rules in a scrollable panel. Configure the engine and control the session at the bottom.
- **External web traffic** — HTTP and HTTPS use the secondary network by default. Private addresses and the primary network's corporate DNS suffix use the primary network unless a selected application rule takes precedence.
- **Engine selection** — The compact build lets you enter or browse for sing-box.exe. The embedded build displays the included engine and disables browsing.

#### Описание изменений

- **Окно маршрутизации** — Выбор двух сетей, прокручиваемый список правил с добавлением, удалением и сохранением, настройки движка внизу.
- **Внешние сайты** — При активном сеансе внешние HTTP/HTTPS-сайты автоматически используют вторичную сеть. Локальные адреса и корпоративный DNS-суффикс используют основную сеть с учётом правил приложений.
- **Выбор движка** — В компактной сборке можно указать sing-box.exe. Во встроенной сборке поле доступно только для чтения.

**Full Changelog**: https://github.com/szanko02/QuickDNS/compare/v2.2.3...v2.2.4
