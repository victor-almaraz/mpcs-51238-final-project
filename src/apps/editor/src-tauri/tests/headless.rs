//! Headless mode end to end (editor-refactor.md §4h.3): the built bundle
//! executable, with the server and the client Tauri placed beside it.

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

const BUNDLE: &str = env!("CARGO_BIN_EXE_espresso-editor");

fn examples() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../../examples")
}

fn server() -> PathBuf {
    let path = Path::new(BUNDLE).with_file_name("espresso-server");
    assert!(
        path.exists(),
        "no server beside {BUNDLE}: build the project (cmake --build build), then cargo test"
    );
    path
}

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("espresso-headless-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    dir.join(name)
}

// A WAV file of this test run's own.
fn wav() -> PathBuf {
    use std::sync::atomic::{AtomicUsize, Ordering};
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    scratch(&format!("{}.wav", NEXT.fetch_add(1, Ordering::SeqCst)))
}

// A port nothing listens on now.
fn free_port() -> String {
    std::net::UdpSocket::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port()
        .to_string()
}

fn render(program: &Path, args: &[&std::ffi::OsStr]) -> Vec<u8> {
    let out = wav();
    let status = Command::new(program)
        .args(args)
        .arg("--out")
        .arg(&out)
        .args(["--seconds", "2"])
        .status()
        .unwrap();
    assert!(status.success(), "{program:?} {args:?}");
    std::fs::read(&out).unwrap()
}

#[test]
fn a_piece_renders_the_same_from_lisp_a_project_and_ir() {
    let piece = examples().join("wavetable");
    let ir = piece.join("wavetable.ir.json");
    let lisp = piece.join("wavetable.lisp");
    let root = piece.as_os_str();
    let direct = render(
        &server(),
        &["--render".as_ref(), ir.as_os_str(), "--root".as_ref(), root],
    );
    assert!(direct.len() > 44, "the server rendered nothing");
    let bundle = Path::new(BUNDLE);
    assert!(
        render(
            bundle,
            &["--headless".as_ref(), "--render".as_ref(), lisp.as_os_str()]
        ) == direct,
        "from .lisp"
    );
    assert!(
        render(bundle, &["--headless".as_ref(), "--render".as_ref(), root]) == direct,
        "from the project"
    );
    assert!(
        render(
            bundle,
            &[
                "--headless".as_ref(),
                "--render".as_ref(),
                ir.as_os_str(),
                "--root".as_ref(),
                root
            ]
        ) == direct,
        "from IR"
    );
}

#[test]
fn a_piece_that_does_not_compile_is_refused_with_the_compilers_message() {
    let bad = scratch("bad.lisp");
    std::fs::write(&bad, "(defpiece main (").unwrap();
    let out = Command::new(BUNDLE)
        .args([
            "--headless".as_ref(),
            "--render".as_ref(),
            bad.as_os_str(),
            "--out".as_ref(),
            wav().as_os_str(),
        ])
        .output()
        .unwrap();
    assert!(!out.status.success());
    assert!(
        String::from_utf8_lossy(&out.stderr).contains("bad.lisp"),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
}

// --headless --play runs until a stop signal, which ends the server too.
fn plays_until(signal: i32) {
    let piece = examples().join("wavetable");
    let port = free_port();
    let mut child = Command::new(BUNDLE)
        .args([
            "--headless".as_ref(),
            "--no-audio".as_ref(),
            "--port".as_ref(),
            port.as_ref(),
            "--play".as_ref(),
            piece.as_os_str(),
        ])
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    thread::sleep(Duration::from_secs(2));
    assert!(
        child.try_wait().unwrap().is_none(),
        "the piece should still play"
    );
    // SAFETY: the child's id, which this has not reaped.
    unsafe {
        libc::kill(child.id() as libc::pid_t, signal);
    }
    let deadline = Instant::now() + Duration::from_secs(5);
    while child.try_wait().unwrap().is_none() {
        assert!(
            Instant::now() < deadline,
            "the bundle did not end on signal {signal}"
        );
        thread::sleep(Duration::from_millis(50));
    }
    // The server it ran has ended with it.
    let left = Command::new("pgrep")
        .args(["-f", &format!("--port {port} ")])
        .output()
        .unwrap();
    assert!(
        left.stdout.is_empty(),
        "a server outlived the bundle: {}",
        String::from_utf8_lossy(&left.stdout)
    );
}

#[test]
fn sigterm_ends_a_headless_play() {
    plays_until(libc::SIGTERM);
}

#[test]
fn sigint_ends_a_headless_play() {
    plays_until(libc::SIGINT);
}

#[test]
fn help_names_the_modes() {
    let out = Command::new(BUNDLE).arg("--help").output().unwrap();
    let usage = String::from_utf8_lossy(&out.stdout);
    assert!(
        out.status.success() && usage.contains("--headless") && usage.contains("--tui"),
        "{usage}"
    );
    let refused = Command::new(BUNDLE)
        .arg("--no-such-option")
        .output()
        .unwrap();
    assert_eq!(refused.status.code(), Some(2));
}

// A piece's sample paths resolve against its project, which headless mode
// gives the server as --root when none is named.
#[test]
fn a_project_with_samples_renders_against_its_root() {
    let piece = examples().join("grain-cloud");
    let ir = piece.join("grain-cloud.ir.json");
    let direct = render(
        &server(),
        &[
            "--render".as_ref(),
            ir.as_os_str(),
            "--root".as_ref(),
            piece.as_os_str(),
        ],
    );
    let bundle = render(
        Path::new(BUNDLE),
        &[
            "--headless".as_ref(),
            "--render".as_ref(),
            piece.as_os_str(),
        ],
    );
    assert!(
        bundle == direct,
        "the project's render differs from the server's with --root"
    );
}
