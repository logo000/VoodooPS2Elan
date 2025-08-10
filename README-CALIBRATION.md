# 🔬 VoodooPS2 ETD0180 Live Calibration System

Ein umfassendes System zur Live-Kalibrierung des ETD0180 Trackpads mit Echtzeit-API für Claude-Integration.

## 🚀 Quick Start

### 1. API Server starten
```bash
cd /Volumes/INTENSO/VoodooPS2Elan
node claude-api-server.js
```

### 2. Kalibrierungs-Tool öffnen
- Öffne `calibration-tool.html` in Browser
- Oder direkte URL: `file:///Volumes/INTENSO/VoodooPS2Elan/calibration-tool.html`

### 3. Live-Kalibrierung starten
1. **Connect to Claude** klicken (aktiviert API)
2. **Start Capture** klicken (startet Log-Monitoring)
3. **Trackpad bewegen** (generiert Kalibrierdaten)
4. **Parameter anpassen** (Gain, Sensitivity, Ranges)
5. **Share Data with Claude** (teilt Daten mit mir)

## 📊 System-Komponenten

### HTML Calibration Tool (`calibration-tool.html`)
- **Live Trackpad-Visualisierung** mit Heat Map
- **Real-time Controls** für Gain, Sensitivity, Coordinate Ranges
- **Live Log Processing** direkt aus macOS Console
- **Professional UI** mit modernem macOS-Design
- **Export/Import** von Kalibrierungs-Settings

### Claude API Server (`claude-api-server.js`)
- **Echtes Log Processing** via `log stream --predicate "message CONTAINS 'ELAN_CALIB'"`
- **Live API Endpunkte** für Claude-Integration
- **Real-time Data Processing** der ETD0180-Pakete
- **WebSocket-ähnliche** Live-Updates
- **Comprehensive Logging** aller Trackpad-Events

## 🔗 API Endpunkte (für Claude)

```bash
# Server Status
GET http://localhost:8080/api/status

# Live-Logs (letzte 20 Einträge)
GET http://localhost:8080/api/logs?limit=20

# Komplett-Status mit allen Daten
GET http://localhost:8080/api/calibration

# Live Trackpad-Daten
GET http://localhost:8080/api/realtime

# Metriken und Statistiken
GET http://localhost:8080/api/metrics

# Settings aktualisieren
POST http://localhost:8080/api/settings
{
  "gain": 3.0,
  "sensitivity": 1.0,
  "maxX": 2080,
  "maxY": 2412
}

# Log-Capture starten/stoppen
POST http://localhost:8080/api/start-capture
POST http://localhost:8080/api/stop-capture
```

## 🎯 Live-Kalibrierungs-Workflow

### Phase 1: System-Setup
1. ✅ ETD0180 Kext installiert und aktiv
2. ✅ API Server läuft (`node claude-api-server.js`)
3. ✅ Calibration Tool geöffnet
4. ✅ Claude API verbunden

### Phase 2: Basis-Kalibrierung
1. **Log Capture starten** → Echte ELAN_CALIB Logs werden verarbeitet
2. **Trackpad-Bewegungen** → Basis-Bewegungsverhalten analysieren
3. **Gain-Anpassung** → Cursor-Geschwindigkeit kalibrieren
4. **Range-Validierung** → Koordinaten-Bereiche verifizieren

### Phase 3: Fein-Tuning
1. **Heat Map Analysis** → Bewegungsmuster identifizieren
2. **Button Testing** → Left/Right/Middle Button-Funktionalität
3. **Edge Cases** → Ecken und Ränder des Trackpads
4. **Performance Metrics** → Paket-Rate und Response-Zeit

### Phase 4: Claude-Integration
1. **Live Data Sharing** → Kontinuierliche Datenübertragung an Claude
2. **Real-time Feedback** → Claude kann Settings live anpassen
3. **Problem Detection** → Automatische Erkennung von Anomalien
4. **Optimization** → KI-gestützte Parameter-Optimierung

## 📈 Live-Monitoring Features

### Real-time Visualisierung
- **Cursor Position** - Live Trackpad-Position mit Heat Map
- **Delta Movements** - dX/dY Bewegungs-Deltas in Echtzeit
- **Button States** - L/R/M Button-Status live
- **Coordinate Ranges** - Min/Max Werte mit Live-Clamping

### Comprehensive Metrics
- **Total Packets** - Gesamtzahl verarbeiteter Pakete
- **Movement Ratio** - Verhältnis Bewegung/Stillstand
- **Button Clicks** - Anzahl Button-Ereignisse
- **Average Delta** - Durchschnittliche Bewegungsstärke
- **Packets/Second** - Übertragungsrate

### Advanced Analytics
- **Heat Map Tracking** - Bewegungsmuster über Zeit
- **Performance Profiling** - Latenz und Response-Zeit
- **Error Detection** - Packet-Verluste und Anomalien
- **Calibration History** - Änderungs-Verlauf der Parameter

## 🤖 Claude-Integration

Das System ist speziell für die Zusammenarbeit mit Claude optimiert:

### Live-Monitoring
```javascript
// Claude kann jederzeit auf aktuelle Daten zugreifen
const status = await fetch('http://localhost:8080/api/realtime');
const data = await status.json();

console.log(`Current Position: ${data.currentPosition.x}, ${data.currentPosition.y}`);
console.log(`Last Delta: dx=${data.lastDelta.dx}, dy=${data.lastDelta.dy}`);
```

### Parameter-Anpassung
```javascript
// Claude kann Settings live ändern
await fetch('http://localhost:8080/api/settings', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({
    gain: 2.5,        // Neue Gain-Einstellung
    maxX: 2100,       // Erweiterte X-Range
    maxY: 2400        // Angepasste Y-Range
  })
});
```

### Anomalie-Erkennung
```javascript
// Claude kann Probleme automatisch erkennen
const logs = await fetch('http://localhost:8080/api/logs?limit=50');
const recentLogs = await logs.json();

// Analyse auf Packet-Verluste, ungewöhnliche Deltas, etc.
const errors = recentLogs.logs.filter(log => log.type === 'error');
if (errors.length > 0) {
  console.log('🚨 Detected issues:', errors);
}
```

## 🔧 Troubleshooting

### Server startet nicht
```bash
# Node.js installieren (falls nicht vorhanden)
brew install node

# Dependencies prüfen
node --version  # Sollte > 14.0 sein

# Port-Konflikt prüfen
lsof -i :8080
```

### Logs werden nicht empfangen
```bash
# Log-Berechtigung prüfen
sudo log stream --predicate 'message CONTAINS "ELAN_CALIB"' --style compact

# Kext-Status prüfen
kextstat | grep VoodooPS2

# System-Logs manuell testen
dmesg | grep ELAN_CALIB
```

### Trackpad reagiert nicht
```bash
# Reboot durchführen (wichtig nach Kext-Installation)
sudo reboot

# Trackpad-Aktivität prüfen
ioreg -l | grep -i elan

# VoodooPS2 Status
sudo kextutil -t /Volumes/INTENSO/EFI/OC/Kexts/VoodooPS2Controller.kext
```

## 📋 Nächste Schritte

1. **Kext installieren und rebooten**
2. **API Server starten** (`node claude-api-server.js`)
3. **Calibration Tool öffnen** (HTML-Datei)
4. **Claude verbinden** und Live-Kalibrierung beginnen
5. **Parameter optimieren** basierend auf Real-time Feedback

Das System ist bereit für professionelle ETD0180-Kalibrierung mit vollständiger Claude-Integration! 🎯