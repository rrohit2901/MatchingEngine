# Design decisions

Every decision behind the Databento replay, the backtester, the web UI and its
deployment, in one place.
- **Status:** **current** (what the code does now) or **superseded** (replaced, with
  what replaced it).
- **Who:** *user* = the project owner decided it; *proposed* = Claude proposed it and the
  user agreed; *Claude* = an implementation choice made while building, with how it was
  disclosed.
- **Dates:** all 2026-10-02 / 10-03.

The plans these came from: [`strategy-replay-plan.md`](strategy-replay-plan.md) (data,
replay, simulator, CLI) and [`ui-plan.md`](ui-plan.md) (UI, sandbox, hosting). How it runs:
[`backtesting.md`](backtesting.md), [`deploy.md`](deploy.md).

## Data

| # | decision | why | who | status |
|---|---|---|---|---|
| 1 | Databento replaces LOBSTER entirely; `parse_lobster.py` and the 2012 sample are deleted | recent data, several symbols, real order ids | user | current |
| 2 | Dataset `XNAS.ITCH`, MBO schema; plus `mbp-1` for validation only | full-depth order-level events; Nasdaq's own top of book to check against | user (`mbp-1`: proposed) | current |
| 3 | Download once, never query Databento during a backtest; cost shown before any spend | cost and reproducibility | user | current |
| 4 | Storage: Parquet, raw `.dbn.zst` kept as the source of truth | typed, compressed, fast to load | proposed | current |
| 5 | One day, 2026-09-29; AAPL, NVDA, TSLA ($1.17) | a recent, clean day (no quarter-end); three liquid names | user ("a recent one"), symbols proposed | current |
| 6 | Request window from UTC midnight to New York midnight | Databento places its MBO snapshot at UTC midnight; New York midnight keeps the whole session | Claude, from Databento's warning | current |
| 7 | Prices as `int32` ticks of 1e-4 $; any non-exact price aborts conversion | fits the engine's `int` prices; exact for Nasdaq | proposed | current |

## Replay and simulator

| # | decision | why | who | status |
|---|---|---|---|---|
| 8 | Venue records go straight to `OrderBook` (no matching, no risk checks) | the venue already matched them | proposed | current |
| 9 | Cancel size = amount removed; executions are `T` → `F` → `C` | measured on the data (`inspect_mbo.py`), not assumed | Claude, measured | current |
| 10 | One symbol per run | keeps the single-symbol engine design | user | current |
| 11 | Strategy callback: a fixed timer, 10 ms by default (configurable) | simple strategies; Python can't keep up with every MBO event | user | current |
| 12 | Fill model: one synthetic aggressor per execution, matched FIFO | gives the strategy queue position and market impact | user | **superseded by 13** |
| 13 | Per-fill reconciliation: strategy orders ahead of the hit order fill first; the shortfall sweeps down the queue; later cancels are clamped or dropped | reproduces Nasdaq exactly when no strategy trades, without assuming plain FIFO (~1% of fills print away from the order's price) | Claude; disclosed after implementing; not objected to | current |
| 14 | `passive_impact = true` by default: passive fills come out of the venue order queued behind | conserves the execution's size (what the user agreed to); the orphaned size it leaves is counted and reported | user | current (Claude had shipped `false` without asking; reverted) |
| 15 | Self-trades execute by default, as on Nasdaq; `self_trade_prevention = true` rejects them | matches the exchange's default (opt-in self-match prevention) | user | current (the plan had prevention always on) |
| 16 | Limit orders only | no unrequested order types | user | current (Claude had added IOC unasked; removed) |
| 17 | USD capital limit per side: shares held at the current mid plus open orders at their limit prices; the order is rejected (`CAPITAL_LIMIT`) when sent; price moves never force trading | "capital deployment", valued at mid | user | current |
| 18 | Strategy orders tracked in the simulator, not an owner tag in `Order`/`OrderManager` | keeps the engine's 32-byte slot and hot path untouched | Claude; disclosed after implementing | current (the plan had the tag) |
| 19 | Trading window 09:31–15:59 New York by default | keeps clear of the opening and closing auctions | proposed | current; **trading halts are not handled** (planned, not built) |
| 20 | Latency: market-data latency is added to the order delay | a strategy that sees time T late acts late; exact, and needs no historical books | proposed | current |
| 21 | Gateway checks: cent price grid, position limit, capital limit, trading window; exchange checks on arrival: `RiskManager` (+ self-trade prevention if on) | where each check happens in reality | proposed | current |

## Web UI

| # | decision | why | who | status |
|---|---|---|---|---|
| 22 | Streamlit; results shown on the page; no email, no stored history | runs take ~25 s; a personal project | user | current |
| 23 | Every non-strategy setting in the sidebar; defaults from `strategies/ob_alpha.toml` | the request | user | current (now from `quote_touch.toml`, with the same values; see 55) |
| 24 | Runs on a background thread polled by a fragment | a widget change reruns the script and would otherwise abort a run | Claude | current |
| 25 | No fills or orders tables; CSV downloads only | the page lagged | user | current |
| 26 | Metrics in rows of 4 with compact values; charts thinned to ~1,500 points; mid chart's y-axis fitted to the data | values were cut off; charts were slow; the mid looked flat from $0 | user reported, Claude fixed | current |
| 27 | Optional run label: heads the results and names the downloads; display only | name runs independently of the class name | user | current |
| 28 | The heading otherwise shows the Strategy class's own name | not hardcoded | — | current |

## Sandbox and limits

| # | decision | why | who | status |
|---|---|---|---|---|
| 29 | isolate (the IOI sandbox) per run, in a long-running app; no container per run | the pattern competitive-programming judges use; millisecond start-up | proposed | current |
| 30 | App runs directly under systemd, no Docker | isolate needs cgroup control (a privileged container otherwise); residual kernel-exploit risk accepted for a small project | user | current |
| 31 | Per run: 120 s wall, 90 s CPU, 20 MB output, 100 KB code, 64 processes/threads, no network | | user (processes: Claude) | current |
| 32 | Per run memory 1 GB; 1 run at a time; queue of 10; 2 GB swap the runs can't use | sized for the t4g.small; measured peak ~430–590 MB | proposed for t4g.small; not explicitly confirmed | current (the plan had 1.5 GB, 2 at once, queue 20, for a larger VM) |
| 33 | Visitor code may read the day's Parquet files inside its box | a personal project, few users, one day | user | current |
| 34 | Box ids: the app from 0, the post-deploy check from 900 | the check must never wipe a live run's box | Claude | current |
| 35 | Fork bomb is contained but reported as `memory_limit` | wording only; no safety impact | user (left as is) | current |

## Abuse protection

| # | decision | why | who | status |
|---|---|---|---|---|
| 36 | Per-IP run limits: 1 running or queued, 30 s apart, 20 per hour | runs are the expensive resource; per-session limits were bypassable with a new tab | user asked for per-IP limits; numbers proposed in PR #11 | current |
| 37 | Client IP from Caddy's `X-Forwarded-For` | Caddy replaces a client-sent value (checked); Streamlit listens on localhost only | Claude, verified | current |
| 38 | Per-IP connection limits in nftables: 30 open, 60 new a minute (burst 120), ports 80/443 | without Cloudflare there is no front layer; Caddy has no built-in rate limit | user asked; numbers proposed in PR #11 | current |
| 39 | Cloudflare in front | | proposed | **superseded by 40** (needs a domain) |

## Hosting and deployment

| # | decision | why | who | status |
|---|---|---|---|---|
| 40 | No domain: `https://<ip-with-dashes>.sslip.io`, Caddy with a Let's Encrypt certificate | the user didn't want to buy a domain | user | current |
| 41 | Hosting on Oracle Cloud Always Free (Ampere A1) | free, roomy | user | **superseded by 42** (no A1 capacity in the home region) |
| 42 | AWS EC2 t4g.small (2 GB, ARM), Elastic IP; up for about a month | only `t4g.small` was available on the account; the site only needs to run until the user's job switch | user | current |
| 43 | Same public repository for engine, app and deployment | they change together; it's linked from a resume | proposed | current |
| 44 | Deploy from GitHub Actions over SSH; port 22 open to all, key-only, no root, two users, fail2ban | simplest; GitHub's runners have no fixed IPs | user ("SSH sounds fine") | current |
| 45 | The deploy key is a forced command (`restrict,command=`): it can only hand over a bundle; a root-owned installer installs it | a leaked key can't get a shell | Claude; announced before implementing | current |
| 46 | Each release is checked in the sandbox before it goes live; switch via `/opt/me/current`; health check; automatic rollback; keep 3 | a broken build never goes live | proposed | current |
| 47 | Wheels built on GitHub's ARM runner with `ME_NATIVE_ARCH=OFF` | the runner's CPU is newer than the Graviton2; `-march=native` could SIGILL | Claude | current |
| 48 | GitHub `production` environment (deploys from `main` only); actions pinned to commit SHAs | public-repo safety | proposed | current |

## Usability and metrics (2026-10-03)

| # | decision | why | who | status |
|---|---|---|---|---|
| 49 | `ctx.pnl` marks at the current mid, not the last equity sample's | it could be up to `pnl_sample_ms` old inside `on_timer` | proposed (a bug) | current |
| 50 | A non-whole quantity is rejected (`QUANTITY_NOT_INTEGER`), not truncated | consistent with off-grid prices (`PRICE_INCREMENT`) | user | current |
| 51 | Every config key has a `me-backtest run` flag | the README said so; fees and order limits had none | proposed (a bug) | current |
| 52 | `ctx.trades`: the tape of the simulated book (executions as reconciled, the strategy's own flagged `own`), plus Nasdaq's hidden prints flagged `hidden` | a tape of Nasdaq's real prints would contradict the simulation wherever the strategy changed the book | user (our book; hidden prints included) | current |
| 53 | `ctx.trades` is built only when read | built every timer call, it cost 1–2 s on a full day | Claude; measured | current |
| 54 | Realized/unrealized PnL at average cost; PnL per share traded; Sharpe of 1-minute PnL changes × √(252 × 390) | standard intraday conventions; Sharpe labelled as one day only | user (methods chosen from options) | current |
| 55 | The page starts with a simple strategy (`quote_touch.py`); an example picker loads it or the order-book alpha | the alpha example overwhelms a first-time visitor | user | current |
| 56 | Code editor: `streamlit-code-editor` (Ace), live autocompletion off | auto-indent; live completion took the Enter key while typing | user (editor); Claude (autocompletion, found in a browser test) | current |
| 57 | Syntax errors are caught by compiling (never running) the code, before the rate limits and the queue | a typo shouldn't cost a slot or the cooldown | user | current |
| 58 | Code and parameters kept in the visitor's browser (localStorage, via an inline `st.components.v2` script); nothing on the server | a reload lost the code | user | current |

## Open

- **The 2.1 GB peak:** seen once in the CLI path, unexplained, and not reproducible since. Each run records its exact peak.
- **Trading halts** aren't handled (decision 19).
- **Replay throughput** (0.7–1.2 M records/s without a strategy) is limited by the venue id map; see the README's *Next*.
