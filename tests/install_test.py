"""Installing AximOS onto blank virtual drives: through the installer's window,
over the whole of one, and by aximos-install itself, into another's free space
beside a partition it keeps. Then the first is booted, with no ISO, and logged
into through wm's log-in screen, and last, its AximOS is replaced."""
import atexit, os, re, shutil, subprocess, tempfile, time
from vm import BUILD, VM, check, finish

NAME, PASSWORD, HOST = 'tester', 'secret123', 'testbox'


# The drives are on the build's disk, not /tmp, which is memory, and go however it ends.
DRIVES = tempfile.mkdtemp(prefix='test-drives-', dir=BUILD)
atexit.register(shutil.rmtree, DRIVES, True)


def disk(name, size):
    path = f'{DRIVES}/{name}.qcow2'
    subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2', path, size], check=True)
    return path


def wait_log(vm, needle, secs):
    end = time.time() + secs
    while time.time() < end:
        log = vm.run('cat /tmp/installer.log', 10)
        if needle in log:
            return log
        time.sleep(3)
    return None


def install(vm, args, secs=900):
    """aximos-install, by hand, with the password on its standard input, and what it said."""
    return vm.run(f'echo {PASSWORD} | aximos-install {args} 2>&1 | grep -v copied; echo exit=${{PIPESTATUS[1]}}', secs)


first, second = disk('first', '24G'), disk('second', '40G')

# Installing, from the live system.
vm = VM(timeout=2400, disks=(first, second))
try:
    vm.boot()
    assert vm.wait_wm('wm: running at', 60), vm.wm_log()
    vm.run('sgdisk -n 1:0:+8G -t 1:8300 -c 1:Keep /dev/vdb >/dev/null && mkfs.ext4 -qF -L Keep /dev/vdb1 && '
           'mount /dev/vdb1 /mnt && echo kept > /mnt/file && umount /mnt', 60)
    listed = vm.run('aximos-install list', 60)
    check('the drives are listed, but not the live one', '/dev/vda' in listed and '/dev/vdb' in listed
          and '/dev/sr0' not in listed, listed)
    check('a blank one can only be erased', re.search(r'^/dev/vda\t.*\twhole$', listed, re.M), listed)
    check('one with room can be shared', re.search(r'^/dev/vdb\t.*\tfree whole$', listed, re.M), listed)

    # The installer's window, by the keyboard: the first drive, whole, then the
    # account, the computer's name and the time zone.
    vm.as_user('/usr/lib/aximos/apps/System/installer > /tmp/installer.log 2>&1')
    check('the installer opens', wait_log(vm, 'installer: drew choose', 60), vm.run('cat /tmp/installer.log'))
    vm.key('ret')
    check('then asks for the account', wait_log(vm, 'installer: drew account', 20), vm.run('cat /tmp/installer.log'))
    vm.keys(f'{NAME}\t{PASSWORD}\t{PASSWORD}\t')
    vm.key('ctrl-a')
    vm.keys(HOST + '\n')
    check('then the time zone', wait_log(vm, 'installer: drew zone', 20), vm.run('cat /tmp/installer.log'))
    vm.keys('Toronto\n')
    check('then whether to erase it', wait_log(vm, 'installer: drew confirm', 20), vm.run('cat /tmp/installer.log'))
    vm.save_screenshot('installer-confirm')
    vm.key('ret')
    log = wait_log(vm, 'installer: drew done', 1200) or wait_log(vm, 'installer: drew failed', 5)
    check('and installs it', log and 'installer: drew done' in log, vm.run('tail -20 /tmp/installer.log'))
    check('saying how far it had got', log and 'aximos-install: copied 50%' in log, log)
    vm.save_screenshot('installer-done')

    # By hand, into the second drive's free space, keeping what's there, with the
    # development tools.
    said = install(vm, f'/dev/vdb free --name second --hostname two --timezone UTC --dev')
    check('aximos-install installs into free space', 'aximos-install: done' in said and 'exit=0' in said, said)
    parts = vm.run('lsblk -lno NAME,PARTLABEL /dev/vdb')
    check('beside what was there', vm.run('mount /dev/vdb1 /mnt && cat /mnt/file; umount /mnt') == 'kept'
          and len(re.findall(r'vdb\d', parts)) == 3, parts)
    menu = vm.run('mount /dev/vdb2 /mnt && cat /mnt/EFI/BOOT/limine.conf; umount /mnt')
    check('with the other drive\'s AximOS in its menu', '/AximOS' in menu and re.search(r'/AximOS, on vda\d', menu) and 'loop' not in menu, menu)
finally:
    vm.close()

# The installed AximOS, booted from its drive alone.
vm = VM(timeout=900, iso=None, disks=(first,))
try:
    vm.log_in(NAME, PASSWORD)
    check('it boots, and its account logs in', True)
    check('with its name', vm.run('cat /etc/hostname') == HOST, vm.run('cat /etc/hostname'))
    check('and time zone', vm.run('readlink /etc/localtime').endswith('America/Toronto'), vm.run('readlink /etc/localtime'))
    check('which can use sudo', 'wheel' in vm.run(f'id {NAME}'), vm.run(f'id {NAME}'))
    check('without the live account', vm.run('id aximos 2>&1').endswith('no such user'), vm.run('id aximos 2>&1'))
    gone = vm.run('pacman -Q aximos-installer hostapd dnsmasq mkinitcpio-archiso gcc git 2>&1')
    check('or the live system\'s tools, or the development ones', gone.count('was not found') == 6, gone)
    check('its root is ext4, and its boot partition FAT', vm.run('findmnt -no FSTYPE /') == 'ext4'
          and vm.run('findmnt -no FSTYPE /boot') == 'vfat', vm.run('findmnt'))
    check('wm\'s log-in screen is up', vm.wait_wm('login: log in', 90), vm.wm_log())
    vm.save_screenshot('installed-login')
    vm.keys(f'{NAME}\t{PASSWORD}\n')
    check('and logs in', vm.wait_wm(f'login: logged in {NAME}', 30), vm.wm_log())
    check('to the desktop', vm.wait_wm('wm: running at', 60) and vm.wm_log().count('wm: running at') >= 2, vm.wm_log())
    vm.save_screenshot('installed-desktop')
finally:
    vm.close()

# Replacing it, as on a computer with an AximOS already.
vm = VM(timeout=1500, disks=(first,))
try:
    vm.boot()
    listed = vm.run('aximos-install list', 60)
    check('an AximOS on a drive can be replaced', re.search(r'^/dev/vda\t.*\treplace whole$', listed, re.M), listed)
    before = vm.run('lsblk -lno NAME,PARTUUID /dev/vda')
    said = install(vm, f'/dev/vda replace --name again --hostname three')
    check('replacing it', 'aximos-install: done' in said and 'exit=0' in said, said)
    check('in its own partitions', vm.run('lsblk -lno NAME,PARTUUID /dev/vda') == before, before)
    check('with the new account', vm.run('mount /dev/vda2 /mnt && ls /mnt/home; umount /mnt') == 'again',
          vm.run('lsblk -f /dev/vda'))
finally:
    vm.close()
finish()
