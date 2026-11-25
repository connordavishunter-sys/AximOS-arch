"""The Wi-Fi panel, on simulated radios: mac80211_hwsim's, one of them NetworkManager's
and the rest access points, from hostapd. It lists the networks nearby and the cable,
joins an open network, a secured one after a wrong password, and a hidden one,
forgets one, and turns Wi-Fi off and airplane mode off again."""
import re, time
from vm import VM, center_of, check, finish

ACCESS_POINTS = {  # each radio's network
    'wlan1': 'ssid=AximTest\nchannel=1\nwpa=2\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\nwpa_passphrase=secret123',
    'wlan2': 'ssid=AximOpen\nchannel=6',
    'wlan3': 'ssid=AximHidden\nchannel=11\nignore_broadcast_ssid=1\nwpa=2\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\n'
             'wpa_passphrase=hidden123',
}


def row(log, ssid):
    """The middle of a network's row in the panel, as wm last said, or None."""
    found = re.findall(rf'wm: wifi row \d+ at (\d+) (\d+) (\d+) (\d+), {re.escape(ssid)}(?:,|$)', log, re.M)
    if not found:
        return None
    x, y, w, h = map(int, found[-1])
    return x + w // 2, y + h // 2


def wait_for(vm, needle, secs=40):
    end = time.time() + secs
    while time.time() < end:
        log = vm.wm_log()
        if needle in log:
            return log
        time.sleep(1)
    return None


def active():
    return vm.run("nmcli -t -f ACTIVE,SSID dev wifi | grep '^yes:' | cut -d: -f2")


vm = VM()
try:
    vm.boot()
    assert vm.wait_wm('wm: running at', 60), vm.wm_log()

    # Four radios: wlan0 for NetworkManager, and the rest access points it leaves alone.
    vm.run("printf '[keyfile]\\nunmanaged-devices=interface-name:wlan1;interface-name:wlan2;interface-name:wlan3\\n' "
           '> /etc/NetworkManager/conf.d/hwsim.conf; nmcli general reload conf; modprobe mac80211_hwsim radios=4; sleep 3', 30)
    started = ''
    for n, (card, network) in enumerate(ACCESS_POINTS.items(), 1):
        conf = f'interface={card}\\ndriver=nl80211\\nhw_mode=g\\n' + network.replace('\n', '\\n')
        started += vm.run(f"printf '{conf}\\n' > /tmp/{card}.conf; hostapd -B /tmp/{card}.conf | tail -1", 20)
        # Each its own addresses, by DHCP, as a router would give.
        vm.run(f'ip addr add 10.9.{n}.1/24 dev {card}; dnsmasq --interface={card} --bind-interfaces --port=0 '
               f'--dhcp-range=10.9.{n}.10,10.9.{n}.50 --pid-file=/tmp/dnsmasq-{card}.pid', 20)
    check('hostapd starts the access points', started.count('AP-ENABLED') == 3, started)
    seen = ''
    for _ in range(20):
        seen = vm.run('nmcli -t -f SSID dev wifi list --rescan yes', 30)
        if 'AximTest' in seen and 'AximOpen' in seen:
            break
        time.sleep(2)
    check('the simulated networks are on the air', 'AximTest' in seen and 'AximOpen' in seen, seen)

    # The panel, from the network's icon.
    vm.click(*center_of(vm.wm_log(), 'network icon'))
    log = wait_for(vm, ', AximOpen') and wait_for(vm, ', AximTest')
    check('the panel lists the networks nearby', log and row(log, 'AximOpen') and row(log, 'AximTest'), vm.wm_log())
    check('and the cable', log and re.search(r'wm: wired \S+ connected', log), log)
    check('but not the hidden one', log and ', AximHidden' not in log, log)
    vm.save_screenshot('wifi-panel')

    # An open network joins with a click.
    vm.click(*row(vm.wm_log(), 'AximOpen'))
    check('an open network joins', wait_for(vm, 'wm: joined AximOpen'), vm.wm_log())
    check('as NetworkManager says', active() == 'AximOpen', active())

    # A secured one asks for its password, and says when it's wrong.
    vm.click(*row(vm.wm_log(), 'AximTest'))
    check('a secured network asks for its password', wait_for(vm, 'wm: wifi field password'), vm.wm_log())
    vm.keys('wrongpass\n')
    log = wait_for(vm, "wm: couldn't join AximTest", 90)
    check('a wrong password is said to be wrong', log and "That password isn't right" in log, vm.wm_log())
    check('and the network is not saved with it', 'AximTest' not in vm.run('nmcli -t -f NAME connection'),
          vm.run('nmcli -t -f NAME connection'))
    vm.click(*row(vm.wm_log(), 'AximTest'))
    wait_for(vm, 'wm: wifi field password')
    vm.keys('secret123\n')
    check('the right password joins it', wait_for(vm, 'wm: joined AximTest', 60), vm.wm_log())
    check('as NetworkManager says', active() == 'AximTest', active())
    vm.save_screenshot('wifi-joined')

    # Forgetting the joined network, from its row.
    vm.click(*row(vm.wm_log(), 'AximTest'))
    log = wait_for(vm, 'wm: wifi button Forget')
    check('the joined network can be forgotten or left', log and 'wm: wifi button Disconnect' in log, vm.wm_log())
    vm.click(*center_of(vm.wm_log(), 'wifi button Forget'))
    check('forgetting it', wait_for(vm, 'wm: forgot AximTest'), vm.wm_log())
    time.sleep(2)
    check('takes it from NetworkManager', 'AximTest' not in vm.run('nmcli -t -f NAME connection'),
          vm.run('nmcli -t -f NAME connection'))

    # A hidden network, by its name.
    vm.click(*center_of(vm.wm_log(), 'wifi row hidden'))
    check('the hidden network row asks for a name', wait_for(vm, 'wm: wifi field name'), vm.wm_log())
    vm.keys('AximHidden\thidden123\n')
    check('a hidden network joins', wait_for(vm, 'wm: joined AximHidden', 60), vm.wm_log())
    check('as NetworkManager says', 'AximHidden' in vm.run('nmcli -t -f NAME connection show --active'),
          vm.run('nmcli -t -f NAME connection show --active'))

    # Wi-Fi off from the panel's switch, then back on from airplane mode's tile.
    vm.click(*center_of(vm.wm_log(), 'wifi on'))
    time.sleep(3)
    check("the switch turns Wi-Fi off", vm.run('nmcli radio wifi') == 'disabled', vm.run('nmcli radio wifi'))
    vm.click(*center_of(vm.wm_log(), "quick settings' icons"))
    time.sleep(1)
    vm.click(*center_of(vm.wm_log(), 'tile 0'))
    time.sleep(3)
    check('turning airplane mode off turns it on', vm.run('nmcli radio wifi') == 'enabled', vm.run('nmcli radio wifi'))
finally:
    vm.close()
finish()
