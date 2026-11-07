#!/bin/sh
# Installe rndis-tether :
#   - service système (LaunchDaemon, root) : démarre au boot, se connecte dès qu'un téléphone est branché
#   - icône de barre de menus (LaunchAgent) pour activer/désactiver
# Usage :  sudo ./install.sh
set -e

if [ "$(id -u)" != 0 ] || [ -z "$SUDO_USER" ] || [ "$SUDO_USER" = root ]; then
    echo "Lance depuis ta session avec : sudo ./install.sh" >&2
    exit 1
fi
USER_NAME="$SUDO_USER"
case "$USER_NAME" in *[!A-Za-z0-9._-]*) echo "Nom d'utilisateur inattendu: $USER_NAME" >&2; exit 1;; esac
USER_UID=$(id -u "$USER_NAME")
USER_HOME=$(dscl . -read "/Users/$USER_NAME" NFSHomeDirectory | awk '{print $2}')
HERE=$(cd "$(dirname "$0")" && pwd)

BIN=/usr/local/bin/rndis-tether
APP=/Applications/RNDISTether.app
DAEMON_LABEL=io.github.rndis-tether
AGENT_LABEL=io.github.rndis-tether.menubar
DAEMON_PLIST=/Library/LaunchDaemons/$DAEMON_LABEL.plist
AGENT_PLIST=$USER_HOME/Library/LaunchAgents/$AGENT_LABEL.plist

echo "==> Compilation (en tant que $USER_NAME)"
sudo -u "$USER_NAME" make -C "$HERE"
sudo -u "$USER_NAME" "$HERE/menubar/build.sh"

echo "==> Arrêt des anciennes instances"
launchctl bootout "system/$DAEMON_LABEL" 2>/dev/null || true
launchctl bootout "gui/$USER_UID/$AGENT_LABEL" 2>/dev/null || true
pkill -x rndis-tether 2>/dev/null || true
sleep 1

echo "==> Installation des fichiers"
mkdir -p /usr/local/bin
install -m 755 "$HERE/rndis-tether" "$BIN"
rm -rf "$APP"
cp -R "$HERE/menubar/build/RNDISTether.app" "$APP"
mkdir -p /usr/local/share/man/man1
install -m 644 "$HERE/man/rndis-tether.1" /usr/local/share/man/man1/rndis-tether.1
# rotation du journal (newsyslog) : 1 Mo, cinq archives compressées
mkdir -p /etc/newsyslog.d
install -m 644 -o root -g wheel "$HERE/etc/rndis-tether.newsyslog.conf" /etc/newsyslog.d/rndis-tether.conf

cat > "$DAEMON_PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$DAEMON_LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$BIN</string>
        <string>-r</string><string>-w</string><string>-a</string>
        <string>-U</string><string>$USER_NAME</string>
    </array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>ThrottleInterval</key><integer>10</integer>
    <key>StandardOutPath</key><string>/var/log/rndis-tether.log</string>
    <key>StandardErrorPath</key><string>/var/log/rndis-tether.log</string>
</dict>
</plist>
EOF
chown root:wheel "$DAEMON_PLIST"
chmod 644 "$DAEMON_PLIST"

sudo -u "$USER_NAME" mkdir -p "$USER_HOME/Library/LaunchAgents"
cat > "$AGENT_PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$AGENT_LABEL</string>
    <key>ProgramArguments</key>
    <array><string>$APP/Contents/MacOS/RNDISTether</string></array>
    <key>RunAtLoad</key><true/>
    <key>LimitLoadToSessionType</key><string>Aqua</string>
</dict>
</plist>
EOF
chown "$USER_NAME" "$AGENT_PLIST"
chmod 644 "$AGENT_PLIST"

echo "==> Démarrage"
launchctl bootstrap system "$DAEMON_PLIST"
launchctl bootstrap "gui/$USER_UID" "$AGENT_PLIST"

echo
echo "Terminé. Le service démarre au boot, l'icône à l'ouverture de session."
echo "Journal : /var/log/rndis-tether.log   Désinstaller : sudo ./uninstall.sh"
