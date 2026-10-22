# Manual entry points: the two regression commands map to tests/ and examples/ respectively, both
# implemented as xmake tasks (xmake smoke / xmake examples); this file carries no test logic.

smoke:
    xmake smoke

examples:
    xmake examples
