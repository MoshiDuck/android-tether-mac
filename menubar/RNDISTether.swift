// SPDX-License-Identifier: GPL-3.0-or-later
// Icône de barre de menus pour rndis-tether.
// Parle au service (root) uniquement par deux fichiers dans /Users/Shared :
//   rndis-tether.disabled  -> présent = tethering désactivé (créé/supprimé ici)
//   rndis-tether.status    -> état écrit par le service (rafraîchi toutes les ~5 s)
import Cocoa

private let disabledPath = "/Users/Shared/rndis-tether.disabled"
private let statusPath = "/Users/Shared/rndis-tether.status"
private let logPath = "/var/log/rndis-tether.log"

enum TetherState {
    case disabled, serviceDown, waiting, connecting
    case connected(ip: String, iface: String)
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let statusLine = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private let toggleItem = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    private var timer: Timer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let menu = NSMenu()
        statusLine.isEnabled = false
        menu.addItem(statusLine)
        menu.addItem(.separator())
        toggleItem.target = self
        toggleItem.action = #selector(toggle)
        menu.addItem(toggleItem)
        menu.addItem(.separator())
        let log = NSMenuItem(title: "Ouvrir le journal", action: #selector(openLog), keyEquivalent: "")
        log.target = self
        menu.addItem(log)
        menu.addItem(NSMenuItem(title: "Quitter l'icône", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
        item.menu = menu

        refresh()
        let t = Timer(timeInterval: 2, repeats: true) { [weak self] _ in self?.refresh() }
        RunLoop.main.add(t, forMode: .common) // continue de se mettre à jour menu ouvert
        timer = t
    }

    private func readState() -> TetherState {
        let fm = FileManager.default
        if fm.fileExists(atPath: disabledPath) { return .disabled }
        guard let attrs = try? fm.attributesOfItem(atPath: statusPath),
              let mtime = attrs[.modificationDate] as? Date,
              Date().timeIntervalSince(mtime) < 20,
              let text = try? String(contentsOfFile: statusPath, encoding: .utf8)
        else { return .serviceDown }
        let parts = text.trimmingCharacters(in: .whitespacesAndNewlines).split(separator: " ").map(String.init)
        switch parts.first {
        case "connected":
            return .connected(ip: parts.count > 1 ? parts[1] : "", iface: parts.count > 2 ? parts[2] : "")
        case "connecting": return .connecting
        default: return .waiting
        }
    }

    private func refresh() {
        let state = readState()
        let symbol: String, line: String, enabled: Bool
        switch state {
        case .disabled:
            symbol = "antenna.radiowaves.left.and.right.slash"
            line = "Tethering USB désactivé"; enabled = false
        case .serviceDown:
            symbol = "exclamationmark.triangle"
            line = "Service arrêté (voir le journal)"; enabled = true
        case .waiting:
            symbol = "cable.connector"
            line = "En attente d'un téléphone…"; enabled = true
        case .connecting:
            symbol = "arrow.triangle.2.circlepath"
            line = "Connexion…"; enabled = true
        case .connected(let ip, let iface):
            symbol = "antenna.radiowaves.left.and.right"
            line = "Connecté — \(ip) (\(iface))"; enabled = true
        }
        statusLine.title = line
        toggleItem.title = enabled ? "Désactiver le tethering" : "Activer le tethering"
        if let button = item.button {
            let image = NSImage(systemSymbolName: symbol, accessibilityDescription: line)
            image?.isTemplate = true
            button.image = image
            button.title = image == nil ? "USB" : ""
            button.toolTip = line
        }
    }

    @objc private func toggle() {
        let fm = FileManager.default
        if fm.fileExists(atPath: disabledPath) {
            try? fm.removeItem(atPath: disabledPath)
        } else {
            fm.createFile(atPath: disabledPath, contents: Data(), attributes: [.posixPermissions: 0o666])
        }
        refresh()
    }

    @objc private func openLog() {
        NSWorkspace.shared.open(URL(fileURLWithPath: logPath))
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
