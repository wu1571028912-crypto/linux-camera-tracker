/* 在真实初始化路径上截获 setsockopt，测试传给内核的过滤器。
 * socket替换为/dev/null，测试绝不发送物理CAN帧。
 * GCC链接参数 --wrap 保证源码仍执行原来的过滤器构造代码。
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#ifdef TEST_TOOL
#define main tool_main
#include "can_test.c"
#undef main
#elif defined(TEST_GIMBAL)
#include "gimbal.c"
#else
#include "motor.c"
#endif
static int checked;
unsigned __wrap_if_nametoindex(const char *name)
{ (void)name; return 1; }
int __wrap_socket(int domain,int type,int protocol)
{ (void)domain;(void)type;(void)protocol; return open("/dev/null",O_RDWR); }
int __wrap_setsockopt(int socket_fd,int level,int option,const void *value,socklen_t len)
{
    const struct can_filter *filters=value;
    (void)socket_fd;
    assert(level==SOL_CAN_RAW && option==CAN_RAW_FILTER);
    assert(len>0 && len%sizeof(*filters)==0);
    for (unsigned i=0;i<len/sizeof(*filters);i++) {
        assert((filters[i].can_mask & CAN_ERR_FLAG)==0);
        assert(filters[i].can_mask==(CAN_EFF_FLAG|CAN_RTR_FLAG|CAN_EFF_MASK));
        assert(filters[i].can_id==(CAN_EFF_FLAG|((canid_t)(i+2)<<8)));
    }
    checked=1;
    /* 阻止初始化进入bind/收发路径。 */
    errno=ECANCELED; return -1;
}
int main(void)
{
#ifdef TEST_TOOL
    assert(open_can("test")<0);
#elif defined(TEST_GIMBAL)
    assert(setenv("GIMBAL_CONFIG","test -20 20 -20 20 1 1",1)==0);
    assert(gimbal_start()<0);
#else
    assert(motor_move_angle(2,1.0f)<0);
#endif
    assert(checked);
    puts("CAN_RAW_FILTER setup regression passed (no physical CAN used)");
    return 0;
}
