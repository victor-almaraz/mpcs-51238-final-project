# The terminal editor

The terminal editor is the client run in a terminal, without the GUI editor.

```bash
build/bin/espresso-client [--settings FILE] [--port N] [PROJECT_DIR [FILE...]]
```

The editor opens `PROJECT_DIR` (default: the current directory) and its `main.lisp`, if present. The file list shows the project's files, except hidden files and `build` directories. A symbolic link is listed under its own name, and a linked directory is listed like a directory; a link to a directory that contains it is not followed. Saving a linked file replaces the file the link names and keeps the link. Every command is reachable with the mouse: run controls in the header bar, file actions and dialogs in the main menu (≡, or F10), and editing actions in the context menu (right-click, or Shift+F10). F1 lists the keyboard shortcuts of the selected scheme. Key bindings, fade, quantization, and pane visibility are set in Preferences and apply immediately. Key bindings follow the scheme selected in `$XDG_CONFIG_HOME/espresso/settings.lisp` (default `~/.config/espresso/settings.lisp`):

```lisp
(settings
  :keys            :emacs   ; :emacs, :cua, :vim, or :nano
  :file-list       #t
  :console         #t
  :fade            50ms
  :quant           :none    ; :none, :beat, or :bar
  :rebuild-on-save #f
  :theme           :terminal
  :file-list-width 24       ; columns, 1 to 1000
  :console-height  8)       ; rows, 1 to 1000
```

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

Every theme assigns colors to the same roles: window, secondary text, rules, raised surfaces, fields, accent, destructive actions, selection, control states, status, and the ten syntax categories. Truecolor themes keep at least 4.5:1 contrast for body text and 3:1 for secondary text, syntax, and status colors, with 7:1 for body text and syntax in High Contrast, and fall back to 256 or 16 colors on terminals without truecolor support. The theme can be changed in Preferences; the change applies immediately.

Controls show their state. A control under the pointer is drawn in its hover color, and a control held down by the pointer in its pressed color. A divider under the pointer or being dragged is drawn in the accent color. In a menu, the item under the pointer takes the focus. A control that has nothing to act on, such as Stop before a plan plays, or Undo with no change to undo, is drawn in the secondary color and ignores presses. Its tooltip names it as for an enabled control.

The × button in the header bar quits, as does each scheme's quit key: `C-x C-c` in `emacs`, `C-q` in `cua`, `:q` in `vim`, and `C-x` in `nano`. In every scheme, F5 plays, F6 stops, and F8 evaluates the form before the cursor.

Play compiles the project's `main.lisp` with unsaved edits (without one, the file being edited), submits the result to the server, and reports compile errors in the console; clicking an error moves the cursor to it. A console message longer than the pane's width wraps onto further rows, indented under its first, and the wheel scrolls the console by rows. Evaluate Form compiles the project the same way. A failed compilation leaves the running piece unchanged.

The status bar shows the server connection, the tempo of the server's beat clock to one decimal place, the beat position, the active plan, and the missed-deadline, xrun, and drop counters. The beat position is the bar and the beat within it, both counted from 1: `Beat 9.3` is the third beat of bar 9, with bars of the active plan's meter. The server reports the tempo and the beat position with its counters, ten times per second. The beat position reads `—` while no plan is active, and the tempo and the beat position read `—` while the server is not connected.
