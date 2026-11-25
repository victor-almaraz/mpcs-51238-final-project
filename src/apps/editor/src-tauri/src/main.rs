// The GUI editor's entry point (tauri-editor.md §3): Tauri, with the shell
// plugin through which the web interface starts the client as its sidecar
// (§4), and the dialog plugin that chooses a project folder. The sidecar is
// the backend; there are no commands written in Rust.
fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_dialog::init())
        .run(tauri::generate_context!())
        .expect("the editor's window could not be opened");
}
