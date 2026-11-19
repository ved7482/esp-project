/* =========================================================
   MULTI-PROTOCOL IOT DASHBOARD
   ESP8266 + DHT11 + INA219
   HTTP / CoAP / MQTT
   ========================================================= */

const $ = (id) => document.getElementById(id);

const protocolButtons = [
    ...document.querySelectorAll('.protocol-option')
];

const runButton = $('run-button');

const protocolNames = ['http', 'coap', 'mqtt'];

let selectedProtocol = 'http';
let isRunning = false;
let requestInProgress = false;
let configRequestInProgress = false;
let lastUserAction = 0;


/* =========================================================
   GENERAL HELPER FUNCTIONS
   ========================================================= */

function normalizeProtocol(value) {
    const protocol = String(value || 'http')
        .trim()
        .toLowerCase();

    if (
        protocol === 'http' ||
        protocol === 'coap' ||
        protocol === 'mqtt'
    ) {
        return protocol;
    }

    return 'http';
}


function firstDefined(...values) {
    return values.find(
        value => value !== undefined && value !== null
    );
}


function validNumber(value) {
    return (
        value !== null &&
        value !== undefined &&
        value !== '' &&
        Number.isFinite(Number(value))
    );
}


function text(id, value, fallback = '—') {
    const node = $(id);

    if (!node) {
        return;
    }

    node.textContent =
        value === null ||
        value === undefined ||
        value === ''
            ? fallback
            : String(value);
}


function fixed(value, places = 1) {
    return validNumber(value)
        ? Number(value).toFixed(places)
        : '—';
}


function bytes(value) {
    return validNumber(value)
        ? `${Math.round(Number(value))} B`
        : '—';
}


function power(value) {
    return validNumber(value)
        ? `${Number(value).toFixed(1)} mW`
        : '—';
}


function setClass(id, className, enabled) {
    const node = $(id);

    if (node) {
        node.classList.toggle(className, Boolean(enabled));
    }
}


function setButtonState(button, active) {
    if (!button) return;

    button.classList.toggle('selected', active);

    button.setAttribute(
        'aria-pressed',
        String(active)
    );
}


/* =========================================================
   BACKEND API FUNCTIONS
   ========================================================= */

async function getJson(url) {
    const response = await fetch(url, {
        method: 'GET',
        cache: 'no-store',
        headers: {
            'Accept': 'application/json'
        }
    });

    if (!response.ok) {
        throw new Error(
            `GET ${url} failed: HTTP ${response.status}`
        );
    }

    return await response.json();
}


async function postJson(url, body) {
    const response = await fetch(url, {
        method: 'POST',

        headers: {
            'Content-Type': 'application/json',
            'Accept': 'application/json'
        },

        cache: 'no-store',

        body: JSON.stringify(body)
    });

    const result = await response.json().catch(() => ({}));

    if (!response.ok) {
        throw new Error(
            result.error ||
            result.message ||
            `POST ${url} failed: HTTP ${response.status}`
        );
    }

    return result;
}


/* =========================================================
   PROTOCOL CONTROL
   ========================================================= */

function renderControls() {

    protocolButtons.forEach(button => {

        const buttonProtocol = normalizeProtocol(
            button.dataset.protocol
        );

        setButtonState(
            button,
            buttonProtocol === selectedProtocol
        );

    });

    text(
        'selected-tag',
        selectedProtocol.toUpperCase()
    );

    if (runButton) {

        runButton.textContent = isRunning
            ? 'Pause transmission'
            : 'Start transmission';

        runButton.classList.toggle(
            'pause',
            isRunning
        );

        runButton.disabled = configRequestInProgress;
    }
}


/* =========================================================
   SENSOR READING DISPLAY
   ========================================================= */

function clearSensorReading(
    protocol,
    message = 'Waiting for sensor packet'
) {

    text('temperature', '—');

    text('humidity', '—');

    text('battery-voltage', '—');

    text('battery-current', '—');

    text('battery-power', '—');

    text(
        'reading-time',
        `${protocol.toUpperCase()} · ${message}`
    );
}


function getSensorValue(item, possibleKeys) {

    for (const key of possibleKeys) {

        if (
            item[key] !== undefined &&
            item[key] !== null
        ) {
            return item[key];
        }
    }

    return null;
}


function renderSensorReading(item, protocol) {

    if (!item) {

        clearSensorReading(
            protocol,
            'Waiting for first reading'
        );

        return;
    }


    // DHT11 values

    const temperature = getSensorValue(item, [
        'temperature',
        'temp',
        'temperature_c'
    ]);


    const humidity = getSensorValue(item, [
        'humidity',
        'relative_humidity',
        'humidity_percent'
    ]);


    // INA219 values

    const voltage = getSensorValue(item, [
        'battery_voltage_v',
        'load_voltage_v',
        'bus_voltage_v',
        'voltage_v',
        'voltage'
    ]);


    const current = getSensorValue(item, [
        'battery_current_ma',
        'current_ma',
        'current'
    ]);


    const batteryPower = getSensorValue(item, [
        'battery_power_mw',
        'power_mw',
        'power'
    ]);


    // Update dashboard

    text(
        'temperature',
        fixed(temperature, 1)
    );

    text(
        'humidity',
        fixed(humidity, 1)
    );

    text(
        'battery-voltage',
        fixed(voltage, 2)
    );

    text(
        'battery-current',
        fixed(current, 1)
    );

    text(
        'battery-power',
        fixed(batteryPower, 1)
    );


    // Reading timestamp

    const timestamp = firstDefined(
        item.received_at,
        item.timestamp,
        item.time
    );

    let timeLabel = 'time unavailable';

    if (
        timestamp !== undefined &&
        timestamp !== null
    ) {

        const date = typeof timestamp === 'number'
            ? new Date(
                timestamp < 1000000000000
                    ? timestamp * 1000
                    : timestamp
            )
            : new Date(timestamp);

        if (!Number.isNaN(date.getTime())) {

            timeLabel = date.toLocaleTimeString();

        } else {

            timeLabel = String(timestamp);
        }
    }


    const sequence = firstDefined(
        item.sequence,
        item.seq,
        '—'
    );

    text(
        'reading-time',
        `${protocol.toUpperCase()} · ${timeLabel} · seq ${sequence}`
    );
}


/* =========================================================
   TRANSMISSION PERFORMANCE METRICS
   ========================================================= */

function renderTransmissionMetrics(metric, protocol) {

    if (!metric) {

        text('payload', '—');

        text('overhead', '—');

        text('total', '—');

        text('ram-drop', '—');

        text(
            'metric-protocol',
            `${protocol.toUpperCase()} · waiting for metrics`
        );

        return;
    }


    text(
        'payload',
        bytes(
            firstDefined(
                metric.payload_bytes,
                metric.application_payload_bytes
            )
        )
    );


    text(
        'overhead',
        bytes(
            firstDefined(
                metric.protocol_overhead_bytes,
                metric.overhead_bytes
            )
        )
    );


    text(
        'total',
        bytes(
            firstDefined(
                metric.estimated_wire_bytes,
                metric.estimated_total_bytes,
                metric.total_bytes
            )
        )
    );


    text(
        'ram-drop',
        bytes(
            firstDefined(
                metric.ram_peak_observed_bytes,
                metric.ram_drop_bytes,
                metric.ram_drop
            )
        )
    );


    const status = metric.success === false
        ? 'failed'
        : 'received';

    text(
        'metric-protocol',
        `${protocol.toUpperCase()} · ${status}`
    );
}


/* =========================================================
   PROTOCOL COMPARISON TABLE
   ========================================================= */

function renderComparison(summary) {

    const body = $('comparison-body');

    if (!body) {

        console.warn(
            'Missing HTML element: comparison-body'
        );

        return;
    }


    body.replaceChildren();


    protocolNames.forEach(protocol => {

        const item =
            summary?.[protocol] ||
            summary?.[protocol.toUpperCase()] ||
            {};


        const sampleCount = Number(
            firstDefined(
                item.sample_count,
                item.samples,
                0
            )
        );


        const hasSamples = sampleCount > 0;

        const powerSamples = Number(
            firstDefined(
                item.power_sample_count,
                sampleCount
            )
        );


        const values = [

            protocol.toUpperCase(),

            String(sampleCount),

            hasSamples
                ? bytes(
                    firstDefined(
                        item.avg_payload_bytes,
                        item.payload
                    )
                )
                : '—',

            hasSamples
                ? bytes(
                    firstDefined(
                        item.avg_protocol_overhead_bytes,
                        item.overhead
                    )
                )
                : '—',

            hasSamples
                ? bytes(
                    firstDefined(
                        item.avg_estimated_wire_bytes,
                        item.total
                    )
                )
                : '—',

            hasSamples
                ? bytes(
                    firstDefined(
                        item.avg_ram_peak_observed_bytes,
                        item.ram
                    )
                )
                : '—',

            powerSamples > 0
                ? power(
                    firstDefined(
                        item.avg_battery_power_mw,
                        item.avg_power_mw
                    )
                )
                : '—'

        ];


        const row = document.createElement('tr');


        values.forEach((value, index) => {

            const cell = document.createElement('td');

            cell.textContent = value;

            if (index === 0) {

                cell.className = `protocol-${protocol}`;
            }

            row.appendChild(cell);

        });


        body.appendChild(row);

    });
}


/* =========================================================
   DEVICE AND NETWORK STATUS
   ========================================================= */

function renderStatus(data) {

    const online = Boolean(data.device_online);


    setClass(
        'device-dot',
        'online',
        online
    );

    setClass(
        'device-dot',
        'offline',
        !online
    );


    text(
        'device-status',
        online
            ? 'NodeMCU connected'
            : 'NodeMCU offline'
    );


    const mqttStatus = String(
        data.mqtt_status || 'unknown'
    );


    // Do not use includes('connected'): 'disconnected' contains that word.
    const mqttConnected =
        mqttStatus.trim().toLowerCase() === 'connected';


    setClass(
        'mqtt-dot',
        'online',
        mqttConnected
    );

    setClass(
        'mqtt-dot',
        'offline',
        !mqttConnected
    );


    const mqttMessages = Number(data.mqtt_message_count || 0);
    const mqttTopic = data.mqtt_topic || 'iot/esp32/sensor';
    text(
        'mqtt-status',
        `MQTT broker: ${mqttStatus} · ${mqttMessages} received · ${mqttTopic}`
    );


    text(
        'coap-status',
        `CoAP: ${data.coap_status || 'unknown'}`
    );
}


/* =========================================================
   GET READING FOR SELECTED PROTOCOL
   ========================================================= */

function getProtocolReading(data, protocol) {

    const latestByProtocol =
        data.latest_by_protocol || {};


    // Preferred backend structure

    let reading =
        latestByProtocol[protocol] ||
        latestByProtocol[protocol.toUpperCase()] ||
        null;


    // Compatibility fallback for older backend responses.
    if (!reading && data.latest) {
        const readingProtocol = normalizeProtocol(data.latest.protocol);
        if (readingProtocol === protocol) {
            reading = data.latest;
        }
    }

    // Also recover the latest sample from telemetry history if the backend
    // response does not contain latest_by_protocol yet.
    if (!reading) {
        const history = data.telemetry_history || data.history || [];
        reading = history.find(
            item => normalizeProtocol(item.protocol) === protocol
        ) || null;
    }

    return reading;
}


/* =========================================================
   GET PERFORMANCE METRICS FOR SELECTED PROTOCOL
   ========================================================= */

function getProtocolMetric(data, protocol) {

    const summary = data.metrics_summary || {};

    const item =
        summary[protocol] ||
        summary[protocol.toUpperCase()] ||
        {};


    if (item.last) {

        return item.last;
    }


    // Fallback for backends returning metrics_history

    const history = data.metrics_history || [];

    const match = history.find(row =>

        normalizeProtocol(row.protocol) === protocol

    );


    return match || null;
}


/* =========================================================
   RENDER ALL DASHBOARD DATA
   ========================================================= */

function renderState(data) {

    if (!data || typeof data !== 'object') {

        throw new Error(
            'The /api/state response is not a valid JSON object.'
        );
    }


    // Synchronize protocol selection with backend

    if (Date.now() - lastUserAction > 1800) {

        if (data.selected_protocol) {

            selectedProtocol = normalizeProtocol(
                data.selected_protocol
            );
        }


        if (data.running !== undefined) {

            isRunning = Boolean(data.running);
        }
    }


    renderControls();

    renderStatus(data);


    // Get protocol-specific sensor readings

    const selectedReading = getProtocolReading(
        data,
        selectedProtocol
    );


    renderSensorReading(
        selectedReading,
        selectedProtocol
    );


    // Get protocol-specific performance measurements

    const selectedMetric = getProtocolMetric(
        data,
        selectedProtocol
    );


    renderTransmissionMetrics(
        selectedMetric,
        selectedProtocol
    );


    // Update comparison table

    renderComparison(
        data.metrics_summary || {}
    );


    // Update dashboard message

    if (!data.device_online) {

        text(
            'control-hint',
            'NodeMCU offline. Check Wi-Fi, laptop IP and device configuration endpoint.'
        );

    } else if (!isRunning) {

        text(
            'control-hint',
            'NodeMCU connected. Transmission is paused.'
        );

    } else if (!selectedReading) {

        const mqttStatus = String(
            data.mqtt_status || ''
        ).toLowerCase();


        if (selectedProtocol === 'mqtt' && mqttStatus !== 'connected') {
            const detail = data.mqtt_last_error
                ? ` Last backend error: ${data.mqtt_last_error}.`
                : '';
            text(
                'control-hint',
                `MQTT subscriber status: ${data.mqtt_status || 'unknown'}. Check the broker address, topic and port 1883.${detail}`
            );
        } else if (selectedProtocol === 'mqtt') {
            const count = Number(data.mqtt_message_count || 0);
            text(
                'control-hint',
                count === 0
                    ? `Flask is subscribed to ${data.mqtt_host || 'unknown host'}:${data.mqtt_port || 1883} on topic ${data.mqtt_topic || 'unknown topic'}, but has received 0 sensor messages. The ESP8266 must publish to this same broker and topic.`
                    : 'MQTT is connected. Waiting for the next MQTT sensor reading.'
            );
        } else {
            text(
                'control-hint',
                `Waiting for a ${selectedProtocol.toUpperCase()} sensor packet. Check the backend terminal for incoming messages.`
            );
        }

    } else {

        const deviceId = firstDefined(
            selectedReading.device_id,
            selectedReading.device,
            'NodeMCU'
        );


        text(
            'control-hint',
            `${selectedProtocol.toUpperCase()} data received from ${deviceId}.`
        );
    }
}


/* =========================================================
   PROTOCOL BUTTON EVENT
   ========================================================= */

protocolButtons.forEach(button => {

    button.addEventListener('click', async () => {

        if (configRequestInProgress) {
            return;
        }


        const nextProtocol = normalizeProtocol(
            button.dataset.protocol
        );


        if (nextProtocol === selectedProtocol) {
            return;
        }


        const previousProtocol = selectedProtocol;

        selectedProtocol = nextProtocol;

        lastUserAction = Date.now();


        renderControls();


        clearSensorReading(
            nextProtocol,
            'Loading latest reading'
        );


        renderTransmissionMetrics(
            null,
            nextProtocol
        );


        text(
            'control-hint',
            `Switching NodeMCU to ${nextProtocol.toUpperCase()}…`
        );


        configRequestInProgress = true;

        renderControls();


        try {

            await postJson('/api/config', {

                protocol: selectedProtocol,

                running: isRunning

            });


            await refresh();


        } catch (error) {

            selectedProtocol = previousProtocol;

            lastUserAction = 0;


            text(
                'control-hint',
                `Could not change protocol: ${error.message}`
            );


            renderControls();


        } finally {

            configRequestInProgress = false;

            renderControls();
        }

    });

});


/* =========================================================
   START / PAUSE TRANSMISSION
   ========================================================= */

if (runButton) {

    runButton.addEventListener('click', async () => {

        if (configRequestInProgress) {
            return;
        }


        const previousRunning = isRunning;

        isRunning = !isRunning;

        lastUserAction = Date.now();


        renderControls();


        configRequestInProgress = true;

        renderControls();


        try {

            await postJson('/api/config', {

                protocol: selectedProtocol,

                running: isRunning

            });


            text(
                'control-hint',

                isRunning
                    ? `Transmission started using ${selectedProtocol.toUpperCase()}. Waiting for sensor data.`
                    : 'Transmission paused.'
            );


            await refresh();


        } catch (error) {

            isRunning = previousRunning;

            lastUserAction = 0;


            text(
                'control-hint',
                `Could not update transmission: ${error.message}`
            );


        } finally {

            configRequestInProgress = false;

            renderControls();
        }

    });

} else {

    console.warn(
        'Dashboard HTML is missing element: run-button'
    );
}


/* =========================================================
   REFRESH DATA FROM PYTHON BACKEND
   ========================================================= */

async function refresh() {

    if (requestInProgress) {
        return;
    }


    requestInProgress = true;


    try {

        const data = await getJson('/api/state');

        renderState(data);


    } catch (error) {

        console.error(
            '[Dashboard] Refresh failed:',
            error
        );


        text(
            'control-hint',
            `Dashboard update failed: ${error.message}`
        );


    } finally {

        requestInProgress = false;
    }
}


/* =========================================================
   INITIALIZE DASHBOARD
   ========================================================= */

renderControls();

refresh();


// Refresh dashboard once every second

window.setInterval(refresh, 1000);