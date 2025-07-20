# 🚑 RemoHealth - ESP32 Wearable Firmware

This repository contains the firmware code for the **RemoHealth** wearable device built using the **ESP32** microcontroller. The firmware reads health vitals like **Heart Rate (BPM)**, **SpO₂**, and **Body Temperature**, then transmits the data to **Firebase Firestore** in real-time.

---

## 📌 Project Overview

**RemoHealth** is a health monitoring system designed to provide real-time vital tracking and remote accessibility. This embedded module forms the wearable component, using **MAX30105** sensor to gather biometric data, process it, and upload it via WiFi to the cloud.

This firmware was developed as part of a **Mini Project** by:
- 👨‍💻 **Arjun P.K** – Embedded interfacing, Firebase integration, and sensor logic.
- 👨‍💻 **Collaborator [Name]** – Assistance with circuit design, sensor tuning, and data validation.

---

## ⚙️ Technologies Used

- **ESP32 DevKit v1**
- **MAX30105** optical sensor
- **Google Firebase Firestore**
- **C++ (Arduino)**
- Libraries:
  - `Firebase_ESP_Client`
  - `MAX30105.h`, `heartRate.h`, `spo2_algorithm.h`
  - `Wire.h`, `WiFi.h`

---

## 🧠 Features

- 🔌 Connects to WiFi automatically
- ❤️ Calculates BPM using pulse detection
- 🩸 Computes SpO₂ levels using red & IR LEDs
- 🌡️ Measures on-skin temperature
- ☁️ Uploads data to Firebase Firestore with timestamps
- ⛔ Rejects false readings when finger is removed
- 📉 Supports noise filtering and peak detection

---

## 📐 Firebase Data Structure

```json
users/
  └── <user_uid>/
      └── health_readings/
           └── reading_<timestamp>/
                ├── bpm: 76
                ├── spo2: 97.2
                ├── temperature_C: 36.7
                ├── temperature_F: 98.0
                └── timestamp: 1723847589
