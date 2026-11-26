"""Updating: a newer AximOS repository, served from the host as AximOS's own is
from its release, with Arch's left empty, so nothing's downloaded from Arch. wm
says when there are updates, Settings' Updates page lists and installs them,
asking for the password in wm's dialog, adding a recovery entry to the boot menu,
then undoes the update, putting back the live system's own versions, which aren't
in pacman's cache, from the release, which keeps them. A package that isn't signed
with AximOS's key is refused."""
import functools, http.server, os, re, shutil, subprocess, threading, time
from vm import BUILD, ROOT, USER, VM, check, finish, mark, said, start, wait_for, widget

PORT, VERSION, PASSWORD = 8642, '0.1.0.r99999.gtest', 'aximos'
SERVED = f'{BUILD}/test-served'

# The newer repository, built once, then served beside empty core and extra ones.
if not os.path.exists(f'{BUILD}/test-repo/aximos.db') or VERSION not in ' '.join(os.listdir(f'{BUILD}/test-repo')):
    subprocess.run(['make', '-C', ROOT, 'packages', f'VERSION={VERSION}', f'REPO={BUILD}/test-repo',
                    f'PACKAGING={BUILD}/test-packaging'], check=True, stdout=subprocess.DEVNULL)
shutil.rmtree(SERVED, ignore_errors=True)
os.makedirs(f'{SERVED}/empty')
shutil.copytree(f'{BUILD}/test-repo', f'{SERVED}/aximos', symlinks=False)
for f in os.listdir(f'{BUILD}/repo'):  # the ISO's versions, beside them, as the release keeps the last
    if '.pkg.tar.zst' in f:
        shutil.copy(f'{BUILD}/repo/{f}', f'{SERVED}/aximos/{f}')
for repo in ('core', 'extra'):
    subprocess.run(['tar', '-czf', f'{SERVED}/empty/{repo}.db', '--files-from', '/dev/null'], check=True)
server = http.server.ThreadingHTTPServer(('', PORT), functools.partial(http.server.SimpleHTTPRequestHandler, directory=SERVED))
threading.Thread(target=server.serve_forever, daemon=True).start()

CONF = f'''[options]
Architecture = auto
SigLevel = Required DatabaseOptional
[core]
Server = http://10.0.2.2:{PORT}/empty
[extra]
Server = http://10.0.2.2:{PORT}/empty
[aximos]
SigLevel = Required
Server = http://10.0.2.2:{PORT}/aximos
'''

vm = VM()
try:
    vm.boot()
    assert vm.wait_wm('wm: polkit\'s agent', 60), vm.wm_log()
    check('installed systems update from AximOS\'s release', 'releases/download/packages' in vm.run('cat /etc/pacman.conf'),
          vm.run('cat /etc/pacman.conf'))
    vm.run('for i in $(seq 120); do systemctl is-active -q pacman-init && break; sleep 1; done', 130)
    check('and trust AximOS\'s key, from boot', '[  full  ] AximOS packages' in vm.run('pacman-key --list-keys AximOS 2>&1', 30),
          vm.run('systemctl status pacman-init 2>&1; pacman-key --list-keys 2>&1 | tail', 30))
    vm.run(f"printf '%s' '{CONF}' > /etc/pacman.conf")
    before = vm.run('pacman -Q aximos-desktop').splitlines()[-1]
    # A boot menu as one installed before recovery entries had, for the update to add one to.
    vm.run("mkdir -p /boot/EFI/BOOT && printf 'timeout: 5\\n\\n/AximOS\\n    protocol: linux\\n"
           "    cmdline: root=PARTUUID=x rw quiet\\n\\n/Windows, on vda1\\n    protocol: efi\\n' > /boot/EFI/BOOT/limine.conf")

    # wm's check, asked for now rather than five minutes in.
    at = mark(vm)
    vm.run('pkill -USR2 -x wm')
    log = wait_for(vm, 'wm: told of', 60, at)
    check('wm says when there are updates', log and re.search(r'wm: told of [4-9] updates', log[at:]), vm.wm_log()[at:])

    # Settings lists them, and installs them, once wm's had the password.
    start(vm, 'settings', '/usr/lib/aximos/apps/System/settings Updates')
    log = said(vm, 'settings', 'settings: at update', 60)
    check('Settings lists them', log and re.search(r'settings: [4-9] updates', log), vm.run('cat /tmp/settings.log'))
    vm.save_screenshot('updates')
    at = mark(vm)
    vm.click(*widget(vm, log, 'settings', 'update'))
    check('installing them asks for a password, in wm', wait_for(vm, 'wm: password dialog at', 30, at), vm.wm_log()[at:])
    vm.keys(PASSWORD + '\n')
    log = said(vm, 'settings', 'settings: updated', 300)
    check('and installs them', log and 'settings: updated: Updated' in log, vm.run('tail -20 /tmp/settings.log'))
    after = vm.run('pacman -Q aximos-desktop')
    check('newer than they were', VERSION in after and after != before, (before, after))
    check('after which it\'s up to date', said(vm, 'settings', 'settings: 0 updates', 60), vm.run('tail -5 /tmp/settings.log'))
    menu = vm.run('cat /boot/EFI/BOOT/limine.conf')
    check('and the boot menu has a recovery entry', re.search(r'/AximOS, recovery\n    protocol: linux\n    cmdline: root=PARTUUID=x rw '
                                                           r'systemd.unit=multi-user.target\n\n/Windows', menu)
          and 'quiet' in vm.run('cat /boot/EFI/BOOT/limine.conf.before-recovery'), menu)
    record = vm.run('cat /var/lib/aximos/last-update')
    check('what the update changed is kept', re.search(rf'aximos-desktop \S+ {re.escape(VERSION)}-1', record), record)

    # Undoing it, from Settings, puts back the versions from before.
    log = said(vm, 'settings', 'settings: at undo', 30)
    check('Settings offers to undo it', log, vm.run('tail -5 /tmp/settings.log'))
    vm.save_screenshot('updates-undo')
    at = mark(vm)
    vm.click(*widget(vm, log, 'settings', 'undo'))
    if wait_for(vm, 'wm: password dialog at', 15, at):  # unless polkit's kept the last answer
        vm.keys(PASSWORD + '\n')
    log = said(vm, 'settings', 'settings: undone', 300)
    check('and undoes it', log and 'settings: undone: Put back' in log, vm.run('tail -20 /tmp/settings.log'))
    check('to the versions from before', vm.run('pacman -Q aximos-desktop').splitlines()[-1] == before, (before, vm.run('pacman -Q aximos-desktop')))
    check('after which there\'s nothing to undo', vm.run('ls /var/lib/aximos') == 'last-update.undone', vm.run('ls /var/lib/aximos'))
    time.sleep(5)
    check('and the update is waiting again', re.search(r'settings: [4-9] updates', vm.run('tail -5 /tmp/settings.log')),
          vm.run('tail -5 /tmp/settings.log'))

    # A package that isn't signed with AximOS's key isn't installed.
    package = next(f for f in os.listdir(f'{SERVED}/aximos') if f.startswith('aximos-apps-') and f.endswith('.zst'))
    os.remove(f'{SERVED}/aximos/{package}.sig')
    vm.run('rm -f /var/cache/pacman/pkg/aximos-*')  # not the verified copy the update left
    refused = vm.run(f'pacman -U --noconfirm http://10.0.2.2:{PORT}/aximos/{package} 2>&1; echo status=$?', 60)
    check('and an unsigned package is refused', 'status=0' not in refused and '.sig' in refused, refused)
finally:
    vm.close()
    server.shutdown()
finish()
