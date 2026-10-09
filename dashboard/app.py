"""
EmbryoGuard Pro — Flask Dashboard Backend
==========================================
Real-time incubator monitoring + actuator control.

Architecture:
  ESP32 Node 1/2 → HTTP POST /api/data  → Flask stores + broadcasts via WebSocket
  Browser         ← WebSocket            ← real-time sensor updates
  Browser         → WebSocket 'control'  → Flask queues command
  ESP32 Node 1    → HTTP GET /api/commands/1 → polls + executes commands
"""

from flask import Flask, render_template, jsonify, request
from flask_socketio import SocketIO, emit
from datetime import datetime
import time, json, sqlite3, os, socket

app = Flask(__name__)
app.config["SECRET_KEY"] = "embryoguard-secret"
app.config["TEMPLATES_AUTO_RELOAD"] = True
app.jinja_env.auto_reload = True
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")

# ── Configuration ─────────────────────────────────────────────────
HISTORY_MAX     = 500          # max data points kept in memory
NODE_TIMEOUT_S  = 15           # seconds before a node is marked offline
DB_PATH         = os.path.join(os.path.dirname(__file__), "incubator.db")

# ── Embryo thresholds (mirrored from firmware) ────────────────────
TH = {
    "temp_danger_low": 35.0, "temp_cold": 36.5,
    "temp_target": 37.5,
    "temp_hot": 38.5, "temp_danger_high": 39.0,
    "hum_danger_low": 38.0, "hum_warn_low": 45.0,
    "hum_warn_high": 65.0, "hum_danger_high": 75.0,
    "mq_warn": 800, "mq_danger": 1200,
    "light_warn": 60, "light_danger": 85,
}

# ── In-memory state ──────────────────────────────────────────────
nodes = {
    1: {"online": False, "last_seen": None, "last_ts": 0,
        "temperature": None, "humidity": None,
        "fan": False, "lamp": "OFF",
        "servo_pos": "A", "servo_angle": 0, "servo_turns": 0,
        "incubation_day": 1, "sensor_ok": True, "cycle": 0,
        "mode": "AUTO", "egg_type": "chicken"},
    2: {"online": False, "last_seen": None, "last_ts": 0,
        "temperature": None, "humidity": None,
        "mq135_raw": 0, "mq135_volts": 0.0, "mq135_dout": 1,
        "light_pct": 0, "wifi_ok": False, "sensor_ok": True, "cycle": 0,
        "mode": "AUTO"},
}
# ── Egg type profiles (thresholds per species) ────────────────────
egg_profiles = {
    "chicken": {
        "name": "Chicken",
        "temp_target": 37.5,
        "temp_cold": 36.5,
        "temp_hot": 38.5,
        "hum_warn_low": 45,
        "hum_warn_high": 65,
        "incubation_day": 21
    },
    "duck": {
        "name": "Duck",
        "temp_target": 37.7,
        "temp_cold": 37.0,
        "temp_hot": 39.0,
        "hum_warn_low": 50,
        "hum_warn_high": 70,
        "incubation_day": 28
    },
    "quail": {
        "name": "Quail",
        "temp_target": 37.8,
        "temp_cold": 37.5,
        "temp_hot": 39.5,
        "hum_warn_low": 40,
        "hum_warn_high": 60,
        "incubation_day": 18
    },
    "goose": {
        "name": "Goose",
        "temp_target": 37.6,
        "temp_cold": 36.8,
        "temp_hot": 38.8,
        "hum_warn_low": 55,
        "hum_warn_high": 75,
        "incubation_day": 25
    },
}

# ── In-memory settings (can be saved to file later) ──────────────
settings = {
    "egg_type": "chicken",  # Current egg type
    "temp_target": 37.5,
    "temp_cold": 36.5,
    "temp_hot": 38.5,
    "hum_warn_low": 45,
    "hum_warn_high": 65,
}

# ── In-memory history (sensor readings over time) ────────────────
history = {
    1: [],  # Node 1 temperature/humidity history
    2: [],  # Node 2 temperature/humidity history
}

# ── Command queue for each node ───────────────────────────────────
commands = {
    1: [],  # Commands for Node 1
    2: [],  # Commands for Node 2
}

# ── Database ─────────────────────────────────────────────────────
def init_db():
    c = sqlite3.connect(DB_PATH)
    c.execute("""CREATE TABLE IF NOT EXISTS sensor_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts TEXT, node INTEGER, temperature REAL, humidity REAL, extra TEXT)""")
    c.commit(); c.close()

def log_to_db(node, temp, hum, extra=None):
    try:
        c = sqlite3.connect(DB_PATH)
        c.execute("INSERT INTO sensor_log (ts,node,temperature,humidity,extra) VALUES (?,?,?,?,?)",
                  (datetime.now().isoformat(), node, temp, hum,
                   json.dumps(extra) if extra else None))
        c.commit(); c.close()
    except Exception as e:
        print(f"[DB] {e}")

# ── Egg type management ──────────────────────────────────────────
def set_egg_type(egg_type):
    """Change egg type and queue command to Node 1"""
    if egg_type not in egg_profiles:
        return False
    
    # Update settings with new profile
    profile = egg_profiles[egg_type]
    settings["egg_type"] = egg_type
    settings["temp_target"] = profile["temp_target"]
    settings["temp_cold"] = profile["temp_cold"]
    settings["temp_hot"] = profile["temp_hot"]
    settings["hum_warn_low"] = profile["hum_warn_low"]
    settings["hum_warn_high"] = profile["hum_warn_high"]
    
    # Sync global thresholds for alerts
    TH["temp_cold"] = profile["temp_cold"]
    TH["temp_hot"] = profile["temp_hot"]
    TH["hum_warn_low"] = profile["hum_warn_low"]
    TH["hum_warn_high"] = profile["hum_warn_high"]
    
    # Queue egg_type command to Node 1
    commands[1].append({"command": "egg_type", "value": egg_type, "temp_target": profile["temp_target"]})
    
    print(f"[EGG] Changed to {profile['name']}")
    return True

# ── Alert evaluation ─────────────────────────────────────────────
def _check_health():
    now = time.time()
    for n in nodes:
        nodes[n]["online"] = (now - nodes[n].get("last_ts", 0)) < NODE_TIMEOUT_S

def evaluate_alerts():
    _check_health()
    alerts = []
    for nid, d in nodes.items():
        if not d["online"]:
            alerts.append({"level": "danger", "msg": f"Node {nid} OFFLINE — no heartbeat", "node": nid})
            continue
        if not d.get("sensor_ok", True):
            alerts.append({"level": "danger", "msg": f"Node {nid}: DHT22 SENSOR FAILURE", "node": nid})
        t, h = d.get("temperature"), d.get("humidity")
        if t is not None:
            if t >= TH["temp_danger_high"]:
                alerts.append({"level": "danger", "msg": f"Node {nid}: TEMP {t:.1f}°C ≥ 39°C — LETHAL", "node": nid})
            elif t <= TH["temp_danger_low"]:
                alerts.append({"level": "danger", "msg": f"Node {nid}: TEMP {t:.1f}°C ≤ 35°C — ARREST RISK", "node": nid})
            elif t > TH["temp_hot"]:
                alerts.append({"level": "warning", "msg": f"Node {nid}: Temp high ({t:.1f}°C)", "node": nid})
            elif t < TH["temp_cold"]:
                alerts.append({"level": "warning", "msg": f"Node {nid}: Temp low ({t:.1f}°C)", "node": nid})
        if h is not None:
            if h >= TH["hum_danger_high"]:
                alerts.append({"level": "danger", "msg": f"Node {nid}: Humidity {h:.1f}% — MOLD RISK", "node": nid})
            elif h <= TH["hum_danger_low"]:
                alerts.append({"level": "danger", "msg": f"Node {nid}: Humidity {h:.1f}% — DESICCATION", "node": nid})
            elif h > TH["hum_warn_high"]:
                alerts.append({"level": "warning", "msg": f"Node {nid}: Humidity high ({h:.1f}%)", "node": nid})
            elif h < TH["hum_warn_low"]:
                alerts.append({"level": "warning", "msg": f"Node {nid}: Humidity low ({h:.1f}%)", "node": nid})
        if nid == 2:
            mq = d.get("mq135_raw", 0)
            if d.get("mq135_dout", 1) == 0 or mq >= TH["mq_danger"]:
                alerts.append({"level": "danger", "msg": f"Node 2: HIGH AMMONIA (ADC:{mq})", "node": 2})
            elif mq >= TH["mq_warn"]:
                alerts.append({"level": "warning", "msg": f"Node 2: Ammonia rising ({mq})", "node": 2})
            lp = d.get("light_pct", 0)
            if lp >= TH["light_danger"]:
                alerts.append({"level": "danger", "msg": f"Node 2: HIGH LIGHT {lp}% — lid open?", "node": 2})
            elif lp >= TH["light_warn"]:
                alerts.append({"level": "warning", "msg": f"Node 2: Light elevated ({lp}%)", "node": 2})
    return alerts

def _build_payload():
    return {
        "nodes": {str(k): v for k, v in nodes.items()},
        "alerts": evaluate_alerts(),
        "history": {str(k): v[-100:] for k, v in history.items()},
        "thresholds": TH,
    }

# ── Routes ────────────────────────────────────────────────────────
@app.route("/")
def index():
    return render_template("index.html")

@app.route("/api/data", methods=["POST"])
def receive_data():
    """ESP32 nodes POST sensor data here every cycle."""
    d = request.get_json(force=True)
    nid = d.get("node", 0)
    if nid not in (1, 2):
        return jsonify(error="bad node"), 400

    n = nodes[nid]
    n["online"]   = True
    n["last_seen"] = datetime.now().isoformat()
    n["last_ts"]  = time.time()

    for k in ("temperature", "humidity", "sensor_ok", "cycle", "mode"):
        if k in d: n[k] = d[k]
    if nid == 1:
        for k in ("fan", "lamp", "servo_pos", "servo_angle", "servo_turns", "incubation_day", "egg_type"):
            if k in d: n[k] = d[k]
    if nid == 2:
        for k in ("mq135_raw", "mq135_volts", "mq135_dout", "light_pct", "wifi_ok"):
            if k in d: n[k] = d[k]

    entry = {"time": datetime.now().strftime("%H:%M:%S"),
             "ts": time.time(),
             "temperature": d.get("temperature"),
             "humidity": d.get("humidity")}
    if nid == 2:
        entry["mq135_raw"] = d.get("mq135_raw", 0)
        entry["light_pct"] = d.get("light_pct", 0)
    history[nid].append(entry)
    if len(history[nid]) > HISTORY_MAX:
        history[nid] = history[nid][-HISTORY_MAX:]

    log_to_db(nid, d.get("temperature"), d.get("humidity"),
              {k: v for k, v in d.items() if k not in ("node", "temperature", "humidity")})

    socketio.emit("sensor_update", _build_payload())
    return jsonify(status="ok", egg_type=settings["egg_type"]), 200

@app.route("/api/commands/<int:nid>", methods=["GET"])
def get_commands(nid):
    """ESP32 polls for queued commands, cleared after retrieval."""
    if nid not in (1, 2):
        return jsonify(error="bad node"), 400
    cmds = commands[nid]
    commands[nid] = []
    return jsonify(commands=cmds), 200

@app.route("/api/server-info", methods=["GET"])
def get_server_info():
    """Return server's IP address and port for QR code generation."""
    try:
        # Get local IP by connecting to a remote socket
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
    except Exception:
        ip = "127.0.0.1"
    return jsonify(ip=ip, port=8080, url=f"http://{ip}:8080"), 200

@app.route("/api/state", methods=["GET"])
def get_state():
    """Browser fetches full state on page load."""
    payload = _build_payload()
    payload["settings"] = settings  # include settings
    return jsonify(payload), 200

@app.route("/api/settings", methods=["POST"])
def set_settings():
    """Update automatic mode settings."""
    global settings
    data = request.get_json(force=True)
    
    # Validate and update
    if "temp_cold" in data:
        settings["temp_cold"] = float(data["temp_cold"])
        TH["temp_cold"] = settings["temp_cold"]
    if "temp_hot" in data:
        settings["temp_hot"] = float(data["temp_hot"])
        TH["temp_hot"] = settings["temp_hot"]
    if "hum_warn_low" in data:
        settings["hum_warn_low"] = int(data["hum_warn_low"])
        TH["hum_warn_low"] = settings["hum_warn_low"]
    if "hum_warn_high" in data:
        settings["hum_warn_high"] = int(data["hum_warn_high"])
        TH["hum_warn_high"] = settings["hum_warn_high"]
    
    print(f"[SETTINGS] Updated: {settings}")
    
    # Broadcast new settings to all clients
    socketio.emit("settings_update", settings)
    
    return jsonify(status="ok", settings=settings), 200

@app.route("/api/egg-type", methods=["POST"])
def set_egg_profile():
    """Change egg type and apply corresponding thresholds."""
    data = request.get_json(force=True)
    egg_type = data.get("egg_type", "chicken").lower()
    
    if set_egg_type(egg_type):
        socketio.emit("egg_type_update", {
            "egg_type": settings["egg_type"],
            "profile": egg_profiles[egg_type]
        })
        return jsonify(status="ok", egg_type=egg_type, profile=egg_profiles[egg_type]), 200
    
    return jsonify(status="error", message="Invalid egg type"), 400

# ── WebSocket ─────────────────────────────────────────────────────
@socketio.on("connect")
def ws_connect():
    emit("sensor_update", _build_payload())

@socketio.on("control")
def ws_control(data):
    """Browser sends actuator command → queued for ESP32 polling."""
    nid = data.get("node", 0)
    cmd = (data.get("command", "") or "").strip()
    val = data.get("value", "")

    # Coerce node ids that may arrive as strings (e.g. "1")
    try:
        nid = int(nid)
    except Exception:
        return
    if nid not in (1, 2):
        return

    # Handle mode command (AUTO/MANUAL)
    if cmd == "mode":
        if isinstance(val, str):
            val = val.strip().upper()
        if val in ("AUTO", "MANUAL"):
            nodes[nid]["mode"] = val
            commands[nid].append({"command": cmd, "value": val, "ts": datetime.now().isoformat()})
            print(f"[CMD] queued nid={nid} cmd={cmd} value={val}")
            # Broadcast mode change immediately to all connected clients
            emit("sensor_update", _build_payload(), broadcast=True)
            emit("command_ack", {"node": nid, "command": cmd, "value": val}, broadcast=True)
        return

    # Normalize common value shapes
    if cmd == "fan":
        if isinstance(val, str):
            v = val.strip().lower()
            if v in ("on", "true", "1", "yes"): val = True
            elif v in ("off", "false", "0", "no"): val = False
        else:
            val = bool(val)
    elif cmd == "lamp":
        if isinstance(val, str):
            val = val.strip().upper()
    elif cmd == "servo":
        # Value is ignored by firmware; keep a consistent payload.
        val = "turn"

    commands[nid].append({"command": cmd, "value": val, "ts": datetime.now().isoformat()})
    print(f"[CMD] queued nid={nid} cmd={cmd} value={val}")
    # Optimistic UI update
    if nid == 1:
        if cmd == "fan":
            nodes[1]["fan"] = bool(val)
        if cmd == "lamp":
            nodes[1]["lamp"] = val
    emit("command_ack", {"node": nid, "command": cmd, "value": val}, broadcast=True)

# ── Main ──────────────────────────────────────────────────────────
if __name__ == "__main__":
    init_db()
    print("=" * 52)
    print("  EmbryoGuard Pro — Incubator Dashboard")
    print("  http://0.0.0.0:8080")
    print("=" * 52)
    # Run with threading async mode (Windows-compatible, avoids deprecated Eventlet)
    socketio.run(app, host="0.0.0.0", port=8080, debug=False, allow_unsafe_werkzeug=True)
