# 策略精选证据

主报告见[策略验收](../../docs/验收/策略探索-2026-11-09.md)。两份quality目录是完整短片的实发/实收包及像素摘要；postprocess-proof仅含5份接收证据，无原片或观测编号。各份60秒acceptance记录包含全部轮次；unknown-desktop旧异常不作解锁后的显示验收依据。

在compression-war运行：

```powershell
.\cw_wire_verify.exe --fast .\evidence\strategy-milestone\quality-camera
.\cw_wire_verify.exe --fast .\evidence\strategy-milestone\quality-motion
.\cw_wire_verify.exe --rx-only --fast .\evidence\strategy-milestone\postprocess-proof
```

重新复核会重写验证报告，不要把重写结果误当原始交付摘要。SHA256SUMS.txt记录交付时的原始字节；先复制到runs下复核可保留原样。图片是同源帧120、统一显示尺度的对比，不是传输文件。
