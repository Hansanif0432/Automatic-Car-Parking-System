const express = require('express');
const http = require('http');
const { Server } = require('socket.io');
const cors = require('cors');
const mqtt = require('mqtt');

const app = express();
app.use(cors());

const server = http.createServer(app);

const io = new Server(server, {
    cors: { origin: "*" },
    transports: ['websocket'],
    maxHttpBufferSize: 1e8
});

// -----------------------------
// MQTT CONNECTION & IDENTITY LEDGER
const mqttClient = mqtt.connect('mqtt://127.0.0.1:1883');

mqttClient.on('connect', () => {
    console.log('🔌 Backend Connected to MQTT Broker');
    mqttClient.subscribe('hardware/sensors'); 
});

mqttClient.on('error', (err) => {
    console.error('🚨 MQTT Connection Error:', err);
});

// HARDCODED LEDGER: Maps physical slots to master RFID card numbers
const slotOwnersLedger = {
    "A1": "04-B0-E1-92",  
    "A2": "F8-2B-40-11",  
    "A3": "1A-7C-99-04",
    "A4": "5B-21-E4-88",
    "B1": "99-AA-22-11",
    "B2": "7C-4A-10-90",
    "B3": "33-44-55-66",
    "B4": "11-99-88-77"
};

let activeDetectedCard = {
    uid: "AWAITING_CAR...",
    status: "IDLE"
};

// -----------------------------
// SYSTEM STATE
// -----------------------------
let liftState = {
    currentFloor: 0,
    status: "IDLE",
    lastOccupiedSlot: null
};

let targetFloor = 0;
let targetSlot = null;
let activeSequenceData = null; 

let parkingSlots = [
    { id: "A1", floor: 1, occupied: false },
    { id: "A2", floor: 1, occupied: false },
    { id: "A3", floor: 1, occupied: false },
    { id: "A4", floor: 1, occupied: false },
    { id: "B1", floor: 2, occupied: false },
    { id: "B2", floor: 2, occupied: false },
    { id: "B3", floor: 2, occupied: false },
    { id: "B4", floor: 2, occupied: false }
];

let systemMode = "AUTO"; 

setTimeout(() => { liftState.status = "IDLE_READY"; }, 2000);

// -----------------------------
// MQTT TELEMETRY INTERCEPTOR
// -----------------------------
mqttClient.on('message', (topic, message) => {
    if (topic === 'hardware/sensors') {
        try {
            const hwData = JSON.parse(message.toString());

            // --- 1. THE OPTICAL ILLUSION: Match ID based on reported slot ---
            if (hwData.current_slot && hwData.current_slot !== "TRANSIT") {
                const matchedUID = slotOwnersLedger[hwData.current_slot];
                if (matchedUID) {
                    activeDetectedCard.uid = matchedUID;
                    activeDetectedCard.status = "authorized";
                }
            } 
            // When lift returns to Ground Zero and stops, reset the card screen
            else if (hwData.actual_floor === 0 && hwData.motor_status === "idle") {
                activeDetectedCard.uid = "AWAITING_CAR...";
                activeDetectedCard.status = "IDLE";
            }

            // --- 2. UPDATE PHYSICAL POSITION ---
            liftState.currentFloor = hwData.actual_floor;
            liftState.raw_y = hwData.raw_y;

            if (hwData.motor_status === "moving") {
                liftState.status = "MOVING";
                
                // CULPRIT 1 FIX: Advance the sequence tracker to "MOVING" only AFTER the hardware actually starts moving!
                if (activeSequenceData && activeSequenceData.step === "DISPATCHED") {
                    activeSequenceData.step = "MOVING";
                }
            }
            else if (hwData.motor_status === "idle") {
                if (liftState.currentFloor === 0) {
                    liftState.status = "PARKING_IDLE";
                } else {
                    liftState.status = "READY";
                }

                // --- 3. SAFE SEQUENCE COMPLETION ---
                // CULPRIT 1 FIX: Only complete the sequence if we VERIFIED the motor actually traveled and returned!
                if (activeSequenceData && activeSequenceData.step === "MOVING" && liftState.currentFloor === 0) {
                    console.log(`🤖 Sequence Round-Trip completed for slot ${activeSequenceData.slotId}.`);
                    const slot = parkingSlots.find(s => s.id === activeSequenceData.slotId);
                    if (slot) {
                        slot.occupied = (activeSequenceData.action === "park");
                        console.log(`Slot ${slot.id} is now ${slot.occupied ? "FULL" : "EMPTY"}`);
                    }
                    // Cleanly clear the sequence
                    activeSequenceData = null; 
                }
            }
            else if (hwData.motor_status === "halted") {
                liftState.status = "HALTED";
                
                // CULPRIT 2 FIX: If a hardware HALT occurs, abort any running web sequences to prevent ghost completions!
                if (activeSequenceData) {
                    console.log("🚨 Hardware HALT detected! Aborting active web sequence memory.");
                    activeSequenceData = null;
                }
            }
            
        } catch (err) {
            console.error("MQTT Parse Error:", message.toString());
        }
    }
});

// -----------------------------
// REAL-TIME DASHCAST LOOP
// -----------------------------
setInterval(() => {
    io.emit('dashboard_update', {
        lift: {
            currentFloor: liftState.currentFloor,
            raw_y: liftState.raw_y || 0,
            status: liftState.status
        },
        slots: parkingSlots,
        recentRFID: activeDetectedCard 
    });
}, 100);

// -----------------------------
// SOCKET CONNECTIONS
// -----------------------------
io.on('connection', (socket) => {
    console.log(`💻 Dashboard UI Connected: ${socket.id}`);
    socket.emit('mode_update', systemMode);

    socket.on('toggle_mode', (newMode) => {
        systemMode = newMode;
        console.log(`🔄 System Mode switched to: ${systemMode}`);
        io.emit('mode_update', systemMode); 
    });

    socket.on('manual_command', (cmd) => {
        if (systemMode === "MANUAL") {
            const slot = parkingSlots.find(s => s.id === cmd.slotId);
            if (slot) {
                targetSlot = slot;
                targetFloor = slot.floor;
                
                // Intelligently choose Park or Retrieve based on current slot status
                targetSlot.action = slot.occupied ? "retrieve" : "park";
                
                liftState.status = "READY";
                
                // Initialize as DISPATCHED. It will not complete until it transitions to MOVING.
                activeSequenceData = {
                    slotId: slot.id,
                    action: targetSlot.action,
                    step: "DISPATCHED" 
                };

                const commandPayload = {
                    action: targetSlot.action,
                    target_floor: slot.floor,
                    slot_id: slot.id
                };
                
                mqttClient.publish('hardware/commands', JSON.stringify(commandPayload), { qos: 1, retain: false });
                console.log(`🕹️ Web Dispatch issued: ${targetSlot.action.toUpperCase()} at ${slot.id}`);
            }
        } else {
            console.log("❌ Blocked: Switch to MANUAL mode to dispatch from web.");
        }
    });

    // --- EMERGENCY HOME COMMAND ---
    socket.on('emergency_home', () => {
        console.log(`🚨 Web UI Triggered EMERGENCY SAFE HOME`);
        activeSequenceData = null; // Clear any autonomous sequence currently running
        mqttClient.publish('hardware/commands', JSON.stringify({ action: "home" }), { qos: 1, retain: false });
    });

    // YOLO AI RELAY (Purely visual feed without logic interrupts)
    socket.on('yolo_feed', (yoloData) => {
        socket.broadcast.emit('yolo_update', yoloData);
    });

    socket.on('disconnect', () => { console.log(`❌ UI Disconnected`); });
});

const PORT = 3001;
server.listen(PORT, () => { console.log(`🚀 CAPS Master Backend active on Port ${PORT}`); });
