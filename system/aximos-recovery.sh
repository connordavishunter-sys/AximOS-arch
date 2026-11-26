# On the recovery console, from the boot menu, where wm isn't started: how to undo
# the last update, which may be why it was chosen.
if [ "$(tty 2>/dev/null)" = /dev/tty1 ] && ! systemctl -q is-active display-manager 2>/dev/null; then
    echo "AximOS recovery: 'sudo aximos-update --undo' puts back what the last update changed,"
    echo "then 'reboot'. 'sudo aximos-update' updates again, once there's a fix."
fi
