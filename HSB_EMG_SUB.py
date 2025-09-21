
from datetime import datetime
from dash import Dash, html, dcc, callback, Output, Input
import dash_daq as daq
import plotly.express as px
import paho.mqtt.client as mqtt
from collections import deque 
import struct
import pandas as pd
import csv

#MQTT
TOPIC = "esp32/emg1" 
BROKER_ADDRESS = "192.168.2.115" 
PORT = 1883 

#CSV
csv_name = ""
recording = False
column_names = ["EMG1", "EMG2", "Time[1e-6 s]"]
output = deque([], maxlen=4000)

#Miscellaneous
msg_size = struct.calcsize('HHI')
# Initialize the app
app = Dash()

# App layout
app.layout = [
    html.Div(children='Bionik Labor Physiologie - EMG'),
    html.Hr(),
    daq.BooleanSwitch(id='toggle-sampling', on=False),
    html.Div(id='boolean-switch-output-1'),
    daq.BooleanSwitch(id='toggle-recording', on=False),
    html.Div(id='boolean-switch-output-2'),
    dcc.Graph(id='graph_emg1'),
    dcc.Graph(id='graph_emg2'),
    dcc.Interval(id='interval-component', interval=500, n_intervals=0)
]

# Add controls to build the interaction
@callback(
    Output('boolean-switch-output-1', 'children'),
    Input('toggle-sampling','on')
)
def update_sampling(on):
    if on == True:
        client.publish("esp32/output", "on")
        return f'Listening to ESP32'
    else:
        client.publish("esp32/output", "off")
        return f'Toggle to listen to ESP32'

@callback(
    Output('interval-component', 'disabled'),
    Input('toggle-sampling','on')
)
def toggle_interval(on):
    if on == True:
        disabled = False
        return disabled
    else:
        disabled = True
        return disabled

@callback(
    Output('boolean-switch-output-2', 'children'),
    Input('toggle-recording','on')
)
def update_recording(on):
    global recording 
    global csv_name
    if on == True:
        recording = True
        now = datetime.now()
        csv_name = now.strftime("%Y%m%d_%H%M%S") + "_EMG.csv"
        with open(csv_name, 'a') as f:
            write = csv.writer(f)
            write.writerow(column_names)
        return f'Writing data to csv file.'
    else:
        recording = False
        return f'Toggle to record data.'

@callback(
    Output('graph_emg1', 'figure'),
    Input('interval-component','n_intervals'),
    prevent_initial_call=True,
)
def update_graph_emg1(n):
    df = pd.DataFrame(output, columns=['emg1','emg2','time'])
    fig = px.line(df, x='time', y='emg1')

    return fig

@callback(
    Output('graph_emg2', 'figure'),
    Input('interval-component','n_intervals'),
    prevent_initial_call=True,
)
def update_graph_emg2(n):
    df = pd.DataFrame(output, columns=['emg1','emg2','time'])
    fig = px.line(df, x='time', y='emg2')

    return fig
    
def on_message(client, userdata, message): 
    msg_items = len(message.payload) / msg_size
    msg = list(struct.iter_unpack('HHI', message.payload))
    for i in range (int(msg_items)):
        output.append(list(msg[i]))
    if recording == True:
        with open(csv_name, 'a', newline='') as f:
            write = csv.writer(f)
            for i in range (int(msg_items)):
                write.writerow(list(msg[i]))

def on_connect(client, userdata, flags, rc): 
    print("Connected to MQTT Broker: " + BROKER_ADDRESS) 
    client.subscribe(TOPIC) 

if __name__ == "__main__":
    client = mqtt.Client()
    client.on_connect = on_connect 
    client.on_message = on_message
    client.connect(BROKER_ADDRESS, PORT)
    client.loop_start()
    app.run(debug=False)
