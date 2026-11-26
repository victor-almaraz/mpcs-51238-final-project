//! The server the editor starts (editor-refactor.md §4h.2): the bundled
//! `espresso-server`, beside the editor's executable, started on demand and
//! stopped when the editor exits. Its output lines go to a callback, and its
//! exit, when it was not asked to stop, to another.
//!
//! On Linux the server is tied to the thread that starts it
//! (`PR_SET_PDEATHSIG`), so it ends if the editor is killed. Linux signals
//! a child when that thread ends, not the process, so every server is
//! started by one launcher thread that lives as long as the `Server`.

use std::ffi::OsString;
use std::io::{self, BufRead, BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

/// How long a start waits for the server's "listening on" line.
const READY_WAIT: Duration = Duration::from_secs(5);
/// How long a stop waits after SIGTERM before it kills the server.
const STOP_WAIT: Duration = Duration::from_secs(3);
/// How often the exit watcher looks at the server.
const POLL: Duration = Duration::from_millis(100);

/// How a start ended.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(tag = "state", rename_all = "camelCase")]
pub enum Started {
    /// The server answers on its port.
    Running,
    /// The server ended before it listened, with its status and its last
    /// line: "cannot listen on 127.0.0.1:57190" when another server has the
    /// port.
    Exited { code: Option<i32>, line: String },
}

/// A line the server wrote, on standard output or standard error.
#[derive(Debug, Clone, serde::Serialize)]
pub struct Line {
    pub error: bool,
    pub text: String,
}

/// The server's end when nothing asked it to stop.
#[derive(Debug, Clone, serde::Serialize)]
pub struct Exit {
    pub code: Option<i32>,
    pub line: String,
}

struct Running {
    child: Child,
    root: Option<PathBuf>,
    /// The start this process came from; a watcher of an older one stops.
    generation: u64,
}

// The thread that starts the servers, for `PR_SET_PDEATHSIG` (above).
struct Launcher {
    requests: mpsc::Sender<(Command, mpsc::Sender<io::Result<Child>>)>,
}

impl Launcher {
    fn new() -> Self {
        let (requests, received) = mpsc::channel::<(Command, mpsc::Sender<io::Result<Child>>)>();
        thread::Builder::new()
            .name("espresso-server launcher".into())
            .spawn(move || {
                for (mut command, reply) in received {
                    let _ = reply.send(command.spawn());
                }
            })
            .expect("the server's launcher thread could not start");
        Launcher { requests }
    }

    fn spawn(&self, command: Command) -> io::Result<Child> {
        let (reply, answer) = mpsc::channel();
        self.requests
            .send((command, reply))
            .map_err(|_| io::Error::other("the launcher has ended"))?;
        answer
            .recv()
            .map_err(|_| io::Error::other("the launcher has ended"))?
    }
}

/// The editor's server process, at most one at a time.
pub struct Server {
    binary: PathBuf,
    port: Option<u16>,
    /// Arguments before the editor's own, for the tests' `--no-audio`.
    args: Vec<OsString>,
    launcher: Launcher,
    running: Arc<Mutex<Option<Running>>>,
    last_line: Arc<Mutex<String>>,
    generation: Arc<AtomicU64>,
    on_line: Arc<dyn Fn(Line) + Send + Sync>,
    on_exit: Arc<dyn Fn(Exit) + Send + Sync>,
}

impl Server {
    /// A server run from `binary`, on `port` or its default, reporting its
    /// lines to `on_line` and an exit it was not asked for to `on_exit`.
    pub fn new(
        binary: PathBuf,
        port: Option<u16>,
        on_line: impl Fn(Line) + Send + Sync + 'static,
        on_exit: impl Fn(Exit) + Send + Sync + 'static,
    ) -> Self {
        Server {
            binary,
            port,
            args: Vec::new(),
            launcher: Launcher::new(),
            running: Arc::new(Mutex::new(None)),
            last_line: Arc::new(Mutex::new(String::new())),
            generation: Arc::new(AtomicU64::new(0)),
            on_line: Arc::new(on_line),
            on_exit: Arc::new(on_exit),
        }
    }

    /// True while the server this started runs.
    #[cfg(test)]
    pub fn is_running(&self) -> bool {
        let mut running = self.running.lock().unwrap();
        match running.as_mut() {
            Some(r) => matches!(r.child.try_wait(), Ok(None)),
            None => false,
        }
    }

    /// Starts the server with `root` as its project root (`--root`), and
    /// waits until it listens or ends. A server already running with that
    /// root is kept; one with another root is stopped first.
    pub fn start(&self, root: Option<PathBuf>) -> std::io::Result<Started> {
        {
            let mut running = self.running.lock().unwrap();
            if let Some(r) = running.as_mut() {
                if matches!(r.child.try_wait(), Ok(None)) && r.root == root {
                    return Ok(Started::Running);
                }
            }
        }
        self.stop();

        let mut command = Command::new(&self.binary);
        command.args(&self.args);
        if let Some(port) = self.port {
            command.arg("--port").arg(port.to_string());
        }
        if let Some(root) = &root {
            command.arg("--root").arg(root);
        }
        command
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        tie_to_parent(&mut command);
        let mut child = self.launcher.spawn(command)?;
        let generation = self.generation.fetch_add(1, Ordering::SeqCst) + 1;
        *self.last_line.lock().unwrap() = String::new();

        let (ready_tx, ready_rx) = mpsc::channel();
        self.read_lines(child.stdout.take(), false, Some(ready_tx));
        self.read_lines(child.stderr.take(), true, None);
        *self.running.lock().unwrap() = Some(Running {
            child,
            root,
            generation,
        });

        // Ready when the server says it listens; ended when it exits first.
        let deadline = Instant::now() + READY_WAIT;
        loop {
            match ready_rx.recv_timeout(POLL) {
                Ok(()) => {
                    self.watch(generation);
                    return Ok(Started::Running);
                }
                Err(mpsc::RecvTimeoutError::Timeout) => {}
                // Standard output closed without the line: the server is ending.
                Err(mpsc::RecvTimeoutError::Disconnected) => thread::sleep(POLL),
            }
            if let Some(status) = self.exited() {
                // Its last lines may still be in the pipes.
                thread::sleep(POLL);
                return Ok(Started::Exited {
                    code: status.code(),
                    line: self.last_line.lock().unwrap().clone(),
                });
            }
            if Instant::now() >= deadline {
                self.watch(generation);
                return Ok(Started::Running);
            }
        }
    }

    /// Stops the server with SIGTERM, which it handles, and kills it when
    /// it has not ended after a few seconds. Its end is not reported.
    pub fn stop(&self) {
        let Some(mut r) = self.running.lock().unwrap().take() else {
            return;
        };
        if !matches!(r.child.try_wait(), Ok(None)) {
            return;
        }
        terminate(&r.child);
        let deadline = Instant::now() + STOP_WAIT;
        while Instant::now() < deadline {
            if !matches!(r.child.try_wait(), Ok(None)) {
                return;
            }
            thread::sleep(POLL);
        }
        let _ = r.child.kill();
        let _ = r.child.wait();
    }

    // The status of a server that has ended, which is then forgotten.
    fn exited(&self) -> Option<ExitStatus> {
        let mut running = self.running.lock().unwrap();
        let status = running
            .as_mut()
            .and_then(|r| r.child.try_wait().ok().flatten())?;
        *running = None;
        Some(status)
    }

    // Reports each line of `stream`; the first "listening on" line of
    // standard output says the server is ready.
    fn read_lines(
        &self,
        stream: Option<impl Read + Send + 'static>,
        error: bool,
        ready: Option<mpsc::Sender<()>>,
    ) {
        let Some(stream) = stream else { return };
        let on_line = Arc::clone(&self.on_line);
        let last_line = Arc::clone(&self.last_line);
        thread::spawn(move || {
            let mut ready = ready;
            for line in BufReader::new(stream).lines().map_while(Result::ok) {
                if line.contains(" listening on ") {
                    if let Some(tx) = ready.take() {
                        let _ = tx.send(());
                    }
                }
                *last_line.lock().unwrap() = line.clone();
                on_line(Line { error, text: line });
            }
        });
    }

    // Reports the end of the server of `generation`, unless it was stopped
    // or replaced first.
    fn watch(&self, generation: u64) {
        let running = Arc::clone(&self.running);
        let current = Arc::clone(&self.generation);
        let last_line = Arc::clone(&self.last_line);
        let on_exit = Arc::clone(&self.on_exit);
        thread::spawn(move || loop {
            thread::sleep(POLL);
            if current.load(Ordering::SeqCst) != generation {
                return;
            }
            let mut slot = running.lock().unwrap();
            let Some(r) = slot.as_mut() else { return };
            if r.generation != generation {
                return;
            }
            if let Ok(Some(status)) = r.child.try_wait() {
                *slot = None;
                drop(slot);
                thread::sleep(POLL); // its last lines may still be in the pipes
                on_exit(Exit {
                    code: status.code(),
                    line: last_line.lock().unwrap().clone(),
                });
                return;
            }
        });
    }
}

/// A bundled executable, `espresso-server` or `espresso-client`: beside the
/// running one, where Tauri puts its external binaries.
pub fn bundled(name: &str) -> PathBuf {
    let exe = std::env::current_exe().unwrap_or_default();
    let dir = exe.parent().unwrap_or(Path::new("."));
    dir.join(if cfg!(windows) {
        format!("{name}.exe")
    } else {
        name.to_owned()
    })
}

impl Drop for Server {
    fn drop(&mut self) {
        self.stop();
    }
}

#[cfg(unix)]
fn terminate(child: &Child) {
    if let Ok(pid) = libc::pid_t::try_from(child.id()) {
        // SAFETY: kill() takes a process id and a signal; the id is the child's, which this has not reaped.
        unsafe {
            libc::kill(pid, libc::SIGTERM);
        }
    }
}

#[cfg(not(unix))]
fn terminate(child: &Child) {
    let _ = child;
}

#[cfg(target_os = "linux")]
fn tie_to_parent(command: &mut Command) {
    use std::os::unix::process::CommandExt;
    // SAFETY: getpid() before the fork, and prctl() and getppid() in the child, are async-signal-safe.
    let parent = unsafe { libc::getpid() };
    unsafe {
        command.pre_exec(move || {
            if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM) != 0 {
                return Err(std::io::Error::last_os_error());
            }
            // The editor ended between the fork and prctl().
            if libc::getppid() != parent {
                return Err(std::io::Error::other("the editor ended"));
            }
            Ok(())
        });
    }
}

#[cfg(not(target_os = "linux"))]
fn tie_to_parent(_command: &mut Command) {}

#[cfg(test)]
mod tests {
    //! Against the real server, `ESPRESSO_SERVER` or the build tree's
    //! `build/bin/espresso-server`, with `--no-audio`.

    use super::*;
    use std::net::UdpSocket;

    fn binary() -> PathBuf {
        let path = std::env::var_os("ESPRESSO_SERVER")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../../build/bin/espresso-server")
            });
        assert!(
            path.exists(),
            "no server at {}: build it (cmake --build build) or set ESPRESSO_SERVER",
            path.display()
        );
        path
    }

    // A port nothing listens on now.
    fn free_port() -> u16 {
        UdpSocket::bind("127.0.0.1:0")
            .unwrap()
            .local_addr()
            .unwrap()
            .port()
    }

    fn server(binary: PathBuf, port: u16, exits: Arc<Mutex<Vec<Exit>>>) -> Server {
        let mut s = Server::new(
            binary,
            Some(port),
            |_| {},
            move |e| exits.lock().unwrap().push(e),
        );
        s.args = vec!["--no-audio".into()];
        s
    }

    #[test]
    fn starts_when_it_listens_and_stops_with_sigterm() {
        let binary = binary();
        let exits = Arc::new(Mutex::new(Vec::new()));
        let s = server(binary, free_port(), Arc::clone(&exits));
        let started = Instant::now();
        assert_eq!(s.start(None).unwrap(), Started::Running);
        assert!(
            started.elapsed() < READY_WAIT,
            "the start waited for its timeout, not the server's line"
        );
        assert!(s.is_running());
        let stopping = Instant::now();
        s.stop();
        assert!(
            stopping.elapsed() < STOP_WAIT,
            "the server did not end on SIGTERM"
        );
        assert!(!s.is_running());
        thread::sleep(POLL * 3);
        assert!(exits.lock().unwrap().is_empty(), "a stop is not an exit");
    }

    #[test]
    fn a_second_server_on_the_port_ends_and_says_why() {
        let binary = binary();
        let port = free_port();
        let exits = Arc::new(Mutex::new(Vec::new()));
        let first = server(binary.clone(), port, Arc::clone(&exits));
        assert_eq!(first.start(None).unwrap(), Started::Running);
        let second = server(binary, port, Arc::clone(&exits));
        match second.start(None).unwrap() {
            Started::Exited { line, .. } => assert!(line.contains("cannot listen"), "{line}"),
            Started::Running => panic!("two servers on one port"),
        }
        first.stop();
    }

    #[test]
    fn an_exit_it_did_not_ask_for_is_reported() {
        let binary = binary();
        let exits = Arc::new(Mutex::new(Vec::new()));
        let s = server(binary, free_port(), Arc::clone(&exits));
        assert_eq!(s.start(None).unwrap(), Started::Running);
        let pid = s.running.lock().unwrap().as_ref().unwrap().child.id();
        // SAFETY: the child's id, which this has not reaped.
        unsafe {
            libc::kill(pid as libc::pid_t, libc::SIGKILL);
        }
        let deadline = Instant::now() + Duration::from_secs(3);
        while exits.lock().unwrap().is_empty() && Instant::now() < deadline {
            thread::sleep(POLL);
        }
        assert_eq!(exits.lock().unwrap().len(), 1);
        assert!(!s.is_running());
    }

    #[test]
    fn a_new_root_restarts_it() {
        let binary = binary();
        let exits = Arc::new(Mutex::new(Vec::new()));
        let s = server(binary, free_port(), Arc::clone(&exits));
        assert_eq!(s.start(Some(PathBuf::from("."))).unwrap(), Started::Running);
        let first = s.running.lock().unwrap().as_ref().unwrap().child.id();
        assert_eq!(s.start(Some(PathBuf::from("."))).unwrap(), Started::Running);
        assert_eq!(
            s.running.lock().unwrap().as_ref().unwrap().child.id(),
            first,
            "the same root keeps it"
        );
        assert_eq!(
            s.start(Some(std::env::temp_dir())).unwrap(),
            Started::Running
        );
        assert_ne!(
            s.running.lock().unwrap().as_ref().unwrap().child.id(),
            first,
            "another root restarts it"
        );
        s.stop();
        thread::sleep(POLL * 3);
        assert!(exits.lock().unwrap().is_empty());
    }
}
