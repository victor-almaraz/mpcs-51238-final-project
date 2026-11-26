// New Project and Open Example (patterns.md §27): the templates and the
// examples bundled with the editor, each a folder, and the copy of one to
// a new folder, which the web interface then opens as the project. A
// folder's README.md describes its folders, a section of each, headed by
// the folder's name; the first sentence of the section is its description.

use std::path::Path;

use serde::Serialize;

/// A template or an example: its folder's name and what it is.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Listed {
    pub name: String,
    pub description: String,
}

/// The folders of `dir`, sorted by name, each with the description its
/// README.md gives it, or none; hidden folders are left out.
pub fn listed(dir: &Path) -> Result<Vec<Listed>, String> {
    let readme = std::fs::read_to_string(dir.join("README.md")).unwrap_or_default();
    let entries =
        std::fs::read_dir(dir).map_err(|e| format!("cannot read {}: {e}", dir.display()))?;
    let mut found: Vec<Listed> = entries
        .filter_map(Result::ok)
        .filter(|e| e.file_type().is_ok_and(|t| t.is_dir()))
        .filter_map(|e| e.file_name().into_string().ok())
        .filter(|name| !name.starts_with('.'))
        .map(|name| Listed {
            description: description(&readme, &name),
            name,
        })
        .collect();
    found.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(found)
}

/// The first sentence of the section `## name` of `readme`, without its
/// Markdown code marks, or "" when it has none.
pub fn description(readme: &str, name: &str) -> String {
    let heading = format!("## {name}");
    let mut lines = readme
        .lines()
        .skip_while(|l| l.trim_end() != heading)
        .skip(1);
    let paragraph: Vec<&str> = lines
        .by_ref()
        .skip_while(|l| l.trim().is_empty())
        .take_while(|l| !l.trim().is_empty() && !l.starts_with('#') && !l.starts_with('|'))
        .collect();
    let text = paragraph.join(" ").replace('`', "");
    match text.find(". ") {
        Some(end) => text[..=end].to_string(),
        None => text,
    }
}

/// The folder `name` of `dir`, or why it names none: a name with a path
/// separator, a hidden or `..` name, or no such folder.
pub fn member(dir: &Path, name: &str) -> Result<std::path::PathBuf, String> {
    if name.is_empty() || name.starts_with('.') || name.contains(['/', '\\']) {
        return Err(format!("{name} is not the name of a bundled folder"));
    }
    let path = dir.join(name);
    if !path.is_dir() {
        return Err(format!("{name} is not bundled with the editor"));
    }
    Ok(path)
}

/// Copies the folder `from` to `to`, which must not exist yet or be an
/// empty folder; hidden entries are left out. The parents of `to` are made
/// as needed.
pub fn copy_project(from: &Path, to: &Path) -> Result<(), String> {
    if !from.is_dir() {
        return Err(format!("{} is not bundled with the editor", from.display()));
    }
    if to.exists() {
        let empty = to.is_dir()
            && std::fs::read_dir(to)
                .map_err(|e| e.to_string())?
                .next()
                .is_none();
        if !empty {
            return Err(format!("{} already exists", to.display()));
        }
    }
    copy_tree(from, to).map_err(|e| format!("cannot copy to {}: {e}", to.display()))
}

fn copy_tree(from: &Path, to: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(to)?;
    for entry in std::fs::read_dir(from)? {
        let entry = entry?;
        let name = entry.file_name();
        if name.to_string_lossy().starts_with('.') {
            continue;
        }
        let target = to.join(&name);
        if entry.file_type()?.is_dir() {
            copy_tree(&entry.path(), &target)?;
        } else {
            std::fs::copy(entry.path(), &target)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const README: &str = "# Examples\n\nIntro.\n\n## wavetable\n\nA `wavetable` oscillator\nover two lines. Then more.\n\n| a | b |\n\n## drone\n\nOne sentence\n";

    #[test]
    fn a_folder_is_described_by_its_section() {
        assert_eq!(
            description(README, "wavetable"),
            "A wavetable oscillator over two lines."
        );
        assert_eq!(description(README, "drone"), "One sentence");
        assert_eq!(description(README, "absent"), "");
    }

    #[test]
    fn a_project_is_copied_to_a_new_folder_only() {
        let base = std::env::temp_dir().join(format!("espresso-projects-{}", std::process::id()));
        let from = base.join("bundled/tone");
        std::fs::create_dir_all(from.join("samples")).unwrap();
        std::fs::write(from.join("main.lisp"), "(module main)\n").unwrap();
        std::fs::write(from.join("samples/a.wav"), [0u8, 1, 2]).unwrap();
        std::fs::write(from.join(".DS_Store"), "").unwrap();
        std::fs::write(base.join("bundled/README.md"), "## tone\n\nA tone. More.\n").unwrap();
        assert_eq!(
            listed(&base.join("bundled")).unwrap(),
            vec![Listed {
                name: "tone".into(),
                description: "A tone.".into()
            }]
        );

        let to = base.join("Projects/mine");
        copy_project(&from, &to).unwrap();
        assert_eq!(
            std::fs::read_to_string(to.join("main.lisp")).unwrap(),
            "(module main)\n"
        );
        assert_eq!(
            std::fs::read(to.join("samples/a.wav")).unwrap(),
            [0u8, 1, 2]
        );
        assert!(!to.join(".DS_Store").exists(), "hidden entries stay behind");
        assert!(copy_project(&from, &to)
            .unwrap_err()
            .contains("already exists"));
        let empty = base.join("Projects/empty");
        std::fs::create_dir_all(&empty).unwrap();
        copy_project(&from, &empty).unwrap();
        assert!(copy_project(&base.join("bundled/absent"), &base.join("x"))
            .unwrap_err()
            .contains("not bundled"));
        assert_eq!(member(&base.join("bundled"), "tone").unwrap(), from);
        for bad in ["", "..", ".hidden", "tone/samples", "absent"] {
            assert!(
                member(&base.join("bundled"), bad).is_err(),
                "{bad:?} names no bundled folder"
            );
        }
        std::fs::remove_dir_all(&base).unwrap();
    }
}
