TLDR: patch is right - on this Mac it brings the click lead within 1 ms of the true wait at every setting (8cdfb0ef was up to 6.8 ms short at 60 fps), at no cost that can be measured.

**What was tested.** Branch `metal-click-exact` (local only, not pushed): 405fcf84, the patch applied with `git am`, unchanged, on 8cdfb0ef (`origin/metal`). `port/build.sh metal` built it with no change. `port/rptest.sh`: "all tests passed", release and ASan/UBSan. The code checks out against this Mac's client: `CL_SendCmd` stores `CL_CmdServerTime() + CL_FrameFraction()` for each command; `GetRenderTimeFraction()` is the same fraction for a drawn frame; `GetUserCmdFrameTime` sits in the same place in `clientGameImport_t` for client and cgame. One small note: when the client's clock would step back, it holds the fraction, so a click in that sliver is booked one command late (rare; not visible in the numbers).

**How.** Real key events posted inside the game, one connection per run to a local server (`sv_fps 20`, 50 ms server frames) through a precise relay: 35 ms down, 35 ms up plus 0–9.2 ms stepped from run to run. Both builds interleaved, 80 shots a connection, 12 connections a row (6 for vsync). Fire held 80–100 ms, so the server never misses one. Error = lead drawn at the picture − true wait (picture = the last drawn frame before the click is read; true wait = first snapshot whose clip dropped − that picture's time). No shot had to be left out: none followed a missing snapshot. `cl_movefps 100`, window 1280×720.

| build | frame cap | sub-grid | shots | mean ms | shot sd | run sd | error of mean | model | Mac agrees? |
|---|---|---|---|---|---|---|---|---|---|
| 8cdfb0ef | 60 | 0 | 960 | −6.75 | 15.7 | 3.9 | 1.11 | −5.8 | yes |
| patch | 60 | 0 | 960 | +0.32 | 14.9 | 1.3 | 0.48 | 0 | yes |
| 8cdfb0ef | 125 | 0 | 960 | −3.86 | 14.8 | 2.1 | 0.60 | −3.0 | yes (1.4 errors off) |
| patch | 125 | 0 | 960 | −0.20 | 14.8 | 1.6 | 0.48 | 0 | yes |
| 8cdfb0ef | 480 | 0 | 960 | −0.98 | 14.4 | 1.0 | 0.47 | −0.8 | yes |
| patch | 480 | 0 | 960 | +0.23 | 14.6 | 0.9 | 0.47 | 0 | yes |
| 8cdfb0ef | 60 | 1 | 960 | −6.56 | 15.4 | 4.4 | 1.27 | −6.3 | yes |
| patch | 60 | 1 | 960 | −0.16 | 14.6 | 2.6 | 0.74 | 0 | yes |
| 8cdfb0ef | 125 | 1 | 960 | −2.76 | 14.8 | 1.7 | 0.49 | −2.3 | yes |
| patch | 125 | 1 | 960 | +0.13 | 14.0 | 2.3 | 0.67 | 0 | yes |
| 8cdfb0ef | 480 | 1 | 960 | −1.02 | 14.8 | 2.7 | 0.79 | −0.7 | yes |
| patch | 480 | 1 | 960 | +0.98 | 14.6 | 2.2 | 0.63 | −0.1 | nearly (1.7 errors high) |
| 8cdfb0ef | vsync | 0 | 480 | −0.28 | 15.2 | 2.2 | 0.92 | none given | – |
| patch | vsync | 0 | 480 | −0.97 | 14.5 | 0.8 | 0.66 | none given | – |
| 8cdfb0ef | vsync | 1 | 480 | −1.18 | 14.2 | 2.3 | 0.93 | none given | – |
| patch | vsync | 1 | 480 | +0.44 | 14.8 | 2.3 | 0.95 | none given | – |

Negative = the lead is too short. Error of mean = the larger of shot sd/√shots and run sd/√runs. Vsync = `r_swapInterval 1`, `com_maxfps 1000`, frames set by the 240 Hz display.

**Per-connection means, sub-grid 1** (ms, in order of uplink delay; one connection's shot noise alone is about ±1.6):
- 60: 8cdfb0ef −1.9 −13.8 −11.7 −1.0 −0.8 −6.5 −5.1 −13.2 −6.6 −6.9 −5.5 −5.6; patch +2.2 +3.0 +2.2 +1.8 −3.1 +1.8 −4.6 +0.7 −1.1 +0.4 −1.8 −3.4
- 125: 8cdfb0ef −3.0 −4.1 −2.7 −4.1 −5.8 −1.4 −2.9 −3.1 −1.3 +0.9 −3.4 −2.1; patch +0.6 +1.3 +1.0 −3.3 −1.0 −3.1 +4.1 −2.1 +2.9 +1.5 +0.9 −1.3
- 480: 8cdfb0ef −1.1 −3.5 −4.0 −4.0 −2.7 +2.5 +1.0 +1.0 +2.5 −0.0 +1.3 −5.1; patch −1.1 −0.8 −2.3 −1.1 +3.6 +1.6 +1.0 +4.1 +2.7 +2.2 +3.1 −1.2
- vsync: 8cdfb0ef −0.7 −3.1 −1.4 −3.1 −1.9 +3.1; patch +2.5 −0.8 +0.6 −2.5 −0.8 +3.7

The model says the patch's single connections reach up to +6 with the sub-grid on; here they stay between −4.6 and +4.1: agrees.

**Frames and commands.** Frames drawn: 60.0, 125.0, 479.8 a second at the caps; 230–234 with vsync. Commands made on their own tick: 99.2–100 % at the caps (made in frames of their own, with no picture: 98–99 % at 60, 86 % at 125, 92 % at 480). With vsync only 30–32 % are on their tick: there are no command-only frames, so each command waits for the next picture. Both builds stay near zero there anyway.

**The decision rule.** Builds: yes. Unit test: yes. Sub-grid 0: the patch is within 1 ms of zero at every setting (the largest is −0.97 ± 0.66, vsync). It is clearly closer than 8cdfb0ef at 60 and 125, and closer at 480. With vsync, 8cdfb0ef is already near zero, and the two are the same within their error bars. Sub-grid 1: the patch's average is +0.35 ms. Every setting is within 1.5 ms, and none is worse than 8cdfb0ef (at 480, +0.98 against −1.02: the same size). Cost: none measurable (below). So: keep it.

**Cost.** Old and new bookkeeping in one binary, each timed on the same drawn frames, in turns, in a live game: new 24.8 / 25.4 / 27.2 ns a frame, old 22.1 / 25.4 / 24.1 at 480 / 125 / 60 fps. That is 0 to 3 ns more a frame, and the per-second spread is ±5–8 ns. Not timed: the client's one store per command made (125 a second at most). Bench (`port/bench.sh run`, 3 runs each, order B A A B), work ms a frame: 8cdfb0ef 0.193 0.227 0.242 | 0.246 0.197 0.239 (mean 0.224, p50 211/217 µs); patch 0.244 0.243 0.245 | 0.244 0.239 0.245 (mean 0.243, p50 236/233 µs). The gap is inside this sitting's spread (8cdfb0ef's own runs spread 53 µs), and it is thousands of times the 3 ns measured directly. It is noise, not the patch.

**Picture to glass** (separate from the lead; the same for both builds). `port/latency.sh` with `MOUSE=0 LATSTATS=0 TRACE=1` shows only what the display did. The window is composited by the WindowServer and never goes direct to the display ("display rejected"), at 60, 125 and 466 swaps a second at caps 60 / 125 / 480. It gives no delay number: the probe times only frames with mouse input, and the synthetic mouse takes over the real mouse, which wasn't asked for. Earlier window numbers in the plan's §2 (sessions 5–6): input to present 2.4–12.9 ms at p50, depending on the compositor's mode.

**Not measured.** `cl_movefps 125` (not enough time). Fullscreen (window only). Picture to glass as a number (above). The real keyboard/mouse path: events are posted inside the game, so the OS and USB delay before the game sees a click is left out (the same for both builds; the lead starts at the picture). A real internet server, and packet loss. Frames that run long because of the game's own work: this Mac never does at 1280×720, so vsync stood in.

**Machine.** Apple M4 Mac mini (Mac16,10), 16 GB, macOS 26.6.2 (25G83). Main display Dell G2524H at 1920×1080, 240 Hz. Five runs were spoiled by someone using the Mac; they were set aside and redone, and are not in the numbers. Scratch and raw data: `build.noindex/s115/` (`out/shots.tsv`, `out/segs.tsv`, `out/bench.txt`, `out/ab-1.log.cg`, `out/lat-*.txt`).

**Not pushed.** Merging into `metal` waits for Charles's "go": a code push to `metal` replaces the public Mac download.
