# ScreenFrame

Нативная утилита для скриншотов на C++ и Qt Widgets. Работает из окна, системного трея и командной строки.

## Запуск

- **Windows 10/11 x64.** Распакуйте ZIP и откройте `bin/ScreenFrameGUI.exe`. Рядом находится CLI `bin/screenframe.exe`.
- **macOS 14.4+, Apple Silicon.** Перетащите `ScreenFrame.app` из DMG в «Программы». При первом захвате разрешите запись экрана в «Системные настройки» → «Конфиденциальность и безопасность», затем перезапустите приложение. У локальной сборки подпись ad hoc.
- **Ubuntu 24.04+ amd64.** Установите `sudo apt install ./screenframe_0.1.0_amd64.deb` и откройте ScreenFrame из меню приложений.

| Действие | Windows и Ubuntu | macOS |
| --- | --- | --- |
| Снять весь экран | `Ctrl` + `Alt` + `Shift` + `1` | `⌘` + `⌥` + `⇧` + `1` |
| Выбрать область | `Ctrl` + `Alt` + `Shift` + `2` | `⌘` + `⌥` + `⇧` + `2` |

Горячие клавиши работают, пока ScreenFrame запущен. Захват всего экрана на Windows, macOS и X11 снимает монитор под указателем. Для области перетащите мышь по замороженному изображению. `Esc` отменяет выбор.

В редакторе доступны стрелка (`A`), перо (`P`), рамка (`R`), размытие (`B`) и непрозрачная заливка (`X`). Цвет и толщину можно менять в панели инструментов. Сохранение и копирование применяют правки к пикселям изображения. Для конфиденциальных данных используйте непрозрачную заливку.

### Ubuntu с Wayland

Системный портал может запросить подтверждение захвата. Снимок всего экрана охватывает рабочий стол, возвращённый порталом. Для выбора области ScreenFrame показывает этот снимок на одном дисплее. GNOME может сохранить исходный снимок в «Изображениях».

Поддержка глобальных клавиш зависит от среды рабочего стола. Если ScreenFrame сообщает, что сочетания недоступны, в настройках клавиатуры создайте два пользовательских сочетания для `screenframe capture full` и `screenframe capture region`. Это позволяет запускать захват и без открытого ScreenFrame.

## Командная строка

```sh
screenframe --help
screenframe --tray
screenframe capture full
screenframe capture region --output area.png
screenframe capture full --output screenshot.png --no-preview
```

В Windows используйте `screenframe.exe`, в macOS — `/Applications/ScreenFrame.app/Contents/MacOS/ScreenFrame`. Захват запускается в графическом сеансе. `--output` записывает исходный снимок сразу после захвата. Правки можно сохранить из редактора. `--no-preview` завершает программу после записи. Код выхода `0` означает успех, `1` — ошибку, `2` — отмену захвата.

## Сборка

Нужны CMake 3.21.1+, компилятор C++20 и Qt 6.4+ Widgets. На Windows рекомендуются MSVC и Qt 6.8+. Для сборки macOS нужны Xcode и Qt для macOS.

На Ubuntu установите зависимости:

```sh
sudo apt install build-essential cmake ninja-build qt6-base-dev libx11-dev libxcb1-dev qt6-wayland
```

Соберите и запустите тесты. Для Qt вне системных путей укажите `-DCMAKE_PREFIX_PATH=/path/to/Qt`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`cmake --install build --prefix <абсолютный-путь>` собирает каталог приложения с Qt на Windows и macOS. На Ubuntu `cpack --config build/CPackConfig.cmake -B dist` создаёт DEB.
