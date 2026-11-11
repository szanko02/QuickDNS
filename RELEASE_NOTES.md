### QuickDNS v2.0.4

Reliability and stability release delivering an auto-updater overhaul, Windows Defender file-lock resilience, atomic replacement retry loops, single-instance process hand-off, and detailed error diagnostics:

- **Defender File-Lock Resilience & Retry Logic**: Implemented retry loops with back-off in `AppUpdater::perform_file_replacement` around `.old` file deletion, module renaming, and downloaded executable placement, preventing transient `ERROR_SHARING_VIOLATION` (32) and `ERROR_ACCESS_DENIED` (5) locks by Windows Defender or antivirus scanners.
- **Atomic Rollback on Replace Failure**: Added safe fallback to `CopyFileW` and guaranteed atomic rollback from `.old` if moving the downloaded executable fails, preventing missing binaries.
- **Single-Instance Race Fix & Process Hand-off**: Introduced `--updated` command-line switch allowing the restarted instance to wait up to 5 seconds for previous window instances and named mutexes to release, preventing immediate exit upon restart.
- **Process Spawn Hardening**: Migrated update restart from `ShellExecuteW` to direct `CreateProcessW` with working directory preservation and `ShellExecuteW` fallback, ensuring elevated token inheritance.
- **Deferred Cleanup of Previous Executables**: Enhanced `AppUpdater::cleanup_previous_update` with retry loops during startup hand-off to reliably remove temporary `.old` binaries once the former process terminates.
- **Detailed Updater Error Diagnostics**: Exposed descriptive error strings with Win32 error codes in `apply_update_and_restart` and `ui_main.cpp` for instant troubleshooting if file system or permission issues occur.

**Full Changelog**: https://github.com/szanko02/QuickDNS/compare/v2.0.3...v2.0.4

---

#### Описание изменений (Русский)

- **Устойчивость к блокировкам Defender и цикл повторов**: Добавлен цикл повторных попыток с задержкой в `AppUpdater::perform_file_replacement` для удаления старых копий, переименования и перемещения загруженного файла, устраняя сбои `ERROR_SHARING_VIOLATION` (32) и `ERROR_ACCESS_DENIED` (5) при сканировании Защитником Windows.
- **Атомарный откат при сбое обновления**: Реализован fallback на `CopyFileW` и гарантированный откат к предыдущему файлу из `.old`, если установка нового бинарного файла не удалась.
- **Устранение гонки единого экземпляра (Single-Instance)**: Добавлен аргумент `--updated`, позволяющий обновленному процессу ожидать до 5 секунд полного завершения работы предыдущего окна и мьютекса перед проверкой единственного экземпляра.
- **Надежный перезапуск процесса**: Переход на прямой запуск через `CreateProcessW` с сохранением рабочего каталога и fallback на `ShellExecuteW`, обеспечивающий наследование прав администратора.
- **Очистка устаревших файлов (.old)**: Функция `AppUpdater::cleanup_previous_update` дополнена циклом повторов для гарантированного удаления `.old` файла после завершения старого процесса.
- **Диагностика ошибок обновления**: В `apply_update_and_restart` и UI добавлены подробные описания ошибок с кодами Win32 API для прозрачной диагностики.
