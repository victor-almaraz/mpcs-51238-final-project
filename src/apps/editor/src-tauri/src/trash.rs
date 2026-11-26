// Move to Trash (patterns.md §20.7): a project's file or folder to the
// platform's trash, the freedesktop.org trash on Linux and the Finder's on
// macOS, through the `trash` crate. The web interface names the entry by
// its path in the project, as it names files to the sidecar; nothing
// outside the project's root is moved.

use std::path::{Component, Path, PathBuf};

/// The entry `relative` names inside `root`, or why it names none: an
/// empty, absolute, or `..` path, or one that does not exist.
pub fn inside(root: &Path, relative: &str) -> Result<PathBuf, String> {
    let path = Path::new(relative);
    let plain = path
        .components()
        .all(|c| matches!(c, Component::Normal(_) | Component::CurDir));
    if relative.is_empty() || !plain {
        return Err(format!(
            "{relative} is not a path inside the project, relative to its root"
        ));
    }
    let full = root.join(path);
    if full.symlink_metadata().is_err() {
        return Err(format!("{relative} does not exist"));
    }
    Ok(full)
}

/// Moves `relative`, in the project at `root`, to the trash.
pub fn trash(root: &Path, relative: &str) -> Result<(), String> {
    let full = inside(root, relative)?;
    ::trash::delete(&full).map_err(|e| format!("cannot move {relative} to the trash: {e}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_entries_inside_the_project_are_named() {
        let root = std::env::temp_dir().join(format!("espresso-trash-{}", std::process::id()));
        std::fs::create_dir_all(root.join("lib")).unwrap();
        std::fs::write(root.join("lib/tone.lisp"), "").unwrap();
        assert_eq!(
            inside(&root, "lib/tone.lisp").unwrap(),
            root.join("lib/tone.lisp")
        );
        assert_eq!(inside(&root, "./lib").unwrap(), root.join("./lib"));
        assert!(inside(&root, "../elsewhere")
            .unwrap_err()
            .contains("not a path inside"));
        assert!(inside(&root, "/tmp")
            .unwrap_err()
            .contains("not a path inside"));
        assert!(inside(&root, "").unwrap_err().contains("not a path inside"));
        assert!(inside(&root, "absent.lisp")
            .unwrap_err()
            .contains("does not exist"));
        std::fs::remove_dir_all(&root).unwrap();
    }
}
