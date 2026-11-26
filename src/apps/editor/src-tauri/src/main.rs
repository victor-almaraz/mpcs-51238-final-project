// The bundle's entry point (editor-refactor.md §4h.3, headless.rs): the GUI
// editor (tauri-editor.md §3), or headless mode, or the terminal editor. The
// editor is Tauri, with the shell plugin through which the web interface
// starts the client as its sidecar (§4), the dialog plugin that chooses a
// project folder, and the server the editor starts and stops (server.rs,
// §4h.2).

mod headless;
mod server;

use std::ffi::OsString;
use std::path::PathBuf;
use std::sync::Arc;

use headless::Launch;
use server::{bundled, Server, Started};
use tauri::{Emitter, Manager, RunEvent};

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
        .invoke_handler(tauri::generate_handler![server_start, launch_project])
        .build(tauri::generate_context!())
        .expect("the editor's window could not be opened")
        .run(|app, event| {
            // Closing the window ends the server the editor started (§4h.2).
            if let RunEvent::Exit = event {
                app.state::<Arc<Server>>().stop();
            }
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
