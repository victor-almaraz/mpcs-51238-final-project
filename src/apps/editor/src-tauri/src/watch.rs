// The project's watcher (patterns.md §13): the files of the open project,
// watched through the platform's notifications (inotify on Linux, FSEvents
// on macOS) by the `notify` crate, changes gathered over a short time, and
// each batch handed on as the paths it touched, relative to the project's
// root, or as none when the platform missed changes and every file is to be
// read again. Hidden entries (".git", an editor's swap files) are left out.

use std::path::{Component, Path, PathBuf};
use std::sync::Mutex;
use std::time::Duration;

use notify_debouncer_mini::notify::{self, RecommendedWatcher, RecursiveMode};
use notify_debouncer_mini::{
    new_debouncer_opt, Config, DebounceEventResult, DebouncedEventBatch, Debouncer,
};

/// A path a change touched, relative to the root, and whether it is there
/// now: the window lists the project again only when a path came or went.
#[derive(Clone, Debug, PartialEq, serde::Serialize)]
pub struct Touched {
    pub path: String,
    pub exists: bool,
}

/// How long changes are gathered before they are handed on: a save that
/// writes a temporary file and renames it is one change, not three.
const GATHER: Duration = Duration::from_millis(150);

/// The watcher of at most one project, replaced when another opens.
#[derive(Default)]
pub struct ProjectWatcher {
    current: Mutex<Option<Debouncer<RecommendedWatcher>>>,
}

impl ProjectWatcher {
    /// Watches the project at `root`, in place of the one watched, and
    /// calls `changed` with each batch of touched paths, or with `None`
    /// when changes were missed; no root stops.
    pub fn watch(
        &self,
        root: Option<&Path>,
        changed: impl Fn(Option<Vec<Touched>>) + Send + 'static,
    ) -> Result<(), String> {
        let mut current = self.current.lock().map_err(|e| e.to_string())?;
        *current = None; // the old watcher stops before the new one starts
        let Some(root) = root else { return Ok(()) };
        let root: PathBuf = root
            .canonicalize()
            .map_err(|e| format!("cannot watch {}: {e}", root.display()))?;
        let refused = |e: notify::Error| format!("cannot watch {}: {e}", root.display());
        let base = root.clone();
        // Links are not followed: a linked folder outside the project is no part of it to watch (inotify
        // would add a watch for every folder it leads to).
        let config = Config::default()
            .with_timeout(GATHER)
            .with_notify_config(notify::Config::default().with_follow_symlinks(false));
        let mut debouncer =
            new_debouncer_opt(config, move |result: DebounceEventResult| match result {
                Ok(DebouncedEventBatch::Events(events)) => {
                    let mut paths: Vec<Touched> = events
                        .iter()
                        .filter_map(|e| {
                            let path = relative(&base, &e.path)?;
                            Some(Touched {
                                path,
                                exists: e.path.exists(),
                            })
                        })
                        .collect();
                    paths.sort_by(|a, b| a.path.cmp(&b.path));
                    paths.dedup_by(|a, b| a.path == b.path);
                    if !paths.is_empty() {
                        changed(Some(paths));
                    }
                }
                // Rescan, or a kind of batch a later version adds: every file is read again.
                Ok(_) => changed(None),
                Err(_) => {}
            })
            .map_err(refused)?;
        debouncer
            .watcher()
            .watch(&root, RecursiveMode::Recursive)
            .map_err(refused)?;
        *current = Some(debouncer);
        Ok(())
    }
}

/// `path` relative to `root`, with "/" between its parts, or nothing for a
/// path outside the root, the root itself, or one with a hidden part.
pub fn relative(root: &Path, path: &Path) -> Option<String> {
    // FSEvents reports paths through the volume's real name, as canonicalize gives it.
    let path = path
        .strip_prefix(root)
        .ok()
        .map(Path::to_path_buf)
        .or_else(|| {
            let real = path.canonicalize().ok()?;
            real.strip_prefix(root).ok().map(Path::to_path_buf)
        })?;
    let mut parts = Vec::new();
    for component in path.components() {
        let Component::Normal(part) = component else {
            return None;
        };
        let part = part.to_str()?;
        if part.starts_with('.') {
            return None;
        }
        parts.push(part);
    }
    (!parts.is_empty()).then(|| parts.join("/"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;

    #[test]
    fn paths_are_named_from_the_root_without_hidden_ones() {
        let root = Path::new("/p");
        assert_eq!(
            relative(root, Path::new("/p/lib/tone.lisp")).as_deref(),
            Some("lib/tone.lisp")
        );
        assert_eq!(relative(root, Path::new("/p")), None);
        assert_eq!(relative(root, Path::new("/p/.git/index")), None);
        assert_eq!(relative(root, Path::new("/p/.main.lisp.swp")), None);
        assert_eq!(relative(root, Path::new("/elsewhere/main.lisp")), None);
    }

    #[test]
    fn a_change_in_the_project_is_handed_on() {
        let root = std::env::temp_dir().join(format!("espresso-watch-{}", std::process::id()));
        std::fs::create_dir_all(root.join("lib")).unwrap();
        let watcher = ProjectWatcher::default();
        let (send, receive) = mpsc::channel();
        watcher
            .watch(Some(&root), move |paths| {
                let _ = send.send(paths.unwrap_or_default());
            })
            .unwrap();
        // FSEvents can report changes made just before the watch began; they are let pass.
        std::thread::sleep(Duration::from_millis(300));
        while receive.try_recv().is_ok() {}
        std::fs::write(root.join("lib/tone.lisp"), "(module tone)\n").unwrap();
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        let mut seen = Vec::new();
        while std::time::Instant::now() < deadline
            && !seen.iter().any(|t: &Touched| t.path == "lib/tone.lisp")
        {
            if let Ok(paths) = receive.recv_timeout(Duration::from_millis(100)) {
                seen.extend(paths);
            }
        }
        assert!(
            seen.iter().any(|t| t.path == "lib/tone.lisp" && t.exists),
            "the write was handed on: {seen:?}"
        );
        // Stopped, it hands on nothing more.
        watcher.watch(None, |_| {}).unwrap();
        std::fs::write(root.join("main.lisp"), "").unwrap();
        std::thread::sleep(Duration::from_millis(400));
        assert!(receive.try_recv().is_err(), "a stopped watcher is quiet");
        std::fs::remove_dir_all(&root).unwrap();
    }
}
