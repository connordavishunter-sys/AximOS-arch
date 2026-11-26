"""Settings as the system's front end, and wm as polkit's agent: renaming the
computer asks for a password in wm's own dialog, which says when it's wrong and
can be cancelled. Settings' Network, Power, Date & time and About pages, and the
lid's setting. Then the menus: a taskbar button's, the desktop's New folder, New
file and Settings, and renaming in place on the desktop and in Files."""
import re, time
from vm import VM, USER, center_of, check, finish

PASSWORD = 'aximos'  # the live account's


def wait_for(vm, needle, secs=30, since=0):
    end = time.time() + secs
    while time.time() < end:
        log = vm.wm_log()
        if needle in log[since:]:
            return log
        time.sleep(1)
    return None


def mark(vm):
    return len(vm.wm_log())


def start(vm, app, command):
    """Starts an app, keeping what it says in /tmp/app.log."""
    vm.run(f'rm -f /tmp/{app}.log')
    vm.as_user(f'{command} > /tmp/{app}.log 2>&1')


def said(vm, app, needle, secs=30):
    """What an app's said, once it's said needle, or None."""
    end = time.time() + secs
    while time.time() < end:
        log = vm.run(f'cat /tmp/{app}.log 2>/dev/null', 10)
        if needle in log:
            return log
        time.sleep(1)
    return None


def window_at(log, app):
    found = re.findall(rf'wm: {re.escape(app)} opened a window, (\d+)x(\d+) at (-?\d+) (-?\d+)', log)
    return tuple(map(int, found[-1])) if found else None


def widget(vm, log, app, what):
    """The middle of a widget an app said it had drawn, on the screen."""
    w, h, x, y = window_at(vm.wm_log(), app)
    found = re.findall(rf'{app}: at {re.escape(what)} (\d+) (\d+) (\d+) (\d+)', log)
    if not found:
        return None
    wx, wy, ww, wh = map(int, found[-1])
    return x + wx + ww // 2, y + wy + wh // 2


def button_of(log, title):
    found = re.findall(rf'wm: taskbar button \d+ at (\d+) (\d+) (\d+) (\d+), {re.escape(title)}', log)
    x, y, w, h = map(int, found[-1])
    return x + w // 2, y + h // 2


vm = VM()
try:
    vm.boot()
    assert vm.wait_wm('wm: running at', 60), vm.wm_log()
    check('wm is polkit\'s agent', vm.wait_wm('wm: polkit\'s agent for session', 20), vm.wm_log())

    # Renaming the computer, from About, which needs an admin's password.
    start(vm, 'settings', '/usr/lib/aximos/apps/System/settings About')
    log = said(vm, 'settings', 'settings: at rename')
    check('Settings opens on About', log and 'settings: tab About' in log, vm.wm_log())
    vm.click(*widget(vm, log, 'settings', 'hostname'))
    vm.key('ctrl-a')
    vm.keys('renamedbox')
    at = mark(vm)
    vm.click(*widget(vm, log, 'settings', 'rename'))
    log = wait_for(vm, 'wm: password dialog at', 20, at)
    check('renaming it asks for a password, in wm', log and 'wm: asked for aximos\'s password' in log, vm.wm_log()[at:])
    vm.save_screenshot('password-dialog')
    vm.keys('wrongpass\n')
    check('a wrong one is said to be wrong', wait_for(vm, 'wm: the password wasn\'t right', 30, at), vm.wm_log()[at:])
    vm.keys(PASSWORD + '\n')
    check('the right one does it', wait_for(vm, 'wm: the password was right', 30, at), vm.wm_log()[at:])
    time.sleep(2)
    check('and the computer has its new name', vm.run('cat /etc/hostname') == 'renamedbox', vm.run('cat /etc/hostname'))

    # Cancelling: polkit keeps the answer a while, so something it hasn't been asked
    # yet, the time zone, from Date & time, is cancelled.
    vm.run('pkill -x settings')
    start(vm, 'settings', '/usr/lib/aximos/apps/System/settings "Date & time"')
    log = said(vm, 'settings', 'settings: at use')
    at = mark(vm)
    vm.click(*widget(vm, log, 'settings', 'search'))
    vm.keys('Tokyo')
    vm.click(*widget(vm, log, 'settings', 'use'))
    if wait_for(vm, 'wm: password dialog at', 20, at):
        vm.key('esc')
        log = wait_for(vm, 'wm: asking for a password cancelled', 10, at)
        check('a password asked for can be cancelled', log, vm.wm_log()[at:])
        check('leaving the zone as it was', 'Tokyo' not in vm.run('timedatectl show -p Timezone'),
              vm.run('timedatectl show -p Timezone'))
    else:
        check('a password asked for can be cancelled', False, vm.wm_log()[at:])
    check('Date & time can set the time from the network', said(vm, 'settings', 'settings: at ntp', 5), vm.run('cat /tmp/settings.log'))

    # Power, and the lid's setting, which wm follows.
    vm.run('pkill -x settings')
    start(vm, 'settings', '/usr/lib/aximos/apps/System/settings Power')
    log = said(vm, 'settings', 'settings: at lid')
    check('Power shows the power modes', 'settings: at mode' in log, log)
    vm.click(*widget(vm, log, 'settings', 'lid'))
    vm.key('down')
    vm.key('esc')
    time.sleep(1)
    conf = vm.run(f'cat /home/{USER}/.config/aximos/gui.conf')
    check('the lid can lock the screen, rather than suspend', 'lid lock' in conf, conf)
    vm.click(*widget(vm, log, 'settings', 'mode'))
    vm.key('up')
    vm.key('esc')
    log = said(vm, 'settings', 'settings: power mode', 10)
    at = 0
    mode = vm.run('busctl get-property org.freedesktop.UPower.PowerProfiles /org/freedesktop/UPower/PowerProfiles '
                  'org.freedesktop.UPower.PowerProfiles ActiveProfile').split('"')[1:2]
    check('and the power mode changes', log and mode and f'settings: power mode {mode[0]}' in log[at:], (mode, log and log[at:]))

    # Network and About show what's there.
    vm.run('pkill -x settings')
    start(vm, 'settings', '/usr/lib/aximos/apps/System/settings Network')
    check('Network opens', said(vm, 'settings', 'settings: tab Network'), vm.run('cat /tmp/settings.log'))
    vm.save_screenshot('settings-network')
    vm.run('pkill -x settings')

    # A taskbar button's menu.
    vm.as_user('/usr/lib/aximos/apps/term')
    log = wait_for(vm, 'wm: taskbar button 0 at')
    title = re.findall(r'wm: taskbar button 0 at \d+ \d+ \d+ \d+, (.*)', log)[-1]
    button = button_of(log, title)
    at = mark(vm)
    vm.pointer(*button, 2)
    vm.pointer(*button, 0)
    check('a taskbar button has a menu', wait_for(vm, 'wm: menu 0 at', 10, at), vm.wm_log()[at:])
    vm.key('down')
    vm.key('ret')
    check('which minimizes its window', wait_for(vm, 'wm: Minimize from its taskbar button', 10, at), vm.wm_log()[at:])
    at = mark(vm)
    vm.pointer(*button, 2)
    vm.pointer(*button, 0)
    vm.key('down')
    vm.key('ret')
    check('and restores it', wait_for(vm, 'wm: Restore from its taskbar button', 10, at), vm.wm_log()[at:])
    at = mark(vm)
    vm.pointer(*button, 2)
    vm.pointer(*button, 0)
    vm.key('up')
    vm.key('ret')
    check('and closes it', wait_for(vm, 'wm: Close from its taskbar button', 10, at) and wait_for(vm, 'wm: term closed', 10, at)
          or 'term' not in vm.run('pgrep -x term'), vm.wm_log()[at:])

    # The desktop's menu: a new folder, renamed where its name is.
    desktop = (900, 300)
    at = mark(vm)
    vm.pointer(*desktop, 2)
    vm.pointer(*desktop, 0)
    vm.key('down')
    vm.key('ret')
    log = wait_for(vm, 'wm: renaming New folder on the desktop', 10, at)
    check('the desktop makes a new folder, to be named', log, vm.wm_log()[at:])
    vm.keys('Projects\n')
    check('typing renames it', wait_for(vm, 'wm: renamed New folder to Projects', 10, at)
          and 'Projects' in vm.run(f'ls /home/{USER}/Desktop'), vm.wm_log()[at:])
    at = mark(vm)
    vm.pointer(*desktop, 2)
    vm.pointer(*desktop, 0)
    vm.key('down')
    vm.key('down')
    vm.key('ret')
    log = wait_for(vm, 'wm: renaming New file.txt on the desktop', 10, at)
    check('and a new file', log, vm.wm_log()[at:])
    vm.save_screenshot('desktop-renaming')
    vm.keys('notes')
    vm.click(*desktop)  # a click elsewhere keeps it, and keeps .txt, which wasn't selected
    check('whose extension stays', wait_for(vm, 'wm: renamed New file.txt to notes.txt', 10, at)
          and 'notes.txt' in vm.run(f'ls /home/{USER}/Desktop'), vm.wm_log()[at:])
    at = mark(vm)
    vm.pointer(*desktop, 2)
    vm.pointer(*desktop, 0)
    vm.key('up')
    vm.key('ret')
    check('and opens Settings', wait_for(vm, 'wm: settings opened a window', 15, at), vm.wm_log()[at:])
    vm.run('pkill -x settings')

    # Files: a new folder from its menu, renamed in place, then F2 on it.
    start(vm, 'files', f'/usr/lib/aximos/apps/files /home/{USER}/Desktop')
    log = said(vm, 'files', 'files: at list')
    w, h, x, y = window_at(vm.wm_log(), 'files')
    lx, ly, lw, lh = map(int, re.findall(r'files: at list (\d+) (\d+) (\d+) (\d+)', log)[-1])
    empty = (x + lx + lw // 2, y + ly + lh - 20)
    at = mark(vm)
    vm.pointer(*empty, 2)
    vm.pointer(*empty, 0)
    time.sleep(1)
    vm.key('down')
    vm.key('ret')
    log = said(vm, 'files', 'files: renaming New folder', 10)
    check('Files makes a new folder, to be named in place', log, vm.run('cat /tmp/files.log'))
    vm.save_screenshot('files-renaming')
    vm.keys('Docs\n')
    check('typing renames it', said(vm, 'files', 'files: renamed', 10) and 'Docs' in vm.run(f'ls /home/{USER}/Desktop'),
          vm.run('cat /tmp/files.log'))
    vm.key('f2')
    check('F2 renames what\'s chosen', said(vm, 'files', 'files: renaming Docs', 10), vm.run('cat /tmp/files.log'))
    vm.keys('Papers')
    vm.key('esc')
    time.sleep(1)
    check('and Escape leaves it as it was', 'Docs' in vm.run(f'ls /home/{USER}/Desktop')
          and 'Papers' not in vm.run(f'ls /home/{USER}/Desktop'), vm.run(f'ls /home/{USER}/Desktop'))

    # Dragging a file from Files: onto its own list, which it takes from its own drag
    # rather than waiting for itself, and onto the desktop, which copies it there.
    vm.run('pkill -x files')
    vm.run(f'mkdir -p /home/{USER}/Documents && echo hi > /home/{USER}/Documents/a.txt && chown -R {USER}: /home/{USER}/Documents')
    start(vm, 'files', f'/usr/lib/aximos/apps/files /home/{USER}/Documents')
    log = said(vm, 'files', 'files: at list')
    w, h, x, y = window_at(vm.wm_log(), 'files')
    lx, ly, lw, lh = map(int, re.findall(r'files: at list (\d+) (\d+) (\d+) (\d+)', log)[-1])
    item = (x + lx + 60, y + ly + 12)

    def drag(to):
        vm.pointer(*item, 1)
        for i in range(1, 9):
            vm.pointer(item[0] + (to[0] - item[0]) * i // 8, item[1] + (to[1] - item[1]) * i // 8, 1)
        vm.pointer(*to, 0)

    drag((x + lx + lw // 2, y + ly + lh - 30))
    check('a file dropped on its own window', said(vm, 'files', 'files: dropped', 10), vm.run('cat /tmp/files.log'))
    at = mark(vm)
    drag((1100, 250))
    check('doesn\'t stop Files, and on the desktop, it\'s copied there',
          wait_for(vm, 'dropped on the desktop', 10, at) and (time.sleep(2) or 'a.txt' in vm.run(f'ls /home/{USER}/Desktop')),
          vm.wm_log()[at:] + vm.run('tail -3 /tmp/files.log'))
    check('and Files still answers', vm.run('pgrep -x files') and said(vm, 'files', 'files: dragging', 5),
          vm.run('tail -5 /tmp/files.log'))
finally:
    vm.close()
finish()
