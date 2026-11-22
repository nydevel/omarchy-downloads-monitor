# Downloads for the Omarchy bar

An [Omarchy](https://omarchy.org/) shell plugin that makes background downloads
visible. While anything downloads, an animated arrow appears in the bar; click
it for a popup listing each download with its file, speed and progress.

It exists because plenty of tools download quietly: umu-launcher fetching a
500 MB Proton build before a game installer opens, pacman, Flatpak, Steam,
Lutris, `git clone`, `curl`... With this widget you can see that something is
happening, what it is and how far along it is.

[Русская версия ниже](#по-русски)

## What it detects

| Source | How | Progress |
|---|---|---|
| Steam | app manifests (`appmanifest_*.acf`) | exact %, ETA |
| aria2 | `.aria2` control files | exact %, ETA |
| umu / Proton (Battle.net, Lutris, Heroic...) | growing files of the process tree | bytes + speed |
| curl, wget, git, yt-dlp, pip/uv, npm/pnpm/bun, cargo/rustup, mise, Lutris, Heroic, Flatpak | growing files of the process tree | bytes + speed |
| pacman, yay, paru, pamac | pacman's `alpm` download user | speed estimated from network traffic |
| anything else | add process names in the widget settings | bytes + speed |

A job is shown while its files grow (≥ 96 KB/s, or any growth of a partial
file such as `*.part`) and network traffic matches. Tools that fill
preallocated files or open them only in bursts (Battle.net does both) are
caught by a fallback: the process tree writing ≥ 512 KB/s to disk while the
network receives ≥ 512 KB/s; their speed is then the network rate, marked `≈`. Disk writing straight
after a download (unpacking) is shown as "Unpacking / writing to disk". When a
download that was visible for a few seconds ends, you get a notification.

Everything is read locally from `/proc` and the file system. The plugin makes
no network requests and needs no root. Its helper is a single C++17 file using
only the standard library; on first start `run.sh` compiles it with the
system's `c++` (part of `base-devel`, which every Omarchy install has) into
`~/.cache/omarchy-downloads-monitor/`, and rebuilds it after updates. The
build takes a few seconds, once.

## Install

```bash
omarchy plugin add https://github.com/nydevel/omarchy-downloads-monitor.git --enable
```

or manually:

```bash
git clone https://github.com/nydevel/omarchy-downloads-monitor.git ~/.config/omarchy/plugins/downloads-monitor
omarchy-shell shell rescanPlugins
omarchy plugin enable io.github.nydevel.downloads-monitor
```

Move it with `omarchy bar move io.github.nydevel.downloads-monitor --section right --index 0`.

## Remove

```bash
omarchy plugin remove io.github.nydevel.downloads-monitor
rm -rf ~/.cache/omarchy-downloads-monitor
```

The second line deletes the compiled helper; nothing else is left behind.

## Settings

Set through the bar's widget settings, or `omarchy bar set io.github.nydevel.downloads-monitor <key> <value>`:

| Key | Default | Meaning |
|---|---|---|
| `alwaysShow` | `false` | keep the icon visible when idle |
| `showSpeed` | `false` | show total speed next to the arrow |
| `notifyOnFinish` | `true` | notify when a download finishes |
| `extraProcesses` | `""` | extra process names to watch, comma separated (e.g. `qbittorrent,transmission-gt`) |

## Limits

- pacman downloads as a separate system user, so its files cannot be read
  without root. Its speed is the machine-wide network rate, marked with `≈`.
- Most tools do not reveal the final size of a file while downloading it, so
  progress is shown as bytes downloaded with a moving bar. Exact percentages
  come from Steam and aria2.
- Downloads that stream into memory or into a pipe (no file) are not seen.
- Browsers are not watched: they have their own download UI.

## Development

```bash
sh run.sh --once                   # build if needed, print one snapshot
sh run.sh                          # live JSON lines
sh run.sh --demo                   # fake downloads for UI work
sh run.sh --self-test              # built-in unit checks
tests/test_helper.sh               # helper against real processes (needs jq)
node tests/test_model.mjs          # popup logic
omarchy plugin validate .
```

`downloads.cpp` prints one JSON object per line (`{"v":1,"rx":…,"jobs":[…],"finished":[…]}`);
`Panel.qml` renders it, with all parsing and validation in `Model.js`.
Translations live in `i18n/` (English and Russian so far; pull requests welcome).

## License

MIT

---

## По-русски

Плагин для панели [Omarchy](https://omarchy.org/), который показывает фоновые
загрузки. Пока что-то скачивается, в панели крутится стрелочка. По клику
открывается окно со списком загрузок: программа, файл, скорость и прогресс.

Программа отслеживает Steam, aria2, umu/Proton (Battle.net, Lutris, Heroic),
curl, wget, git, yt-dlp, pip/uv, npm, cargo, mise, Flatpak, pacman, yay и paru.
Другие программы можно добавить в настройке `extraProcesses`. Точный процент
и оставшееся время известны для Steam и aria2. Для остальных показываются
скачанный объём и скорость. Скорость pacman оценивается по общему сетевому
трафику. Когда загрузка заканчивается, приходит уведомление.

Плагин ничего не отправляет в сеть и не требует прав root. Фоновая часть
написана на C++ и при первом запуске сама собирается системным компилятором
(он входит в `base-devel`, который есть в любой установке Omarchy). Сборка
занимает несколько секунд и нужна только один раз.

Установка:

```bash
omarchy plugin add https://github.com/nydevel/omarchy-downloads-monitor.git --enable
```

Удаление:

```bash
omarchy plugin remove io.github.nydevel.downloads-monitor
rm -rf ~/.cache/omarchy-downloads-monitor
```
