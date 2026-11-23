# Sourced by the other scripts. Resolves the repo root and the disc files.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$HOME/.local/share/mise/shims:$PATH"

GAME_DIR="$ROOT/game"
ELF="$GAME_DIR/SLUS_203.09"

