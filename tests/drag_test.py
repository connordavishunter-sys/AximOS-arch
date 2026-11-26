"""Dragging files: from wm's desktop into apps, the editor, the terminal and Files,
which moves it out of the desktop, and from one app, Files, to others. Each app is
put on the right half of the screen, so the desktop's icons and Files stay in view."""
import re, time
from vm import VM, USER, check, finish, mark, said, start, wait_for, window_at

HOME = f'/home/{USER}'
EDITOR = '/usr/lib/aximos/apps/Accessories/editor'
TERM = '/usr/lib/aximos/apps/term'


def drag(vm, start_at, to):
    vm.pointer(*start_at, 1)
    for i in range(1, 11):
        vm.pointer(start_at[0] + (to[0] - start_at[0]) * i // 10, start_at[1] + (to[1] - start_at[1]) * i // 10, 1)
        time.sleep(0.05)
    vm.pointer(*to, 0)


def icon(vm, name):
    found = re.findall(rf'wm: desktop icon at (\d+) (\d+) (\d+) (\d+), {re.escape(name)}\n', vm.wm_log())
    x, y, w, h = map(int, found[-1])
    return x + w // 2, y + h // 2


def on_the_right(vm, app, command):
    """Starts an app and snaps it to the right half, returning a point in it."""
    at = mark(vm)
    start(vm, app, command)
    wait_for(vm, f'wm: {app} opened a window', 20, at)
    time.sleep(1)
    vm.key('meta_l-right')
    wait_for(vm, 'wm: snapped', 10, at)
    time.sleep(1)
    return width * 3 // 4, height // 2


vm = VM()
try:
    vm.boot()
    assert vm.wait_wm('wm: running at', 60), vm.wm_log()
    width, height = map(int, re.findall(r'wm: running at (\d+)x(\d+)', vm.wm_log())[-1])

    # Files put in the desktop's folder show straight away, without wm being asked.
    at = mark(vm)
    vm.run(f'cd {HOME}/Desktop && echo hello > note.txt && echo spaced > "my notes.txt" && echo moving > move.txt '
           f'&& mkdir -p {HOME}/Documents && chown -R {USER}: {HOME}/Desktop {HOME}/Documents')
    check('files put on the desktop show', wait_for(vm, 'wm: desktop icon at', 5, at)
          and 'move.txt' in vm.wm_log()[at:], vm.wm_log()[at:])

    # From the desktop: onto the editor, which opens it.
    into = on_the_right(vm, 'editor', EDITOR)
    at = mark(vm)
    drag(vm, icon(vm, 'note.txt'), into)
    check('a desktop file is given to the app it\'s dragged over', wait_for(vm, 'wm: gave', 10, at), vm.wm_log()[at:])
    check('and the editor opens it', said(vm, 'editor', f'editor: opened {HOME}/Desktop/note.txt', 10),
          vm.run('cat /tmp/editor.log'))
    check('which leaves it on the desktop', 'note.txt' in vm.run(f'ls {HOME}/Desktop'), vm.run(f'ls {HOME}/Desktop'))
    vm.run('pkill -x editor')

    # Onto the terminal, which types its path, quoted.
    into = on_the_right(vm, 'term', TERM)
    vm.keys('cat ')
    drag(vm, icon(vm, 'my notes.txt'), into)
    check('the terminal takes a dropped file', said(vm, 'term', 'term: dropped', 10), vm.run('cat /tmp/term.log'))
    vm.keys('> /tmp/dropped.txt\n')
    time.sleep(2)
    check('typing its path, quoted', vm.run('cat /tmp/dropped.txt') == 'spaced', vm.run('cat /tmp/dropped.txt; tail -5 /tmp/term.log'))
    vm.run('pkill -x term')

    # Onto Files, which moves it into its folder, and off the desktop.
    into = on_the_right(vm, 'files', f'/usr/lib/aximos/apps/files {HOME}/Documents')
    said(vm, 'files', 'files: at list')
    at = mark(vm)
    drag(vm, icon(vm, 'move.txt'), into)
    check('Files takes a desktop file', said(vm, 'files', 'files: moved', 10) and 'move.txt' in vm.run(f'ls {HOME}/Documents'),
          vm.run('tail -5 /tmp/files.log'))
    time.sleep(1)
    desktops = re.findall(r'wm: desktop:.*', vm.wm_log()[at:])
    check('and it\'s gone from the desktop straight away', desktops and 'move.txt' not in desktops[-1], desktops)
    at = mark(vm)
    drag(vm, icon(vm, 'note.txt'), (icon(vm, 'note.txt')[0], icon(vm, 'note.txt')[1] + 200))
    check('a desktop icon dropped on the desktop moves on it', wait_for(vm, 'wm: moved note.txt to', 10, at), vm.wm_log()[at:])
    vm.run('pkill -x files')

    # From one app to another: Files, unsnapped on the left, to the editor and the terminal.
    def from_files():
        at = mark(vm)
        start(vm, 'files', f'/usr/lib/aximos/apps/files {HOME}/Documents')
        log = said(vm, 'files', 'files: at list')
        w, h, x, y = window_at(vm.wm_log()[at:], 'files')
        lx, ly, lw, lh = map(int, re.findall(r'files: at list (\d+) (\d+) (\d+) (\d+)', log)[-1])
        return x + lx + 40, y + ly + 12  # its first, and only, file

    item = from_files()
    into = on_the_right(vm, 'editor', EDITOR)
    check('Files\' file is clear of the app', item[0] < width // 2, (item, width))
    vm.pointer(*item, 0)
    drag(vm, item, into)
    check('a file from Files opens in the editor', said(vm, 'editor', f'editor: opened {HOME}/Documents/move.txt', 10),
          vm.run('cat /tmp/editor.log; tail -3 /tmp/files.log'))
    vm.run('pkill -x editor')
    into = on_the_right(vm, 'term', TERM)
    drag(vm, item, into)
    check('and in the terminal, as its path', said(vm, 'term', f'term: dropped {HOME}/Documents/move.txt', 10),
          vm.run('cat /tmp/term.log | tail -3; tail -3 /tmp/files.log'))
    check('and Files still has it', 'move.txt' in vm.run(f'ls {HOME}/Documents'), vm.run(f'ls {HOME}/Documents'))
finally:
    vm.close()
finish()
