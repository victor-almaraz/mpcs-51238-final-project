# Performance

## Threads

The server runs each block on `--threads` threads: the audio thread and workers. When a plan is built, the server groups its nodes into tasks and orders the tasks by what they share: two nodes keep their array order when they use one pipe, buffer, I/O port, or the device's outputs and at least one writes it. A thread runs a task once every task it waits for has finished, and the audio thread runs tasks too, so a late worker delays a block by at most the task it is running. A plan whose tasks are estimated to save less than 4 µs a block on `--threads` threads, counting the cost of passing data between processors, runs every node on the audio thread in array order, as with `--threads 1`. When fewer threads are estimated to save within 4 µs a block of what all of them save, the tasks run on the fewest such threads, and the other workers sleep. Writers of one audio pipe write private pipes that the server adds in array order, so the output is bit-identical at every thread count, offline and live. With `--threads 1` the server runs every node on the audio thread in array order.

In a live server, workers ask for real-time scheduling, as the audio thread has; on Linux this needs an rtprio limit (the `audio` group's, on most distributions), and the server reports when it is refused. A worker without real-time priority shares its processor with ordinary threads, so a busy system can delay it past a block's deadline; `--threads 1` runs every node on the audio thread instead. A worker with no node to run spins for `--worker-spin` microseconds, then sleeps until the next block.

On Linux, three settings of the system decide whether blocks meet their deadlines:

- **Real-time scheduling.** Workers run at real-time priority only within the rtprio limit (`ulimit -r`), and the device callback only through JACK.
- **The `performance` frequency governor.** It keeps each processor at its highest frequency. Other governors lower the frequency of a processor that idles between blocks and raise it only after load returns, which lengthens the blocks that follow. `cpupower frequency-set -g performance` selects it until the next boot.
- **A CPU latency request.** A worker that sleeps between blocks leaves its processor idle, and the deeper the idle state, the later the worker starts the next block. While a process holds `/dev/cpu_dma_latency` open with a latency in microseconds written to it, the kernel keeps every processor out of idle states that take longer to leave. The file is writable only by root by default: `sudo sh -c 'exec 3>/dev/cpu_dma_latency; printf 0 >&3; exec sleep infinity'` holds a request of 0 until the command is stopped. The server makes no request.

`scripts/straintest.sh` times offline renders of `examples/straintest`, the piece of the v2 load test, on 1, 2, 4, 8, and 16 threads, and prints each count's mean render time and speedup over one thread.

`/counters` carries the thread count, the blocks in which the audio thread waited for a worker, and each thread's load: its share of the block period spent running nodes. The editor's status bar shows the busiest thread's load and, past one thread, the thread count.
