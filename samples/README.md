# NSF samples

Worked examples of the NSF networking stack. The first is **NSFECHO**, a UDP
echo server built on the EZASOKET C API — the M3 milestone's user-visible
deliverable and its exit-gate demonstration.

## NSFECHO — a UDP echo server

`nsfecho.c` is a small, heavily commented UDP echo server. It shows the whole
shape of an NSF application:

1. bring the Phase-1 stack up (CTCI device + IP + UDP) in the same address
   space as the app;
2. `nsf_initapi` → `nsf_socket(AF_INET, DGRAM)` → `nsf_bind` a UDP port;
3. loop: blocking `nsf_recvfrom` → `nsf_sendto` the same bytes back to the
   sender;
4. stop cleanly when a 4-byte **`QUIT`** datagram arrives (reply `BYE`,
   `nsf_close`, `nsf_termapi`), and print a leak gate.

It is deliberately minimal — no operator STOP, no timers, one socket. `QUIT`
is the only termination path. Read it top to bottom; it is meant as API
documentation as much as a program.

### Phase 1 vs. Phase 2

In Phase 1 the stack and the application share one address space and run on two
tasks (the executive event loop, and the app on an ATTACHed subtask). In M5
the stack moves into its own subsystem address space (`NSFS`) behind a
cross-memory transport; **this program relinks unchanged**, because the only
thing crossing the application↔stack boundary — the `NSFRQE` request block — is
frozen.

### Binary transparency (why `QUIT` is compared as raw bytes)

NSF never transcodes payload between EBCDIC and ASCII (spec §15.3) — that is the
application's job, exactly as IBM's stack works. So the `QUIT` sentinel is
compared, and `BYE` is sent, as **raw ASCII bytes** (`51 55 49 54` / `42 59 45`),
never as C string literals: on the EBCDIC target `"QUIT"` would compile to
EBCDIC and never match the ASCII bytes a host client puts on the wire. This is
the single most common host/MVS divergence the project guards against, made
visible in a few lines.

## Build & deploy

NSFECHO is its own load module (`[[module]]` in `project.toml`); it carries the
full Phase-1 stack.

```sh
make modules      # cross-compile + assemble on MVS
make deploy       # upload into NSF.LINKLIB (NSFECHO + NSF)
```

The device is wired to the mvsdev CTCI pair (`0500/0501`,
`192.168.200.1 ↔ .2`) by constants at the top of `nsfecho.c`. For a different
pair, rebuild with `-DNSFECHO_CUU=…`, `-DNSFECHO_SRC=…`, `-DNSFECHO_DST=…`.
(A production started task reads this from `PROFILE.TCPIP` instead — see
`src/nsfmain.c`; the sample hardcodes it to stay small.)

## Run the server

Submit `jcl/NSFECHO.jcl`. `PARM` is the UDP port (default `7`, the well-known
echo port):

```jcl
//ECHO     EXEC PGM=NSFECHO,PARM='7'
```

The step log lands in `SYSPRINT` (INITAPI/BIND lines, the echoed count, the
leak gates); device-up messages (`NSF210I`/`NSF211I`) go to the console. Stop
the server by sending a `QUIT` datagram (below) or `CANCEL NSFECHO`.

## Run the client

`host/echo_client.py` (Python 3, stdlib only) runs on the Hercules host against
the live server. Each scenario prints a `PASS`/`FAIL` verdict and sets the
process exit code (0 = PASS).

```sh
# individual scenarios
./echo_client.py echo  --host 192.168.200.1 --port 7 --count 1000
./echo_client.py sizes --mtu 1500
./echo_client.py kill9
./echo_client.py quit

# the composite M3 exit gate (echo -> sizes -> kill9 -> quit)
./echo_client.py gate  --host 192.168.200.1 --port 7
```

| scenario | what it proves |
|----------|----------------|
| `echo`   | N datagrams, every reply verified byte-for-byte; asserts 100 % replies (the CTCI link is lossless). |
| `sizes`  | 1-byte / mid / max (MTU − 28) all echo; one oversize (max + 1) is **not** echoed — the v1 no-fragmentation limit, made visible. |
| `kill9`  | a streaming client is `SIGKILL`ed mid-stream; the server keeps answering, and (at shutdown) leaked nothing — the connectionless-server robustness gate. |
| `quit`   | `QUIT` → `BYE`, and the server ends. |
| `gate`   | all of the above against one server run. |

## What a passing gate looks like

Client side:

```
[PASS] echo -- 1000/1000 echoed, 0 lost, 0 mismatched, 1.0s
[PASS] sizes: max MTU-28 (1472B) -- echoed 1472B
[PASS] sizes: oversize (1473B) -- correctly dropped (no fragmentation in v1)
[PASS] kill9 -- server answered 50/50 after the hard kill
[PASS] quit -- got BYE
[PASS] gate
```

Server side (`SYSPRINT`, at shutdown):

```
NSFECHO: echoed=1050 send_fail=0 quit=yes
NSFECHO:   NSFIP  fragdrop  = 1        <- the oversize datagram
NSFECHO: leak gate clean -- CC 0
```

and the job ends with `COND CODE 0000`, no `SYSUDUMP`.
