from pathlib import Path
import sys

_DESKTOP_ROOT = Path(__file__).resolve().parents[1]
if str(_DESKTOP_ROOT) not in sys.path:
    sys.path.insert(0, str(_DESKTOP_ROOT))

