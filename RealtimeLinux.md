# Real-time Linux — PREEMPT_RT

ဒီစာတွင် “RTLinux” ဆိုသည်မှာ ပေးထားသော repository နှင့်သက်ဆိုင်သည့် **Linux PREEMPT_RT** ကို ဆိုလိုသည်။

## ဘာအတွက်သုံးတာလဲ?

အချိန်အရေးကြီးသော task တစ်ခု အလုပ်လုပ်ရန်အသင့်ဖြစ်ပြီးနောက် CPU ရရှိသည်အထိ စောင့်ရသော **scheduling latency** နှင့် ထိုစောင့်ချိန်၏ အတက်အကျ **jitter** ကို လျှော့ချရန်ဖြစ်သည်။ ပိုမြန်သည့် CPU သို့မဟုတ် computation accelerator မဟုတ်ပါ။

ဥပမာ—၁ ms တိုင်း အလုပ်လုပ်ရန်လိုသော control loop တွင် ပျမ်းမျှအမြန်နှုန်းထက် သတ်မှတ်ချိန်မီ အလုပ်စနိုင်ခြင်းက ပိုအရေးကြီးသည်။ PREEMPT_RT သည် ထိုရည်ရွယ်ချက်ကို ကူညီပေးသော်လည်း kernel တင်လိုက်ရုံနှင့် deadline အားလုံးမီကြောင်း အာမမခံပါ။ [Kernel theory](https://docs.kernel.org/core-api/real-time/theory.html)

## Kernel ထဲမှာ ဘာပြောင်းလဲသလဲ?

| လုပ်ဆောင်ချက် | အကျိုးသက်ရောက်မှု |
| --- | --- |
| ပိုများသော preemption points | Kernel အလုပ်တချို့ကို ခေတ္တရပ်ပြီး priority မြင့် task ကို CPU ပေးနိုင်သည် |
| Threaded interrupts | Interrupt အများစု၏ handler ကို scheduler ထိန်းချုပ်နိုင်သော thread အဖြစ် လုပ်ဆောင်သည် |
| Priority inheritance ပါသော locks | Priority မြင့် task ကိုပိတ်ဆို့နေသော lock owner ၏ priority ကို ယာယီမြှင့်ပေးသည် |

Low-level interrupt handling နှင့် scheduler စသည့် critical code အချို့မှာ preempt လုပ်မရသေးပါ။ Kernel lock ပြောင်းလဲမှုက application mutex အားလုံးကို priority inheritance အလိုအလျောက်ပေးခြင်း မဟုတ်ပါ။ [Kernel theory](https://docs.kernel.org/core-api/real-time/theory.html)

## ပေးထားသော repository နဲ့ ဘာလုပ်နိုင်သလဲ?

မူရင်းလိပ်စာ—`git://git.kernel.org/pub/scm/linux/kernel/git/rt/linux-stable-rt.git`

၎င်းသည် stable RT kernel များ၏ source/history ကို ထိန်းသိမ်းထားသော repository ဖြစ်သည်။ Source ကိုဖတ်ခြင်း၊ RT ပြင်ဆင်ချက်များနှိုင်းယှဉ်ခြင်း၊ ကိုယ့် hardware အတွက် kernel configure/build လုပ်ခြင်းနှင့် kernel/driver ပြင်ဆင်စမ်းသပ်ခြင်းတို့ လုပ်နိုင်သည်။ Application ထဲ link လုပ်ရသော `.so` library မဟုတ်ပါ။ [PREEMPT_RT repositories](https://wiki.linuxfoundation.org/realtime/preempt_rt_versions)

Branch/tag များကို download အပြည့်မလုပ်ဘဲ ကြည့်နိုင်သည်—

```sh
git ls-remote --heads --tags https://git.kernel.org/pub/scm/linux/kernel/git/rt/linux-stable-rt.git
```

Build လုပ်မည်ဆိုလျှင် hardware နှင့်ကိုက်ပြီး ထိန်းသိမ်းနေဆဲ RT branch/release ကို တိတိကျကျရွေးပါ။ Default `master` ကို နောက်ဆုံး RT release ဟု မယူဆပါနှင့်။ RT branch နှင့် patches-only branch တို့၏ အကြောင်းအရာလည်း မတူနိုင်ပါ။ [Repository mirror](https://kernel.googlesource.com/pub/scm/linux/kernel/git/rt/linux-stable-rt/)

**Linux 6.12 မှစ၍** support ရှိသော architecture များတွင် mainline kernel မှ PREEMPT_RT ကို patch သီးသန့်မလိုဘဲ enable လုပ်နိုင်သည်။ ထို့ကြောင့် RT သုံးရန် ဒီ repository ကို မဖြစ်မနေ build လုပ်ရန်မလိုပါ။ ကိုက်ညီသော distribution RT kernel ကိုလည်း သုံးနိုင်သည်။ Kernel version အသစ်ဖြစ်ရုံနှင့် RT enabled ဖြစ်နေခြင်းမဟုတ်ပါ။ [Linux scheduling manual](https://man7.org/linux/man-pages/man7/sched.7.html)

## ကောင်းကျိုးနှင့် trade-off

| အကြောင်းအရာ | ရှင်းလင်းချက် |
| --- | --- |
| အချိန်တိကျမှု | Priority နှင့် workload မှန်ကန်စွာစီမံလျှင် wake-up delay/jitter လျော့နိုင်သည် |
| အသုံးဝင်သောနေရာ | Periodic control၊ industrial automation၊ အချိန်မီတုံ့ပြန်ရန်လိုသော data acquisition |
| Performance | Scheduling/locking ပြောင်းလဲမှုကြောင့် workload အလိုက် overhead ရှိနိုင်သည်။ Throughput တိုးမည်ဟု မယူဆဘဲ နှိုင်းယှဉ်တိုင်းတာရမည် |
| Priority မှားယွင်းခြင်း | CPU မလွှတ်သော high-priority RT thread က lower-priority အလုပ်များကို CPU မရအောင် ဖြစ်စေနိုင်သည် |
| Hardware ကန့်သတ်ချက် | Firmware၊ USB bus၊ memory contention နှင့် power management တို့ကြောင့် delay ကျန်နိုင်သည် |
| ထိန်းသိမ်းမှု | Kernel ပြောင်းသည့်အခါ အသုံးပြုမည့် drivers/modules နှင့် configuration ကို ပြန်လည်စစ်ဆေးရန်လိုသည် |

Deadline အတွက် hardware နှင့် application ပါ အတူစစ်ဆေးရသည်။ Kernel တစ်ခုတည်းကြောင့် hard-real-time guarantee မဖြစ်ပါ။ [Hardware considerations](https://docs.kernel.org/core-api/real-time/hardware.html), [Scheduling policies](https://man7.org/linux/man-pages/man7/sched.7.html)

## အသုံးပြုရန် အဆင့်တိုများ

1. **လိုအပ်ချက်သတ်မှတ်ပါ။** Period၊ deadline နှင့် လက်ခံနိုင်သော worst-case latency ကို အရင်သတ်မှတ်ပါ။
2. **Kernel ရွေးပါ။** Hardware support နှင့် maintenance status စစ်ပြီး `CONFIG_PREEMPT_RT=y` ဖြင့် build/enable လုပ်ထားသော kernel ကို boot လုပ်ပါ။ [Kernel configuration](https://docs.kernel.org/core-api/real-time/kernel-configuration.html)
3. **Thread များကို ခွဲစီမံပါ။** အချိန်အရေးကြီးသော thread များအတွက်သာ `SCHED_FIFO` / `SCHED_RR` စသည့် policy နှင့် သင့်တော်သော priority ကို သတ်မှတ်ပါ။ CPU affinity နှင့် IRQ placement ကို workload အလိုက်ညှိပါ။
4. **မခန့်မှန်းနိုင်သောနှောင့်နှေးမှု လျှော့ပါ။** Critical path မှ blocking I/O၊ dynamic allocation နှင့်ရှည်လျားသော lock ကို လျှော့ပါ။ လိုအပ်လျှင် memory ကိုကြိုတင် allocate/touch လုပ်ပြီး `mlockall()` ဖြင့် paging delay လျှော့ပါ။ RT priority နှင့် memory locking အတွက် permission/resource limits ကိုပါ စီမံရသည်။ [Scheduling manual](https://man7.org/linux/man-pages/man7/sched.7.html)
5. **Load အောက်မှာတိုင်းတာပါ။** `rtla timerlat hist` ဖြင့် timer/OS latency distribution ကို စစ်နိုင်သည်။ ထို့အပြင် application ၏ deadline misses နှင့် အမှန်တကယ် end-to-end latency ကိုပါ သီးခြားတိုင်းပါ။ စမ်းသပ်မှုတစ်ခုပေါ်မှ maximum သည် အနာဂတ် latency ၏ သက်သေပြထားသော upper bound မဟုတ်ပါ။ [RTLA timerlat](https://docs.kernel.org/tools/rtla/rtla-timerlat-hist.html)

ရည်ရွယ်ချက်မှာ **အလုပ်အဆင်သင့်ဖြစ်ချိန်မှ CPU ပေါ်စတင်လုပ်ဆောင်ချိန်အထိ ပိုခန့်မှန်းနိုင်စေရန်** ဖြစ်သည်။ Deadline ပြည့်မီမှုကို system တစ်ခုလုံးအပေါ် တိုင်းတာအတည်ပြုရမည်။

မှတ်ချက်—Kernel.org repository web page ကို တိုက်ရိုက်ဖတ်ရာတွင် HTTP 403 ဖြစ်သောကြောင့် အထက်ပါ repository mirror နှင့် official documentation ကို အသုံးပြုထားသည်။ ဤဖိုင်အတွက် kernel install/build သို့မဟုတ် latency benchmark မလုပ်ထားပါ။
