"""The terminal wraps long lines again as its window changes size: a line too long for
it goes on over the next rows, and maximized, it's joined back up, and put back,
split again, with the prompt still where it was."""
import re, time
from vm import VM, check, finish, start, said, window_at

TERM = '/usr/lib/aximos/apps/term'
LINE = 'A' * 150 + 'END'


def screen(vm):
    """The terminal's columns and rows of text, as it prints them on SIGUSR1."""
    vm.run('pkill -USR1 -x term')
    time.sleep(1)
    log = vm.run('cat /tmp/term.log', 10)
    log = log[log.rfind('term: screen'):]
    columns = int(re.search(r'term: screen (\d+)x', log).group(1))
    return columns, re.findall(r'term: row \d+ ?(.*)', log)


def wrapped(columns, rows):
    """Whether LINE's on the screen, over as many rows as it takes at this width, each full but the last."""
    for i, row in enumerate(rows):
        if row.startswith('A'):
            parts = rows[i:i + (len(LINE) + columns - 1) // columns]
            return ''.join(parts) == LINE and all(len(p) == columns for p in parts[:-1])
    return False


vm = VM()
try:
    vm.boot()
    assert vm.wait_wm('wm: running at', 60), vm.wm_log()
    start(vm, 'term', TERM)
    check('the terminal opens', said(vm, 'term', 'term:', 20), vm.run('cat /tmp/term.log'))
    time.sleep(2)
    vm.keys("head -c 150 /dev/zero | tr '\\0' A; echo END\n")
    time.sleep(1)
    columns, rows = screen(vm)
    check('a long line goes on over the next rows', wrapped(columns, rows), rows[:8])

    vm.key('meta_l-up')
    time.sleep(2)
    wide, rows = screen(vm)
    check('maximized, it\'s joined back up', wide > columns and wrapped(wide, rows), (wide, rows[:6]))

    vm.key('meta_l-down')
    time.sleep(2)
    columns, rows = screen(vm)
    check('and put back, split again', wrapped(columns, rows), (columns, rows[:8]))

    vm.keys('echo still here\n')
    time.sleep(1)
    _, rows = screen(vm)
    check('with the prompt still where it was', 'still here' in rows and rows.index('still here') > 0
          and rows[rows.index('still here') - 1].endswith('echo still here'), rows[:10])

    # libgui's tabs, find bar and menu, around the cells.
    _, _, x, y = window_at(vm.wm_log(), 'term')
    vm.key('ctrl-shift-t')
    check('Ctrl+Shift+T opens a tab, in libgui\'s tab bar', said(vm, 'term', 'term: tab 2 of 2', 10), vm.run('tail -3 /tmp/term.log'))
    time.sleep(1)
    vm.key('ctrl-tab')
    check('Ctrl+Tab goes back to the first', said(vm, 'term', 'term: tab 1 of 2', 10), vm.run('tail -3 /tmp/term.log'))
    vm.key('ctrl-shift-f')
    time.sleep(1)
    vm.keys('still')
    vm.key('ret')
    check('the find bar finds what\'s typed in it', said(vm, 'term', 'term: found still', 10), vm.run('tail -3 /tmp/term.log'))
    print('screenshot:', vm.save_screenshot('term-find'))
    vm.key('esc')
    time.sleep(1)
    vm.keys('echo typed\n')
    time.sleep(1)
    _, rows = screen(vm)
    check('and Escape closes it, giving the keys back', 'typed' in rows, rows[-12:])
    vm.pointer(x + 200, y + 120, 0)
    vm.pointer(x + 200, y + 120, 2)
    vm.pointer(x + 200, y + 120, 0)
    time.sleep(1)
    print('screenshot:', vm.save_screenshot('term-menu'))
    vm.key('esc')
    time.sleep(1)
    vm.pointer(1100, 300, 0)
    vm.pointer(1100, 300, 2)
    vm.pointer(1100, 300, 0)
    time.sleep(1)
    print('screenshot:', vm.save_screenshot('desktop-menu'))
    vm.key('esc')
    check('the terminal keeps running', 'term' in vm.run('pgrep -x term >/dev/null && echo term'), vm.run('tail -5 /tmp/term.log'))
finally:
    vm.close()
finish()
