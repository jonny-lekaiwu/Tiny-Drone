# Tiny-Drone 探秘者开源无人机

> 一款基于 ESP32-S3 开发的开源微型无人机，支持手机 App、手机浏览器和遥控器控制，并提供 Wi-Fi 图传与 RID 广播功能。

<p align="left">
  <img src="assets/TinyDrone/tiny-drone-overview.jpg" alt="Tiny-Drone 探秘者开源无人机" width="600">
</p>

## 项目简介

Tiny-Drone（探秘者开源无人机）是一款基于 ESP32-S3 开发的无人机玩具。项目主要代码源自 [ESP-Drone](https://github.com/espressif/esp-drone) 开源工程，并遵循  [GPL-3.0 License](LICENSE)。 

主要功能：

- 支持 Android App 控制
- 支持手机浏览器控制
- 支持遥控器控制
- 支持 Wi-Fi 图传
- 支持 RID 广播
- 支持扩展气压计、激光测距及定点悬停模块

## 视频演示

[B 站视频：功能演示及介绍](https://www.bilibili.com/video/BV1T7th6gEEx/)

## 注意事项

> [!IMPORTANT]
> 室外飞行前，建议自行烧录产品序列号，完成实名登记后再开启 RID 功能。请遵守当地无人机飞行法规。

- Wi-Fi 图传可能出现少量横纹，电池电压过低时会更加明显。
- 出现航向角偏移时，应调整电机高度和桨叶安装深度。
- 使用定高功能前，必须确保桨叶与电机已调整至合适角度，否则可能出现失控乱飞。
- 安装桨叶时，可用手抵住电机底部，避免用力过大冲破后盖并导致电机脱落。

## 硬件设计

### 电源

使用锂电池供电，经升压芯片升压后，再通过 LDO 为传感器和主控供电。

<p align="left">
  <img src="assets/TinyDrone/power-supply.png" alt="Tiny-Drone 电源电路" width="600">
</p>

### 状态指示灯及电源指示灯

<p align="left">
  <img src="assets/TinyDrone/status-and-power-led.png" alt="状态指示灯及电源指示灯电路" width="600">
</p>

### 传感器

使用 ZY-MPU6050 模块，便于手工焊接。

<p align="left">
  <img src="assets/TinyDrone/zy-mpu6050-sensor.png" alt="ZY-MPU6050 传感器电路" width="600">
</p>

### 主控及 ADC 电量采集

<p align="left">
  <img src="assets/TinyDrone/esp32s3-and-adc.png" alt="ESP32-S3 主控及 ADC 电量采集电路" width="600">
</p>

### 电机驱动

<p align="left">
  <img src="assets/TinyDrone/motor-driver.png" alt="电机驱动电路" width="600">
</p>

### 摄像头

<p align="left">
  <img src="assets/TinyDrone/camera-circuit.png" alt="摄像头电路" width="600">
</p>

### 扩展模块

支持 SPL06-001 气压计和 VL53L1X 定高模块，并预留定点悬停模块接口。

<p align="left">
  <img src="assets/TinyDrone/expansion-modules.png" alt="扩展模块接口电路" width="600">
</p>

## 软件与控制端

- 无人机固件：[Tiny-Drone](https://github.com/jonny-lekaiwu/Tiny-Drone)
- Android App：[ESP-Drone-Android](https://github.com/jonny-lekaiwu/ESP-Drone-Android) 

- 扫码下载安卓app
  <p align="left">
    <img src="assets/android_app.png" alt="Android App">
  </p>

苹果手机可通过浏览器访问 `192.168.43.42` 进行控制。iOS App 正在规划中，将根据实际需求安排开发。

<p align="left">
  <img src="assets/TinyDrone/android-app-control.jpg" alt="Android App 控制界面" width="400">
</p>
<p align="left"><em>Android App 控制界面</em></p>

<p align="left">
  <img src="assets/TinyDrone/mobile-browser-control.jpg" alt="手机浏览器控制界面" width="400">
</p>
<p align="left"><em>手机浏览器控制界面</em></p>

## 实物展示

<p align="left">
  <img src="assets/TinyDrone/tiny-drone-photo-1.jpg" alt="Tiny-Drone 实物图一" width="600">
</p>

<p align="left">
  <img src="assets/TinyDrone/tiny-drone-photo-2.jpg" alt="Tiny-Drone 实物图二" width="600">
</p>

<p align="left">
  <img src="assets/TinyDrone/tiny-drone-photo-3.jpg" alt="Tiny-Drone 实物图三" width="600">
</p>

<p align="left">
  <img src="assets/TinyDrone/tiny-drone-photo-4.jpg" alt="Tiny-Drone 实物图四" width="600">
</p>

## 复刻无人机

复刻前可准备中温锡膏和加热台，以提高焊接效率。

### 硬件组装

<p align="left">
  <img src="assets/TinyDrone/hardware-assembly.png" alt="Tiny-Drone 硬件组装示意图" width="600">
</p>

1. **准备元器件**

   按照 BOM 清单购买对应元器件，也可以直接购买[物料包](https://item.taobao.com/item.htm?ft=t&id=1080586148318)。建议选择质量较好的元器件，以免影响飞行效果；ZY-MPU6050 和 OV2640 最好购买经过卖家测试的产品。

   <p align="left">
     <img src="assets/TinyDrone/components-and-materials.jpg" alt="Tiny-Drone 元器件与物料" width="600">
   </p>

2. **PCB 打板**

   在嘉立创打开项目工程并导出 PCB 下单。PCB 厚度请选择 **1.6 mm**，否则胶圈与 PCB 之间可能产生间隙，不利于固定电机位置。

3. **焊接元器件**

   - 使用 ZY-MPU6050 模块代替 MPU6050，以支持手工焊接陀螺仪传感器。
   - 焊接 FPC 座时，建议使用刀型 USB 电烙铁；出现连锡时，可使用助焊剂和松香拖开焊锡。
   - 焊接 TYPE-C 16Pin 接口时，可使用中温锡膏搭配小体积刀型电烙铁。

### 编译及烧录

* 基于esp-idf-v5.5.3编译
    - 下载windows版本 [esp-idf](https://dl.espressif.cn/dl/esp-idf/?idf=5.5.3)

  <img src="assets/download_idf.png" width="360"/> 

  ~~~
  ./build.bat tiny-drone
  idf.py flash monitor -p COMX
  ~~~

### 一键烧录
* 打开在线烧录[网站](https://espressif.github.io/esptool-js/)

* 打开连接识别到的COM并打开

  <img src="assets/connect.png" width="360"/> 

* 选择要烧录的文件，设置地址为0

  <img src="assets/choose_file.png" width="360"/> 

* 点击烧录，等待烧录完成

  <img src="assets/program.png" width="360"/> 

### 调整电机位置和桨叶安装深度

#### 电机位置

电机与底座之间应保留适当间隙，以便在发生碰撞时保护电机。

<p align="left">
  <img src="assets/TinyDrone/motor-position.jpg" alt="电机与底座的正确间隙" width="600">
</p>

#### 电机角度

电机应与水平面保持垂直，以避免航向角出现明显偏移。

<p align="left">
  <img src="assets/TinyDrone/motor-angle.jpg" alt="电机安装角度" width="600">
</p>

#### 桨叶深度

桨叶与电机之间应保留约 **2 mm** 的间距。

<p align="left">
  <img src="assets/TinyDrone/propeller-clearance.jpg" alt="桨叶与电机之间保留约 2 mm 间距" width="600">
</p>

## 故障排除

<details>
<summary><strong>打开电源后，拉油门没有反应</strong></summary>

观察状态指示灯是否处于慢闪状态。可能是 MPU6050 未通过自检，请同时确认无人机是否保持水平。

</details>

<details>
<summary><strong>撞到物体后，再次拉油门没有反应</strong></summary>

无人机可能检测到跌落并进入急停状态，请重启设备。

</details>

<details>
<summary><strong>摄像头没有反应</strong></summary>

检查日志，确认摄像头是否初始化成功。若初始化成功，请逐一检查信号线是否焊接良好，以及信号线是否与 GND、3.3 V 或其他信号线短路。

</details>

<details>
<summary><strong>起飞后，航向角总是偏向一侧</strong></summary>

如果无人机向右偏，可能是左侧电机位置过高。可将左侧电机调低，或将右侧电机适当调高。

</details>

<details>
<summary><strong>起飞后，其中一个桨叶不转</strong></summary>

电机可能因碰撞损坏，或内部接线已经断开，需要更换电机。如果更换电机后问题仍然存在，请为 MCU 相关信号线重新上锡，并检查是否存在虚焊。

</details>

<details>
<summary><strong>桨叶转动，但无人机无法起飞</strong></summary>

检查桨叶的型号、方向和安装位置是否正确。 

<p align="left">
  <img src="assets/TinyDrone/propeller-installation.png" alt="Tiny-Drone 桨叶正确安装方式" width="600">
</p>

</details> 