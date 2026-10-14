# Linux Camera Tracker

嵌入式 Linux 摄像头触摸选点与单目标跟踪程序，使用 C11、POSIX 线程、V4L2、Linux framebuffer、evdev 与 SocketCAN。默认编译器为 ARM hard-float 交叉工具链。

## 目录

```text
hard/
  camera/     V4L2 采集
  display/    LCD、坐标变换和图像叠加
  input/      触摸输入
  can/        电机与云台
  network/    独立 TCP 图像客户端
user/
  main.c      入口、线程与资源清理
  core/       帧队列
  tracking/   模板跟踪与日志
tests/        CAN、云台、网络及 V4L2 ABI 测试
Makefile      交叉编译及主机测试目标
```

## 编译

需要 GNU Make、GCC 交叉工具链，以及与目标开发板兼容的 Linux 头文件、libc 和静态 pthread 库。默认前缀为 `arm-linux-gnueabihf-`：

```bash
git clone https://github.com/wu1571028912-crypto/linux-camera-tracker.git
cd linux-camera-tracker
make
# 或选择实际安装的交叉工具链
make CROSS_COMPILE=arm-linux-gnueabihf-
```

输出文件名沿用原工程为 `make`，运行时必须带 `./`。Makefile 默认采用静态链接，并为老 ARM32 V4L2 ABI 指定 32 位时间布局；更换 libc、内核或架构时须检查兼容性。现有编译期检查位于 `tests/v4l2_abi_test.c`。

## 在开发板运行

硬件需要可用的摄像头、16 bpp framebuffer 和触摸设备。将程序复制到目标板，确保当前用户有设备访问权限：

```bash
./make /dev/video0 /dev/input/event1
```

第二个设备参数可省略，触摸默认路径为 `/dev/input/event1`；LCD 路径在 `hard/display/lcd.c` 中固定为 `/dev/fb0`。实际设备编号和驱动支持需按硬件确认。按 Ctrl+C 请求退出。

不需要 CAN 模块时不设置 `GIMBAL_CONFIG`。该变量控制云台模块是否启动，但不代表主循环中的视觉控制调用已启用。详见 [GIMBAL.md](GIMBAL.md)。

## 独立模块与测试

在 Linux / WSL 中使用本机 GCC 运行模拟接口测试，无需连接 CAN 电机或摄像头：

```bash
make test test-network
```

`make network-module` 单独交叉编译网络模块；`make v4l2-abi` 交叉编译 ABI 检查。测试结果与验证边界见 [docs/VALIDATION.md](docs/VALIDATION.md)。

