"""Runs one backtest per request, each in its own limited process.

The UI hands over the settings, the strategy's code and its parameters; the
runner validates them, waits for a free slot, executes
`python -m matching_engine.cli run --out ...` in a fresh directory, and turns
whatever happened into a RunResult the page can show.

    runner = Runner(data_dir=Path("data/databento"))
    result = runner.run(settings, code, params, on_status=print)

Two things are deliberately separate:

* Queueing: at most `max_concurrent` runs execute at once (2 by default, one
  per core); up to `max_queue` more wait in arrival order and are told their
  place in line. Every visitor's Streamlit session is a thread in one process,
  so one Runner shared by all of them is the whole queue.
* Execution: a backend starts the process and enforces the limits.
  LocalBackend (here) uses an empty environment, rlimits and a watchdog. It is
  NOT a sandbox: the code can still read files and use the network. It is for
  running the UI on your own machine. Production (docs/ui-plan.md, U4) adds an
  isolate backend with namespaces and cgroups.
"""

from __future__ import annotations

import json
import math
import os
import queue
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from collections import deque
from dataclasses import asdict, dataclass, field
from datetime import time as Time
from pathlib import Path
from typing import Any, Callable, Optional


# --- what a visitor can set ---------------------------------------------------

@dataclass
class Settings:
    """Every non-strategy parameter of a run (the TOML sections of a backtest config)."""
    date: str
    symbol: str
    start: str = "09:31:00"
    end: str = "15:59:00"
    timer_ms: float = 10.0
    order_latency_us: float = 50.0
    md_latency_us: float = 20.0
    max_position: int = 2_000
    max_capital: float = 0.0
    max_order_qty: int = 10_000
    min_order_qty: int = 1
    max_price_deviation: float = 1.0
    maker_fee: float = 0.0
    taker_fee: float = 0.0
    passive_impact: bool = True
    self_trade_prevention: bool = False
    pnl_sample_ms: float = 1_000.0


@dataclass
class Limits:
    wall_s: float = 120.0
    cpu_s: float = 90.0
    memory_mb: int = 1_536
    output_mb: int = 20
    code_kb: int = 100


def available_data(data_dir: Path) -> dict[str, list[str]]:
    """{date: [symbols]} for every converted day under data_dir (scripts/convert_mbo.py)."""
    days: dict[str, list[str]] = {}
    if not data_dir.is_dir():
        return days
    for day in sorted(p for p in data_dir.iterdir() if p.is_dir() and p.name[:4].isdigit()):
        symbols = sorted(f.name.split(".")[0] for f in day.glob("*.mbo.parquet"))
        if symbols:
            days[day.name] = symbols
    return days


def _clock(text: str) -> Optional[Time]:
    try:
        return Time.fromisoformat(text)
    except (TypeError, ValueError):
        return None


def _finite(x: Any) -> bool:
    return isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x)


def _plain(value: Any, depth: int = 0) -> bool:
    """Strategy parameters must be plain TOML-able data: str, int, float, bool, lists, tables."""
    if depth > 5:
        return False
    if isinstance(value, bool) or isinstance(value, str):
        return True
    if isinstance(value, (int, float)):
        return _finite(value)
    if isinstance(value, list):
        return all(_plain(v, depth + 1) for v in value)
    if isinstance(value, dict):
        return all(isinstance(k, str) and _plain(v, depth + 1) for k, v in value.items())
    return False


def validate(settings: Settings, code: str, params: dict[str, Any], data_dir: Path, limits: Limits) -> list[str]:
    """Every problem with a request, in words a visitor can act on. Empty means valid."""
    errors: list[str] = []
    days = available_data(data_dir)
    if settings.date not in days:
        errors.append(f"No data for {settings.date}; available: {', '.join(days) or 'none'}.")
    elif settings.symbol not in days[settings.date]:
        errors.append(f"No {settings.symbol} data on {settings.date}; available: {', '.join(days[settings.date])}.")

    start, end = _clock(settings.start), _clock(settings.end)
    session_open, session_close = Time(4, 0), Time(20, 0)
    if start is None or end is None:
        errors.append("Start and end must be New York times as HH:MM:SS.")
    elif not (session_open <= start < end <= session_close):
        errors.append("The window must satisfy 04:00:00 <= start < end <= 20:00:00 (New York).")

    def check(name: str, value: Any, low: float, high: float) -> None:
        if not _finite(value) or not (low <= value <= high):
            errors.append(f"{name} must be between {low:g} and {high:g}.")

    check("Timer interval (ms)", settings.timer_ms, 1, 60_000)
    check("Order latency (µs)", settings.order_latency_us, 0, 1_000_000)
    check("Market-data latency (µs)", settings.md_latency_us, 0, 1_000_000)
    check("Max position (shares)", settings.max_position, 0, 10_000_000)
    check("Max capital ($)", settings.max_capital, 0, 1e10)
    check("Max order quantity", settings.max_order_qty, 1, 10_000_000)
    check("Min order quantity", settings.min_order_qty, 1, 10_000_000)
    check("Max price deviation ($)", settings.max_price_deviation, 0.01, 10_000)
    check("Maker fee ($/share)", settings.maker_fee, -0.1, 0.1)
    check("Taker fee ($/share)", settings.taker_fee, -0.1, 0.1)
    check("PnL sample interval (ms)", settings.pnl_sample_ms, 100, 3_600_000)
    if _finite(settings.min_order_qty) and _finite(settings.max_order_qty) and settings.min_order_qty > settings.max_order_qty:
        errors.append("Min order quantity can't exceed max order quantity.")

    if not code.strip():
        errors.append("The strategy code is empty.")
    elif len(code.encode()) > limits.code_kb * 1024:
        errors.append(f"The strategy code is over {limits.code_kb} KB.")
    if not _plain(params):
        errors.append("Strategy parameters must be plain values: text, numbers, true/false, lists and tables.")
    return errors


# --- run.toml --------------------------------------------------------------------

def _toml(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, str):
        return json.dumps(value)   # JSON string escapes are valid TOML basic strings
    if isinstance(value, list):
        return "[" + ", ".join(_toml(v) for v in value) + "]"
    if isinstance(value, dict):
        return "{ " + ", ".join(f"{json.dumps(k)} = {_toml(v)}" for k, v in value.items()) + " }"
    raise TypeError(f"cannot write {type(value).__name__} to TOML")


def run_config(settings: Settings, params: dict[str, Any], data_dir: "Path | str") -> str:
    """The run's config file. Generated here, never taken from the visitor."""
    s = settings
    sections = {
        "data": {"date": s.date, "symbol": s.symbol, "dir": str(data_dir)},
        "session": {"start": s.start, "end": s.end, "timer_ms": s.timer_ms},
        "latency": {"order_us": s.order_latency_us, "market_data_us": s.md_latency_us},
        "risk": {"max_position": s.max_position, "max_capital": s.max_capital, "max_order_qty": s.max_order_qty,
                 "min_order_qty": s.min_order_qty, "max_price_deviation": s.max_price_deviation},
        "fees": {"maker": s.maker_fee, "taker": s.taker_fee},
        "model": {"passive_impact": s.passive_impact, "self_trade_prevention": s.self_trade_prevention},
        "output": {"pnl_sample_ms": s.pnl_sample_ms},
        "strategy": {"path": "strategy.py"},
    }
    lines = []
    for name, table in sections.items():
        lines.append(f"[{name}]")
        lines.extend(f"{key} = {_toml(value)}" for key, value in table.items())
        lines.append("")
    lines.append("[strategy.params]")
    lines.extend(f"{json.dumps(key)} = {_toml(value)}" for key, value in params.items())
    return "\n".join(lines) + "\n"


# --- execution -------------------------------------------------------------------

PROGRESS_PREFIX = "ME-PROGRESS"   # matching_engine.cli writes these on stderr with --progress -
STDERR_TAIL_LINES = 200


@dataclass
class Outcome:
    returncode: int
    wall_s: float
    peak_mb: int
    killed_for: Optional[str] = None   # "time", "cpu", "memory", "output" or None
    stderr_tail: str = ""              # what the run wrote on stderr, minus progress lines


class _StderrReader(threading.Thread):
    """Reads a child's stderr as it runs: progress lines go to on_progress, the rest
    is kept (only the last lines, so a chatty strategy can't use up memory)."""

    def __init__(self, stream, on_progress: Callable[[float], None]):
        super().__init__(daemon=True)
        self.stream, self.on_progress = stream, on_progress
        self.lines: deque[str] = deque(maxlen=STDERR_TAIL_LINES)

    def run(self) -> None:
        for raw in self.stream:
            line = raw.decode(errors="replace").rstrip("\n")
            if line.startswith(PROGRESS_PREFIX + " "):
                try:
                    self.on_progress(float(line.split()[1]))
                except (IndexError, ValueError):
                    pass
            else:
                self.lines.append(line)

    def text(self) -> str:
        return "\n".join(self.lines)


class LocalBackend:
    """A child process with an empty environment, rlimits and a watchdog. Not a sandbox:
    the code can still read files and use the network. For running the UI locally."""

    poll_s = 0.2

    def visible_data_dir(self, data_dir: Path) -> str:
        return str(data_dir)

    def execute(self, run_dir: Path, command: list[str], limits: Limits,
                on_progress: Callable[[float], None]) -> Outcome:
        env = {"PATH": "/usr/bin:/bin", "HOME": str(run_dir), "LANG": "C.UTF-8",
               "PYTHONHASHSEED": "0", "PYTHONDONTWRITEBYTECODE": "1"}
        log = open(run_dir / "log.txt", "wb")
        started = time.monotonic()
        proc = subprocess.Popen([sys.executable, *command], cwd=run_dir, env=env, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.PIPE, start_new_session=True)
        # Limits are set on the child from here rather than with preexec_fn, which is
        # unsafe in a multi-threaded parent such as Streamlit. Python's own start-up
        # takes far longer than this call, so no user code runs before it applies.
        cpu = int(limits.cpu_s)
        size = limits.output_mb * 1024 * 1024
        try:
            resource.prlimit(proc.pid, resource.RLIMIT_CPU, (cpu, cpu + 5))
            resource.prlimit(proc.pid, resource.RLIMIT_FSIZE, (size, size))
        except ProcessLookupError:
            pass   # already gone; wait4 below reports how it ended
        reader = _StderrReader(proc.stderr, on_progress)
        reader.start()
        killed_for = None
        try:
            while True:
                # wait4 rather than Popen.wait: it also returns the child's exact peak memory.
                pid, status, usage = os.wait4(proc.pid, os.WNOHANG)
                if pid:
                    proc.returncode = os.waitstatus_to_exitcode(status)
                    reader.join(timeout=2)
                    code = proc.returncode
                    if killed_for is None and code == -signal.SIGXCPU:
                        killed_for = "cpu"
                    elif killed_for is None and code == -signal.SIGXFSZ:
                        killed_for = "output"
                    return Outcome(code, time.monotonic() - started, usage.ru_maxrss // 1024, killed_for,
                                   reader.text())
                if killed_for is None:
                    if time.monotonic() - started > limits.wall_s:
                        killed_for = "time"
                    elif _rss_mb(proc.pid) > limits.memory_mb:
                        killed_for = "memory"
                    if killed_for:
                        _kill_group(proc.pid)
                time.sleep(self.poll_s)
        finally:
            log.close()
            if proc.returncode is None:   # interrupted while waiting: never leave it running
                _kill_group(proc.pid)
                os.waitpid(proc.pid, 0)


class IsolateBackend:
    """Each run in an isolate box (https://github.com/ioi/isolate): its own PID, mount and
    network namespaces (no network), a UID of its own, cgroup limits on memory and
    processes, CPU and wall-clock limits, a file-size limit, and an empty environment.
    Inside the box: the system's /usr and /lib (read-only), the virtualenv (read-only),
    the data at /data (read-only) and a private /box working directory.

    Box ids come from a pool of `boxes` (one per concurrent run), starting at
    `first_box`. Anything else using isolate on the same machine (the post-deploy
    check) must use other ids: initialising a box wipes whatever is in it."""

    def __init__(self, boxes: int, data_dir: Path, isolate: str = "isolate", venv: Optional[Path] = None,
                 first_box: int = 0):
        self.data_dir = Path(data_dir).resolve()   # mounted read-only at /data
        self.isolate = isolate
        # Resolved: /opt/me/current is a symlink, and the box must mount the real release.
        self.venv = Path(venv or sys.prefix).resolve()
        self._free: "queue.Queue[int]" = queue.Queue()
        for box in range(first_box, first_box + boxes):
            self._free.put(box)

    def visible_data_dir(self, data_dir: Path) -> str:
        return "/data"

    def _isolate(self, box: int, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([self.isolate, "--cg", f"--box-id={box}", *args],
                              capture_output=True, text=True, check=False)

    def execute(self, run_dir: Path, command: list[str], limits: Limits,
                on_progress: Callable[[float], None]) -> Outcome:
        box = self._free.get()
        try:
            return self._execute(box, run_dir, command, limits, on_progress)
        finally:
            self._isolate(box, "--cleanup")
            self._free.put(box)

    def _execute(self, box: int, run_dir: Path, command: list[str], limits: Limits,
                 on_progress: Callable[[float], None]) -> Outcome:
        self._isolate(box, "--cleanup")   # a crashed earlier run may have left it initialised
        init = self._isolate(box, "--init")
        if init.returncode != 0:
            raise RuntimeError(f"isolate --init failed: {init.stderr.strip()}")
        box_dir = Path(init.stdout.strip()) / "box"
        for name in ("strategy.py", "run.toml"):
            shutil.copy(run_dir / name, box_dir / name)

        meta = run_dir / "meta.txt"
        env = {"PATH": "/usr/bin:/bin", "HOME": "/box", "LANG": "C.UTF-8", "PYTHONHASHSEED": "0",
               "PYTHONDONTWRITEBYTECODE": "1", "OMP_NUM_THREADS": "1", "OPENBLAS_NUM_THREADS": "1"}
        args = [self.isolate, "--cg", f"--box-id={box}", "--run", f"--meta={meta}",
                f"--time={limits.cpu_s:g}", "--extra-time=2", f"--wall-time={limits.wall_s:g}",
                f"--cg-mem={limits.memory_mb * 1024}", f"--fsize={limits.output_mb * 1024}",
                "--processes=64",   # threads count too: pyarrow and numpy start a few
                f"--dir=/data={self.data_dir}", f"--dir={self.venv}", "--stdout=log.txt",
                *[f"--env={k}={v}" for k, v in env.items()],
                "--", str(self.venv / "bin" / "python"), *command]
        started = time.monotonic()
        proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        reader = _StderrReader(proc.stderr, on_progress)
        reader.start()
        proc.wait()
        reader.join(timeout=2)
        wall = time.monotonic() - started

        # The box is the caller's again once the run is over: bring the results out.
        (run_dir / "result").mkdir(exist_ok=True)
        if (box_dir / "result").is_dir():
            for f in (box_dir / "result").iterdir():
                shutil.copy(f, run_dir / "result" / f.name)
        if (box_dir / "log.txt").exists():
            shutil.copy(box_dir / "log.txt", run_dir / "log.txt")
        info = _parse_meta(meta)
        killed_for = None
        if info.get("cg-oom-killed") == "1":
            killed_for = "memory"
        elif info.get("status") == "TO":
            killed_for = "cpu" if float(info.get("time", 0)) >= limits.cpu_s else "time"
        elif info.get("exitsig") == str(int(signal.SIGXFSZ)):
            killed_for = "output"
        code = int(info["exitcode"]) if "exitcode" in info else -int(info.get("exitsig", 0) or 0)
        peak_kb = int(info.get("cg-mem") or info.get("max-rss") or 0)
        # isolate's own one-line verdict ("OK (…)", "Time limit exceeded", …) is not the strategy's output.
        tail = reader.text().splitlines()
        if tail and tail[-1].split(" ", 1)[0] in {"OK", "Time", "Wall", "Caught", "Exited", "Out", "Memory"}:
            tail = tail[:-1]
        return Outcome(code, wall, peak_kb // 1024, killed_for, "\n".join(tail))


def _parse_meta(path: Path) -> dict[str, str]:
    info: dict[str, str] = {}
    try:
        for line in path.read_text().splitlines():
            key, _, value = line.partition(":")
            info[key] = value
    except OSError:
        pass
    return info


def _rss_mb(pid: int) -> int:
    try:
        with open(f"/proc/{pid}/status") as fh:
            for line in fh:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) // 1024
    except OSError:
        pass
    return 0


def _kill_group(pid: int) -> None:
    try:
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


# --- the runner ------------------------------------------------------------------

@dataclass
class Status:
    state: str                 # "queued", "running" or "finished"
    position: int = 0          # place in line while queued (1 = next)
    fraction: float = 0.0      # replay progress while running
    elapsed_s: float = 0.0


@dataclass
class RunResult:
    ok: bool
    result: Optional[dict] = None        # result.json of a successful run
    fills_csv: Optional[bytes] = None
    orders_csv: Optional[bytes] = None
    error: Optional[dict] = None         # {"kind", "message", "location", "traceback"}
    log: str = ""                        # the end of what the strategy printed
    queue_wait_s: float = 0.0
    run_s: float = 0.0
    peak_mb: int = 0


class QueueFull(Exception):
    pass


class Runner:
    LOG_TAIL_BYTES = 20_000

    def __init__(self, data_dir: Path, backend: Optional[LocalBackend] = None, limits: Optional[Limits] = None,
                 max_concurrent: int = 2, max_queue: int = 20, work_dir: Optional[Path] = None):
        self.data_dir = Path(data_dir).resolve()
        self.backend = backend or LocalBackend()
        self.limits = limits or Limits()
        self.max_concurrent = max_concurrent
        self.max_queue = max_queue
        self.work_dir = work_dir
        self._cond = threading.Condition()
        self._waiting: list[object] = []
        self._running = 0

    # Queue ------------------------------------------------------------------
    def _acquire(self, on_status: Callable[[Status], None]) -> float:
        ticket = object()
        started = time.monotonic()
        with self._cond:
            if len(self._waiting) >= self.max_queue:
                raise QueueFull()
            self._waiting.append(ticket)
        last = None
        while True:
            with self._cond:
                if self._waiting[0] is ticket and self._running < self.max_concurrent:
                    self._waiting.pop(0)
                    self._running += 1
                    self._cond.notify_all()
                    return time.monotonic() - started
                position = self._waiting.index(ticket) + 1
            if position != last:   # report outside the lock: the callback touches the page
                on_status(Status("queued", position=position))
                last = position
            with self._cond:
                self._cond.wait(timeout=1.0)

    def _release(self) -> None:
        with self._cond:
            self._running -= 1
            self._cond.notify_all()

    def queue_length(self) -> tuple[int, int]:
        """(running, waiting), for showing load on the page."""
        with self._cond:
            return self._running, len(self._waiting)

    # A run ------------------------------------------------------------------
    def run(self, settings: Settings, code: str, params: dict[str, Any],
            on_status: Callable[[Status], None] = lambda s: None) -> RunResult:
        problems = validate(settings, code, params, self.data_dir, self.limits)
        if problems:
            return RunResult(False, error={"kind": "invalid", "message": " ".join(problems),
                                           "location": None, "traceback": []})
        try:
            waited = self._acquire(on_status)
        except QueueFull:
            return RunResult(False, error={"kind": "busy", "message": "Too many runs are waiting. Try again in a minute.",
                                           "location": None, "traceback": []})
        run_dir = Path(tempfile.mkdtemp(prefix="me-run-", dir=self.work_dir))
        try:
            started = time.monotonic()
            on_status(Status("running"))
            (run_dir / "strategy.py").write_text(code)
            (run_dir / "run.toml").write_text(
                run_config(settings, params, self.backend.visible_data_dir(self.data_dir)))
            # The backend supplies the interpreter (the host's, or the virtualenv inside the box).
            command = ["-m", "matching_engine.cli", "run", "--config", "run.toml",
                       "--out", "result", "--progress", "-", "--quiet"]
            outcome = self.backend.execute(
                run_dir, command, self.limits,
                lambda f: on_status(Status("running", fraction=f, elapsed_s=time.monotonic() - started)))
            result = self._collect(run_dir, outcome)
            result.queue_wait_s = waited
            on_status(Status("finished", fraction=1.0, elapsed_s=outcome.wall_s))
            return result
        finally:
            self._release()
            shutil.rmtree(run_dir, ignore_errors=True)

    def _collect(self, run_dir: Path, outcome: Outcome) -> RunResult:
        log_path = run_dir / "log.txt"
        log = log_path.read_bytes()[-self.LOG_TAIL_BYTES:].decode(errors="replace") if log_path.exists() else ""
        if outcome.stderr_tail.strip():
            log = (log.rstrip("\n") + "\n" if log.strip() else "") + outcome.stderr_tail[-self.LOG_TAIL_BYTES:]
        base = dict(log=log, run_s=outcome.wall_s, peak_mb=outcome.peak_mb)
        limits = self.limits

        def failure(kind: str, message: str) -> RunResult:
            return RunResult(False, error={"kind": kind, "message": message, "location": None, "traceback": []}, **base)

        if outcome.killed_for == "time":
            return failure("time_limit", f"The run was stopped after {limits.wall_s:g} s (the time limit).")
        if outcome.killed_for == "memory":
            return failure("memory_limit", f"The run was stopped for using over {limits.memory_mb} MB of memory.")
        if outcome.killed_for == "cpu":
            return failure("cpu_limit", f"The run was stopped after {limits.cpu_s:g} s of CPU time (the limit).")
        if outcome.killed_for == "output":
            return failure("output_limit", f"The run wrote more than {limits.output_mb} MB.")

        out = run_dir / "result"
        size = sum(f.stat().st_size for f in out.glob("*")) if out.is_dir() else 0
        if size > limits.output_mb * 1024 * 1024:
            return failure("output_limit", f"The run wrote more than {limits.output_mb} MB.")
        try:
            payload = json.loads((out / "result.json").read_text())
        except (OSError, ValueError):
            return failure("internal", f"The run ended (exit code {outcome.returncode}) without a result. "
                                       "The log below may say why.")
        if not payload.get("ok"):
            return RunResult(False, error=payload.get("error"), **base)
        return RunResult(True, result=payload, fills_csv=(out / "fills.csv").read_bytes(),
                         orders_csv=(out / "orders.csv").read_bytes(), **base)


def settings_dict(settings: Settings) -> dict[str, Any]:
    return asdict(settings)
