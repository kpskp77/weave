# 手工入口：两个回归命令分别对应 tests/ 与 examples/，均以 xmake 任务实现
# （xmake smoke / xmake examples），本文件不承载任何测试逻辑。

smoke:
    xmake smoke

examples:
    xmake examples
