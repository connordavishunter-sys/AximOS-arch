"""Boots AximOS's ISO in QEMU. The serial port has a root shell, which commands are
typed at, and keys and the pointer go through the monitor and QMP to the keyboard
and a USB tablet, as the desktop gets them."""
import json, os, re, shlex, shutil, socket, subprocess, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = ROOT + '/build'
OVMF = '/usr/share/edk2/x64/OVMF.4m.fd'
WORK = tempfile.mkdtemp(prefix='aximos-test-')
ESCAPES = re.compile(r'\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Z0-9]|\x1b[=>]')
KERNEL_LOG = re.compile(r'^\[ *\d+\.\d+\] ')  # the kernel's own messages, which quiet leaves only for problems
PROMPT = 'root@aximos'
NAMED = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', '\\': 'backslash', '=': 'equal', ',': 'comma',
         ';': 'semicolon', "'": 'apostrophe', '\n': 'ret', '\t': 'tab', '[': 'bracket_left', ']': 'bracket_right',
         '`': 'grave_accent'}
SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0',
           '_': 'minus', '+': 'equal', '"': 'apostrophe', ':': 'semicolon', '?': 'slash', '~': 'grave_accent',
           '>': 'dot', '<': 'comma', '|': 'backslash', '{': 'bracket_left', '}': 'bracket_right'}
WIDTH, HEIGHT = 1280, 800
USER = 'aximos'


def key_for(c):
    if c in NAMED:
        return NAMED[c]
    if c in SHIFTED:
        return 'shift-' + SHIFTED[c]
    return 'shift-' + c.lower() if c.isupper() else c


class VM:
    def __init__(self, timeout=600, memory='4G', smp=4, iso=BUILD + '/aximos.iso'):
        """Boots the ISO, with a virtio GPU the size of the old tests' screen."""
        self.dir = tempfile.mkdtemp(dir=WORK)
        self.log = self.dir + '/serial.log'
        serial, monitor, qmp = self.dir + '/serial.sock', self.dir + '/monitor.sock', self.dir + '/qmp.sock'
        self.proc = subprocess.Popen(
            ['timeout', str(timeout), 'qemu-system-x86_64', '-accel', 'kvm', '-cpu', 'host', '-smp', str(smp),
             '-m', memory, '-M', 'q35', '-display', 'none', '-bios', OVMF, '-cdrom', iso,
             '-device', f'virtio-vga,xres={WIDTH},yres={HEIGHT}', '-netdev', 'user,id=net0',
             '-device', 'virtio-net,netdev=net0', '-device', 'qemu-xhci', '-device', 'usb-tablet', '-device', 'usb-kbd',
             '-audiodev', 'none,id=snd0', '-device', 'ich9-intel-hda', '-device', 'hda-output,audiodev=snd0',
             '-chardev', f'socket,id=serial,path={serial},server=on,wait=off,logfile={self.log}', '-serial', 'chardev:serial',
             '-monitor', f'unix:{monitor},server=on,wait=off', '-qmp', f'unix:{qmp},server=on,wait=off'],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.serial = self._connect(serial)
        threading.Thread(target=self._drain, daemon=True).start()
        self.monitor = self._connect(monitor)
        self.monitor.settimeout(0.05)
        self.qmp = self._connect(qmp).makefile('rw')
        self.qmp.readline()
        self.qmp_command('qmp_capabilities')

    @staticmethod
    def _connect(path):
        while not os.path.exists(path):
            time.sleep(0.05)
        s = socket.socket(socket.AF_UNIX)
        s.connect(path)
        return s

    def _drain(self):
        """Reads what the serial port sends, which QEMU also logs, so it never stops
        for want of a reader."""
        try:
            while self.serial.recv(65536):
                pass
        except OSError:
            pass

    def raw(self):
        return open(self.log, errors='replace').read() if os.path.exists(self.log) else ''

    def mark(self):
        return len(self.raw())

    def text(self, since=0):
        """What the serial port showed since a mark, without escapes or kernel messages."""
        lines = ESCAPES.sub('', self.raw()[since:]).replace('\r', '').split('\n')
        return '\n'.join(l for l in lines if not KERNEL_LOG.match(l))

    def wait(self, needle, secs=10, since=0):
        end = time.time() + secs
        while time.time() < end:
            if needle in self.text(since):
                return True
            time.sleep(0.1)
        return False

    def type(self, s):
        self.serial.sendall(s.encode())

    def boot(self, secs=180):
        """Waits for the root shell the serial port logs into."""
        assert self.wait(PROMPT, secs), self.text()[-2000:]
        self.run('stty -echo cols 200; export PS1="# "; export SYSTEMD_PAGER= SYSTEMD_COLORS=0', 1)

    def run(self, line, secs=10.0):
        """Runs a command line in the root shell, returning what it printed, once it's done."""
        token = f'done-{time.time_ns()}'
        at = self.mark()
        self.type(f'{line}; echo {token}\n')
        end = time.time() + secs
        while time.time() < end:
            out = self.text(at)
            if f'\n{token}' in out or out.startswith(token):
                return out[:out.rfind(token)].replace('# ', '').strip()
            time.sleep(0.1)
        return self.text(at)

    def wm_log(self):
        """What wm and the programs it started have printed, as the journal keeps it."""
        return self.run('journalctl -b -o cat --no-pager -t wm', 20)

    def wait_wm(self, needle, secs=30):
        end = time.time() + secs
        while time.time() < end:
            if needle in self.wm_log():
                return True
            time.sleep(1)
        return False

    def as_user(self, command):
        """Starts a program on the desktop, as the account that's logged in, with wm's
        own Wayland and X displays."""
        log = self.wm_log()
        wayland = re.findall(r'wm: apps connect to (\S+)', log)[-1]
        x = re.findall(r'wm: X apps connect to (\S+)', log)[-1]
        uid = self.run(f'id -u {USER}')
        env = f'WAYLAND_DISPLAY={wayland} DISPLAY={x} XDG_RUNTIME_DIR=/run/user/{uid}'
        self.run(f'(runuser -u {USER} -- env {env} sh -c {shlex.quote("cd; " + command)} >/dev/null 2>&1 &)', 5)

    def command(self, line):
        """Runs a QEMU monitor command, without waiting for it to finish."""
        self.monitor.sendall(line.encode() + b'\n')
        try:
            while self.monitor.recv(4096):
                pass
        except socket.timeout:
            pass

    def key(self, name):
        self.command(f'sendkey {name}')
        time.sleep(0.05)

    def keys(self, s):
        """Types on the keyboard, as the desktop takes it, rather than at the serial port."""
        for c in s:
            self.key(key_for(c))

    def pointer(self, x, y, buttons=None):
        """Puts the tablet's pointer on pixel x, y of the screen, then sets the buttons
        held: 1 left, 2 right, 4 middle."""
        axis = lambda name, at, size: {'type': 'abs', 'data': {'axis': name, 'value': at * 0x7FFF // (size - 1)}}
        self.qmp_command('input-send-event', events=[axis('x', x, WIDTH), axis('y', y, HEIGHT)])
        time.sleep(0.1)
        if buttons is not None:
            self.command(f'mouse_button {buttons}')
        time.sleep(0.2)

    def click(self, x, y):
        self.pointer(x, y, 1)
        self.pointer(x, y, 0)

    def screenshot(self):
        """Returns the screen as (width, height, pixel(x, y) giving 0xRRGGBB)."""
        path = self.dir + '/screen.ppm'
        if os.path.exists(path):
            os.remove(path)
        self.command(f'screendump {path}')
        for _ in range(100):
            if os.path.exists(path) and os.path.getsize(path) > 15:
                time.sleep(0.2)
                break
            time.sleep(0.05)
        data = open(path, 'rb').read()
        magic, width, height, depth, pixels = data.split(maxsplit=4)
        width, height = int(width), int(height)
        assert magic == b'P6' and depth == b'255'
        return width, height, lambda x, y: int.from_bytes(pixels[(y * width + x) * 3:(y * width + x) * 3 + 3], 'big')

    def save_screenshot(self, name):
        """Keeps the screen, as a PNG beside the test's log in build/, for looking at."""
        self.command(f'screendump {self.dir}/shot.ppm')
        time.sleep(1)
        out = f'{BUILD}/test-{name}.png'
        subprocess.run(['magick', f'{self.dir}/shot.ppm', out], capture_output=True)
        return out

    def qmp_command(self, name, **arguments):
        self.qmp.write(json.dumps({'execute': name, 'arguments': arguments}) + '\n')
        self.qmp.flush()
        while 'event' in (reply := json.loads(self.qmp.readline())):
            pass
        return reply

    def close(self):
        self.proc.terminate()
        self.proc.wait()


def window_of(log, app):
    """The size wm said an app's window opened at, or None."""
    found = re.findall(rf'wm: {re.escape(app)} opened a window, (\d+)x(\d+)', log)
    return tuple(map(int, found[-1])) if found else None


failures = 0


def check(name, ok, detail=''):
    global failures
    failures += not ok
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else '\n    ' + repr(str(detail)[-600:])), flush=True)
    return ok


def finish():
    shutil.rmtree(WORK, ignore_errors=True)
    print(f'{failures} failed')
    raise SystemExit(failures > 0)
