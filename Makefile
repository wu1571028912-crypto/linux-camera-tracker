# ==========================================
# 1. 编译器与工具链配置
# ==========================================
# 定义交叉编译器前缀（默认是 ARM 平台的 gcc，？= 表示如果外部未定义则使用该默认值）
CROSS_COMPILE ?= arm-linux-gnueabihf-
# 组合完整的 C 编译器命令 (例如 arm-linux-gnueabihf-gcc)
CC = $(CROSS_COMPILE)gcc

# ==========================================
# 2. 目标与目录定义
# ==========================================
BUILD_DIR = build
TARGET = make                 # 摄像头主程序的输出文件名
ABI_CHECK = $(BUILD_DIR)/v4l2_abi_test.o # V4L2 (Video for Linux) 兼容性测试的目标文件路径
NETWORK_OBJ = $(BUILD_DIR)/network_client.o
NETWORK_TEST = $(BUILD_DIR)/test_network_client

# ==========================================
# 3. 编译参数配置 (CFLAGS / LDFLAGS)
# ==========================================
# 指定头文件的查找目录 (-I 表示 include path)
INCLUDES = -Ihard/camera -Ihard/display -Ihard/input \
    -Iuser/tracking -Iuser/core -Ihard/can -Ihard/network

# V4L2 相关的宏定义，强制使用 32 位时间格式以兼容老旧的开发板内核
V4L2_ABI_CFLAGS = -U_TIME_BITS -U_FILE_OFFSET_BITS -D_TIME_BITS=32

# 最终的 C 编译选项：开启所有警告(-Wall -Wextra)，开启调试信息(-g)，开启O2优化，指定C11标准，支持多线程(-pthread)等
CFLAGS = -Wall -Wextra -g -O2 -std=c11 -D_POSIX_C_SOURCE=200809L \
    -pthread $(V4L2_ABI_CFLAGS) $(INCLUDES)

# 链接选项：静态编译 (-static，生成的程序体积大但不需要开发板有对应的动态库)，支持多线程 (-pthread)
LDFLAGS = -static -pthread

# ==========================================
# 4. 源文件与头文件依赖列表
# ==========================================
# 摄像头主程序的 C 源码文件列表
SRCS = user/main.c \
    hard/camera/camera.c \
    hard/display/lcd.c hard/display/display_transform.c \
    hard/display/processing_imag.c \
    hard/input/touch.c \
    user/tracking/tracker.c user/tracking/tracking_log.c \
    user/core/frame_queue.c hard/can/motor.c hard/can/gimbal.c
# 摄像头主程序的头文件列表
HDRS = hard/camera/camera.h hard/display/lcd.h \
    hard/display/display_transform.h hard/display/processing_imag.h \
    hard/input/touch.h user/tracking/tracker.h \
    user/tracking/tracking_log.h user/core/frame_queue.h hard/can/motor.h hard/can/gimbal.h

# ==========================================
# 5. 编译规则 (Rules)
# ==========================================
# 默认目标：输入 make 默认执行
all: camera 

# 伪目标，指向实际生成的可执行文件
camera: $(TARGET)

# 如果 build 目录不存在，则创建它
$(BUILD_DIR):
	mkdir -p $@

# 编译摄像头主程序：依赖所有 SRCS、HDRS 和 ABI测试文件
$(TARGET): $(SRCS) $(HDRS) $(ABI_CHECK) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

$(ABI_CHECK): tests/v4l2_abi_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 单独构建网络模块，不链接进现有摄像头主程序。
network-module: $(NETWORK_OBJ)

$(NETWORK_OBJ): hard/network/network_client.c hard/network/network_client.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(NETWORK_TEST): tests/test_network_client.c hard/network/network_client.c \
                 hard/network/network_client.h | $(BUILD_DIR)
	gcc -std=c11 -Wall -Wextra -Werror -pthread -Ihard/network \
	    $< hard/network/network_client.c \
	    -Wl,--wrap=send -Wl,--wrap=recv -o $@

test-network: $(NETWORK_TEST)
	./$(NETWORK_TEST)

# 提供一个单独编译 v4l2-abi 的快捷指令
v4l2-abi: $(ABI_CHECK)

# 执行子目录的测试脚本
test: | $(BUILD_DIR)
	gcc -std=c11 -Wall -Wextra -Werror -pthread -DTEST_GIMBAL -Ihard/can tests/test_can_filter.c -Wl,--wrap=socket,--wrap=setsockopt,--wrap=if_nametoindex -o $(BUILD_DIR)/test_filter_gimbal
	./$(BUILD_DIR)/test_filter_gimbal
	gcc -std=c11 -Wall -Wextra -Werror -pthread -Ihard/can tests/test_can_filter.c -Wl,--wrap=socket,--wrap=setsockopt,--wrap=if_nametoindex -o $(BUILD_DIR)/test_filter_motor
	./$(BUILD_DIR)/test_filter_motor
	gcc -std=c11 -Wall -Wextra -Werror -pthread -Ihard/can tests/test_gimbal.c hard/can/gimbal.c -o $(BUILD_DIR)/test_gimbal
	./$(BUILD_DIR)/test_gimbal

# 清除所有生成的文件
clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# 声明伪目标：防止同名文件（比如当前目录下有个文件叫clean）导致命令不执行
.PHONY: all camera network-module test-network v4l2-abi test clean
