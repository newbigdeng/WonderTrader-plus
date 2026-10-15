"""Monitor-managed simnow-03 entry: always use the existing observation mode."""
import sys
if len(sys.argv) != 1:
    raise SystemExit('This monitor entry accepts no trade-mode arguments.')
from run_live import main
main()
