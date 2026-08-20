import cv2
import base64
import socketio
import time
from ultralytics import YOLO

# 1. Connect to our Node.js Dashboard Backend
sio = socketio.Client()

@sio.event
def connect():
    print("✅ Connected to Node.js Server!")

@sio.event
def disconnect():
    print("❌ Disconnected from Node.js Server")

try:
    sio.connect('http://localhost:3001', transports=['websocket'])
except Exception as e:
    print(f"Make sure Node.js is running! Error: {e}")
    exit()

# 2. Load the model ONLY ONCE
print("Loading YOLO Model...")
model = YOLO("yolov8n.pt")

# 3. Open the camera
# ==========================================
# CAMERA SOURCE SELECTION
# Uncomment the ONE you want to use:
# ==========================================

# --- OPTION 1: Laptop Built-in Webcam (CURRENTLY ACTIVE) ---
print("Connecting to local laptop webcam...")
cap = cv2.VideoCapture(0)

# --- OPTION 2: Mobile IP Webcam ---
# IP_WEBCAM_URL = "http://192.168.1.100:8080/video" # <-- CHANGE THIS TO YOUR PHONE'S IP
# print(f"Connecting to IP Webcam at: {IP_WEBCAM_URL}")
# cap = cv2.VideoCapture(IP_WEBCAM_URL)

print("🎥 Streaming live YOLO feed to dashboard...")

while True:
    ret, frame = cap.read()
    if not ret:
        print("Failed to grab frame")
        break
        
    # Resize for better performance over network
    frame = cv2.resize(frame, (640, 480))
    
    # Run detection
    results = model(frame, conf=0.5)
    detections = []
    
    for r in results:
        for box in r.boxes:
            x1, y1, x2, y2 = box.xyxy[0].tolist()
            cls = int(box.cls[0])
            conf = float(box.conf[0])
            class_name = model.names[cls]
            
            # ONLY detect 'person' or 'car'
            if class_name in ['person', 'car']:
                detections.append({
                    "className": class_name,
                    "confidence": round(conf, 2),
                    "box": [int(x1), int(y1), int(x2), int(y2)]
                })
            
    # Encode annotated frame to base64
    annotated_frame = results[0].plot()
    _, buffer = cv2.imencode('.jpg', annotated_frame)
    image_base64 = base64.b64encode(buffer).decode('utf-8')
    
    # 4. SEND TO NODE.JS instantly
    sio.emit('yolo_feed', {
        "image": image_base64,
        "detections": detections
    })
    
    # Sleep to limit FPS, save CPU, and STOP Network/Websocket lag.
    # 0.3 seconds = ~3 frames per second, which keeps the stream stable.
    time.sleep(0.2) 

cap.release()
sio.disconnect()
