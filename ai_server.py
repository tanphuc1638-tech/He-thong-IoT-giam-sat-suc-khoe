import json
import paho.mqtt.client as mqtt


BROKER = "localhost"
PORT = 1883
TOPIC_SUB = "---------"    # Lắng nghe dữ liệu thô từ ESP32
TOPIC_PUB = "--------------" # Đẩy kết quả lên Web Dashboard

def rule_based_ai(bpm, spo2):
    """Hệ chuyên gia phân loại nhịp tim và SpO2 cơ bản"""
    if spo2 < 90:
        return {"status": "Nguy hiểm", "warning": "SpO2 quá thấp, thiếu oxy máu cấp!"}
    elif spo2 < 95:
        return {"status": "Cảnh báo", "warning": "SpO2 thấp, cần theo dõi thêm."}
    
    if bpm < 50:
        return {"status": "Cảnh báo", "warning": "Nhịp tim chậm (Bradycardia)."}
    elif bpm > 100:
        return {"status": "Cảnh báo", "warning": "Nhịp tim nhanh (Tachycardia)."}
    
    return {"status": "Bình thường", "warning": "Chỉ số sinh tồn ổn định."}

def on_connect(client, userdata, flags, rc):
    print(f"Đã kết nối MQTT Broker với mã: {rc}")
    client.subscribe(TOPIC_SUB)

def on_message(client, userdata, msg):
    payload = msg.payload.decode('utf-8')
    try:
        data = json.loads(payload)
        bpm = data.get("bpm", 0)
        spo2 = data.get("spo2", 0)
        
        # Đưa dữ liệu qua AI Logic
        ai_result = rule_based_ai(bpm, spo2)
        
        # Đóng gói dữ liệu hoàn chỉnh
        final_data = {
            "sensor_data": data,
            "ai_analysis": ai_result
        }
        
        # Publish kết quả cho Web Frontend
        client.publish(TOPIC_PUB, json.dumps(final_data))
        print(f"Đã xử lý và đẩy dữ liệu lên Web: {final_data}")
        
    except json.JSONDecodeError:
        print("Lỗi định dạng JSON")

client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message

client.connect(BROKER, PORT, 60)
print("Python AI Service đang chạy...")
client.loop_forever()
