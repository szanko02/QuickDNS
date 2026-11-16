### QuickDNS v2.2.3

The default Windows x64 executable is compact again. Routing support is available with sing-box next to QuickDNS.exe or with a separate self-contained build.

- **Default executable and updates:** QuickDNS.exe and QuickDNS.exe.sha256 refer to the build without an embedded engine. Automatic updates install that build.
- **Portable ZIP:** Extract both executables into the same folder. QuickDNS preselects the adjacent sing-box.exe when no other engine path has been chosen.
- **Embedded edition:** QuickDNS-Embedded.exe includes sing-box 1.14.4 and has a separate SHA-256 file. Download it manually if you prefer a single executable.
- **Updating from an embedded release:** Before updating, extract sing-box.exe from the new Portable ZIP into the folder containing QuickDNS.exe if you want to continue using routing after the update.

---

#### Описание изменений (Русский)

- **Основная сборка:** QuickDNS.exe без встроенного движка используется и для автообновления.
- **Portable ZIP:** sing-box.exe лежит рядом с QuickDNS.exe и подставляется в поле пути при открытии маршрутизации.
- **Дополнительная сборка:** QuickDNS-Embedded.exe содержит sing-box и скачивается вручную.
- **Переход со встроенной сборки:** перед автообновлением распакуйте sing-box.exe из нового Portable ZIP рядом с вашим QuickDNS.exe, чтобы сохранить возможность запуска маршрутизации.

**Full Changelog**: https://github.com/szanko02/QuickDNS/compare/v2.2.2...v2.2.3
