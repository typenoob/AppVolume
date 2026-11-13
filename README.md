# AppVolume

给单个 iOS App 注入一个独立音量：在 App 内调节 0–100% 增益，只影响该 App 自己的播放器，不动系统音量。

- 平台：iOS 15+ / arm64，无越狱依赖，形态为单个 dylib
- 覆盖：`AVPlayer`、`AVAudioPlayer`、`AVAudioPlayerNode`、`AVSampleBufferAudioRenderer`、`AVAudioMixerNode.outputVolume`（即 `AVAudioEngine` 的混音器出口）、AudioQueue 播放路径，以及 App 自己创建的 Audio Unit（RemoteIO / VoiceProcessingIO）的渲染缓冲
- 存储：每个 App 的增益和开关状态保存在该 App 自己的 `NSUserDefaults` 里，互不干扰

## 使用

1. 用你手上的 dylib 注入器把 `AppVolume.dylib` 注入目标 App，重新签名安装。
2. 注入后**重启该 App**（方法替换在进程启动时完成，注入瞬间已在播放的音频对象不会被接管）。
3. 在 App 内**三指双击**打开音量面板。

面板提供滑块（0–100%）、启用开关、恢复 100%，以及一段诊断计数：已捕获多少个 AVFoundation 播放器/混音器、是否已接管 `AVAudioEngine` 的 `mainMixerNode`、AudioQueue 个数与下发次数、Audio Unit 挂载/卸载数与实际缩放帧数、采样格式（位宽/浮点/声道/最近一次 bus 号）、模块扫描与拦截状态（含 `errno`）、应用目录和首个可见镜像路径。这些信息用来区分「注入没生效」「路径没覆盖」「格式不认识」三种情况。

100% 表示完全保留播放器自己的原始音量，不做任何缩放；0% 为静音；关闭开关时按缓存的原始音量写回。

对 ManicEMU 这类 RetroShell/Delta 系模拟器（音频从 `AVAudioEngine` + `AVAudioSourceNode` 送入 `mainMixerNode`，全程不碰上面四个播放器类），v0.2 之所以计数为 0、完全无效，就是因为缺少混音器这一层；v0.3 起 `mainMixerNode.outputVolume` 即为缩放点。

对抖音这类自带音频引擎的 App（RealX / byteaudio 直接驱动 RemoteIO，音量在自家软件混音器里算），连 `AVAudioEngine` 都不经过，只能在 Audio Unit 交给硬件的那一帧缓冲上乘增益；见下节。

## 它能做什么，不能做什么

这是一份明确的边界，不要期待它成为系统级混音器：

- **只作用于被注入的那个进程。** dylib 拦截的是进程内的 Objective-C 方法，以及该 App 自身镜像里的 AudioQueue / Audio Unit 符号；系统框架（AVFoundation、AudioToolbox）保留原生调用，因此不会跨越进程边界影响别的 App，也不会影响系统音量。
- **不放大音量。** 增益上限是 1.0，即在播放器自身音量基础上按比例衰减，不是超频放大，因此不存在削波或损伤单元的问题。
- **未覆盖的音频路径**：运行在独立进程里的 WebKit 播放（Safari 内嵌网页、WebView 组件）、`AudioServicesPlaySystemSound` 之类由系统代理播放的音效，以及用 `dlsym` 在运行时解析 AudioToolbox 函数指针的 App（fishhook 只能重绑定符号绑定表里的那一份）。这类路径表现为面板对应计数为 0。
- **Audio Unit 缩放只认线性 PCM 的 16 位整型 / 32 位浮点 / 32 位定点**，其他格式（如 24 位 packed）会被记为「格式不支持」并原样放过，绝不猜测内存布局。
- **Audio Unit 只缩放非 0 号 bus**：RemoteIO 的 1 号元素是扬声器、0 号元素是麦克风，因此录音/连麦的采集电平保持 App 自己的值。
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

三条独立的拦截路径：

- `src/AppVolume.m`：`objc/runtime` 方法替换。为每个播放器对象维护一份 `QAVVolumePolicy`（请求音量 + 渐变包络），`volume` getter 返回请求值而非缩放后的值，因此不会产生累积衰减。用线程局部计数器 `QAVInternalVolumeWrite` / `QAVTransportDepth` 区分「App 发起的写入」和「我们自己发起的写入」，避免递归与重复缩放。原生调用一律在锁外执行，靠 revision + generation 快照重试来保证并发下的最终一致。
- `src/AudioUnitVolume.c`：给 App 自己启动的每个 Audio Unit 挂一个 render notify，只在 `kAudioUnitRenderAction_PostRender` 且 bus 号非 0 时按 gain 缩放 `AudioBufferList`。增益与格式都是原子量，实时线程不加锁、不分配内存；单元 stop/dispose 时先把槽位标记为「不缩放」再卸载通知，避免在途渲染撞上已复用的槽位。因为只绑定 bundle 内的镜像，`AVAudioEngine` 与 AudioQueue 自己在系统框架里对同一批函数的调用不会命中，所以这个缩放点和混音器、AudioQueue 两个缩放点互不重叠，不会把增益乘两次。
- `src/AudioQueueVolume.c` + `vendor/fishhook`：遍历 dyld 镜像，只对路径位于该 App bundle 内的镜像重绑定 AudioQueue 与 Audio Unit 符号。本 dylib 自身和系统框架保留原生调用，防止递归。注意 `_dyld_register_func_for_add_image` 与已加载镜像的遍历都会执行一遍：构造函数运行时 App 主二进制和它的 frameworks 早已映射完毕，只等通知会一个都扫不到。

## 适配 ManicEMU（Delta 系模拟器）

- 面板计数为 0 是预期现象，不是注入失败：DeltaCore 的 `AudioManager` 走 `AVAudioEngine` + `AVAudioSourceNode` → `AVAudioUnitTimePitch` → `mainMixerNode`，音量只体现在 `mainMixerNode.outputVolume` 上。
- 静态分析（v2.0.2 SideloadJIT，arm64 主二进制）确认导入的音频类只有 `AVAudioEngine`/`AVAudioPlayerNode`/`AVAudioSourceNode`/`AVAudioUnitTimePitch`/`AVAudioConverter`/`AVAudioPlayer`/`AVPlayer`，`__objc_methname` 里存在 `setOutputVolume:` 与 `mainMixerNode`，全 App 没有任何 `AudioQueue*` 符号 —— 42 个 `libretro.framework` 核心只导出 `retro_set_audio_sample*`，采样最终由 Swift 侧灌进引擎。
- 因此本版的混音器接管即为该 App 的缩放点；`AudioManager` 自己写的 0.0（静音/其他 App 正在播放）与 1.0 都作为「请求音量」记录，缩放后仍然保留其静音语义。

## 适配抖音（AwemeCore + RealX/byteaudio）

- 抖音 37.1.0 是解密的模块化 IPA：`Aweme` 只是 240KB 壳，真正的代码在 `Frameworks/AwemeCore.framework/AwemeCore`（687MB）。
- AwemeCore 的音频导入只有 `AVAudioSession*` 常量与 `AudioComponentFindNext/InstanceNew/InstanceDispose`，**没有** `AudioQueue*`、`AVAudioEngine`、`AVPlayer`；渲染相关的 `AudioUnit*` / `AudioOutputUnitStart` 出现在同 bundle 的 `byteaudio`、`CLMEngineKit`、`VolcEngineRTC`、`AWESearchFramework` 里。
- `byteaudio`/`CLMEngineKit`（RealX）的符号与字符串里是 `RXAudioEngineImp`、`AudioMixer`、`RXAudioVolumeController`、`AXVolumeController` 和 `playbackVolume` / `linearVolume` / `nonlinearVolume` / `volumeGain` 这些 ivar —— 音量在 ByteDance 自家软件混音器里完成，最后交给 RemoteIO。v0.2 的四个播放器类和 AudioQueue 都拦不到它，因此完全无效。
- 本版对抖音的缩放点即 Audio Unit 的输出缓冲：`AudioOutputUnitStart`（bundle 内镜像）触发挂载，render notify 在 PostRender 上按 gain 缩放非 0 号 bus 的 PCM。真机要看面板里 `AudioUnit：挂载 N 个，缩放 X 帧`：若 `缩放` 为 0 而 `输入侧帧数` 也在涨，说明该 App 的输出走 bus 0，需要按诊断调整判定；若 `格式不支持` > 0，则说明它用了本表未覆盖的采样布局。

## 限制与风险

- 仅 arm64 单架构切片，未提供 arm64e。
- 方法替换假设目标类在 iOS 15 SDK 中的签名稳定；Apple 改动内部实现时需要同步跟进。
- Audio Unit 的 render notify 跑在实时音频线程上，那里只能做原子读和一次逐样本乘法；任何新加的逻辑都要按这个约束review，否则丢帧会直接变成爆音。
- 面板依赖 `UISheetPresentationController`，在非 UIKit 场景（如纯 `AVPlayer` 后台进程、扩展）不会显示。
- 请勿把本项目的 hook 逻辑用于绕过 App 的付费、广告或内容限制。

## 许可

- 本项目源码：MIT，见 [LICENSE](LICENSE)
- `vendor/fishhook`：Facebook, Inc. 的 BSD-3-Clause 代码，保留原始版权声明
