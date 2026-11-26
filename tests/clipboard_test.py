"""Copying and pasting between programs: AximOS's editor to its terminal, an app to
wm, as a name typed on the desktop, wm to an app, from the desktop's renaming and its
Copy path, and an app's own copy, replaced by another's, isn't pasted again."""
import re, time
from vm import VM, USER, check, finish, mark, start, wait_for, window_at

HOME = f'/home/{USER}'


def term_paste(vm, n):
    """What the terminal pastes, as it puts it in a file."""
    vm.keys('echo \'')
    vm.key('ctrl-shift-v')
    time.sleep(1)
    vm.keys(f'\' > /tmp/pasted{n}\n')
    time.sleep(2)
    return vm.run(f'cat /tmp/pasted{n} 2>&1')


def button(vm, title):
    x, y, w, h = map(int, re.findall(rf'wm: taskbar button \d+ at (\d+) (\d+) (\d+) (\d+), {re.escape(title)}', vm.wm_log())[-1])
    return x + w // 2, y + h // 2


def icon(vm, name):
    x, y, w, h = map(int, re.findall(rf'wm: desktop icon at (\d+) (\d+) (\d+) (\d+), {re.escape(name)}\n', vm.wm_log())[-1])
    return x + w // 2, y + h // 2


vm = VM()
try:
    vm.boot()
    assert vm.wait_wm('wm: running at', 60), vm.wm_log()

    # The editor's copy, pasted in the terminal.
    start(vm, 'editor', '/usr/lib/aximos/apps/Accessories/editor')
    assert wait_for(vm, 'wm: editor opened a window', 30), vm.wm_log()[-2000:]
    time.sleep(2)
    w, h, x, y = window_at(vm.wm_log(), 'editor')
    vm.click(x + w // 2, y + h // 2)
    vm.keys('fromeditor')
    vm.key('ctrl-a')
    vm.key('ctrl-c')
    time.sleep(1)
    vm.as_user('/usr/lib/aximos/apps/term > /tmp/term.log 2>&1')
    assert wait_for(vm, 'wm: term opened a window', 30), vm.wm_log()[-2000:]
    time.sleep(3)
    check('the editor\'s copy pastes in the terminal', term_paste(vm, 0) == 'fromeditor', vm.run('cat /tmp/pasted0'))

    # wm has it too: pasted as a new folder's name on the desktop.
    vm.key('meta_l-down')  # the terminal out of the way
    time.sleep(1)
    at = mark(vm)
    vm.pointer(1000, 300, 2)
    vm.pointer(1000, 300, 0)
    vm.key('down')
    vm.key('ret')
    check('a new folder on the desktop', wait_for(vm, 'wm: renaming New folder on the desktop', 10, at), vm.wm_log()[at:])
    vm.key('ctrl-a')
    vm.key('ctrl-v')
    vm.keys('\n')
    check('takes it as its name, pasted', wait_for(vm, 'wm: renamed New folder to fromeditor', 10, at)
          and 'fromeditor' in vm.run(f'ls {HOME}/Desktop'), vm.wm_log()[at:])

    # And what's copied in wm is the apps': from renaming, then Copy path.
    at = mark(vm)
    vm.pointer(1000, 400, 2)
    vm.pointer(1000, 400, 0)
    vm.key('down')
    vm.key('ret')
    wait_for(vm, 'wm: renaming New folder on the desktop', 10, at)
    vm.key('ctrl-a')
    vm.key('ctrl-c')
    vm.key('esc')
    time.sleep(1)
    vm.click(*button(vm, 'aximos@'))
    time.sleep(1)
    check('a name copied on the desktop pastes in an app', term_paste(vm, 1) == 'New folder', vm.run('cat /tmp/pasted1'))
    vm.key('meta_l-down')
    time.sleep(1)
    vm.pointer(*icon(vm, 'fromeditor'), 2)
    vm.pointer(*icon(vm, 'fromeditor'), 0)
    for _ in range(4):  # Open, Cut, Copy, Copy path
        vm.key('down')
    vm.key('ret')
    time.sleep(1)
    vm.click(*button(vm, 'aximos@'))
    time.sleep(1)
    check('and Copy path, from a desktop icon', term_paste(vm, 2) == f'{HOME}/Desktop/fromeditor', vm.run('cat /tmp/pasted2'))

    # The editor copied before, but what it pastes now is what was copied since.
    vm.click(*button(vm, '*Untitled'))
    time.sleep(1)
    vm.key('ctrl-a')
    vm.key('ctrl-v')
    vm.key('ctrl-a')
    vm.key('ctrl-c')
    time.sleep(1)
    vm.click(*button(vm, 'aximos@'))
    time.sleep(1)
    check('an app pastes what was copied since its own copy', term_paste(vm, 3) == f'{HOME}/Desktop/fromeditor',
          vm.run('cat /tmp/pasted3'))
finally:
    vm.close()
finish()
