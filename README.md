# AppVolume

给单个 iOS App 注入一个独立音量：在 App 内调节 0–100% 增益，只影响该 App 自己的播放器，不动系统音量。

- 平台：iOS 15+ / arm64，无越狱依赖，形态为单个 dylib
- 覆盖：`AVPlayer`、`AVAudioPlayer`、`AVAudioPlayerNode`、`AVSampleBufferAudioRenderer`、`AVAudioMixerNode.outputVolume`（即 `AVAudioEngine` 的混音器出口）以及 AudioQueue 播放路径
- 存储：每个 App 的增益和开关状态保存在该 App 自己的 `NSUserDefaults` 里，互不干扰

## 使用

1. 用你手上的 dylib 注入器把 `AppVolume.dylib` 注入目标 App，重新签名安装。
2. 注入后**重启该 App**（方法替换在进程启动时完成，注入瞬间已在播放的音频对象不会被接管）。
3. 在 App 内**三指双击**打开音量面板。

面板提供滑块（0–100%）、启用开关、恢复 100% 以及诊断计数（已捕获多少个 AVFoundation 播放器/混音器、是否已接管 `AVAudioEngine` 的 `mainMixerNode`、多少个 AudioQueue、音量实际下发次数，以及模块拦截状态、`errno`、应用目录与首个可见镜像路径，用于排查注入问题）。

100% 表示完全保留播放器自己的原始音量，不做任何缩放；0% 为静音；关闭开关时按缓存的原始音量写回。

对 ManicEMU 这类 RetroShell/Delta 系模拟器（音频从 `AVAudioEngine` + `AVAudioSourceNode` 送入 `mainMixerNode`，全程不碰上面四个播放器类），v0.2 之所以计数为 0、完全无效，就是因为缺少混音器这一层；v0.3 起 `mainMixerNode.outputVolume` 即为缩放点。

## 它能做什么，不能做什么

这是一份明确的边界，不要期待它成为系统级混音器：

- **只作用于被注入的那个进程。** dylib 拦截的是进程内的 Objective-C 方法和该 App 自身镜像里的 AudioQueue 符号，不会跨越进程边界影响别的 App，也不会影响系统音量。
- **不放大音量。** 增益上限是 1.0，即在播放器自身音量基础上按比例衰减，不是超频放大，因此不存在削波或损伤单元的问题。
- **未覆盖的音频路径**：绕过 `AVAudioEngine` 混音器、直接调用 Audio Unit（`AURemoteIO` / `AudioUnitRender`）的 App，以及运行在独立进程里的 WebKit 播放，目前都不会被拦截。这类 App 表现为面板计数为 0。
- **AudioQueue 增益有延迟**：改动在下一次 `AudioQueueStart` 或送入音频缓冲时才下发到原生队列，不是即时生效。
- **混音器是唯一边界**：一旦接管了某个 `AVAudioEngine` 的 `mainMixerNode`，同一引擎里的 `AVAudioPlayerNode.volume` 就只转发原始请求值、不再重复缩放，避免增益被平方。代价是：若 App 用同一引擎把音频渲染到文件（录音/导出），导出结果也会带上该增益。
- **真机验证情况**：编译、Mach-O 签名校验和音量策略的单元测试已通过；**音频听感和面板交互尚未在真机上系统验证**。

## 构建

```sh
SDKROOT=/path/to/iPhoneOS.sdk bash build.sh
```

在 macOS 上不带 `SDKROOT` 时会自动用 `xcrun --sdk iphoneos`。在 Linux 上交叉编译需要 LLVM ≥ 19 的 `clang` 与 `ld64.lld`，以及一份解出来的 iPhoneOS SDK（默认取 `.toolchain/iPhoneOS15.6.sdk`）。

构建流程依次执行：`tests/volume_policy_test.c` 的音量策略单元测试 → 编译链接 dylib → `tests/verify_macho.py` 校验架构、最低系统版本、依赖路径和逐页签名哈希。

产物在 `dist/AppVolume.dylib`，install_name 为 `@rpath/AppVolume.dylib`。若注入器要求固定路径，把 `build.sh` 里的 `-Wl,-install_name` 改成 `@executable_path/Frameworks/AppVolume.dylib`。

## 实现要点

两条独立的拦截路径：

- `src/AppVolume.m`：`objc/runtime` 方法替换。为每个播放器对象维护一份 `QAVVolumePolicy`（请求音量 + 渐变包络），`volume` getter 返回请求值而非缩放后的值，因此不会产生累积衰减。用线程局部计数器 `QAVInternalVolumeWrite` / `QAVTransportDepth` 区分「App 发起的写入」和「我们自己发起的写入」，避免递归与重复缩放。原生调用一律在锁外执行，靠 revision + generation 快照重试来保证并发下的最终一致。
- `src/AudioQueueVolume.c` + `vendor/fishhook`：遍历 dyld 镜像，只对路径位于该 App bundle 内的镜像重绑定 AudioQueue 符号。本 dylib 自身和系统框架保留原生调用，防止递归。注意 `_dyld_register_func_for_add_image` 与已加载镜像的遍历都会执行一遍：构造函数运行时 App 主二进制和它的 frameworks 早已映射完毕，只等通知会一个都扫不到。

## 适配 ManicEMU（Delta 系模拟器）

- 面板计数为 0 是预期现象，不是注入失败：DeltaCore 的 `AudioManager` 走 `AVAudioEngine` + `AVAudioSourceNode` → `AVAudioUnitTimePitch` → `mainMixerNode`，音量只体现在 `mainMixerNode.outputVolume` 上。
- 静态分析（v2.0.2 SideloadJIT，arm64 主二进制）确认导入的音频类只有 `AVAudioEngine`/`AVAudioPlayerNode`/`AVAudioSourceNode`/`AVAudioUnitTimePitch`/`AVAudioConverter`/`AVAudioPlayer`/`AVPlayer`，`__objc_methname` 里存在 `setOutputVolume:` 与 `mainMixerNode`，全 App 没有任何 `AudioQueue*` 符号 —— 42 个 `libretro.framework` 核心只导出 `retro_set_audio_sample*`，采样最终由 Swift 侧灌进引擎。
- 因此本版的混音器接管即为该 App 的缩放点；`AudioManager` 自己写的 0.0（静音/其他 App 正在播放）与 1.0 都作为「请求音量」记录，缩放后仍然保留其静音语义。

## 限制与风险

- 仅 arm64 单架构切片，未提供 arm64e。
- 方法替换假设目标类在 iOS 15 SDK 中的签名稳定；Apple 改动内部实现时需要同步跟进。
- 面板依赖 `UISheetPresentationController`，在非 UIKit 场景（如纯 `AVPlayer` 后台进程、扩展）不会显示。
- 请勿把本项目的 hook 逻辑用于绕过 App 的付费、广告或内容限制。

## 许可

- 本项目源码：MIT，见 [LICENSE](LICENSE)
- `vendor/fishhook`：Facebook, Inc. 的 BSD-3-Clause 代码，保留原始版权声明
