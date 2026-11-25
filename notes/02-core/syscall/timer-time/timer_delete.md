### 3.4 timer_delete - kernel/time/posix-timers.c:1052

```
timer_delete(timer_id)
  ├─ scoped_timer_get_or_fail(timer_id)                   // 加锁查找
  ├─ posix_timer_delete(timer)
  │    ├─ hrtimer_cancel(&timr->it.real.timer)            // 取消 hrtimer
  │    └─ 释放 sigqueue
  └─ posix_timer_unhash_and_free(timer)                   // 释放 k_itimer
```

---

## 4 timerfd 定时器文件描述符
