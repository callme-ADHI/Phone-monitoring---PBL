# Classroom Phone & Occupancy Monitoring System (PBL)

An end-to-end IoT and computer vision monitoring system designed for classrooms and lecture halls.

It combines:
1. **ESP32 WiFi NAT Router Node** (`wifi/wifi.ino`): Creates an access point for student phones, routes upstream internet using lwIP NAPT, sniffs DNS domain requests, captures hardware MAC addresses, and reports traffic metrics.
2. **ESP32-CAM / Webcam AI Person Counter** (`person_counter.py` & `cam/cam.ino`): Uses YOLOv8n to perform real-time optical occupancy detection.
3. **Next.js & Supabase Web Dashboard** (`web/` & `database/`): Maps hardware MAC addresses to registered students, tracks in-class phone violations based on outgoing network traffic, provides attendance verification, and features a remote internet kill-switch.

---

## Architecture Overview

```
                      +-------------------+
                      |   Student Phone   |
                      +---------+---------+
                                | (Connects to SSID: PBL-MC)
                                v
               +---------------------------------+
               |   ESP32 Router Node (wifi.ino)  |
               | - SoftAP (192.168.4.1)          |
               | - NAPT Internet Routing         |
               | - DNS Sniffer (Port 53)         |
               | - Real MAC & Telemetry Stream   |
               +----------------+----------------+
                                |
               +----------------v----------------+
               |   Next.js Dashboard & Backend   |
               |   (http://localhost:3000)       |
               +--------+---------------+--------+
                        |               |
                        v               v
           +--------------------+  +-------------------------+
           | Supabase Database  |  |  AI Person Counter      |
           | - Student Roster   |  |  (person_counter.py)    |
           | - MAC Registry     |  |  - Webcam or ESP32-CAM  |
           | - Class Timetable  |  |  - YOLOv8n Object Model |
           +--------------------+  +-------------------------+
```

---

## Hardware Requirements

| Component | Quantity | Purpose |
| :--- | :--- | :--- |
| **ESP32 Dev Module (WROOM-32)** | 1 | Acts as the WiFi Router, NAPT gateway, and DNS sniffer |
| **ESP32-CAM (AI-Thinker OV2640)** *(Optional)* | 1 | Wireless camera node (or use system webcam) |
| **Micro-USB Cables** | 1–2 | Programming and power |
| **Host PC / Laptop** | 1 | Runs Web Dashboard, Supabase integration, and YOLOv8n |

---

## Step-by-Step Installation on a New System

### 1. Clone the Repository

```bash
git clone https://github.com/callme-ADHI/Phone-monitoring---PBL.git
cd Phone-monitoring---PBL
```

---

### 2. Set Up the Database (Supabase)

1. Create a free project at [supabase.com](https://supabase.com).
2. Go to your project's **SQL Editor**.
3. Open [`database/schema.sql`](database/schema.sql), paste its contents into the editor, and click **Run**.
4. Open [`database/seed.sql`](database/seed.sql), paste its contents, and click **Run** to load demo student profiles and class schedules.
5. In your Supabase dashboard, go to **Project Settings -> API** and copy:
   - **Project URL**
   - **Anon / Public API Key**

---

### 3. Set Up and Run the Web Dashboard

#### Prerequisites
- Node.js (v18.17+ or v20+)
- npm or pnpm

#### Configuration
Navigate to the `web` directory and create `.env.local`:
```bash
cd web
cp .env.example .env.local 2>/dev/null || true
```

Edit `web/.env.local` with your Supabase credentials:
```env
NEXT_PUBLIC_SUPABASE_URL=https://your-project-id.supabase.co
NEXT_PUBLIC_SUPABASE_ANON_KEY=your-supabase-anon-key
```

#### Install Dependencies and Run
```bash
npm install
npm run dev
```
Open **`http://localhost:3000`** in your browser.

- **`/`**: Combined Live Dashboard (Camera count vs. Connected phones cross-validation)
- **`/phones`**: Real-time Phone Monitor, MAC to Student assignment, DNS activity, and Internet toggle
- **`/camera`**: Optical Person Counter stream and occupancy chart

---

### 4. Flash the Router ESP32 Firmware (`wifi.ino`)

1. Install [Arduino IDE](https://www.arduino.cc/en/software).
2. Go to **Preferences -> Additional Board Manager URLs** and add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Open **Boards Manager** and install **esp32 by Espressif Systems** (version 3.x recommended).
4. Open [`wifi/wifi.ino`](wifi/wifi.ino).
5. At the top of the file, configure:
   ```cpp
   // Upstream WiFi settings (Hotspot or Home/Campus Router)
   static const char* STA_SSID = "Your_WiFi_Name";
   static const char* STA_PASS = "Your_WiFi_Password";

   // IP address of your laptop running the web dashboard
   static const char* DASHBOARD_HOST = "192.168.1.50"; // Replace with your laptop's local IP
   static const int   DASHBOARD_PORT = 3000;
   ```
6. Connect your standard ESP32 board via USB.
7. Select Board: **"ESP32 Dev Module"** and the correct USB Port.
8. Click **Upload**.
9. Open the Serial Monitor at **115200 baud** to confirm initialization.

---

### 5. Run the Optical Person Counter (Python + YOLOv8)

#### Setup Python Environment
```bash
# Return to root directory
cd ..

# Create and activate virtual environment
python3 -m venv .venv
source .venv/bin/activate  # On Windows: .venv\Scripts\activate

# Install dependencies
pip install ultralytics opencv-python numpy requests
```

#### Run with System Webcam
```bash
python person_counter.py --source webcam
```

#### Run with ESP32-CAM *(Optional)*
If using an ESP32-CAM board:
1. Flash [`cam/cam.ino`](cam/cam.ino) to your ESP32-CAM with board **"AI Thinker ESP32-CAM"**.
2. Note the IP printed in the Serial Monitor (e.g. `10.38.17.14`).
3. Run:
```bash
python person_counter.py --source esp32cam --cam-url http://10.38.17.14
```

---

## Features Walkthrough

### 1. Automatic MAC-to-Student Resolution
When a phone connects to the ESP32 network (`PBL-MC`):
- The hardware MAC address is transmitted to the dashboard.
- If pre-registered, the student's **Name**, **Roll Number**, and **Department** are automatically shown.
- If unregistered, an **"Assign"** button lets the instructor link the phone to a student in one click.

### 2. Classroom Policy & Usage Violation Detection
- During scheduled class hours, devices actively sending outgoing packets (TX) or generating frequent DNS queries are flagged with a red **`In-Use Violation`** badge.
- Quiet/idle phones in bags or pockets are marked **`Compliant (Idle)`**.
- Absent students are automatically highlighted in the compliance summary.

### 3. Remote Internet Kill-Switch
- Teachers can click **"Cut Classroom Internet"** in the web dashboard.
- The ESP32 instantly pauses NAPT internet forwarding while keeping the SoftAP and attendance logging fully active.

---

## Project Structure

```
.
├── database/
│   ├── schema.sql           # Complete Supabase PostgreSQL schema
│   └── seed.sql             # Demo classroom roster and schedules
├── wifi/
│   └── wifi.ino             # Main ESP32 Router, NAPT, DNS sniffer firmware
├── cam/
│   └── cam.ino              # Optional ESP32-CAM video streaming firmware
├── web/                     # Next.js 14 frontend and API routes
│   ├── src/app/             # Pages: /, /phones, /camera, /api/esp32
│   ├── src/components/      # UI components (Compliance cards, tables, modals)
│   └── src/lib/supabase.ts  # Supabase client with offline demo fallback
├── person_counter.py        # YOLOv8n real-time optical person detection API
├── wifi.ino                 # Root mirror of wifi sketch for single-file flash
└── README.md
```

---

## Troubleshooting

- **ESP32 logs `connection refused`**:
  - Make sure `npm run dev` is running on your laptop.
  - Check that your laptop's firewall is not blocking port 3000.
  - Ensure the laptop and ESP32 are on the same Wi-Fi network, or connect the laptop's Wi-Fi directly to `PBL-MC`.
- **YOLOv8 Camera error**:
  - If `--camera 0` fails, try `--camera 1` or check that no other application (like Zoom or browser) is using your webcam.
- **Supabase credentials**:
  - If no Supabase credentials are provided in `.env.local`, the dashboard automatically runs in **Offline Demo Mode** with built-in mock data so you can test all features immediately.
