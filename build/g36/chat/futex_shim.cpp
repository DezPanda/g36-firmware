// The toolchain's libstdc++ was built without these two (std::future uses them);
// the usual Linux futex versions.
#include <bits/atomic_futex.h>
#include <cerrno>
#include <climits>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace std {
void __atomic_futex_unsigned_base::_M_futex_notify_all(unsigned *addr)
{
   syscall(SYS_futex, addr, FUTEX_WAKE, INT_MAX);
}

bool __atomic_futex_unsigned_base::_M_futex_wait_until(unsigned *addr, unsigned val, bool has_timeout,
      chrono::seconds s, chrono::nanoseconds ns)
{
   if (!has_timeout)
   {
      syscall(SYS_futex, addr, FUTEX_WAIT, val, nullptr);
      return true;
   }
   struct timespec now, rt;
   clock_gettime(CLOCK_REALTIME, &now);
   rt.tv_sec = s.count() - now.tv_sec;
   rt.tv_nsec = ns.count() - now.tv_nsec;
   if (rt.tv_nsec < 0)
   {
      rt.tv_nsec += 1000000000;
      --rt.tv_sec;
   }
   if (rt.tv_sec < 0)
      return false;
   if (syscall(SYS_futex, addr, FUTEX_WAIT, val, &rt) == -1 && errno == ETIMEDOUT)
      return false;
   return true;
}
}
