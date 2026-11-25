// The GUI editor's entry point (tauri-editor.md §3): Tauri, with the shell
// plugin through which the web interface starts the client as its sidecar
// (§4), the dialog plugin that chooses a project folder, and the server the
// editor starts and stops (server.rs, editor-refactor.md §4h.2).

mod server;

use std::path::PathBuf;
use std::sync::Arc;

use server::{Server, Started};
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

fn main() {
    // Before any thread starts, so that every thread inherits the mask.
    #[cfg(unix)]
    let signals = block_stop_signals();
    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_dialog::init())
        .setup(move |app| {
            let binary = Server::bundled_binary()?;
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
        .invoke_handler(tauri::generate_handler![server_start])
        .build(tauri::generate_context!())
        .expect("the editor's window could not be opened")
        .run(|app, event| {
            // Closing the window ends the server the editor started (§4h.2).
            if let RunEvent::Exit = event {
                app.state::<Arc<Server>>().stop();
            }
        });
}

/// SIGTERM and SIGINT, blocked in every thread, so that one thread can wait
/// for them (`exit_on_stop_signals`).
#[cfg(unix)]
fn block_stop_signals() -> libc::sigset_t {
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
