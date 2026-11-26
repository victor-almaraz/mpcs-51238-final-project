//! The bundle's command line (editor-refactor.md §4h.3): one executable,
//! which opens the editor, or runs the bundled server in headless mode, or
//! the terminal editor.
//!
//! ```text
//! espresso [PROJECT]                                  the editor, with its server
//! espresso --headless [server options]                the server alone: espresso-server
//! espresso --headless --play PIECE [options]          a piece live, until stopped
//! espresso --headless --render PIECE --out FILE.wav   a piece offline
//! espresso --tui [client options]                     the terminal editor
//! ```
//!
//! PIECE is IR (an `.ir.json` file, or `-` for standard input), a `.lisp`
//! entry file, or a project directory. A `.lisp` piece is compiled by the
//! bundled `espresso-client --compile` and given to the server on its
//! standard input, so the server still links no interpreter.

use std::ffi::{OsStr, OsString};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};

/// What the command line asks for.
#[derive(Debug, PartialEq, Eq)]
pub enum Launch {
    /// The editor, on a project when one is named.
    Editor { project: Option<PathBuf> },
    /// The server, with these arguments.
    Headless(Vec<OsString>),
    /// The terminal editor, with these arguments.
    Tui(Vec<OsString>),
    /// The usage, on standard output.
    Help,
    /// A refused command line: the message, for standard error.
    Refused(String),
}

pub const USAGE: &str = "\
usage: espresso [PROJECT]
       espresso --headless [espresso-server options]
       espresso --headless --play PIECE [espresso-server options]
       espresso --headless --render PIECE --out FILE.wav [espresso-server options]
       espresso --tui [espresso-client options]

Opens the editor, on PROJECT when one is named, with a server of its own.
--headless runs the server without the editor, with espresso-server's
options (espresso --headless --help lists them). --play and --render take a
PIECE: IR (an .ir.json file, or - for standard input), a .lisp entry file,
or a project directory, whose entry is main.lisp, else NAME.lisp in a
directory NAME, else its only .lisp file other than project.lisp.
--tui runs the terminal editor, with espresso-client's options.
";

/// Reads the command line, without the program's name.
pub fn parse(args: &[OsString]) -> Launch {
    let Some(first) = args.first() else {
        return Launch::Editor { project: None };
    };
    match first.to_str() {
        Some("--headless") => Launch::Headless(args[1..].to_vec()),
        Some("--tui") => Launch::Tui(args[1..].to_vec()),
        Some("--help" | "-h") => Launch::Help,
        _ if args.len() == 1 && !first.to_string_lossy().starts_with('-') => {
            let project = PathBuf::from(first);
            if project.is_dir() {
                Launch::Editor {
                    project: Some(project),
                }
            } else {
                Launch::Refused(format!("{} is not a directory", project.display()))
            }
        }
        _ => Launch::Refused(format!("unknown option {}", first.to_string_lossy())),
    }
}

/// A piece for `--play` or `--render`.
#[derive(Debug, PartialEq, Eq)]
pub enum Piece {
    /// IR the server reads as it is.
    Ir,
    /// A `.lisp` entry file to compile, and the project root it resolves against.
    Lisp { file: PathBuf, root: PathBuf },
}

/// What `--play` or `--render` was given.
pub fn piece(path: &Path) -> Result<Piece, String> {
    if path.is_dir() {
        return Ok(Piece::Lisp {
            file: entry_file(path)?,
            root: path.to_path_buf(),
        });
    }
    if path.extension() == Some(OsStr::new("lisp")) {
        let root = path
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or(Path::new("."))
            .to_path_buf();
        return Ok(Piece::Lisp {
            file: path.to_path_buf(),
            root,
        });
    }
    Ok(Piece::Ir)
}

/// The entry file of the project in `dir`: main.lisp, else NAME.lisp in a
/// directory NAME, else its only .lisp file other than project.lisp.
pub fn entry_file(dir: &Path) -> Result<PathBuf, String> {
    let main = dir.join("main.lisp");
    if main.is_file() {
        return Ok(main);
    }
    if let Some(name) = dir
        .canonicalize()
        .ok()
        .and_then(|d| d.file_name().map(OsStr::to_owned))
    {
        let own = dir.join(name).with_extension("lisp");
        if own.is_file() {
            return Ok(own);
        }
    }
    let read = std::fs::read_dir(dir).map_err(|e| format!("cannot read {}: {e}", dir.display()))?;
    let files: Vec<PathBuf> = read
        .filter_map(Result::ok)
        .map(|e| e.path())
        .filter(|p| {
            p.extension() == Some(OsStr::new("lisp"))
                && p.file_name() != Some(OsStr::new("project.lisp"))
        })
        .collect();
    match files.as_slice() {
        [only] => Ok(only.clone()),
        [] => Err(format!("{} has no .lisp file to play", dir.display())),
        _ => Err(format!(
            "{} has no main.lisp and several .lisp files: name the entry file",
            dir.display()
        )),
    }
}

/// The server's arguments for `args`, with a compiled piece read from
/// standard input, and the IR to write there; or a message.
pub fn server_args(
    args: &[OsString],
    client: &Path,
) -> Result<(Vec<OsString>, Option<Vec<u8>>), String> {
    let mut out = args.to_vec();
    let Some(at) = out.iter().position(|a| a == "--play" || a == "--render") else {
        return Ok((out, None));
    };
    let Some(value) = out.get(at + 1).cloned() else {
        return Ok((out, None));
    }; // the server refuses it
    if value == "-" {
        return Ok((out, None));
    }
    let Piece::Lisp { file, root } = piece(Path::new(&value))? else {
        return Ok((out, None));
    };
    let compiled = Command::new(client)
        .arg("--compile")
        .arg(&file)
        .arg("--root")
        .arg(&root)
        .stderr(Stdio::inherit())
        .output()
        .map_err(|e| format!("cannot run {}: {e}", client.display()))?;
    if !compiled.status.success() {
        return Err(String::new()); // the client said why
    }
    out[at + 1] = "-".into();
    if !out.iter().any(|a| a == "--root") {
        // Sample paths resolve against the project.
        out.push("--root".into());
        out.push(root.into());
    }
    Ok((out, Some(compiled.stdout)))
}

/// Runs `binary` with `args` and `input` on its standard input, passes it
/// the stop signals this process gets, and answers its exit status.
pub fn run(binary: &Path, args: &[OsString], input: Option<Vec<u8>>) -> i32 {
    #[cfg(unix)]
    let signals = crate::block_stop_signals();
    let mut command = Command::new(binary);
    command.args(args);
    if input.is_some() {
        command.stdin(Stdio::piped());
    }
    let mut child = match command.spawn() {
        Ok(child) => child,
        Err(e) => {
            eprintln!("espresso: cannot run {}: {e}", binary.display());
            return 1;
        }
    };
    #[cfg(unix)]
    forward_stop_signals(&child, signals);
    if let (Some(ir), Some(mut stdin)) = (input, child.stdin.take()) {
        // The server reads all of it before it plays; a write error is the server's to report.
        let _ = stdin.write_all(&ir);
    }
    wait(&mut child)
}

fn wait(child: &mut Child) -> i32 {
    match child.wait() {
        Ok(status) => status.code().unwrap_or_else(|| {
            #[cfg(unix)]
            {
                use std::os::unix::process::ExitStatusExt;
                128 + status.signal().unwrap_or(0)
            }
            #[cfg(not(unix))]
            1
        }),
        Err(e) => {
            eprintln!("espresso: {e}");
            1
        }
    }
}

// SIGTERM and SIGINT to this process go on to the child, which ends as it
// does on its own; this process then ends with the child's status.
#[cfg(unix)]
fn forward_stop_signals(child: &Child, set: libc::sigset_t) {
    let Ok(pid) = libc::pid_t::try_from(child.id()) else {
        return;
    };
    std::thread::spawn(move || loop {
        let mut signal = 0;
        // SAFETY: sigwait reads the set built by block_stop_signals and writes one int; kill takes the
        // child's id, which the main thread reaps only after the child ends.
        unsafe {
            if libc::sigwait(&set, &mut signal) != 0 {
                return;
            }
            libc::kill(pid, signal);
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(list: &[&str]) -> Vec<OsString> {
        list.iter().map(OsString::from).collect()
    }

    fn examples() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../../examples")
    }

    #[test]
    fn the_first_argument_chooses() {
        assert_eq!(parse(&[]), Launch::Editor { project: None });
        assert_eq!(
            parse(&args(&["--headless", "--no-audio"])),
            Launch::Headless(args(&["--no-audio"]))
        );
        assert_eq!(
            parse(&args(&["--tui", "piece"])),
            Launch::Tui(args(&["piece"]))
        );
        assert_eq!(parse(&args(&["--help"])), Launch::Help);
        let dir = examples();
        assert_eq!(
            parse(&[dir.clone().into()]),
            Launch::Editor { project: Some(dir) }
        );
        assert!(matches!(parse(&args(&["no-such-dir"])), Launch::Refused(_)));
        assert!(
            matches!(parse(&args(&["--play", "x"])), Launch::Refused(_)),
            "--play belongs to --headless"
        );
    }

    #[test]
    fn a_project_enters_at_main_then_its_own_name_then_its_only_file() {
        let e = examples();
        assert_eq!(
            entry_file(&e.join("phasing")).unwrap(),
            e.join("phasing/main.lisp")
        );
        assert_eq!(
            entry_file(&e.join("hectodrone")).unwrap(),
            e.join("hectodrone/main.lisp")
        );
        assert_eq!(
            entry_file(&e.join("page-keys")).unwrap(),
            e.join("page-keys/page-keys.lisp")
        );
        let only = std::env::temp_dir().join(format!("espresso-entry-{}", std::process::id()));
        std::fs::create_dir_all(&only).unwrap();
        std::fs::write(only.join("project.lisp"), "(project :name \"x\")").unwrap();
        std::fs::write(only.join("tone.lisp"), "").unwrap();
        assert_eq!(entry_file(&only).unwrap(), only.join("tone.lisp"));
        std::fs::write(only.join("other.lisp"), "").unwrap();
        assert!(entry_file(&only).unwrap_err().contains("several"));
        std::fs::remove_dir_all(&only).unwrap();
    }

    #[test]
    fn a_piece_is_ir_or_lisp() {
        let e = examples();
        assert_eq!(
            piece(&e.join("wavetable/wavetable.ir.json")).unwrap(),
            Piece::Ir
        );
        assert_eq!(
            piece(&e.join("wavetable/wavetable.lisp")).unwrap(),
            Piece::Lisp {
                file: e.join("wavetable/wavetable.lisp"),
                root: e.join("wavetable")
            }
        );
        assert_eq!(
            piece(Path::new("tone.lisp")).unwrap(),
            Piece::Lisp {
                file: "tone.lisp".into(),
                root: ".".into()
            }
        );
    }
}
