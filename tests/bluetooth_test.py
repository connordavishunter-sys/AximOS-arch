"""Bluetooth, on two virtual radios from btvirt, served from the host: wm uses the
first, and the second plays a speaker nearby. wm's tray has Bluetooth's icon, its
panel finds the speaker, pairs with it and connects, disconnects and forgets it, and
turns Bluetooth off and on again."""
import functools, http.server, os, re, subprocess, threading, time
from vm import BUILD, VM, check, finish, mark, wait_for

PORT, BLUEZ = 8643, 'bluez-5.84'
EMULATOR = f'{BUILD}/btvirt/{BLUEZ}/emulator'

# btvirt, which Arch doesn't package, built once from BlueZ's source.
if not os.path.exists(f'{EMULATOR}/btvirt'):
    subprocess.run(f'mkdir -p {BUILD}/btvirt && cd {BUILD}/btvirt && curl -sfLO https://www.kernel.org/pub/linux/bluetooth/{BLUEZ}.tar.xz '
                   f'&& tar -xf {BLUEZ}.tar.xz && cd {BLUEZ} && ./configure -q --enable-testing --disable-systemd --disable-udev '
                   '--disable-cups --disable-obex --disable-client --disable-manpages --disable-monitor --disable-tools '
                   '--disable-library && make -j8 emulator/btvirt', shell=True, check=True, stdout=subprocess.DEVNULL)
server = http.server.ThreadingHTTPServer(('', PORT), functools.partial(http.server.SimpleHTTPRequestHandler,
                                                                       directory=EMULATOR))
threading.Thread(target=server.serve_forever, daemon=True).start()


def middle(log, pattern):
    x, y, w, h = map(int, re.findall(pattern, log)[-1])
    return x + w // 2, y + h // 2


def row_of(log, name):
    found = re.findall(rf'wm: bluetooth row \d+ at (\d+) (\d+) (\d+) (\d+), {re.escape(name)}(.*)', log)
    return found[-1] if found else None


vm = VM()
try:
    vm.boot()
    assert vm.wait_wm('wm: running at', 60), vm.wm_log()
    vm.run(f'modprobe hci_vhci && curl -sf -o /tmp/btvirt http://10.0.2.2:{PORT}/btvirt && chmod +x /tmp/btvirt '
           '&& (setsid /tmp/btvirt -l2 > /tmp/btvirt.log 2>&1 &)', 30)
    log = wait_for(vm, 'wm: bluetooth icon at', 20)
    check('Bluetooth\'s icon shows in the tray', log, vm.wm_log() + vm.run('bluetoothctl list'))
    check('and wm\'s its agent', wait_for(vm, 'wm: bluetooth\'s agent', 10), vm.wm_log())

    # The speaker: the second radio, with a name and an audio class, that can be found,
    # and that pairs without asking anyone.
    vm.run('btmgmt --index 1 name "Test Speaker"; btmgmt --index 1 class 4 6; btmgmt --index 1 io-cap 3; '
           'btmgmt --index 1 connectable on; btmgmt --index 1 discov yes', 30)

    at = mark(vm)
    vm.click(*middle(vm.wm_log(), r'wm: bluetooth icon at (\d+) (\d+) (\d+) (\d+)'))
    check('it opens the Bluetooth panel', wait_for(vm, 'wm: bluetooth panel', 10, at), vm.wm_log()[at:])
    log = wait_for(vm, ', Test Speaker', 30, at)
    check('which finds the speaker nearby', log and row_of(log[at:], 'Test Speaker'), vm.wm_log()[at:])
    time.sleep(3)
    vm.save_screenshot('bluetooth-found')

    row = row_of(vm.wm_log()[at:], 'Test Speaker')
    vm.click(int(row[0]) + int(row[2]) // 2, int(row[1]) + int(row[3]) // 2)
    log = wait_for(vm, 'wm: bluetooth button Pair at', 10, at)
    check('a device not paired can be', log, vm.wm_log()[at:])
    vm.click(*middle(log[at:], r'wm: bluetooth button Pair at (\d+) (\d+) (\d+) (\d+)'))
    end = time.time() + 40
    while time.time() < end and not re.search(r'wm: bluetooth (paired|couldn)', vm.wm_log()[at:]):
        asked = re.findall(r'wm: bluetooth answer yes at (\d+) (\d+) (\d+) (\d+)', vm.wm_log()[at:])
        if asked:  # if it asks for a yes after all, it's given
            vm.click(*middle(vm.wm_log()[at:], r'wm: bluetooth answer yes at (\d+) (\d+) (\d+) (\d+)'))
            time.sleep(2)
        time.sleep(1)
    check('pairing with it', 'wm: bluetooth paired Test Speaker' in vm.wm_log()[at:], vm.wm_log()[at:])
    log = wait_for(vm, 'wm: bluetooth connected Test Speaker', 30, at)
    check('then connecting to it', log, vm.wm_log()[at:] + vm.run('journalctl -b -u bluetooth --no-pager | tail -15'))
    time.sleep(1)
    row = row_of(vm.wm_log(), 'Test Speaker')
    check('which its row says', row and 'connected' in row[4] and 'paired' in row[4], row)
    time.sleep(3)
    vm.save_screenshot('bluetooth-connected')

    # Disconnecting and forgetting it, from its row.
    at = mark(vm)
    if 'wm: bluetooth button Disconnect at' not in vm.wm_log()[-2000:]:
        vm.click(int(row[0]) + int(row[2]) // 2, int(row[1]) + int(row[3]) // 2)
    log = wait_for(vm, 'wm: bluetooth button Disconnect at', 10, at - 2000)
    vm.click(*middle(vm.wm_log(), r'wm: bluetooth button Disconnect at (\d+) (\d+) (\d+) (\d+)'))
    check('it can be disconnected', wait_for(vm, 'wm: bluetooth disconnected Test Speaker', 20, at), vm.wm_log()[at:])
    log = wait_for(vm, 'wm: bluetooth button Forget at', 10, at)
    vm.click(*middle(vm.wm_log(), r'wm: bluetooth button Forget at (\d+) (\d+) (\d+) (\d+)'))
    check('and forgotten', wait_for(vm, 'wm: bluetooth forgot Test Speaker', 10, at)
          and 'Paired: yes' not in vm.run('bluetoothctl --timeout 2 info 00:AA:01:01:00:01 2>&1', 10), vm.wm_log()[at:])

    # Off and on again, with the switch.
    at = mark(vm)
    switch = middle(vm.wm_log(), r'wm: bluetooth on at (\d+) (\d+) (\d+) (\d+)')
    vm.click(*switch)
    check('the switch turns Bluetooth off', wait_for(vm, 'wm: bluetooth turned off', 10, at)
          and wait_for(vm, 'wm: bluetooth off at', 10, at), vm.wm_log()[at:] + vm.run('rfkill; bluetoothctl show | grep Powered'))
    time.sleep(3)
    vm.save_screenshot('bluetooth-off')
    at = mark(vm)
    vm.click(*switch)
    check('and on again', wait_for(vm, 'wm: bluetooth on at', 15, at), vm.wm_log()[at:] + vm.run('rfkill; bluetoothctl show | grep Powered'))
finally:
    vm.close()
    server.shutdown()
finish()
