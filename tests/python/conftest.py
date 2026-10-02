import sys
from pathlib import Path

# The web app (webapp/) is not part of the installed package; make it importable from the repo.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
