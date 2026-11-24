"""Phase one: the ISO boots into wm, through greetd, and libgui's apps, Wayland's (foot)
and X's (xterm, through XWayland) open in wm's windows and take the keyboard and mouse.
Each terminal proves what's typed went to it by writing its own TERM."""
import time
from vm import VM, USER, check, finish, window_of

vm = VM()
try:
    vm.boot()
    check('greetd is running', vm.run('systemctl is-active greetd') == 'active', vm.run('systemctl status greetd'))
    check('wm runs as the live account', vm.wait_wm('wm: running at', 60), vm.wm_log())
    check('wm is on the screen through DRM', vm.run(f'pgrep -u {USER} -x wm') != '', vm.run('ps -ef | grep wm'))

    # The desktop is on the screen: more than a few colours, as the wallpaper, taskbar and text have.
    width, height, pixel = vm.screenshot()
    colours = {pixel(x, y) for x in range(0, width, 16) for y in range(0, height, 16)}
    check('the desktop is shown', (width, height) == (1280, 800) and len(colours) > 50, (width, height, len(colours)))
    taskbar = {pixel(x, height - 20) for x in range(0, width, 8)}
    check('with its taskbar', len(taskbar) > 3, len(taskbar))

    # A libgui app, the terminal, which Super+Enter opens, typing into it.
    vm.key('meta_l-ret')
    check('Super+Enter opens the terminal', vm.wait_wm('wm: term opened a window', 30), vm.wm_log())
    time.sleep(2)
    vm.keys('echo $TERM > /tmp/term-typed\n')
    time.sleep(2)
    check('the terminal takes what is typed', vm.run('cat /tmp/term-typed') == 'xterm-256color', vm.wm_log())

    # A Wayland app.
    vm.as_user('foot')
    check('foot opens in a wm window', vm.wait_wm('wm: foot opened a window', 30), vm.wm_log())
    time.sleep(2)
    vm.keys('echo $TERM > /tmp/foot-typed\n')
    time.sleep(2)
    check('foot takes what is typed', vm.run('cat /tmp/foot-typed') == 'foot', vm.wm_log())

    # An X app, through XWayland.
    vm.as_user('xterm')
    check('xterm opens in a wm window', vm.wait_wm('wm: XTerm opened a window', 60), vm.wm_log())
    time.sleep(2)
    vm.keys('echo $TERM > /tmp/xterm-typed\n')
    time.sleep(2)
    check('xterm takes what is typed', vm.run('cat /tmp/xterm-typed') == 'xterm', vm.wm_log())

    # Clicking foot's window, under xterm's, gives it the keyboard back.
    log = vm.wm_log()
    size = window_of(log, 'foot')
    shot = vm.save_screenshot('desktop')
    check('foot and xterm have their sizes', size and window_of(log, 'XTerm'), log)
    vm.click(80 + 32 + 20, 60 + 32 + 24 + 10) # the second window's corner, where xterm, the third, doesn't reach
    time.sleep(1)
    vm.keys('echo $TERM > /tmp/foot-clicked\n')
    time.sleep(2)
    check('clicking foot gives it the keyboard', vm.run('cat /tmp/foot-clicked') == 'foot', vm.wm_log())
    print('screenshot:', shot)
finally:
    vm.close()
finish()
