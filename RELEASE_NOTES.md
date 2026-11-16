### QuickDNS v2.2.2

QuickDNS can now route several applications through the same secondary network adapter in one session. This lets the desktop ChatGPT app and its sign-in browser use the secondary network together.

- **Multiple application rules:** Select several EXE or MSIX rules for the same secondary adapter. The TUN session routes all selected processes through it.
- **Browser sign-in:** Route the desktop app and the browser that opens its sign-in link through the same adapter. Keep a separate browser executable for work traffic on the primary network.
- **Split DNS:** ChatGPT domains use the secondary DNS and other names use the primary DNS when split DNS is enabled.

**Full Changelog**: https://github.com/szanko02/QuickDNS/compare/v2.2.1...v2.2.2

---

#### Описание изменений (Русский)

- **Несколько приложений:** можно выбрать несколько правил EXE или MSIX для одного второго адаптера и запустить один сеанс.
- **Вход в ChatGPT:** приложение и браузер, открывающий страницу входа, могут одновременно использовать вторую сеть. Для корпоративного браузера нужен отдельный EXE.
- **Раздельный DNS:** при включённом режиме домены ChatGPT используют второй DNS, остальные имена — основной.
