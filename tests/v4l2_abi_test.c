#include <stdint.h>
#include <sys/time.h>
#include <linux/videodev2.h>
/* ARM32 老内核要求传统 timeval/V4L2 布局；编译期拒绝 time64 ABI。 */
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(struct timeval) == 8, "V4L2 needs 32-bit timeval");
_Static_assert(sizeof(struct v4l2_buffer) == 68, "Unexpected ARM32 V4L2 buffer ABI");
#endif
