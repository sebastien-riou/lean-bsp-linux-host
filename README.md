# lean-bsp-linux-host

The [lean-bsp](https://github.com/sebastien-riou/lean-bsp) BSP that builds an application as a native Linux
process, so every lean-bsp application builds, runs and debugs on a developer's machine with no hardware attached.
It targets whatever machine it runs on: its single target, `native`, uses the host's default compiler.

````
lbsp build-load-run --bsp linux-host
````

`gcc` by default; set `CC` and `CFLAGS` (default `-Og -g`) to change the compiler or its flags.
The executable lands in the build directory, `<app>/build/linux-host/native/<app name>`, ready for `gdb`.

## How the contract maps to Linux

| Primitive | On linux-host |
|---|---|
| UART 0 (and `printf`) | The process's stdin/stdout. With `lbsp run --detach`, a pseudo-terminal instead, reported as `com_device` for a host-side tool to open. Only port 0 exists. |
| Cycle counter, CPU frequency | `bsp_cycle_count()` counts nanoseconds of `CLOCK_MONOTONIC`; `bsp_cpu_freq_hz()` returns its 1 GHz rate, so cycles / frequency is still seconds. |
| Delay | `nanosleep` |
| Heap usage | lean-bsp's heap counter (contract 1.4), `src/bsp_heap.c`, with the application and the BSP linked with `--wrap=malloc,calloc,realloc,free`: the bytes their own code requests are counted, not the C library's internal allocations. It follows up to 4096 live blocks (`LEAN_BSP_HEAP_MAX_BLOCKS`), and has no lock: threads that allocate while the count matters make it wrong. |
| Cache clear | Nothing to do. |
| LEDs, buttons | None (`n_leds` and `n_buttons` are 0). |
| Reset | Re-executes the program; a `--detach` run keeps its pseudo-terminal across the reset. |
| `app_tick` | Called every millisecond from a thread, started only when the application defines `app_tick`. |
| `app_on_fault` | Called on `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE` (`APP_FAULT_HARD`) and `SIGABRT` (`APP_FAULT_ASSERT`), before the default action. The `pc` argument is 0. |
| `app_on_exit` | Default: `exit(code)`, so `lbsp run` reports `app_main`'s return value as `exit_code`. |

## Layout

- `bsp.toml`: the manifest lean-bsp reads.
- `bsp_cli.py`: the entry point answering `capabilities`, `build`, `load` and `run`.
- `include/bsp_api.h`: this BSP's copy of the contract header, identical to lean-bsp's canonical one.
- `src/`: `main.c` (the glue and the weak default hooks), `bsp_linux.c` (primitives), `bsp_heap.c` (the heap counter, a byte-identical copy of lean-bsp's).
