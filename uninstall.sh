#!/bin/sh
# Désinstalle rndis-tether (service, icône, fichiers). Usage : sudo ./uninstall.sh
set -e

if [ "$(id -u)" != 0 ] || [ -z "$SUDO_USER" ] || [ "$SUDO_USER" = root ]; then
    echo "Lance depuis ta session avec : sudo ./uninstall.sh" >&2
    exit 1
fi
USER_NAME="$SUDO_USER"
USER_UID=$(id -u "$USER_NAME")
USER_HOME=$(dscl . -read "/Users/$USER_NAME" NFSHomeDirectory | awk '{print $2}')
DAEMON_LABEL=io.github.rndis-tether
AGENT_LABEL=io.github.rndis-tether.menubar

# bootout envoie SIGTERM : le service retire ses routes et son DNS avant de quitter
launchctl bootout "gui/$USER_UID/$AGENT_LABEL" 2>/dev/null || true
launchctl bootout "system/$DAEMON_LABEL" 2>/dev/null || true
sleep 1
pkill -x rndis-tether 2>/dev/null || true

rm -f "/Library/LaunchDaemons/$DAEMON_LABEL.plist" "$USER_HOME/Library/LaunchAgents/$AGENT_LABEL.plist"
rm -f /usr/local/bin/rndis-tether /usr/local/share/man/man1/rndis-tether.1
rm -rf /Applications/RNDISTether.app
rm -f /Users/Shared/rndis-tether.disabled /Users/Shared/rndis-tether.status /var/log/rndis-tether.log /var/log/rndis-tether.log.*
rm -f /etc/newsyslog.d/rndis-tether.conf

echo "rndis-tether désinstallé."
