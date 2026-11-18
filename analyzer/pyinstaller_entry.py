"""Entry point for the standalone analyzer (PyInstaller): b4nd-analyzer(.exe) run --job job.json"""

import sys

from b4nd_analyzer.cli import main

if __name__ == "__main__":
    sys.exit(main())
