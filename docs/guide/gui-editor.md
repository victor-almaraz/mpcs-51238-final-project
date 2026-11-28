# The GUI editor

The GUI editor is a desktop application built with Tauri: a web interface in the system's webview, over the client run as its sidecar. The sidecar reads and writes the project's files, compiles it, and talks to the server. [Installing](installing.md#the-gui-editor) describes building it.

The editor opens the project named on its command line (`espresso PROJECT`), else the project open when it last closed. It starts the bundled `espresso-server` with the project as its root, and stops it when the window closes. When another `espresso-server` already listens on the port, the editor plays on that server and leaves it running.

Keys below are written as on Linux. On macOS, ⌘ takes the place of Ctrl in every binding written with Ctrl and a letter, number, or symbol of the CUA scheme; Ctrl+Tab, Ctrl+Shift+Tab, and the nano scheme's keys stay on Ctrl.

## The window

| Part | Holds |
|---|---|
| Top bar | Play, which reads Update while a piece plays, and its ▾ Play Options; Stop; Page, while the playing piece serves a page; the command center, which shows the project's name and opens Go to File; the Side Bar and Panel buttons |
| Banner | Under the top bar, only while the server or the client has stopped: the reason, and Retry or Restart |
| Side bar | The project's files as one list, folders dimmed, the current file highlighted, and the entry file tagged "entry" |
| Tabs | One tab a document, and the page's tab. A dot replaces the close button of a file with unsaved changes. A middle click closes a tab |
| Code | The file, with line numbers, a margin of marks and problems, and fold chevrons |
| Panel | Three views: Console, Problems (with a count), and Search. Its top edge drags to resize it, and Maximize Panel fills the window with it |
| Status bar | The server, the tempo and beat, the plan, the problems, the cursor's line and column, and the key scheme |

F9 shows or hides the side bar and Ctrl+F9 the panel, in every scheme, as do the top bar's buttons and the View menu. In a window 1000 pixels wide or less, the side bar lies over the code and closes when a file opens; at 640 pixels or less, the command center, the buttons' labels, and some status items are hidden. The window keeps the layout across sessions.

The menu bar holds File, Edit, Selection, View, Go, Play, and Help: the system's bar on macOS, a bar in the window on Linux. Each item shows its key in the current scheme when the key is one chord. Right-click, Shift+F10, or the Menu key opens the context menu of the code, the line gutter, a file in the side bar, a tab, the page's tab, the console, or a problem. A few commands are only in context menus: Copy Path, Copy Relative Path, and Open Containing Folder (a file); Reveal in Side Bar, Close Other Editors, and Close Saved Editors (a tab). Show All Commands… (Ctrl+Shift+K) lists every command that applies at the moment, with its key.

## Projects

A project is a folder. File menu commands open one:

- **New Project…** copies a template into a new folder and opens it: Empty piece, MIDI instrument, or Sine tone. The folder is named in the dialog, under the location used last, or `~/Projects` the first time. The folder must not exist, or must be empty.
- **Open Folder…** opens a folder as the project.
- **Open Recent…** lists the last ten projects; Clear Recent Folders empties the list.
- **Open Example…** copies one of the examples into a folder of its own and opens it.
- **Close Folder** closes the project.

Opening another project while a piece plays asks first, then stops the piece.

The entry file, the one Play plays, is `main.lisp`; else `NAME.lisp`, where `NAME` is the project's folder; else the only `.lisp` file at the project's top level other than `project.lisp`. Without an entry file, Play plays the file being edited.

## Files

- **New File…** (side bar, File menu) asks for a path in the project and creates the file.
- **Save**, **Save As…**, and **Save All** write files. Save As writes to a new path in the project and refuses an existing one; the tab becomes the new file.
- **Rename…** (a file's context menu; F2 in the CUA scheme) renames a file, and its marks and history follow it.
- **Move to Trash** (a file's context menu, or Delete in the side bar) closes the file and moves it to the system's trash a few seconds later, after a toast whose Undo keeps it.
- **Close Editor**, **Close All Editors**, and **Reopen Closed Editor** close and reopen tabs. A file with unsaved changes asks: Cancel, Discard, or Save.

The editor watches the project's folder. A file changed by another program reloads when its tab has no unsaved changes; otherwise the tab keeps its changes, and Save asks whether to overwrite the other version or reload it. A file deleted by another program keeps its tab, marked unsaved, and Save writes it again. Quitting or opening another project asks once about every file with unsaved changes.

## Playing

| Command | Effect |
|---|---|
| Play (Ctrl+Shift+Space) | Compiles the project, with the unsaved text of every open `.lisp` file, and plays it |
| Update (Ctrl+Shift+Space) | The same, while a piece plays: the server swaps the new graph in |
| Stop (F6) | Stops the piece, fading it out over the crossfade |
| Evaluate Form | Checks the syntax of the top-level form at the cursor, then compiles and plays the project as Play does; the form's lines flash |
| Evaluate Last Form | The same, from the form that ends at or before the cursor |
| Update on Save | A check item: each save while a piece plays updates it |

The top bar's ▾ Play Options holds the crossfade (No Crossfade, 50 ms, 250 ms, 1 s), the quantize grid (None, Beat, Bar), which delays a swap to the next beat or bar of the server's beat clock, and Update on Save. Preferences holds the same settings.

A compile that fails leaves the playing piece unchanged. Its errors appear in the console and in Problems, and a toast offers to show them: "Not played: N errors", or "Not updated: N errors; the playing plan goes on".

## Pages

A project whose `project.lisp` names a page port serves its page while a piece plays ([Pages](../language.md#pages)). The first time a plan with a page plays, the page opens in a tab, "Page · NAME". When that tab is closed, Play opens it again in front, and Update opens it behind the code. The Page button in the top bar and View → Show Page bring it forward.

The page's tab has Reload, the page's address, Open in Window, and Open in Browser. Dragging the tab out of the tab strip opens the page in a window of its own, for a second screen; the window closes with the editor. Save Page as ui/index.html, in Show All Commands, writes the drawn page into the project as its own page, after asking when one exists.

## The status bar

- **The server.** `● espresso-server · NN%`: the server and its busiest thread's load. It opens a popover of the server's address, thread count, load, missed deadlines, xruns, drops, overflows, and plan. While the server is not connected, the item reads "Connecting…", "Not connected", "espresso-server · No reply", or "espresso-client stopped".
- **Tempo and beat.** `120 BPM · 3.2`: the beat clock's tempo, and the bar and beat, both counted from 1. Each reads `—` while it is unknown.
- **Plan.** "Plan N", or "No Plan".
- **Problems.** The counts of errors and warnings; a click shows Problems.
- **Cursor.** `Ln L, Col C`; a click opens Go to Line.
- **Key scheme.** CUA, Emacs, Vim with its mode (`Vim · NORMAL`), or nano; a click chooses another.

## Console and problems

The console shows the client's and the server's messages, newest last, at most 1000. A message that begins with a place, `path:line:column`, links to it. Filter messages shows only the messages that contain its text. Clear Console empties it.

Problems lists every error and warning that has a place in the project, cleared at each Play. Each problem underlines its token in the code and shows an icon in the margin, with the message on hover. F8 and Shift+F8 go to the next and previous problem in the file.

Marks are places to come back to: Toggle Mark sets or clears one on the cursor's line, as does a click in the marks margin; Next Mark and Previous Mark go round the file; Show Marks… lists every mark of the project; Clear Marks clears the file's. They are in Go → Marks, and kept across sessions.

## Editing

- **Completion** offers, as a word is typed: the names bound around the cursor, the definitions of the open files and the standard library, and the language's forms, builtins, and primitives. After a `:`, it offers the keywords of the call: a primitive's parameters and ports, the `:initarg`s of the class a `make` names, a function's `&key` parameters, and after a choice parameter, its choices. Ctrl+Space asks for it in the CUA and Vim schemes.
- **Folding.** A list, a vector, or a `#| |#` comment over several lines folds from the chevron beside its first line, or with View → Folding: Fold, Unfold, Fold All, and Unfold All.
- **Find and replace.** Find… opens a bar over the code, with Match Case, Match Whole Word, and Regular Expression; Find and Replace… adds a replace row. Find in Files… searches the project, open files as edited, in the panel's Search view.
- **Go to File…** (the command center) finds a file by name. In its field, `name:12` opens the file at line 12, `:12` goes to a line of the current file, `@` lists the current file's definitions, and `>` lists the commands.
- **Go to Definition** (F12) goes to the definition of the name at the cursor, in the open files and then the project's; a name the standard library or the language defines is reported in a toast. Back and Forward return through the places visited.
- **Selection.** Expand Selection and Shrink Selection grow and shrink the selection by the enclosing lists. The Selection menu also adds cursors, selects occurrences, duplicates, moves, and deletes lines, and its Transform submenu joins and sorts lines and changes case.
- **Comments.** Toggle Comment comments the lines with `;`, and Toggle Block Comment the selection with `#| |#`.

## Key schemes

The status bar and Preferences choose the key scheme: CUA (the default), Emacs, Vim, or nano. CUA follows GNOME's standard keys, then VS Code's where GNOME has none. Help → Keyboard Shortcuts (Ctrl+?) lists the keys of the current scheme.

Every scheme has these keys:

| Key | Command |
|---|---|
| Ctrl+Shift+Space | Play or Update |
| F6 | Stop |
| F8, Shift+F8 | Next Problem, Previous Problem |
| F9, Ctrl+F9 | Toggle Side Bar, Toggle Panel |
| F12 | Go to Definition |
| F1 | Language Reference |
| Ctrl+Shift+K | Show All Commands… |
| Ctrl+, | Preferences |
| Ctrl+? | Keyboard Shortcuts |
| Ctrl+F6, Ctrl+Shift+F6 | Move focus to the next or previous part of the window |
| Alt+1 … Alt+9 | Go to tab 1 to 9 (⌘1 to ⌘9 on macOS) |
| Shift+F10, Menu | Context menu |

The schemes differ in the rest:

| Command | CUA | Emacs | Vim | nano |
|---|---|---|---|---|
| Save | Ctrl+S | C-x C-s | `:w` | ^O |
| Open Folder… | Ctrl+O | C-x C-f | `:e` | ^R |
| Close Editor | Ctrl+W | C-x k | `:bd` | M-K |
| Quit | Ctrl+Q | C-x C-c | `:q` | ^X |
| Next, previous editor | Ctrl+Tab, Ctrl+Shift+Tab; Ctrl+Page Down, Ctrl+Page Up | C-x Right, C-x Left | `gt`, `gT`; `:bn`, `:bp` | M-., M-, |
| Go to File… | Ctrl+K | C-x b | `:b` | M-B |
| Show All Commands… | Ctrl+Shift+K | M-x | Ctrl+Shift+K | Ctrl+Shift+K |
| Go to Line… | Ctrl+I | M-g | | M-G |
| Find… | Ctrl+F | C-s | `/` | ^W |
| Find Next, Previous | Ctrl+G, Ctrl+Shift+G; F3, Shift+F3 | | `n`, `N` | M-W, M-Q |
| Undo, Redo | Ctrl+Z, Ctrl+Shift+Z | C-/ | `u`, Ctrl+R | M-U, M-E |
| Stop | Ctrl+. | C-c C-s | `:stop` | F6 |
| Play or Update | Ctrl+Shift+Space | C-c C-c | `:play` | Ctrl+Shift+Space |
| Evaluate Form | Ctrl+Enter | C-M-x | | |
| Evaluate Last Form | Shift+Enter | C-x C-e | `:eval`, Ctrl+X Ctrl+E | M-R |
| Toggle Side Bar | F9 | C-c f | `:filelist` | F9 |
| Toggle Panel | Ctrl+F9 | C-c o | `:console` | Ctrl+F9 |
| Toggle Mark, Show Marks… | Ctrl+D, Ctrl+Alt+D | C-x r m, C-x r l | | |

Vim's `:wq` and `:x` save and quit, and `:q!` quits without saving. nano's editing keys move, mark (M-A), cut (^K), copy (M-6), and paste (^U) as nano's do.

The CUA scheme also has: Ctrl+N New File…, Ctrl+Shift+S Save As…, Ctrl+Alt+S Save All, Ctrl+Alt+O Open Recent…, Ctrl+Shift+W Close All Editors, Ctrl+Shift+T Reopen Closed Editor, F2 Rename…, Ctrl+H Find and Replace…, Ctrl+Shift+F Find in Files…, Ctrl+Shift+M Show Problems, Ctrl+/ Toggle Comment, Ctrl+Alt+/ Toggle Block Comment, Ctrl+Alt+[ and Ctrl+Alt+] Fold and Unfold, Ctrl+= and Ctrl+- zoom the code, Ctrl+0 resets it, F11 Fullscreen, Alt+Left and Alt+Right Back and Forward, Ctrl+B and Ctrl+Shift+B Next and Previous Mark, and the Selection menu's keys shown beside its items.

## Preferences

Preferences (Ctrl+,; Settings… on macOS) apply as they change.

| Group | Setting | Choices | Default |
|---|---|---|---|
| Editor | Key Scheme | CUA, Emacs, Vim, nano | CUA |
| Editor | Interface Size | 100% to 200% | 100% |
| Editor | Word Wrap | On, off | Off |
| Editor | Code Size | 8 to 32 pixels | 14 |
| Play | Crossfade | No Crossfade, 50 ms, 250 ms, 1 s | 50 ms |
| Play | Quantize | None, Beat, Bar | None |
| Play | Update on Save | On, off | Off |

The editor keeps its preferences, layout, recent projects, and marks in the webview's storage; it has no settings file.

## Help

Help → Language Reference (F1) and Help → Standard Library show [the language reference](../language.md) and [the standard library](../library.md) in a tab, with a contents list, a find bar, and a Copy button on each example. Help → Keyboard Shortcuts lists the current scheme's keys, and About espresso gives the client's version, the protocol and IR metadata versions, and the license.

## The command line

The application's executable, `espresso`, chooses by its first argument:

| Command | Runs |
|---|---|
| `espresso` | The editor, on the project open last |
| `espresso PROJECT` | The editor, on the folder `PROJECT` |
| `espresso --headless [server options]` | The server alone, with `espresso-server`'s options ([Playing](playing.md#the-server)) |
| `espresso --headless --play PIECE [options]` | A piece live, without the editor, until Ctrl+C |
| `espresso --headless --render PIECE --out FILE.wav [options]` | A piece offline to a WAV file |
| `espresso --tui [client options]` | The [terminal editor](terminal-editor.md) |
| `espresso --help` | The usage |

`PIECE` is IR (an `.ir.json` file, or `-` for standard input), a `.lisp` entry file, or a project folder, whose entry file follows the rule of Projects. A `.lisp` piece is compiled by the bundled client and given to the server, with its folder as `--root` unless one is named. Ctrl+C and SIGTERM pass to the server or the terminal editor, and the command exits with its status, or 128 plus the signal's number. A command line it refuses exits with status 2; a piece that does not compile, or an entry file it cannot find, with status 1.

## The sidecar

```bash
build/bin/espresso-client --serve [--port N]
```

Runs the client without the terminal editor, for a graphical editor that starts it as a child process. The client reads JSON-RPC 2.0 requests from standard input and writes responses and notifications to standard output, one message per line, and reaches the server on `127.0.0.1:N` as the terminal editor does. The methods are `hello`, `project.open`, `project.files`, `file.read`, `file.stat`, `file.write`, `file.create` (an empty file, and the folders above it), `file.rename`, `run.play`, `run.eval`, `run.stop`, `server.connect`, `page.render`, `highlight`, `language`, and `shutdown`; the notifications are `console` and `status`. Paths are relative to the project root that `project.open` names. After `shutdown`, or at the end of standard input, the client finishes the run requests already read and exits with status 0.
