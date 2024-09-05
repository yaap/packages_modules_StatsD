To push config to be used at device boot automatically

```
adb push perfetto/atrace.pbtxt /data/misc/perfetto-configs/boottrace.pbtxt
adb shell setprop persist.debug.perfetto.boottrace 1
```

The output trace will be written at /data/misc/perfetto-traces/boottrace.perfetto-trace.
The file will be removed before a new trace is started.

```
adb pull /data/misc/perfetto-traces/boottrace.perfetto-trace
```

# Links
- https://perfetto.dev/docs/case-studies/android-boot-tracing
