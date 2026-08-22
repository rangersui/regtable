"""`python -m regtable ...`, and `python python/regtable ...` from a
checkout (no install): when run as a file or a directory the package
parent is not on sys.path yet, so add it."""

import sys

if __package__ in (None, ""):
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from regtable.cli import main  # noqa: E402

sys.exit(main())
