# Multi-Container Runtime — OS Jackfruit

## 1. Team Information

| Name | SRN |
|------|-----|
| Prithivi Aithal | PES1UG24CS909 |
| S. Hemanth Kumar | PES1UG24CS418 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) git gcc-12
```

### Prepare the Alpine Root Filesystem
```bash
cd boilerplate
mkdir rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs
```

### Build All Targets
```bash
cd boilerplate
make
```
This builds `engine`, `cpu_hog`, `io_pulse`, `memory_hog`, and `monitor.ko`.

### Load the Kernel Module
```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
sudo dmesg | tail -5
```

### Start the Supervisor
```bash
sudo ./engine supervisor ./rootfs
```

### In a Second Terminal — Launch Containers
```bash
# Start containers in background
sudo ./engine start alpha ./rootfs /bin/ls
sudo ./engine start beta  ./rootfs /bin/ls

# List tracked containers
sudo ./engine ps

# View container logs
sudo ./engine logs alpha

# Stop a container
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Run Workloads Inside Containers
```bash
# Copy workload binaries into rootfs first
cp cpu_hog    ./rootfs/
cp io_pulse   ./rootfs/
cp memory_hog ./rootfs/

# Run CPU workload
sudo ./engine start cpuwork ./rootfs /cpu_hog

# Run I/O workload
sudo ./engine start iowork ./rootfs /io_pulse

# Run memory workload (triggers soft/hard limits)
sudo ./engine start memwork ./rootfs /memory_hog
```

### Run Scheduling Experiments
```bash
# Start two CPU-bound containers
sudo ./engine start high ./rootfs /cpu_hog
sudo ./engine start low  ./rootfs /cpu_hog

# Apply different nice values on host
sudo renice -5 -p $(pgrep cpu_hog | head -1)
sudo renice  5 -p $(pgrep cpu_hog | tail -1)

# Check results after 15 seconds
cat logs/high.log
cat logs/low.log
```

### Stop Everything and Unload Module
```bash
# Stop supervisor with Ctrl+C in Terminal 1
# Then unload kernel module
sudo rmmod monitor
sudo dmesg | tail -5
```

---

## 3. Demo Screenshots

### Screenshot 1 — Multi-Container Supervision
![Demo 1](screenshots/1.jpeg)

### Screenshot 2 — Metadata Tracking
![Demo 2](screenshots/2.jpeg)

### Screenshot 3 — Bounded-Buffer Logging
![Demo 3](screenshots/3.jpeg)

### Screenshot 4 — CLI and IPC
![Demo 4](screenshots/4.jpeg)

### Screenshot 5 — Kernel Module Loaded
![Demo 5](screenshots/5.jpeg)

### Screenshot 6 — Container Registration and Monitoring
![Demo 6](screenshots/6.jpeg)

### Screenshot 7 — Scheduling Experiment
![Demo 7.1](screenshots/7.1.jpeg)
![Demo 7.2](screenshots/7.2.jpeg)

### Screenshot 8 — Clean Teardown
![Demo 8](screenshots/8.jpeg)

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Our runtime uses three Linux namespace flags passed to `clone()`:

- **CLONE_NEWPID** — gives the container its own PID namespace. The first process inside sees itself as PID 1, completely isolated from the host process tree.
- **CLONE_NEWUTS** — gives the container its own hostname, set via `sethostname()` to the container ID.
- **CLONE_NEWNS** — gives the container its own mount table. We bind-mount the Alpine rootfs and mount a fresh `/proc` inside it, then `chroot` to make it the container's root filesystem.

At the kernel level, namespaces work by giving each process a pointer to a namespace object rather than the global one. When `clone()` is called with `CLONE_NEWPID`, the kernel allocates a new `pid_namespace` struct and links it to the child. Any `fork()` inside the container allocates PIDs from this child namespace, starting at 2 (since the container's init is already PID 1).

The host kernel still shares the underlying hardware, scheduling queues, network stack (we do not use `CLONE_NEWNET`), and the same physical memory. The containers are isolated in naming and visibility, not in resource access — which is why the kernel memory monitor (`monitor.c`) must run in kernel space to enforce limits across namespace boundaries.

### 4.2 Supervisor and Process Lifecycle

A long-running supervisor is necessary because container metadata (PID, state, log path, memory limits) must persist across the container's lifecycle and be accessible to CLI commands issued later. If the supervisor exited after each launch, there would be no process to reap children (causing zombies), no place to store metadata, and no IPC endpoint for the CLI.

Process creation: `clone()` is used instead of `fork()` + `unshare()` because it atomically creates the child in new namespaces in one syscall.

Child reaping: We install a `SIGCHLD` handler that calls `waitpid(-1, WNOHANG)` in a loop. The loop is critical because a single `SIGCHLD` can represent multiple children exiting simultaneously — if we only called `waitpid()` once per signal, some children would remain as zombies.

Metadata tracking: The `container_record_t` linked list is protected by `pthread_mutex_t metadata_lock`. Both the SIGCHLD handler (which updates state to `CONTAINER_EXITED` or `CONTAINER_KILLED`) and the CLI dispatch loop (which reads/writes state) acquire this lock before touching the list.

Signal delivery: `SIGINT`/`SIGTERM` to the supervisor set `g_should_stop = 1`, which causes the event loop to exit. The shutdown sequence sends `SIGTERM` to all running containers, waits 2 seconds, then sends `SIGKILL` to any that remain, and finally calls `waitpid()` to reap all of them.

### 4.3 IPC, Threads, and Synchronization

Our project uses two distinct IPC mechanisms:

**IPC 1 — Log pipe (producer-consumer):**
Each container's stdout and stderr are redirected into the write end of a `pipe()`. A per-container producer thread reads from the read end and pushes `log_item_t` chunks into the bounded buffer. A single consumer thread (`logging_thread`) pops chunks and writes them to per-container log files.

The bounded buffer uses:
- `pthread_mutex_t mutex` — protects `head`, `tail`, `count`
- `pthread_cond_t not_full` — producers wait here when buffer is full
- `pthread_cond_t not_empty` — consumer waits here when buffer is empty

Without the mutex, two producers could simultaneously read `tail`, both write to the same slot, and one chunk would be lost (lost update race). Without condition variables, producers would busy-spin on a full buffer, wasting CPU. Deadlock is avoided because we always acquire only one lock at a time and always signal after releasing.

**IPC 2 — UNIX domain socket (CLI control plane):**
The supervisor binds a `SOCK_STREAM` socket at `/tmp/mini_runtime.sock`. CLI commands (`start`, `stop`, `ps`, `logs`) connect, send a `control_request_t` struct, and read back a `control_response_t`. This is separate from the log pipe because mixing control messages and log data in one channel would require framing and demultiplexing — two channels keeps the design clean.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical memory pages currently mapped into a process's address space. It does not measure:
- Pages that have been swapped out
- Pages in shared libraries counted once per library
- Memory that has been allocated with `malloc()` but not yet touched (demand paging)

This is why `memory_hog.c` uses `memset()` after each `malloc()` — to actually fault in the pages and make RSS grow.

**Soft vs Hard limits:**
- The soft limit is a warning threshold. When RSS first exceeds it, the monitor logs a `KERN_WARNING` to `dmesg` but does not kill the process. This gives the application a chance to free memory or the operator a chance to intervene.
- The hard limit is a termination threshold. When RSS exceeds it, the monitor sends `SIGKILL` via `send_sig()` and removes the entry from the list.

**Why kernel space?**
User-space enforcement would require the supervisor to periodically read `/proc/<pid>/status` and parse the `VmRSS` field. This has two problems: (1) it is racy — the process could exceed the limit and allocate more memory between checks; (2) it requires the supervisor to have permission to read every container's `/proc` entry across PID namespace boundaries. The kernel monitor runs in the same context as the memory allocator and can check RSS atomically, making enforcement more reliable and timely.

### 4.5 Scheduling Behavior

We ran two experiments:

**Experiment 1 — CPU-bound with different priorities:**
Two `cpu_hog` containers ran simultaneously for 10 seconds. The `high` container had nice value -5 (higher priority) and the `low` container had nice value +5 (lower priority). The Linux CFS scheduler assigns CPU time proportional to weight, where weight is determined by nice value. The high-priority container completed its 10-second computation in roughly 10 seconds of wall time, while the low-priority container took approximately 15 seconds of wall time to complete the same computation, demonstrating that the CFS scheduler gave more CPU time to the higher-priority process.

**Experiment 2 — CPU-bound vs I/O-bound:**
`cpu_hog` (CPU-bound) and `io_pulse` (I/O-bound) ran concurrently. `cpu_hog` consumed its full CPU quantum each time it was scheduled, while `io_pulse` voluntarily yielded the CPU during `usleep()` and `fsync()` calls. The CFS scheduler quickly identified `io_pulse` as an interactive/I/O-bound task and gave it shorter but more frequent scheduling slots, while `cpu_hog` received longer but less frequent time slices. Both completed within expected time, showing that CFS naturally balances throughput (CPU-bound) and responsiveness (I/O-bound) workloads.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** `chroot` instead of `pivot_root` for filesystem isolation.
**Tradeoff:** `chroot` is simpler to implement but slightly less secure than `pivot_root` (a privileged process inside can potentially escape). For this project, simplicity outweighs the security difference since all containers run trusted workloads.
**Justification:** `pivot_root` requires the new root to be on a different filesystem mount, adding complexity. `chroot` after a bind-mount achieves the same visible isolation for our purposes.

### Supervisor Architecture
**Choice:** Single long-running supervisor with a `select()` event loop.
**Tradeoff:** Single-threaded event loop is simpler to reason about but cannot handle blocking operations in the dispatch path. We mitigate this by keeping all dispatcher code non-blocking.
**Justification:** The alternative (one thread per CLI connection) adds thread synchronization complexity without meaningful benefit for a system that handles at most a handful of concurrent CLI commands.

### IPC / Logging
**Choice:** UNIX domain socket for CLI, pipes for logging, bounded buffer in between.
**Tradeoff:** Two separate IPC channels add complexity but provide clean separation of concerns. A single channel would require multiplexing.
**Justification:** The project guide explicitly requires the control channel to be different from the logging pipe. UNIX sockets support structured binary messages (our `control_request_t` struct) without framing overhead.

### Kernel Monitor
**Choice:** `mutex` over `spinlock` for protecting the monitored list.
**Tradeoff:** Mutexes can sleep, so they cannot be used in hard interrupt context. Spinlocks are safe everywhere but waste CPU spinning.
**Justification:** Both our ioctl handler (process context) and timer callback (softirq context with `mod_timer`) are sleepable contexts. A mutex is correct here and avoids priority inversion issues that spinlocks can cause.

### Scheduling Experiments
**Choice:** `renice` on the host to change priorities rather than implementing a custom scheduler.
**Tradeoff:** `renice` changes the nice value of the host-side process, which indirectly affects the container process since they share the same kernel scheduler. This is simpler than implementing cgroup-based CPU limits.
**Justification:** The goal is to observe and analyse Linux scheduling behaviour, not to reimplement a scheduler. Using `renice` gives clear, measurable results that connect directly to CFS theory.

---

## 6. Scheduler Experiment Results

### Experiment 1 — Two CPU-bound Containers, Different Priorities

| Container | Nice Value | Wall Time to Complete 10s Work |
|-----------|------------|-------------------------------|
| high      | -5         | ~10 seconds                   |
| low       | +5         | ~15 seconds                   |

The high-priority container received approximately 60% of CPU time while the low-priority container received approximately 40%, consistent with the CFS weight ratio for nice values -5 and +5.

**Conclusion:** Linux CFS correctly allocates more CPU time to higher-priority processes. The accumulator values in `cpu_hog` output confirm that the high-priority container completed more computation per unit of wall time.

### Experiment 2 — CPU-bound vs I/O-bound Containers

| Container | Type    | Behaviour |
|-----------|---------|-----------|
| cpuwork   | CPU-bound | Runs continuously, uses full time slice |
| iowork    | I/O-bound | Sleeps between writes, yields CPU voluntarily |

`cpu_hog` completed 10 seconds of computation and printed 8 progress lines. `io_pulse` completed all 20 I/O iterations with 200ms sleep between each, demonstrating that I/O-bound workloads naturally yield CPU and are not starved by CPU-bound workloads running alongside them.

**Conclusion:** Linux CFS handles mixed CPU/IO workloads fairly without starvation. I/O-bound tasks get high responsiveness because they voluntarily sleep and accumulate scheduling credit (vruntime stays low), while CPU-bound tasks get high throughput by using their full time slice.
