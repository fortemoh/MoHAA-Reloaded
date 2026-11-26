TLDR: patch is right - on this Mac it brings the click lead within 1 ms of the true wait at every setting (8cdfb0ef was up to 6.8 ms short at 60 fps), at no cost that can be measured.

**What was tested.** Branch `metal-click-exact` (local only, not pushed): 405fcf84, the patch applied with `git am`, unchanged, on 8cdfb0ef (`origin/metal`). `port/build.sh metal` built it with no change. `port/rptest.sh`: "all tests passed", release and ASan/UBSan. The code checks out against this Mac's client: `CL_SendCmd` stores `CL_CmdServerTime() + CL_FrameFraction()` for each command; `GetRenderTimeFraction()` is the same fraction for a drawn frame; `GetUserCmdFrameTime` sits in the same place in `clientGameImport_t` for client and cgame. One small note: when the client's clock would step back, it holds the fraction, so a click in that sliver is booked one command late (rare; not visible in the numbers).

**How.** Real key events posted inside the game, one connection per run to a local server (`sv_fps 20`, 50 ms server frames) through a precise relay: 35 ms down, 35 ms up plus 0–9.2 ms stepped from run to run. Both builds interleaved, 12 connections a row (6 for vsync), 80 shots a connection (40 at `cl_movefps 125`). Fire held 80–100 ms, so the server never misses one. Error = lead drawn at the picture − true wait (picture = the last drawn frame before the click is read; true wait = first snapshot whose clip dropped − that picture's time). No shot had to be left out: none followed a missing snapshot. Window 1280×720.

Each cell: mean error in ms (run-to-run sd) ± error of the mean. Negative = the lead is too short. One shot's own sd is 14–16 ms everywhere. Error of mean = the larger of shot sd/√shots and run sd/√runs.

| cl_movefps / cap | sub-grid | shots each | 8cdfb0ef | patch | model, old → new | Mac agrees? |
|---|---|---|---|---|---|---|
| 100 / 60 | 0 | 960 | −6.75 (3.9) ± 1.11 | +0.32 (1.3) ± 0.48 | −5.8 → 0 | yes, yes |
| 100 / 125 | 0 | 960 | −3.86 (2.1) ± 0.60 | −0.20 (1.6) ± 0.48 | −3.0 → 0 | yes (1.4 errors off), yes |
| 100 / 480 | 0 | 960 | −0.98 (1.0) ± 0.47 | +0.23 (0.9) ± 0.47 | −0.8 → 0 | yes, yes |
| 100 / 60 | 1 | 960 | −6.56 (4.4) ± 1.27 | −0.16 (2.6) ± 0.74 | −6.3 → 0 | yes, yes |
| 100 / 125 | 1 | 960 | −2.76 (1.7) ± 0.49 | +0.13 (2.3) ± 0.67 | −2.3 → 0 | yes, yes |
| 100 / 480 | 1 | 960 | −1.02 (2.7) ± 0.79 | +0.98 (2.2) ± 0.63 | −0.7 → −0.1 | yes, nearly (1.7 errors high) |
| 100 / vsync | 0 | 480 | −0.28 (2.2) ± 0.92 | −0.97 (0.8) ± 0.66 | none given | – |
| 100 / vsync | 1 | 480 | −1.18 (2.3) ± 0.93 | +0.44 (2.3) ± 0.95 | none given | – |
| 125 / 60 | 0 | 480 | −6.23 (4.0) ± 1.15 | +0.56 (1.2) ± 0.71 | – → 0 | –, yes |
| 125 / 125 | 0 | 480 | −2.75 (2.2) ± 0.69 | −0.93 (2.4) ± 0.68 | 0 → 0 | **no**, yes (1.4 errors off) |
| 125 / 480 | 0 | 480 | +0.42 (2.2) ± 0.66 | +0.99 (3.0) ± 0.85 | – → 0 | –, yes |
| 125 / 60 | 1 | 480 | −5.47 (3.6) ± 1.03 | −0.04 (1.6) ± 0.69 | −7.6 → −0.04 | no (2.1 errors less short), yes |
| 125 / 125 | 1 | 480 | −2.18 (1.6) ± 0.66 | +1.04 (1.3) ± 0.67 | none given | – |
| 125 / 480 | 1 | 480 | −0.72 (2.3) ± 0.68 | −0.78 (1.0) ± 0.63 | −0.7 → −0.09 | yes, yes |

Vsync = `r_swapInterval 1`, `com_maxfps 1000`, frames set by the 240 Hz display. At cap 125 with `cl_movefps 125` the model expects every frame to make its own command ("the lock case", 0 for both). On the Mac the client wakes for each command by itself, and 83–86 % of commands are made in frames with no picture. So the old code is 2.75 ms short there, and the patch is right anyway.

**Per-connection means, sub-grid 1** (ms, in order of uplink delay; one connection's shot noise alone is about ±1.6 at 80 shots, ±2.3 at 40):
- 100/60: old −1.9 −13.8 −11.7 −1.0 −0.8 −6.5 −5.1 −13.2 −6.6 −6.9 −5.5 −5.6; patch +2.2 +3.0 +2.2 +1.8 −3.1 +1.8 −4.6 +0.7 −1.1 +0.4 −1.8 −3.4
- 100/125: old −3.0 −4.1 −2.7 −4.1 −5.8 −1.4 −2.9 −3.1 −1.3 +0.9 −3.4 −2.1; patch +0.6 +1.3 +1.0 −3.3 −1.0 −3.1 +4.1 −2.1 +2.9 +1.5 +0.9 −1.3
- 100/480: old −1.1 −3.5 −4.0 −4.0 −2.7 +2.5 +1.0 +1.0 +2.5 −0.0 +1.3 −5.1; patch −1.1 −0.8 −2.3 −1.1 +3.6 +1.6 +1.0 +4.1 +2.7 +2.2 +3.1 −1.2
- 100/vsync: old −0.7 −3.1 −1.4 −3.1 −1.9 +3.1; patch +2.5 −0.8 +0.6 −2.5 −0.8 +3.7
- 125/60: old +0.2 −4.7 +0.2 −9.4 −3.2 −7.6 −10.5 −8.4 −8.5 −3.8 −3.6 −6.5; patch +1.1 +1.0 −2.5 +0.2 +0.0 −2.4 +2.5 +0.7 −0.6 +1.9 −0.5 −1.9
- 125/125: old −1.4 −2.4 −0.0 −2.5 −3.7 −0.6 −2.9 −1.9 −2.1 −2.5 −5.9 −0.3; patch +0.8 +2.1 +0.1 +0.2 +0.6 +0.3 +0.7 +4.0 −0.6 +2.7 +1.1 +0.3
- 125/480: old −1.3 −3.5 −4.6 −1.6 +0.2 +4.1 −2.4 +1.1 −0.8 −0.5 −0.3 +0.8; patch −2.7 −0.1 −1.3 +0.8 −1.7 −1.0 −0.1 +0.3 −1.9 −0.0 −0.3 −1.5

The model says a single connection with the sub-grid on can be up to +6 ms off (`cl_movefps 100`) or 2.5 (125). Here the patch's connections stay between −4.6 and +4.1, within the model plus each connection's shot noise: agrees.

**Frames and commands.** Frames drawn: 60.0, 125.0, 479.8 a second at the caps; 230–234 with vsync. Commands made on their own tick: 99.2–100 % at the caps. The share made in frames of their own, with no picture: 98–99 % at 60; at 125, 86 % (`cl_movefps 100`) and 80–86 % (125); 92 % at 480. With vsync only 30–32 % are on their tick: there are no command-only frames, so each command waits for the next picture. Both builds stay near zero there anyway.

**The decision rule.** Builds: yes. Unit test: yes. Sub-grid 0: the patch is within 1 ms of zero at every setting (the largest: +0.99 ± 0.85 and −0.97 ± 0.66). It is clearly closer than 8cdfb0ef at caps 60 and 125, and closer at 480 with `cl_movefps 100`. Where 8cdfb0ef is already near zero (vsync, and 480 at `cl_movefps 125`), the two are the same within their error bars. Sub-grid 1: the patch's average over the seven settings is +0.23 ms. Every setting is within 1.5 ms, and none is worse than 8cdfb0ef by more than the error bars. Cost: none measurable (below). So: keep it.

**Cost.** Old and new bookkeeping in one binary, each timed on the same drawn frames, in turns, in a live game: new 24.8 / 25.4 / 27.2 ns a frame, old 22.1 / 25.4 / 24.1 at 480 / 125 / 60 fps. That is 0 to 3 ns more a frame, and the per-second spread is ±5–8 ns. Not timed: the client's one store per command made (125 a second at most). Bench (`port/bench.sh run`, 3 runs each, order B A A B), work ms a frame: 8cdfb0ef 0.193 0.227 0.242 | 0.246 0.197 0.239 (mean 0.224, p50 211/217 µs); patch 0.244 0.243 0.245 | 0.244 0.239 0.245 (mean 0.243, p50 236/233 µs). The gap is inside this sitting's spread (8cdfb0ef's own runs spread 53 µs), and it is thousands of times the 3 ns measured directly. It is noise, not the patch.

**Picture to glass** (separate from the lead, which is short by it before and after; the patch doesn't touch drawing). `port/latency.sh`'s probe with its synthetic mouse, on the patched build, `cl_movefps 100`, window, from the start of the frame to the picture on the screen, p50 / p95: cap 60: 2.63 / 3.18 ms; 125: 2.45 / 2.88; 480: 1.95 / 2.47; vsync: 11.80 / 11.90. Of that, about 0.7–1.0 ms (1.8 with vsync) is the frame reaching the GPU's end; the rest is the macOS compositor. The window is composited and never goes direct to the display ("display rejected"; `MOUSE=0 LATSTATS=0 TRACE=1`). Every frame with mouse input was shown, none lost.

**Not measured.** Fullscreen (window only). The real keyboard/mouse path: events are posted inside the game, so the OS and USB delay before the game sees a click is left out (the same for both builds; the lead starts at the picture). A real internet server, and packet loss. Frames that run long because of the game's own work: this Mac never does at 1280×720, so vsync stood in.

**Machine.** Apple M4 Mac mini (Mac16,10), 16 GB, macOS 26.6.2 (25G83). Main display Dell G2524H at 1920×1080, 240 Hz. Six runs were spoiled by someone using the Mac; they were set aside and redone, and are not in the numbers. Scratch and raw data: `build.noindex/s115/` (`out/shots.tsv`, `out/segs.tsv`, `out/bench.txt`, `out/ab-1.log.cg`, `out/probe-*.txt`).

**Not pushed.** Merging into `metal` waits for Charles's "go": a code push to `metal` replaces the public Mac download.
