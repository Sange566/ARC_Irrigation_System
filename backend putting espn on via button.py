from flask import Flask, render_template_string
from flask_socketio import SocketIO
import paho.mqtt.client as mqtt
import json
import threading

# --- Flask & SocketIO Setup ---
app = Flask(__name__)
socketio = SocketIO(app, async_mode='eventlet')

# --- MQTT Configuration ---
MQTT_BROKER = "192.168.43.183" # Your MQTT Broker IP
MQTT_PORT = 1883
MQTT_DATA_TOPIC = "grp6_irrigation_data"
MQTT_COMMAND_TOPIC = "grp6_irrigation_command" # Topic to send commands on

# --- HTML & JavaScript for the Control Panel ---
html_template = """
<!DOCTYPE html>
<html>
<head>
    <title>Irrigation Control Panel</title>
    <style>
        body { font-family: sans-serif; background-color: #282c34; color: #abb2bf; text-align: center; }
        .container { display: flex; justify-content: center; gap: 2rem; flex-wrap: wrap; }
        .card { background-color: #323842; margin: 1rem; padding: 1.5rem; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.2); min-width: 300px; }
        h1, h2 { color: #61afef; }
        #data span { color: #98c379; font-weight: bold; font-size: 1.2em; }
        button { background-color: #61afef; color: #282c34; border: none; padding: 15px 30px; margin: 10px; border-radius: 5px; cursor: pointer; font-size: 1.1em; transition: background-color 0.3s; }
        button:hover { background-color: #c678dd; }
        #btnOn { background-color: #98c379; }
        #btnOff { background-color: #e06c75; }
    </style>
</head>
<body>
    <h1>💧 Irrigation Control Panel</h1>
    <div class="container">
        <div class="card">
            <h2>Live Data</h2>
            <div id="data">
                <p>Timestamp: <span id="timestamp">--</span></p>
                <p>Flow Rate: <span id="flow_rate">--</span> L/min</p>
                <p>Water Used (Cycle): <span id="water_used">--</span> L</p>
                <p>Pump State: <span id="pump_state">--</span></p>
            </div>
        </div>
        <div class="card">
            <h2>Controls</h2>
            <button id="btnOn">Turn Pump ON</button>
            <button id="btnOff">Turn Pump OFF</button>
        </div>
    </div>
    <script src="https://cdn.socket.io/4.7.5/socket.io.min.js"></script>
    <script>
        const socket = io();

        // Listen for data updates from the server
        socket.on('mqtt_message', function(data) {
            document.getElementById('timestamp').innerText = data.timestamp;
            document.getElementById('flow_rate').innerText = data.flow_rate_Lmin;
            document.getElementById('water_used').innerText = data.water_used_cycle;
            document.getElementById('pump_state').innerText = data.pump_state;
        });

        // Send commands when buttons are clicked
        document.getElementById('btnOn').addEventListener('click', () => {
            console.log('Sending ON command');
            socket.emit('pump_command', { action: 'ON' });
        });

        document.getElementById('btnOff').addEventListener('click', () => {
            console.log('Sending OFF command');
            socket.emit('pump_command', { action: 'OFF' });
        });
    </script>
</body>
</html>
"""

# Create a single, shared MQTT client instance
mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)

@app.route('/')
def index():
    return render_template_string(html_template)

# --- WebSocket Event Handler for receiving commands from the web page ---
@socketio.on('pump_command')
def handle_pump_command(json_data):
    action = json_data['action']
    print(f"Web client sent command: {action}")
    # Publish the command to the MQTT broker
    mqtt_client.publish(MQTT_COMMAND_TOPIC, action)

# --- MQTT Logic ---
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("MQTT Connected!")
        client.subscribe(MQTT_DATA_TOPIC)
    else:
        print(f"Failed to connect to MQTT, return code {rc}")

def on_message(client, userdata, msg):
    try:
        data = json.loads(msg.payload.decode('utf-8'))
        print(f"MQTT -> Backend: {data}")
        # Forward data to all connected web clients
        socketio.emit('mqtt_message', data)
    except Exception as e:
        print(f"Error processing message: {e}")

def mqtt_thread():
    mqtt_client.on_connect = on_connect
    mqtt_client.on_message = on_message
    mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
    mqtt_client.loop_forever()

if __name__ == '__main__':
    threading.Thread(target=mqtt_thread, daemon=True).start()
    socketio.run(app, host='0.0.0.0', port=5000)