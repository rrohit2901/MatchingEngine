"""Post-deploy check, run on the VM as the app user:

    sudo -u me-web /opt/me/current/venv/bin/python /opt/me/current/src/deploy/sandbox_check.py

1. A real backtest (OB alpha, the first symbol of the latest day) completes
   through isolate with the production memory limit.
2. Hostile strategies are each stopped: network, environment, writing to or
   reading from the host, memory, CPU, wall time, fork bomb.
Exits non-zero if anything is not as expected.
"""

import os
import sys
import time
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC))
from webapp.runner import IsolateBackend, Limits, Runner, Settings, available_data  # noqa: E402

DATA = Path(os.environ.get("ME_DATA_DIR", "/srv/me/data"))
MEMORY_MB = int(os.environ.get("ME_MEMORY_MB", "1024"))


def runner(limits: Limits) -> Runner:
    return Runner(DATA, backend=IsolateBackend(boxes=1, data_dir=DATA), limits=limits, max_concurrent=1)


def strategy(body: str) -> str:
    lines = "".join("        " + line + "\n" for line in body.splitlines())
    return f"from matching_engine.backtest import Strategy\nclass S(Strategy):\n    def on_start(self, ctx):\n{lines}"


def main() -> int:
    days = available_data(DATA)
    if not days:
        print(f"FAIL: no data under {DATA}")
        return 1
    day = sorted(days)[-1]
    symbol = days[day][0]
    failures = 0

    cfg = tomllib.load(open(SRC / "strategies/ob_alpha.toml", "rb"))
    r = runner(Limits(memory_mb=MEMORY_MB)).run(Settings(date=day, symbol=symbol),
                                                (SRC / "strategies/ob_alpha.py").read_text(),
                                                cfg["strategy"]["params"])
    if r.ok:
        print(f"ok    backtest {symbol} {day}: {r.run_s:.1f} s, peak {r.peak_mb} MB, "
              f"PnL ${r.result['summary']['pnl']:,.2f}")
    else:
        failures += 1
        print(f"FAIL  backtest {symbol} {day}: {r.error}")

    short = Settings(date=day, symbol=symbol, start="09:31:00", end="09:32:00")
    m = MEMORY_MB
    # (name, strategy body, limits, expected error kind, text the message must contain)
    cases = [
        ("network", "import socket\nsocket.create_connection(('1.1.1.1', 80), timeout=3)", Limits(memory_mb=m),
         "strategy_runtime", "Network is unreachable"),
        ("environment", "import os\nextra = set(os.environ) - {'HOME','LANG','LIBC_FATAL_STDERR_','OMP_NUM_THREADS',"
                        "'OPENBLAS_NUM_THREADS','PATH','PYTHONHASHSEED','PYTHONDONTWRITEBYTECODE'}\n"
                        "raise RuntimeError(f'env extra={sorted(extra)}' if extra else 'env ok')",
         Limits(memory_mb=m), "strategy_runtime", "env ok"),
        ("host write", f"open('{DATA}/pwned', 'w').write('x')", Limits(memory_mb=m), "strategy_runtime", "Error"),
        ("host read", "open('/etc/shadow').read()", Limits(memory_mb=m), "strategy_runtime", "Error"),
        ("sys.exit", "import sys\nsys.exit(3)", Limits(memory_mb=m), "strategy_runtime", "SystemExit"),
        ("memory", "x = []\nwhile True:\n    x.append(bytearray(50_000_000))", Limits(memory_mb=m), "memory_limit", ""),
        ("cpu", "while True:\n    pass", Limits(cpu_s=5, wall_s=60, memory_mb=m), "cpu_limit", ""),
        ("wall time", "import time\ntime.sleep(60)", Limits(cpu_s=30, wall_s=5, memory_mb=m), "time_limit", ""),
        ("fork bomb", "import os\nwhile True:\n    os.fork()", Limits(cpu_s=10, wall_s=15, memory_mb=m), None, ""),
    ]
    for name, body, limits, expected, text in cases:
        started = time.monotonic()
        res = runner(limits).run(short, strategy(body), {})
        err = res.error or {}
        stopped = (not res.ok and (expected is None or err.get("kind") == expected)
                   and text in err.get("message", ""))
        failures += not stopped
        print(f"{'ok' if stopped else 'FAIL':<5} {name:<12} stopped after {time.monotonic() - started:4.1f} s: "
              f"{err.get('kind')} | {err.get('message', '')[:80]}")
    print("all checks passed" if not failures else f"{failures} check(s) failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
