# Plan: a web UI for the backtester

## Context
`me-backtest` runs a user's Python strategy against a replayed Nasdaq day. Phases 0–4 are
in [`strategy-replay-plan.md`](strategy-replay-plan.md), and the user guide is
[`backtesting.md`](backtesting.md).

This plan puts a public web page in front of it:
- The visitor picks every non-strategy setting, pastes a strategy, clicks **Run**, and sees
  the results on the page about 25–30 s later.
- There is no email and no stored run history.
- It is a personal project, so it runs on one free Oracle Cloud Always Free ARM VM.

### Decided
| decision | choice |
|---|---|
| UI framework | **Streamlit**, served from the VM |
| result delivery | **on the page only**: the request waits for the run to finish |
| hosting | **Oracle Cloud Always Free** ARM VM, with Cloudflare (free) in front |
| sandbox | **isolate** (the IOI sandbox, also used by Judge0): one sandboxed process per run, inside a long-running app; no container per run |
| how the app runs on the VM | **directly under systemd**, no Docker. isolate needs cgroup control, which inside Docker would mean a privileged container. The remaining risk is a kernel exploit escaping isolate onto a VM that holds no secrets; accepted for a personal project shared with a few users. |
| concurrency | **2 runs at once**; further runs wait in a queue and see their place in it |
| per-run limits | **120 s wall time, 90 s CPU, 1.5 GB memory** (after U0's memory fix), **20 MB of output, 100 KB of strategy code**, a **30 s cooldown** per session |
| data exposure | **accepted.** User code can read the day's Parquet files inside its sandbox. That's acceptable for a personal project shown to a few users with one day of data; the output cap stays. |
| data offered | **2026-09-29: AAPL, NVDA, TSLA** only; more days later |
| repository | **this one** (public): `webapp/` and `deploy/` beside the engine, so engine and app changes ship together |
| deployment | **GitHub Actions over SSH**: build the ARM wheel on GitHub's runner, install it on the VM as a new release, smoke test, switch, roll back on failure |

---

## Architecture
```
browser ── Cloudflare (HTTPS, per-IP rate limit, DDoS) ── Oracle ARM VM
                                                              ├─ Caddy  :443 → :8501
                                                              └─ Streamlit app (one process, one thread per visitor)
                                                                   └─ runner: waits for a free slot (at most N runs at once)
                                                                        └─ isolate box per run
                                                                             python -m matching_engine.cli run
                                                                               --config run.toml --out result/
                                                                             · reads  /data (read-only, Databento Parquet)
                                                                             · writes /box  (run.toml, strategy.py, result/)
                                                                             · no network, empty environment, cgroup limits
```

Why no separate job queue or worker service:
- Streamlit runs every visitor's session as a thread in one process.
- A module-level semaphore in the runner is the queue: a run waits for a slot, shows its
  place in the line, and then starts its sandbox.
- With results shown on the page and no history, there is nothing a database or a separate
  worker would hold.
- If either is ever wanted, the runner interface below doesn't change.

---

## Phase U0: backend changes the UI needs
In `python/matching_engine/cli.py` and `backtest.py`, plus tests.

1. **`me-backtest run --out DIR`** writes machine-readable results into `DIR`:
   - `result.json`:
     - `summary`: the report's numbers
     - `stats`: the reconciliation counters
     - `config`: the effective settings, after flags
     - `timings`
     - `equity`: the sampled curve, as `[ts, position, cash, mid, equity]` rows
     - `rejects`: counts by reason
     - `fills_by_source`
   - `fills.csv` and `orders.csv`, the same columns as today's `--fills-csv` and
     `--orders-csv`.

   The text report still prints unless `--quiet` is given.
2. **Structured failures.** A strategy that fails to import, raises, or has bad parameters
   leaves `result.json` with `{"error": {"kind": ..., "message": ..., "traceback": ...}}`,
   with the traceback trimmed to the strategy's own frames, and exits non-zero. The UI
   shows this, not a raw stack dump.
3. **`--progress FILE`.** Roughly every second of wall time the run writes its current
   exchange time and the window start and end. The UI turns that into a progress bar.
4. **Find and fix the 2.1 GB peak in the CLI path.** Measured: with the data loaded first,
   a run needs about 600–700 MB and stays flat. The `me-backtest` path peaked at 2.1 GB
   for a reason not yet found.
   - It has to be found before the sandbox's memory limit is set, or every run would hit
     the limit.
   - Also cut pyarrow's temporary copies when loading: read by row group into
     pre-allocated arrays.
   - Target: under 1 GB per AAPL run.

## Phase U1: the runner (`webapp/runner.py`)
One function the UI calls, with two backends behind it.

```python
@dataclass
class RunRequest:
    settings: dict          # the [data]/[session]/[latency]/[risk]/[fees]/[model] values
    strategy_code: str
    strategy_params: dict

@dataclass
class RunResult:
    ok: bool
    result: dict | None     # result.json
    fills_csv: bytes | None
    orders_csv: bytes | None
    error: dict | None      # strategy error, timeout, memory limit, sandbox failure
    queue_wait_s: float
    run_s: float

def run(request: RunRequest, on_status: Callable[[Status], None]) -> RunResult
```

1. **Validate and build the run directory.**
   - Every setting is range-checked: symbol and date must exist under `data/`, the window
     must lie in 04:00–20:00 ET, timer at least 1 ms, latencies 0–1 s, limits ≥ 0.
   - `strategy.py` gets the code (at most 100 KB). `run.toml` is generated, never taken
     from the visitor.
2. **Wait for a slot.**
   - A module-level `threading.BoundedSemaphore(N)`.
   - `on_status` reports the place in the line and then "running".
   - A visitor already waiting or running can't start another (one active run per
     session).
3. **Execute.**
   - **`LocalBackend`** (development, tests): `subprocess` with `resource.setrlimit` for
     memory and CPU, a wall-clock timeout, and an empty environment. Not a sandbox, so it
     is only ever used locally.
   - **`IsolateBackend`** (production):
     - `isolate --cg --box-id=K --init`
     - copy in `strategy.py` and `run.toml`
     - `isolate --cg --run` with the options below
     - collect `result/`
     - `--cleanup`

     Options for the run:

     | option | purpose |
     |---|---|
     | `--cg-mem=<mem limit>` | memory limit |
     | `--time=<cpu limit>`, `--wall-time=<wall limit>` | time limits |
     | `--processes=<small n>` | pyarrow and numpy start threads |
     | `--fsize=<cap on output files>` | output size limit |
     | `--dir=/data=<data dir>` | data, read-only |
     | `--dir=/opt/me=<venv>` | the virtualenv, read-only |
     | `--env=PATH=…` and `PYTHONHASHSEED=0` | the only variables set; everything else stripped |

     isolate gives no network by default, and its own PID and mount namespaces.
   - Box ids 0..N-1 are handed out with the semaphore slots.
4. **Map outcomes to errors the UI can show:**
   - strategy error (from `result.json`)
   - time limit, with the run time
   - memory limit, with the limit
   - output too large
   - sandbox failure: logged on the server, shown as "internal error"
5. **Progress.** The runner polls the box's progress file and passes it to `on_status`.

## Phase U2: the Streamlit app (`webapp/app.py`)
**Sidebar: settings.** Every non-strategy parameter, defaulting to
`strategies/ob_alpha.toml`'s values.

| group | widgets |
|---|---|
| Data | date (from the days present in `data/`), symbol (from that day's files) |
| Session | start and end time (New York), timer ms |
| Latency | order µs, market-data µs |
| Risk | max position (shares), max capital (USD), max and min order quantity, max price deviation ($) |
| Fees | maker and taker, $/share (negative = rebate) |
| Model | passive impact (on), self-trade prevention (off), PnL sample ms |

**Main area:**
1. **Strategy editor:** prefilled with `ob_alpha.py`, with an upload button for a `.py`
   file. The editor is the `streamlit-code-editor` component, or `st.text_area` if that
   component misbehaves.
2. **Strategy parameters:** a TOML text box, prefilled from `ob_alpha.toml`'s
   `[strategy.params]`.
3. **A short "how to write a strategy" panel,** taken from `docs/backtesting.md` (the
   `ctx` table and the reject reasons).
4. **Run** (disabled while this visitor has a run in progress), then a status line: queued
   (position k), then a progress bar from the exchange-time progress file, then done or the
   error.

**Results:**
- Metric tiles: PnL, max drawdown, final position, fills, fill ratio, maker share.
- Equity curve and position over the day, from `result.json["equity"]`, with Streamlit's
  built-in charts.
- Filled quantity by source, and reject reasons, as bars or tables.
- Reconciliation counters (orphaned orders and size, sweeps, clamped cancels, venue
  anomalies), each with a one-line explanation.
- Fills and orders as tables with download buttons, previewing the first rows.
- On error: the message and the trimmed traceback.

Packaging: a `[web]` extra in `pyproject.toml` (`streamlit`, plus the code-editor
component), and `webapp/` stays outside the wheel.

## Phase U3: abuse protection
- **Cloudflare (free):** HTTPS, caching of static assets, and a rate-limiting rule on the
  app, e.g. at most a few Run requests per IP per minute. The run is triggered over
  Streamlit's websocket, so the server-side checks below are the real control.
- **Per session:** one active run, plus a cooldown between runs (e.g. 30 s).
- **Global:** at most 2 concurrent runs. Further runs wait in a queue and see their place
  in it. The queue has a maximum length (default 20), and a visitor who joins past it is
  told to try later.
- **Limits enforced by isolate:** CPU time, wall time, memory, process count, file size, no
  network.
- **The VM itself:**
  - the app runs as an unprivileged user
  - SSH by key only
  - the Oracle security list and the host firewall open only 443 (and 22)
  - automatic security updates
- **The data:** only results leave the box, never `/data` itself. A visitor's code can read
  the Parquet files inside its sandbox and could write parts of them into its results,
  capped by the 20 MB output limit. Accepted for this project (see *Decided*).

## Phase U4: deployment (one-time setup, then CI/CD over SSH)
The repo stays public (it is linked from a resume). The deployment code lives in it next
to the app:
```
webapp/                          Streamlit app and runner (not in the pip package)
deploy/provision.sh              one-time VM setup, run by hand
deploy/deploy.sh                 install a built wheel as a new release, on the VM
deploy/systemd/me-web.service    the app
deploy/Caddyfile                 HTTPS reverse proxy
deploy/sudoers.d/me-deploy       what the deploy user may run
docs/deploy.md                   step-by-step guide, written for a first deployment
.github/workflows/deploy.yml     the CD job
```

### U4a. One-time setup (by hand, guided by `docs/deploy.md`)
1. **The VM:** Oracle Always Free, Ampere A1, 2 OCPU / 12 GB (of the free 4 / 24), Ubuntu
   24.04 ARM, 50–100 GB boot volume. SSH key login.
2. **`deploy/provision.sh`,** run once as root on the VM:
   - packages: Python 3.12, build tools for isolate, Caddy, fail2ban, unattended-upgrades
   - **isolate,** built from source for cgroup v2, with its systemd service
   - **users:**
     - `me-web`: unprivileged; runs the app; no shell or SSH login
     - `me-deploy`: SSH login with the CI key only, no password; may write to
       `/opt/me/releases/` and run exactly one root command through sudo,
       `/opt/me/deploy.sh`, with fixed arguments
   - **SSH hardening:** key-only login, no root login, `AllowUsers` limited to your admin
     user and `me-deploy`, fail2ban on SSH
   - **firewall:** open 443 (and 22) in both the Oracle security list and the VM's
     iptables (Oracle's Ubuntu images block everything except 22 by default)
   - **layout:** `/opt/me/releases/<git-sha>/` (one virtualenv per release),
     `/opt/me/current` (link to the active one), `/srv/me/data/` (Parquet files, readable
     by `me-web`), and isolate's box root
   - **services:** `me-web.service` runs Streamlit from `/opt/me/current`, bound to
     127.0.0.1:8501; Caddy serves 443 and forwards to it
3. **Data:** `rsync` the converted `data/databento/2026-09-29/*.parquet` from your laptop
   to `/srv/me/data/` once. Data never goes through the repo or CI.
4. **DNS and HTTPS:** a domain, or a free subdomain, in Cloudflare with the record
   proxied. SSH goes to the VM's IP directly, since Cloudflare's proxy only carries web
   traffic.

### U4b. Continuous deployment (`.github/workflows/deploy.yml`)
**Triggers:** a push to `main` (after the existing CI passes), or a manual run. Never on
pull requests.

```
1. build   (GitHub's ARM runner, ubuntu-24.04-arm)
     pip wheel . → matching_engine-<sha>-cp312-linux_aarch64.whl
     smoke test the wheel in a fresh virtualenv (import, a tiny synthetic backtest)
2. deploy  (needs: build; environment: production)
     scp the wheel and webapp/ to me-deploy@VM:/opt/me/releases/<sha>/
     ssh me-deploy@VM sudo /opt/me/deploy.sh <sha>
         creates the virtualenv for <sha> and installs the wheel ([backtest,web] extras)
         runs a smoke backtest inside isolate (proves the sandbox works on this release)
         points /opt/me/current at <sha>, restarts me-web
         checks https://localhost via Caddy; on failure switches the link back and restarts
         keeps the last 3 releases, deletes older ones
3. verify  curl the public URL through Cloudflare: expect 200
```
- **Builds happen on GitHub's runner,** so the VM never compiles during a deploy and holds
  no build tools beyond those isolate needed once. The free ARM runners are available for
  public repos; confirm this when setting it up.
- **Rollback by hand:** run `deploy.sh` again with an earlier sha (any of the last 3
  releases).

**Secrets** (GitHub Actions secrets, in a `production` environment, never in files):
- `DEPLOY_SSH_KEY`: private key for `me-deploy`
- `DEPLOY_HOST`: the VM's IP address, kept out of the public workflow file
- `DEPLOY_KNOWN_HOSTS`: the VM's SSH host key, so the job verifies it is talking to your VM

### Public-repo safety rules
- Secrets only in Actions secrets. The deploy job never prints them, and `set -x` stays
  off in steps that use them.
- The deploy runs only on `push` to `main` and on manual runs. Pull requests from forks
  never get secrets.
- Branch protection on `main`: changes only through pull requests, with CI passing.
  Optionally, the `production` environment requires your approval before each deploy.
- Third-party actions are pinned to a full commit hash, not a tag.
- The CI key can only deploy, through `me-deploy`'s single sudo rule. It can't open a root
  shell or read the data.
- Never commit the Databento key, the data, or `.env` files. Add `logs/` to `.gitignore`.
  (Checked on 2026-10-03: no key or data in any commit; `data/` is ignored.)

### Oracle-specific notes
- Ubuntu images ship iptables rules that block ports other than 22; open 443 there as well
  as in the security list.
- Always Free VMs that stay idle may be reclaimed unless the account is upgraded to
  pay-as-you-go. The free resources stay free.
- Ampere capacity is often short in popular regions; pick another region, or retry.

## Verification
- **U0:**
  - `--out` matches the text report, checked on OB alpha.
  - A failing strategy produces the error JSON.
  - Peak memory per run is measured on AAPL and NVDA and stays under the limit chosen.
- **U1:**
  - Tests with `LocalBackend`: validation rejects bad settings; a timeout, a memory blow-up
    and a raising strategy each map to the right error; the semaphore orders concurrent
    requests.
  - On the VM, sandbox checks with deliberately hostile strategies, each of which must
    fail:
    - open a socket
    - read an environment variable
    - write outside the box
    - start many processes
    - allocate past the memory limit
    - spin forever
- **U2:** manual run through the full form on the VM, plus a Streamlit `AppTest` smoke test
  (render the form, submit with `LocalBackend` and a tiny synthetic data set, check the
  metric tiles).
- **U4:**
  - a first deploy from a manual run of the workflow
  - a deliberately broken release (failing smoke test) rolls back on its own
  - the site answers over HTTPS through Cloudflare
  - from outside, only 22 and 443 respond on the VM
  - SSH with a password is refused
- **U3/U4:**
  - two browsers running at once (one queues)
  - a rapid second run is rejected by the cooldown
  - the site is reachable over HTTPS through Cloudflare, and nothing else is reachable on
    the VM

---

## Progress
U0–U2 done on `feature/web-ui` (2026-10-03). A running UI locally with `LocalBackend`:
- **Loading:** reads the data one row group at a time; the load peak dropped from about
  590 MB to 415 MB.
- **Memory:** the earlier 2.1 GB CLI peak no longer reproduces, and is still unexplained.
  The runner now records each run's exact peak (`peak_mb`).
- **Real runs through the runner:** OB alpha on AAPL takes 21.5 s and peaks at 508 MB;
  NVDA takes 23.5 s and 588 MB. Both are well inside the 120 s and 1.5 GB limits, and the
  AAPL PnL matches the CLI.

Next: U3 (abuse protection) and U4 (isolate backend, provisioning, deploy workflow), once a
VM is chosen.

## Decisions log
All five open questions were answered on 2026-10-03 and are recorded in the table under
*Decided* at the top.
