# Health Dashboard - Frontend (Next.js + R3F + MQTT + Firebase)

Dashboard realtime hien thi BPM/SpO2 tu ESP32, ket qua chan doan AI, quan
tim 3D dap theo nhip tim, bieu do xu huong va lich su do luu Firestore.

## 1. Cau truc thu muc

```
health-dashboard/
├── mosquitto/
│   └── mosquitto.conf          # Cau hinh broker (TCP 1883 + WebSocket 9001)
├── src/
│   ├── app/
│   │   ├── layout.tsx          # Root layout, nap font Inter + Orbitron
│   │   ├── page.tsx            # Trang chinh - ghep toan bo component
│   │   └── globals.css         # Tailwind directives + utility glow
│   ├── components/
│   │   ├── Heart3D.tsx         # Quan tim 3D (R3F) dap theo BPM
│   │   ├── MetricsPanel.tsx    # So BPM lon + goi SpO2Gauge
│   │   ├── SpO2Gauge.tsx       # Gauge vong tron SpO2 (SVG)
│   │   ├── AIDiagnosisCard.tsx # The chan doan AI, vien glow theo severity
│   │   ├── TrendChart.tsx      # Line chart Recharts, xu huong BPM
│   │   └── HistoryTable.tsx    # Bang 5 ban ghi gan nhat tu Firestore
│   ├── lib/
│   │   ├── mqttService.ts      # Hook useHealthMqtt() - ket noi WS toi Mosquitto
│   │   ├── firebase.ts         # Khoi tao Firebase + doc/ghi Firestore
│   │   └── theme.ts            # Anh xa status AI -> mau sac (severity)
│   └── types/
│       └── health.ts           # Kieu du lieu dung chung
├── package.json
├── tailwind.config.ts
├── tsconfig.json
├── postcss.config.js
├── next.config.js
└── .env.local.example
```

## 2. Cai dat

```bash
npm install
cp .env.local.example .env.local
# -> dien NEXT_PUBLIC_FIREBASE_* tu Firebase Console vao .env.local
```

> **Luu y ve Tailwind**: project nay dung Tailwind **v3** (file
> `tailwind.config.ts` + `@tailwind base/components/utilities` trong
> `globals.css`). Neu `npm install` keo ve Tailwind v4 do khong ghi ro
> version, cau hinh se khong chay dung - package.json da pin san
> `"tailwindcss": "^3.4.0"` nen chi can `npm install` binh thuong la du.

## 3. Cau hinh Mosquitto mo them WebSocket (port 9001)

Frontend chay trong trinh duyet KHONG the mo TCP socket thuan toi port 1883
nhu ESP32/Backend Python - bat buoc phai qua WebSocket. File
`mosquitto/mosquitto.conf` da chuan bi san 2 listener:

```
listener 1883
protocol mqtt

listener 9001
protocol websockets

allow_anonymous true
```

Chay Mosquitto voi file cau hinh nay (thay duong dan cho phu hop he dieu hanh):

```bash
mosquitto -c ./mosquitto/mosquitto.conf -v
```

`-v` de in log chi tiet, huu ich khi debug ket noi luc dau. Kiem tra thay 2
dong `Opening ipv4 listen socket on port 1883` va `...port 9001` la broker
da san sang cho ca ESP32/Backend lan Frontend.

## 4. Chay Frontend

```bash
npm run dev
```

Mo `http://localhost:3000`. Goc tren ben phai se hien "Da ket noi MQTT" khi
WebSocket toi Mosquitto thanh cong; sau do moi khi Backend Python publish
len topic `tdtu/health/ai_result`, toan bo dashboard (quan tim, so lieu,
chan doan, bieu do, lich su) se tu cap nhat realtime.

## 5. Firestore

Tao Firestore Database (Native mode) trong Firebase Console, khong can tao
truoc collection `health_records` - `saveHealthRecord()` trong
`lib/firebase.ts` se tu tao khi co ban ghi dau tien. Nho thiet lap Security
Rules phu hop truoc khi dua ra ngoai moi truong dev (mac dinh Firestore moi
tao o "test mode" se tu khoa ghi/doc sau 30 ngay).
