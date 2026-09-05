# Tiny-Drone Open-Source Micro Drone

[简体中文](README.md) | **English**

> An open-source ESP32-S3 micro drone supporting Android app, mobile browser, and remote-controller operation, plus Wi-Fi video transmission and Remote ID broadcasting.

<p align="left">
  <img src="assets/TinyDrone/tiny-drone-overview.jpg" alt="Tiny-Drone open-source micro drone" width="600">
</p>

## Overview

Tiny-Drone is an ESP32-S3-based micro drone. Its main codebase is derived from the open-source [ESP-Drone](https://github.com/espressif/esp-drone) project and is distributed under the [GPL-3.0 License](LICENSE). The [hardware design is also open source](https://oshwhub.com/xiaochen_study/project_wsijdhkn).

Key features:

- Android app control
- Mobile browser control
- Remote-controller support
- Wi-Fi video transmission
- Remote ID broadcasting
- Expansion support for a barometer, laser ranging sensor, and position-hold module

## Video Demonstrations

- [Indoor flight](https://www.bilibili.com/video/BV1T7th6gEEx/)
- [Assembly tutorial](https://www.bilibili.com/video/BV1CBtU6UEvK/)

## Important Notes

> [!IMPORTANT]
> Before flying outdoors, program a unique product serial number, complete the required registration, and then enable Remote ID. Always comply with local drone regulations.

- Minor horizontal lines may appear in the Wi-Fi video feed, especially when battery voltage is low.
- If the yaw angle drifts, adjust the motor height and propeller installation depth.
- Before using altitude hold, make sure the propellers and motors are correctly installed and aligned. Incorrect alignment may cause unstable flight.
- Support the bottom of the motor by hand when installing a propeller. Excessive force may push through the rear cover and detach the motor.

## Hardware Design

### Power Supply

The drone uses a lithium battery. A boost converter raises the battery voltage, and an LDO supplies regulated power to the sensors and main controller.

<p align="left">
  <img src="assets/TinyDrone/power-supply.png" alt="Tiny-Drone power supply circuit" width="600">
</p>

### Status and Power LEDs

<p align="left">
  <img src="assets/TinyDrone/status-and-power-led.png" alt="Status and power LED circuit" width="600">
</p>

### Sensor

A ZY-MPU6050 module is used to make manual soldering easier.

<p align="left">
  <img src="assets/TinyDrone/zy-mpu6050-sensor.png" alt="ZY-MPU6050 sensor circuit" width="600">
</p>

### Main Controller and ADC Battery Measurement

<p align="left">
  <img src="assets/TinyDrone/esp32s3-and-adc.png" alt="ESP32-S3 controller and ADC battery measurement circuit" width="600">
</p>

### Motor Drivers

<p align="left">
  <img src="assets/TinyDrone/motor-driver.png" alt="Motor driver circuit" width="600">
</p>

### Camera

<p align="left">
  <img src="assets/TinyDrone/camera-circuit.png" alt="Camera circuit" width="600">
</p>

### Expansion Modules

The board supports an SPL06-001 barometer and a VL53L1X sensor for altitude hold. An interface is reserved for a position-hold module.

<p align="left">
  <img src="assets/TinyDrone/expansion-modules.png" alt="Expansion module interfaces" width="600">
</p>

## Software and Control Interfaces

After powering on the drone, connect your phone to the Wi-Fi network `TINY-DRONE_XXXXXXXXXXXX` using the password `87654321`.

- Drone firmware: [Tiny-Drone](https://github.com/jonny-lekaiwu/Tiny-Drone)
- Android app: [ESP-Drone-Android](https://github.com/jonny-lekaiwu/ESP-Drone-Android)
- Scan the QR code to download the Android app:

  <p align="left">
    <img src="assets/android_app.png" alt="Android app download QR code">
  </p>

On an iPhone, open `192.168.43.42` in a browser to control the drone. A native iOS app is planned according to demand.

<p align="left">
  <img src="assets/TinyDrone/android-app-control.jpg" alt="Android app control interface" width="400">
</p>
<p align="left"><em>Android app control interface</em></p>

<p align="left">
  <img src="assets/TinyDrone/mobile-browser-control.jpg" alt="Mobile browser control interface" width="400">
</p>
<p align="left"><em>Mobile browser control interface</em></p>

## Hardware Gallery

<p align="left"><img src="assets/TinyDrone/tiny-drone-photo-1.jpg" alt="Tiny-Drone hardware photo 1" width="600"></p>
<p align="left"><img src="assets/TinyDrone/tiny-drone-photo-2.jpg" alt="Tiny-Drone hardware photo 2" width="600"></p>
<p align="left"><img src="assets/TinyDrone/tiny-drone-photo-3.jpg" alt="Tiny-Drone hardware photo 3" width="600"></p>
<p align="left"><img src="assets/TinyDrone/tiny-drone-photo-4.jpg" alt="Tiny-Drone hardware photo 4" width="600"></p>

## Building Your Own Drone

Prepare medium-temperature solder paste and a hot plate before assembly to speed up soldering.

### Hardware Assembly

<p align="left">
  <img src="assets/TinyDrone/hardware-assembly.png" alt="Tiny-Drone hardware assembly diagram" width="600">
</p>

1. **Prepare the components**

   Purchase the required components according to the BOM, or buy a complete [component kit](https://item.taobao.com/item.htm?ft=t&id=1080586148318). Use good-quality parts because poor-quality components may affect flight performance. Seller-tested ZY-MPU6050 and OV2640 modules are recommended.

   <p align="left">
     <img src="assets/TinyDrone/components-and-materials.jpg" alt="Tiny-Drone components and materials" width="600">
   </p>

2. **Order the PCB**

   Open the project in JLCEDA, export the PCB, and order it from JLCPCB. Select a thickness of **1.6 mm**; otherwise, gaps may remain between the rubber rings and PCB, making the motors difficult to secure.

3. **Solder the components**

   - Use a ZY-MPU6050 module instead of a bare MPU6050 to support hand soldering.
   - Use a small knife-tip USB soldering iron for the FPC connector. Remove solder bridges with flux and a dragging motion.
   - For the 16-pin USB Type-C connector, use medium-temperature solder paste and a compact knife-tip soldering iron.
   - SMT reference:

     <img src="assets/TinyDrone/SMT.png" alt="Tiny-Drone SMT reference" width="360">

### Building and Flashing

- Built with ESP-IDF v5.5.3.
- Download the Windows version of [ESP-IDF](https://dl.espressif.cn/dl/esp-idf/?idf=5.5.3).

  <img src="assets/download_idf.png" alt="Download ESP-IDF for Windows" width="360">

~~~shell
./build.bat tiny-drone
idf.py flash monitor -p COMX
~~~

Replace `COMX` with the serial port assigned to the drone.

### One-Click Flashing

1. Open the online [ESP Web Tools flasher](https://espressif.github.io/esptool-js/).
2. Connect to and open the detected COM port.

   <img src="assets/connect.png" alt="Connect to the detected COM port" width="360">

3. Select the firmware file and set its flash address to `0`.

   <img src="assets/choose_file.png" alt="Select the firmware and flash address" width="360">

4. Start flashing and wait for it to finish.

   <img src="assets/program.png" alt="Firmware flashing progress" width="360">

### Adjusting Motor Position and Propeller Depth

#### Motor Position

Leave a small gap between each motor and its mount to protect the motor during a collision.

<p align="left">
  <img src="assets/TinyDrone/motor-position.jpg" alt="Recommended gap between motor and mount" width="600">
</p>

#### Motor Angle

Each motor should be perpendicular to the horizontal plane to avoid noticeable yaw drift.

<p align="left">
  <img src="assets/TinyDrone/motor-angle.jpg" alt="Correct motor installation angle" width="600">
</p>

#### Propeller Depth

Leave approximately **2 mm** between each propeller and motor.

<p align="left">
  <img src="assets/TinyDrone/propeller-clearance.jpg" alt="Two-millimeter propeller clearance" width="600">
</p>

## Troubleshooting

<details>
<summary><strong>The throttle does not respond after power-on</strong></summary>

Check whether the status LED is blinking slowly. The MPU6050 may have failed its self-test. Also ensure that the drone remains level during initialization.

</details>

<details>
<summary><strong>The throttle no longer responds after a collision</strong></summary>

The drone may have detected a fall and entered emergency-stop mode. Restart it.

</details>

<details>
<summary><strong>The camera does not respond</strong></summary>

Check the logs to confirm that the camera initialized successfully. Inspect each signal connection for poor solder joints and check for shorts to GND, 3.3 V, or adjacent signal lines.

</details>

<details>
<summary><strong>The drone consistently yaws to one side after takeoff</strong></summary>

If it yaws to the right, the left-side motors may be too high. Lower the left-side motors or slightly raise the right-side motors.

</details>

<details>
<summary><strong>One propeller does not spin after takeoff</strong></summary>

The motor may be damaged, or an internal wire may be broken. Replace the motor. If the issue remains, reflow the corresponding MCU signal connections and check for cold solder joints.

</details>

<details>
<summary><strong>The propellers spin, but the drone cannot take off</strong></summary>

Check that every propeller has the correct type, orientation, and installation position.

<p align="left">
  <img src="assets/TinyDrone/propeller-installation.png" alt="Correct Tiny-Drone propeller installation" width="600">
</p>

</details>
