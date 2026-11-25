### 4.1 timerfd_create - fs/timerfd.c:394

```c
SYSCALL_DEFINE2(timerfd_create, int, clockid, int, flags)
{
    ctx = kzalloc_obj(*ctx);
    init_waitqueue_head(&ctx->wqh);
    ctx->clockid = clockid;
    // alarm 模式或普通模式
    if (isalarm(ctx))
        ctx->tmr = &ctx->t.alarm.timer;
    else
        ctx->tmr = &ctx->t.tmr;
    // hrtimer_init(ctx->tmr, clockid, HRTIMER_MODE_ABS);
    // ctx->tmr->function = timerfd_tmrproc;
    return anon_inode_getfd("[timerfd]", &timerfd_fops, ctx, ...);
}
```
