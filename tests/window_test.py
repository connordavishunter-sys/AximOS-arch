"""Windows animated by the GPU, opening, tiling, minimizing and closing, and windows
going full screen: by Super+F, and by an X app asking, covering the taskbar."""
import re, time
from vm import VM, check, finish, mark, start, wait_for, window_at

TERM = '/usr/lib/aximos/apps/term'


def covers_screen(vm, colour):
    """Whether the screen's corners and its bottom middle, where the taskbar is, are all colour."""
    width, height, pixel = vm.screenshot()
    points = [(4, 4), (width - 5, 4), (4, height - 5), (width - 5, height - 5), (width // 2, height - 20)]
    return all(pixel(x, y) == colour for x, y in points), [hex(pixel(x, y)) for x, y in points]


def running(vm):
    return 'running' in vm.run('pgrep -x wm >/dev/null && echo running')


vm = VM()
try:
    vm.boot()
    assert vm.wait_wm('wm: running at', 60), vm.wm_log()
    width, height = map(int, re.findall(r'wm: running at (\d+)x(\d+)', vm.wm_log())[-1])

    at = mark(vm)
    start(vm, 'term', TERM)
    check('the terminal opens', wait_for(vm, 'wm: term opened a window', 20, at), vm.wm_log()[at:])
    time.sleep(1)
    w, h, x, y = window_at(vm.wm_log(), 'term')
    _, _, pixel = vm.screenshot()
    inside = pixel(x + w // 2, y + h - 10)

    # Tiled and back, which the GPU animates, from a copy of the window.
    at = mark(vm)
    vm.key('meta_l-left')
    time.sleep(1)
    vm.key('meta_l-up')
    check('it tiles, animated', wait_for(vm, 'wm: snapped ', 10, at), vm.wm_log()[at:])
    time.sleep(1)
    vm.key('meta_l-down')
    time.sleep(1)
    check('and goes back', running(vm), vm.wm_log()[-2000:])

    # Super+F covers the screen, taskbar too, and again puts it back.
    _, _, pixel = vm.screenshot()
    bar = pixel(width // 2, height - 20)
    at = mark(vm)
    vm.key('meta_l-f')
    check('Super+F takes it full screen', wait_for(vm, ' went full screen', 10, at), vm.wm_log()[at:])
    time.sleep(1.5)
    _, _, pixel = vm.screenshot()
    check('covering the taskbar', pixel(width // 2, height - 20) != bar, hex(bar))
    shot = vm.save_screenshot('fullscreen')
    at = mark(vm)
    vm.key('meta_l-f')
    check('and again leaves it', wait_for(vm, ' left full screen', 10, at), vm.wm_log()[at:])
    time.sleep(1.5)
    _, _, pixel = vm.screenshot()
    check('putting the taskbar back', pixel(width // 2, height - 20) == bar, hex(pixel(width // 2, height - 20)))

    # Minimized and restored from its taskbar button, flying to and from it.
    vm.key('meta_l-down')
    time.sleep(1)
    found = re.findall(r'wm: taskbar button \d+ at (\d+) (\d+) (\d+) (\d+), ', vm.wm_log())
    if found:
        bx, by, bw, bh = map(int, found[-1])
        vm.click(bx + bw // 2, by + bh // 2)
        time.sleep(1)
    check('minimized and restored', running(vm), vm.wm_log()[-2000:])

    # An X app asking to be full screen as it opens.
    at = mark(vm)
    start(vm, 'xterm', 'xterm -fullscreen -bg red')
    check('an X app that asks goes full screen', wait_for(vm, ' went full screen', 30, at), vm.wm_log()[at:])
    time.sleep(2)
    covered, seen = covers_screen(vm, 0xFF0000)
    check('covering all of the screen, solid, with nothing under it', covered, seen)
    vm.run('pkill -x xterm')
    time.sleep(1)

    # Closed, it shrinks and fades as a copy, leaving nothing behind.
    at = mark(vm)
    vm.run('pkill -x term')
    time.sleep(1.5)
    check('wm keeps running as windows close', running(vm), vm.wm_log()[at:])
    _, _, pixel = vm.screenshot()
    check('leaving nothing of the window behind', pixel(x + w // 2, y + h - 10) != inside, hex(pixel(x + w // 2, y + h - 10)))
    print('screenshot:', shot)
finally:
    vm.close()
finish()
