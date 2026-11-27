"""USB drives in Files: a stick plugged in shows down the side, opens, mounted by
udisks2 without a password, ejects so it can be taken out, and goes when it's pulled
out."""
import re, subprocess, time
from vm import VM, check, finish, start, said, window_at

FILES = '/usr/lib/aximos/apps/files'


def drive_line(vm, needle, secs=20):
    """Files' last list of drives, once it has needle in it, or None."""
    end = time.time() + secs
    while time.time() < end:
        lines = re.findall(r'files: drives:.*', vm.run('cat /tmp/files.log', 10))
        if lines and needle(lines[-1]):
            return lines[-1]
        time.sleep(1)
    return None


def plug(vm, image, name):
    vm.command(f'drive_add 0 if=none,id={name},file={image},format=raw')
    vm.command(f'device_add usb-storage,drive={name},id={name}dev,removable=on')


vm = VM()
try:
    # A stick with a FAT filesystem, labelled, with a file on it.
    image = vm.dir + '/stick.img'
    subprocess.run(['truncate', '-s', '64M', image], check=True)
    subprocess.run(['mkfs.fat', '-n', 'AXIMSTICK', image], check=True, capture_output=True)
    with open(vm.dir + '/hello.txt', 'w') as f:
        f.write('from the stick\n')
    subprocess.run(['mcopy', '-i', image, vm.dir + '/hello.txt', '::hello.txt'], check=True)

    vm.boot()
    assert vm.wait_wm('wm: running at', 60), vm.wm_log()
    start(vm, 'files', FILES)
    check('Files opens', said(vm, 'files', 'files: drives:', 20), vm.run('cat /tmp/files.log'))
    time.sleep(1)

    # Plugged in, it shows down the side, and opens, mounted, without a password.
    plug(vm, image, 'stick')
    line = drive_line(vm, lambda l: 'AXIMSTICK at' in l)
    check('a stick plugged in shows in Files', line, vm.run('tail -5 /tmp/files.log; udisksctl status'))
    if line:
        x, y, w, h = map(int, re.search(r'AXIMSTICK at (\d+) (\d+) (\d+) (\d+)', line).groups())
        _, _, wx, wy = window_at(vm.wm_log(), 'files')
        vm.click(wx + x + w // 2, wy + y + h // 2)
        log = said(vm, 'files', 'files: mounted AXIMSTICK', 20)
        check('clicking it mounts it', log, vm.run('tail -5 /tmp/files.log'))
        check('and shows what\'s on it', said(vm, 'files', 'hello.txt', 10), vm.run('tail -3 /tmp/files.log'))
        time.sleep(1)
        print('screenshot:', vm.save_screenshot('drives'), drive_line(vm, lambda l: True))
        mount = re.findall(r'files: mounted AXIMSTICK at (\S+)', log or '')
        check('which the user can read', mount and 'from the stick' in vm.run(f'su aximos -c "cat {mount[-1]}/hello.txt"'),
              mount)

        # Ejected, it's unmounted and powered off, and Files goes home.
        found = re.findall(r'files: at eject (\d+) (\d+) (\d+) (\d+)', vm.run('cat /tmp/files.log'))
        check('an Eject button shows for it', found, vm.run('tail -5 /tmp/files.log'))
        if found:
            x, y, w, h = map(int, found[-1])
            vm.click(wx + x + w // 2, wy + y + h // 2)
            check('Eject unmounts it', said(vm, 'files', 'files: ejected AXIMSTICK', 20)
                  and 'AXIMSTICK' not in vm.run('findmnt -rno TARGET'), vm.run('tail -5 /tmp/files.log; findmnt -rno TARGET'))
            check('so it can be taken out', drive_line(vm, lambda l: 'AXIMSTICK at' not in l or 'mounted' not in l),
                  vm.run('tail -3 /tmp/files.log'))
    vm.command('device_del stickdev')
    time.sleep(2)

    # Plugged in again and pulled straight out, it comes and goes.
    plug(vm, image, 'again')
    check('plugged in again, it shows', drive_line(vm, lambda l: 'AXIMSTICK at' in l), vm.run('tail -3 /tmp/files.log'))
    vm.command('device_del againdev')
    check('and pulled out, it goes', drive_line(vm, lambda l: 'AXIMSTICK' not in l), vm.run('tail -3 /tmp/files.log'))
    check('Files keeps running', 'files' in vm.run('pgrep -x files >/dev/null && echo files'), vm.run('tail -5 /tmp/files.log'))
finally:
    vm.close()
finish()
