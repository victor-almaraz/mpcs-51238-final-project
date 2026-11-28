# The terminal editor

The terminal editor is the client run in a terminal, without the GUI editor. It connects to a server started separately, by default on `127.0.0.1:57190`.

```bash
build/bin/espresso-client [--settings FILE] [--port N] [PROJECT_DIR [FILE...]]
```

The editor opens `PROJECT_DIR`, by default the current directory, and each `FILE`, by its path in the project; without a `FILE`, it opens the project's `main.lisp` when there is one, else an empty `*scratch*` buffer. `--settings FILE` reads and writes the settings in `FILE` ([Settings](#settings)), and `--port N` reaches the server on port `N`, from 1 to 65535. A `PROJECT_DIR` that is not a directory, or an unknown option, exits with status 2; `--help` prints the usage and `--version` the version. Quitting exits with status 0.

## The screen

| Part | Holds |
|---|---|
| Header bar | ▶ Play and ■ Stop; the current file's name, with `•` while it has unsaved changes, and the project's folder; the Fade and Quantize fields; ≡, the main menu; ×, which quits |
| File list | The project's files as one list of paths; a click opens one. Its divider drags to resize it |
| Tabs | One tab a buffer, `•` after the name of one with unsaved changes; × closes a tab and + opens a file |
| Code | The buffer, with line numbers, syntax highlighting, and the bracket that matches the one at the cursor in bold |
| Console | The messages of the client and the server. Its heading drags to resize it, and its Clear button empties it |
| Status bar | The server, the beat clock, the plan, the counters, the key scheme, and the cursor's line and column |
| Message line | The prompt, when one is open; otherwise the last message, or the tooltip of the control under the pointer |

F7 shows or hides the file list and F9 the console, in every scheme; so do the main menu's check items and Preferences. Below 72 columns, the file list is hidden. The file list skips hidden files and folders and folders named `build`; a symbolic link is listed under its own name, and a link to a folder that contains it is not followed. The list is read when the editor starts and after each save.

The mouse reaches every command of the menus: ≡ (or F10) opens the main menu, and a right-click in the code (or Shift+F10) the context menu. A click places the cursor and a drag selects; the wheel scrolls.

| Menu | Items |
|---|---|
| Main menu | Open File…, Switch Buffer…, Save, Save As…, Close Tab; Find…, Evaluate Form; Show File List, Show Console; Preferences, Keyboard Shortcuts, About espresso |
| Context menu | Undo, Redo; Cut, Copy, Paste, Select All; Find…, Find Backward…, Find Next, Find Previous; Evaluate Form |

F1 lists the keyboard shortcuts of the current scheme. Each menu item shows its key.

Controls show their state. A control under the pointer is drawn in its hover color, and a control held down by the pointer in its pressed color. A divider under the pointer or being dragged is drawn in the accent color. In a menu, the item under the pointer takes the focus. A control that has nothing to act on, such as Stop before a plan plays, or Undo with no change to undo, is drawn in the secondary color and ignores presses. Its tooltip names it as for an enabled control.

## Files

Open File… prompts for a path, with the project's files as completions (Tab completes the longest common prefix); an open file is switched to, and a path that does not exist opens an empty buffer that the first save creates. Switch Buffer… prompts for an open buffer's name. A file that cannot be read, or is not UTF-8, is refused, with the reason in the console.

Save writes the file in one step: a temporary file beside it, then a rename, so an interrupted save leaves the old file whole. A file's permissions are kept, and a symbolic link stays a link to the file it names, whose contents change. Save does not create missing folders. Save As… prompts for a path and writes the buffer there, replacing a file of that name. The editor does not watch for changes other programs make to open files.

Closing a tab with unsaved changes asks: Cancel (Esc), Discard (D), or Save (Enter). Quitting asks about each buffer with unsaved changes in turn; Vim's `:q!` quits without asking.

## Playing

| Command | Effect |
|---|---|
| Play (▶, F5) | Compiles the project and plays it, or updates the playing piece |
| Stop (■, F6) | Stops the piece, fading it out over the crossfade |
| Evaluate Form (F8) | Checks the syntax of the form that ends at or before the cursor, then compiles and plays the project as Play does |

Play compiles the project's `.lisp` files, each open file's unsaved text in place of its file. The entry file is `main.lisp`; else the file being edited; else the only `.lisp` file other than `project.lisp`. A compile that fails leaves the playing piece unchanged; its errors appear in the console, which opens, and the message line reads `Errors found — see the console`. A console message with a place in the project, such as an error, a warning, or a `called from` line, is underlined, and a click opens the file at that line and column.

The header's Fade field sets the crossfade of a swap: a click prompts for a duration, such as `50ms` (a plain number is in milliseconds), and the wheel changes it by 10 ms. The Quantize field delays a swap to the next beat or bar of the server's beat clock: None, Beat, or Bar. With Rebuild on Save, each save plays the project.

At startup the client sends the server `/hello`. While the server does not answer, the client tries again every second, in the background; the console reports the first failure and the connection, and nothing for the attempts between them. A server that sends nothing for 1.5 seconds is reported lost, and the client tries it again the same way.

## The status bar

The status bar shows, from the left:

- `● NAME`, the server, while connected; `○ Not connected` otherwise.
- The tempo of the server's beat clock, rounded to a tenth (`120 BPM`, `120.5 BPM`), or `— BPM` while it is unknown.
- `Beat B.N`: the bar and the beat within it, both counted from 1, in bars of the active plan's meter. `Beat 9.3` is the third beat of bar 9. It reads `—` while no plan is active or the server is not connected.
- While connected, `Plan N`, or `Plan —` with no active plan, and `Load NN%`, the busiest thread's load, with `Threads N` when the server runs more than one.
- `Missed`, `Xruns`, and `Drops`: the server's missed deadlines, device xruns, and dropped messages.
- At the right, the key scheme, with Vim's mode (`Vim Normal`), and `Ln L, Col C`.

The server reports these values ten times a second. When the bar is too narrow for them all, it shows the connection, the key scheme, and the cursor alone.

## The console

The console keeps the newest 1000 messages: information, errors (marked `✕`), and the server's messages. A message longer than the pane's width wraps onto further rows, indented under its first. The wheel scrolls the console by rows, and a new message scrolls it to the end. Clear empties it.

## Editing

- **Search.** Find… prompts and searches forward as the text is typed, for that text exactly, case included, wrapping at the end; Find Backward… searches backward. While the prompt is open, the scheme's search keys go to the next and previous match. Enter keeps the match; Esc returns to where the search started. Find Next and Find Previous search again for the last text kept.
- **Selection.** Shift with a movement key selects, as does a drag, Set Mark followed by movement, or Vim's visual mode. Typing replaces the selection. Cut and Copy with no selection take the current line.
- **Clipboard.** The editor's clipboard is its own, separate from the system's.
- **Undo and redo** are kept for each buffer; a run of typed characters is one step.
- **Tab** inserts two spaces.

## Key schemes

The key scheme is set in Preferences or in the settings file: `emacs` (the default), `cua`, `vim`, or `nano`. In every scheme, the arrow keys, Home, End, Page Up, and Page Down move, with Shift to select; Ctrl with the arrows moves by words, and Ctrl+Home and Ctrl+End to the ends of the buffer; Delete deletes forward.

| Key | Command, in every scheme |
|---|---|
| F1 | Keyboard Shortcuts |
| F5, F6 | Play, Stop |
| F7, F9 | Show File List, Show Console |
| F8 | Evaluate Form |
| F10, Shift+F10 | Main menu, context menu |
| F12 | Save As… |
| Alt+Right, Alt+Left | Next tab, previous tab |

| Command | emacs | cua | vim | nano |
|---|---|---|---|---|
| Open File… | C-x C-f | Ctrl+O | `:e` | Ctrl+R |
| Switch Buffer… | C-x b | Ctrl+P | `:b` | Alt+B |
| Save | C-x C-s | Ctrl+S | `:w` | Ctrl+O |
| Save As… | C-x C-w | F12 | `:saveas` | Alt+O |
| Close Tab | C-x k | Ctrl+W | `:bd` | Alt+K |
| Quit | C-x C-c | Ctrl+Q | `:q` | Ctrl+X |
| Next, previous tab | C-x Right, C-x Left | Ctrl+Page Down, Ctrl+Page Up | `gt`, `gT`; `:bn`, `:bp` | Alt+., Alt+, |
| Undo, Redo | C-/, C-M-_ | Ctrl+Z, Ctrl+Y | `u`, Ctrl+R | Alt+U, Alt+E |
| Cut, Copy, Paste | C-w, M-w, C-y | Ctrl+X, Ctrl+C, Ctrl+V | `d`, `y`, `p` (visual) | Ctrl+K, Alt+6, Ctrl+U |
| Select All | C-x h | Ctrl+A | | |
| Set Mark | C-SPC | | | Alt+A |
| Find…, Find Backward… | C-s, C-r | Ctrl+F, Ctrl+R | `/`, `?` | Ctrl+W, Ctrl+Q |
| Find Next, Previous | | F3, Shift+F3 | `n`, `N` | Alt+W, Alt+Q |
| Play, Stop | C-c C-c, C-c C-s | F5, F6 | `:play`, `:stop` | F5, F6 |
| Evaluate Form | C-x C-e | Ctrl+E | `:eval`, Ctrl+X Ctrl+E | Alt+R |
| Show File List, Console | C-c f, C-c o | F7, F9 | `:filelist`, `:console` | F7, F9 |
| Preferences | C-c , | | `:set` | |
| Cancel | C-g | Esc | Esc | Ctrl+C |

The emacs scheme also moves as Emacs does (C-f, C-b, C-n, C-p, M-f, M-b, C-a, C-e, M-m, C-v, M-v, M-<, M->) and kills to the end of the line with C-k. The vim scheme starts in normal mode, with Vim's movement keys, `i`, `a`, `A`, `I`, `o`, and `O` to insert, `x`, `X`, `dd`, `D`, `yy`, `p`, and `J` to edit, and `:wq` and `:x` to save and quit. The nano scheme moves as nano does (Ctrl+F, Ctrl+B, Ctrl+N, Ctrl+P, Ctrl+A, Ctrl+E, Ctrl+V, Ctrl+Y, Alt+\, Alt+/).

## Preferences

Preferences, in the main menu, apply as they change and write the settings file.

| Group | Setting | Settings key |
|---|---|---|
| Appearance | Theme | `:theme` |
| Keyboard | Key Bindings | `:keys` |
| Playback | Fade: the crossfade, in steps of 10 ms | `:fade` |
| Playback | Quantize | `:quant` |
| Playback | Rebuild on Save | `:rebuild-on-save` |
| Layout | Show File List | `:file-list` |
| Layout | Show Console | `:console` |

## Settings

The settings file is `$XDG_CONFIG_HOME/espresso/settings.lisp`, or `~/.config/espresso/settings.lisp` when `XDG_CONFIG_HOME` is not an absolute path, or the file `--settings` names. It holds one `settings` form, which the editor reads and never evaluates:

```lisp
(settings
  :keys            :emacs   ; :emacs, :cua, :vim, or :nano
  :file-list       #t
  :console         #t
  :fade            50ms     ; a time from 0; a plain number is in seconds
  :quant           :none    ; :none, :beat, or :bar
  :rebuild-on-save #f       ; #t plays the project after each save
  :theme           :terminal
  :file-list-width 24       ; columns, 1 to 1000
  :console-height  8)       ; rows, 1 to 1000
```

The values shown are the defaults. The editor writes the file, and the folders above it, when a setting changes; the rewrite keeps the values and drops comments. A setting with a value it does not take keeps its default, and the console names the file, line, and column; a file that does not read leaves every setting at its default.

## Themes

| Theme | Setting |
|---|---|
| Terminal (default): the terminal's own palette | `:terminal` |
| Adwaita Dark | `:adwaita-dark` |
| Adwaita Light | `:adwaita-light` |
| Solarized Dark | `:solarized-dark` |
| Solarized Light | `:solarized-light` |
| Nord | `:nord` |
| High Contrast | `:high-contrast` |

Every theme assigns colors to the same roles: window, secondary text, rules, raised surfaces, fields, accent, destructive actions, selection, control states, status, and the ten syntax categories. Truecolor themes keep at least 4.5:1 contrast for body text and 3:1 for secondary text, syntax, and status colors, with 7:1 for body text and syntax in High Contrast, and fall back to 256 or 16 colors on terminals without truecolor support. An unknown theme name leaves the Terminal theme in use, and the console lists the themes.
