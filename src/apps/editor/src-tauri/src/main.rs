// The bundle's entry point (editor-refactor.md §4h.3, headless.rs): the GUI
// editor (tauri-editor.md §3), or headless mode, or the terminal editor. The
// editor is Tauri, with the shell plugin through which the web interface
// starts the client as its sidecar (§4), the dialog plugin that chooses a
// project folder, the opener that shows a file in the file manager, the
// server the editor starts and stops (server.rs, §4h.2), Move to Trash
// (trash.rs), the project's watcher (watch.rs), and the templates and
// examples New Project and Open Example copy (projects.rs).

mod headless;
mod projects;
mod server;
mod trash;
mod watch;

use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use headless::Launch;
use server::{bundled, Server, Started};
use tauri::{Emitter, Manager, RunEvent, WindowEvent};
use watch::{ProjectWatcher, Touched};

/// Starts the bundled server with the project `root`, or keeps the one
/// running with that root, and answers once it listens or has ended. The
/// web interface calls it at start, when a project opens, and from Retry.
#[tauri::command]
async fn server_start(
    server: tauri::State<'_, Arc<Server>>,
    root: Option<String>,
) -> Result<Started, String> {
    let server = Arc::clone(&server);
    tauri::async_runtime::spawn_blocking(move || server.start(root.map(PathBuf::from)))
        .await
        .map_err(|e| e.to_string())?
        .map_err(|e| format!("espresso-server could not start: {e}"))
}

/// The project named on the command line, made absolute, or none.
struct LaunchProject(Option<String>);

/// Moves `path`, in the project at `root`, to the platform's trash (trash.rs).
#[tauri::command]
async fn file_trash(root: String, path: String) -> Result<(), String> {
    trash::trash(Path::new(&root), &path)
}

/// A batch of the project's changes, as the page receives it: the project's
/// root, as the watcher was given it, and the paths touched, or none when
/// changes were missed.
#[derive(Clone, serde::Serialize)]
struct ProjectChanged {
    root: String,
    paths: Option<Vec<Touched>>,
}

/// Watches the project at `root` (watch.rs) in place of the one watched; the
/// paths a change touched reach the page as a "project-changed" event. No
/// root stops the watcher. Async, so a large tree's first walk does not
/// hold the window's thread.
#[tauri::command]
async fn project_watch(
    app: tauri::AppHandle,
    watcher: tauri::State<'_, ProjectWatcher>,
    root: Option<String>,
) -> Result<(), String> {
    let named = root.clone().unwrap_or_default();
    watcher.watch(root.as_deref().map(Path::new), move |paths| {
        let _ = app.emit(
            "project-changed",
            ProjectChanged {
                root: named.clone(),
                paths,
            },
        );
    })
}

/// The bundled "templates" or "examples" (projects.rs), each with its description.
#[tauri::command]
async fn project_list(
    app: tauri::AppHandle,
    kind: String,
) -> Result<Vec<projects::Listed>, String> {
    projects::listed(&bundled_projects(&app, &kind)?)
}

/// Copies the bundled template or example `name` to the new folder `to`, and answers `to`.
#[tauri::command]
async fn project_copy(
    app: tauri::AppHandle,
    kind: String,
    name: String,
    to: String,
) -> Result<String, String> {
    let from = projects::member(&bundled_projects(&app, &kind)?, &name)?;
    projects::copy_project(&from, Path::new(&to))?;
    Ok(to)
}

// The folder of the bundle's templates or examples (bundle.resources).
fn bundled_projects(app: &tauri::AppHandle, kind: &str) -> Result<PathBuf, String> {
    if kind != "templates" && kind != "examples" {
        return Err(format!("no bundled {kind}"));
    }
    let resources = app.path().resource_dir().map_err(|e| e.to_string())?;
    Ok(resources.join(kind))
}

/// The project the editor was started on (`espresso PROJECT`), which it opens in place of the last one.
#[tauri::command]
fn launch_project(project: tauri::State<'_, LaunchProject>) -> Option<String> {
    project.0.clone()
}

fn main() {
    // macOS gives an application opened from the Finder a -psn_ argument on older systems.
    let args: Vec<OsString> = std::env::args_os()
        .skip(1)
        .filter(|a| !a.to_string_lossy().starts_with("-psn_"))
        .collect();
    let project = match headless::parse(&args) {
        Launch::Editor { project } => project,
        Launch::Headless(args) => std::process::exit(headless_mode(&args)),
        Launch::Tui(args) => {
            std::process::exit(headless::run(&bundled("espresso-client"), &args, None))
        }
        Launch::Help => {
            print!("{}", headless::USAGE);
            return;
        }
        Launch::Refused(message) => {
            eprint!("espresso: {message}\n{}", headless::USAGE);
            std::process::exit(2);
        }
    };
    let project = project
        .and_then(|p| p.canonicalize().ok())
        .map(|p| p.to_string_lossy().into_owned());
    // Before any thread starts, so that every thread inherits the mask.
    #[cfg(unix)]
    let signals = block_stop_signals();
    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .setup(move |app| {
            let binary = bundled("espresso-server");
            let lines = app.handle().clone();
            let exits = app.handle().clone();
            // The server's lines and its unasked-for end go to the page as events.
            app.manage(Arc::new(Server::new(
                binary,
                None,
                move |line| {
                    let _ = lines.emit("server-line", line);
                },
                move |exit| {
                    let _ = exits.emit("server-exit", exit);
                },
            )));
            #[cfg(unix)]
            exit_on_stop_signals(app.handle().clone(), signals);
            Ok(())
        })
        .manage(LaunchProject(project))
        .manage(ProjectWatcher::default())
        .invoke_handler(tauri::generate_handler![
            server_start,
            launch_project,
            file_trash,
            project_watch,
            project_list,
            project_copy
        ])
        .build(tauri::generate_context!())
        .expect("the editor's window could not be opened")
        .run(|app, event| match event {
            // The page's windows are sub-windows of the editor's: when the
            // editor's window goes, the app ends, and they go with it.
            RunEvent::WindowEvent {
                label,
                event: WindowEvent::Destroyed,
                ..
            } if label == "main" => app.exit(0),
            // Closing the window ends the server the editor started (§4h.2).
            RunEvent::Exit => app.state::<Arc<Server>>().stop(),
            _ => {}
        });
}

/// Headless mode: the bundled server, given a compiled piece when `--play`
/// or `--render` names a `.lisp` file or a project.
fn headless_mode(args: &[OsString]) -> i32 {
    match headless::server_args(args, &bundled("espresso-client")) {
        Ok((args, ir)) => headless::run(&bundled("espresso-server"), &args, ir),
        Err(message) => {
            if !message.is_empty() {
                eprintln!("espresso: {message}");
            }
            1
        }
    }
}

/// SIGTERM and SIGINT, blocked in every thread, so that one thread can wait
/// for them (`exit_on_stop_signals`, `headless::run`).
#[cfg(unix)]
pub(crate) fn block_stop_signals() -> libc::sigset_t {
    // SAFETY: the set is initialized by sigemptyset before it is read, and
    // pthread_sigmask changes only the calling thread's mask.
    unsafe {
        let mut set: libc::sigset_t = std::mem::zeroed();
        libc::sigemptyset(&mut set);
        libc::sigaddset(&mut set, libc::SIGTERM);
        libc::sigaddset(&mut set, libc::SIGINT);
        libc::pthread_sigmask(libc::SIG_BLOCK, &set, std::ptr::null_mut());
        set
    }
}

/// A stop signal ends the editor as closing its window does, so the server
/// it started ends too: Ctrl+C in the terminal that ran it, or `kill`.
#[cfg(unix)]
fn exit_on_stop_signals(app: tauri::AppHandle, set: libc::sigset_t) {
    std::thread::spawn(move || {
        let mut signal = 0;
        // SAFETY: sigwait reads the set the main thread built and writes one int.
        if unsafe { libc::sigwait(&set, &mut signal) } == 0 {
            app.exit(0);
        }
    });
}
