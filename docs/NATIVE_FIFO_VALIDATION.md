# Connected native FIFO qualification

## Scope and source

- Baseline: `42fdf86f4e66b15e7cc1d22404294f2234dfcb85`.
- Worktree: `pipewire-native-fifo`, branch `test/native-fifo-qualification-20261006`; clean at creation.
- Production source is unchanged. The fixture compiles the stock ndarray helper from that baseline and calls the installed native library through its public API.
- Installed daemon reports compiled/linked libpipewire `1.7.0`. Exact daemon, link tool, native DSO, fixture source, and executable SHA-256 values and compiler commands are in [qualification.json](validation/native-fifo-20261006/qualification.json).
- All fixture processes inherit CPU 11. No CPUs 0/1, science workload, shared library rebuild, installation, or HEART changes.

## Fixture contract

The connected topology is a finite `pw_stream` driver → stock FIFO ndarray helper → `pw_stream` sink on an isolated native core. The source explicitly negotiates 16 buffers; the observed sink/helper output pool is three buffers (requested two, native reliable transport reserves one). Node reliability is explicitly enabled. This is a functional test with logging and a 20 ms interval between original source cycles, not a latency benchmark or the specialized scientific row transport protocol.

The source publishes exactly eight scalar row tokens. Their Header identities are `(seq, offset)` = `(100, 0/2/4/6)` followed by `(101, 0/2/4/6)`. Values 1…8, PTS, DTS offset, frame marker, and full Acquisition V2 metadata are checked at the helper and receiver. Header offsets are original row identities; the scalar payload is a fixture token, not a scientific matrix.

Before publication, a fixture callback on the actual producer data loop dequeues every available producer output loan. It retains only opaque handles obtained from `pw_filter_new_simple()` and `pw_filter_add_port()`, and actual `pw_buffer` loans obtained from `pw_filter_dequeue_buffer()`. Capture wrappers delegate to the installed public implementation. They do not inspect private filter/helper layouts. Holds and releases execute through `pw_loop_invoke()` on the producer data loop, serialized with the owner callback.

After all eight original inputs are published, the harness verifies zero user callbacks while output capacity is held. It returns producer loans through actual `pw_filter_queue_buffer()` calls, as empty capacity tokens, and issues a related native RequestProcess wake. The sink returns those empty tokens and issues a native RequestProcess for each capacity return. After input eight, the source callback cannot publish another original input. Subsequent driver cycles service native requests only.

This is **controlled producer capacity fault injection**. Holding a downstream stream loan did not exhaust the producer pool in the earlier experiments; this test does not claim otherwise or prove application backpressure behavior.

Input one intentionally produces **no artifact**: the callback writes 41 into its output loan and sets `OUTPUT_UNAVAILABLE`. Input two verifies the same loan and value 41, then publishes packet two. The exact expected artifacts are packets **2…8**, seven outputs. There is no expected terminal output for packet one. Each completed artifact preserves the original Header and Acquisition record.

## Exact oracle and negative control

The stock helper's bounded diagnostic trace must have zero omitted records. The harness requires:

- Eight input `G` (given), `E` (completed), and `R` (recycled) identities, exactly once and in original order.
- Eight `D` (dequeued) identities in the same order.
- Each next input `D` occurs in the **same activation** as the preceding callback `E` and recycle `R`, proving that next input was already queued at callback end.
- Seven output `O` identities, exactly packets 2…8; no artifact for the unavailable first output.
- Every recorded result is zero, and the receiver validates all seven actual output loans.

The negative executable suppresses only the stock helper's backlog `pw_filter_trigger_process()` call. Producer/sink buffer operations, capacity-return requests, core, and driver remain native. It must leave a live stable prefix of fewer than eight callbacks with helper error zero. Observed runs stop at two callbacks, with original packet three already dequeued in callback two's activation. This negative control passes by proving the stall; it does not treat a setup error, process crash, or pool-removal error as success. The harness stops the helper before removing source pools.

## Verification

| Check | Result |
|---|---|
| Five independent native positive runs | 5/5, eight callbacks, seven received artifacts; 39 trace records, omitted 0 |
| Five independent native negative controls | 5/5, stable two-callback prefix; 10 trace records, omitted 0; helper error 0 |
| ASan + UBSan connected positive | Pass, including leak detection |
| ASan + UBSan connected negative | Pass, including leak detection |
| Existing admission suite under ASan + UBSan | Pass, all ten calls in its `main()` |
| Existing connected FIFO retained-output test | Pass |
| `-Wall -Wextra -Werror` fixture compilation | Pass |
| Meson file parser and diff whitespace check | Pass |

ASan/UBSan instrument the fixture executables and the included stock ndarray helper. **The installed native DSO is not sanitizer instrumented**; this is not native-library sanitizer qualification. Meson registrations were syntax parsed; direct compiler commands ran the same fixture sources. A full Meson configure/build was not performed because shared rebuild/install is excluded and disk is constrained.

Original representative positive, negative, sanitizer logs and the run/build/hash manifest are retained in [validation/native-fifo-20261006](validation/native-fifo-20261006). The manifest identifies all repeated run logs in `/home/dgamroth/.cache/rtc-native-fifo-20261006`. Each harness log also records its retained original private-core directory and complete helper CSV.

## Rejected experimental setups

Original diagnostic logs remain in that cache: `progress-before.log`, `reliable-stock.log`, and `port-reliable.log` show that held downstream stream loans did not reliably exhaust the producer output pool. `controlled-capacity.log`, `controlled-paced.log`, `controlled-paced-reliable.log`, and `controlled-native-activation.log` show that queuing the producer's empty loans requires consumer capacity-return wakes before producer loans become usable again. These were fixture assumptions, not established production defects. `initial.log` was a Python stdin setup error; `reliable-before.log` used a stale binary after a failed compile and is excluded from qualification.

## Disposition and remaining gates

This evidence establishes the AO-REV-003 **connected queued-backlog and controlled output-capacity recovery** gate for the declared driver/topology. It requires primary review before promoting the corresponding issue claim. No production defect was demonstrated and no production fix is included.

It does not establish end-to-end scientific source losslessness, automatic recovery for every driver or consumer, two-input skew/absence behavior on a connected core, specialized row-transport pacing, overload delivery, latency tails, or native-DSO sanitizer coverage. Those remain separate qualification obligations. The existing mocked admission tests and retained-output test retain their original scopes.
