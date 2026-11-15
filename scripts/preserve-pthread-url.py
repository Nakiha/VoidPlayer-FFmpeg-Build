"""Keep the importing module's content hash on nested pthread requests."""
from pathlib import Path
import sys

glue = Path(sys.argv[1])
source = glue.read_text()
entry = f'new URL("{glue.name}",import.meta.url)'
if source.count(entry) != 1:
    raise SystemExit('Emscripten pthread URL changed; review version preservation')
glue.write_text(source.replace(entry, 'new URL(import.meta.url)'))
