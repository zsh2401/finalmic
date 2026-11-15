# finalmic

[![ci](https://github.com/zsh2401/finalmic/actions/workflows/ci.yml/badge.svg)](https://github.com/zsh2401/finalmic/workflows/ci.yml)

> 把「你的麦克风」和「采集卡的声音」混成一路虚拟麦克风 —— 给抖音直播伴侣 / OBS / 腾讯会议用。
>
> macOS 命令行混音器，自带一块叫 **finalmic** 的虚拟声卡，Go + CoreAudio，48 kHz / 32-bit float。

```
   麦克风 ─┐
           ├─► finalmic（混音器）─► finalmic（虚拟声卡）─► 抖音直播伴侣「麦克风」
   采集卡 ─┘
```

## 为什么需要它

抖音直播伴侣一次只能选一个麦克风设备：

- 选电脑麦克风 → 采集卡里的游戏/电脑声音进不来；
- 选采集卡 → 你说话的声音进不去。

finalmic 把两路输入实时混合，写进一块**真正由它自己的**虚拟声卡。直播伴侣看到的是一支名叫
`finalmic` 的普通麦克风，里面同时有你的声音和游戏声音。

## 特性

- **自带虚拟声卡**：`finalmic.driver` 是一个 AudioServerPlugIn，装上后系统里多出一块
  `finalmic` 声卡（2 进 2 出，48 kHz），不依赖 BlackHole、WeMeet 等第三方设备
- **两路（或最多四路）实时混音**，每路独立增益：`--mic "设备名@-3"`
- **48 kHz / 32-bit float** 全浮点管线；超过 -1.4 dBFS 时平滑软限幅，不会数字硬削波
- **时钟漂移补偿**：两支设备的晶振不可能完全一致，finalmic 用 Catmull-Rom 分数重采样 +
  PI 控制器把每路锁在 30 ms 抖动缓冲上，连续跑几小时也不爆音、不断音
- **实时线程里没有 Go 代码**：采集/混音/输出全在 C 里，Go 只做控制面和 UI
- 实时电平表、欠载/丢弃计数、漂移 ppm、运行时长
- `--record mix.wav` 可把混音录成 32-bit float WAV，方便试听和排查
- `finalmic doctor` 一键自检：设备、麦克风权限、每路电平、输出通路

## 环境要求

- macOS 13 或更新（Apple Silicon / Intel 均可）
- Xcode Command Line Tools：`xcode-select --install`
- Go 1.22+
- 安装自带驱动需要**一次管理员密码**

## 快速开始

```bash
make build                 # 构建 bin/finalmic 和虚拟声卡驱动
sudo make install-driver   # 安装 finalmic.driver（只需一次，会要密码）
make run                   # 启动（首次运行进入配置向导）
```

`make install-driver` 做三件事：把驱动拷到 `/Library/Audio/Plug-Ins/HAL/finalmic.driver`、
设置 root 权限、重启 coreaudiod。之后系统里就有 `finalmic` 这块声卡了。

首次运行会依次询问：

```
① 哪一个是你的麦克风（你说话的声音）
② 哪一个是采集卡（游戏 / 电脑的声音）
③ 混音输出到哪个设备（虚拟麦克风）      ← 选 finalmic
```

选择会保存到 `~/Library/Application Support/finalmic/config.json`，以后 `make run` 直接启动。

启动后终端显示实时电平表：

```
  finalmic 0.1.0  ·  48.0 kHz / 立体声  ·  运行 00:12:03
  麦克风    MacBook Pro Microphone          ███████░░░░░░░   -18.2 dBFS
  采集卡    UGREEN 25173                    ████░░░░░░░░░░   -27.4 dBFS
  ────────────────────────────────────────────────────────────────────────────
  输出      finalmic                        █████████░░░░░   -15.1 dBFS
  抖动缓冲  30.0 ms · 漂移 +3 ppm · 欠载 0 · 丢弃 0
```

然后在**抖音直播伴侣**里：设置 → 音频 → 麦克风 → 选择 `finalmic`。

### 不想装驱动？

`finalmic` 可以写入任何回环声卡。如果机器上已经有 BlackHole 2ch 或 WeMeet Audio Device，
`make setup` 时直接选它们也能用，无需管理员权限：

```bash
brew install --cask blackhole-2ch    # 一个替代方案，需要密码
```

## 虚拟声卡驱动

`driver/` 是一个最小的 [AudioServerPlugIn](https://developer.apple.com/documentation/coreaudio/audioserverplugin)：

- 一块 2 进 / 2 出的设备，名字 `finalmic`，UID `com.zsh2401.finalmic.device`
- 回环：输出侧写入环形缓冲，输入侧原样读回（预充 2 个缓冲，约 21 ms）
- 采样率固定 48 kHz，虚拟格式 32-bit float / 2ch
- 实时 IO 只碰预分配的环形缓冲，无锁、无 malloc、无 CoreFoundation
- 离线测试不需要安装：`make test-driver`

安装位置：`/Library/Audio/Plug-Ins/HAL/finalmic.driver`。卸载：`sudo make uninstall-driver`。

> 驱动是 ad-hoc 签名的本地构建。如果系统在第一次安装后弹窗拦截，去
> 「系统设置 → 隐私与安全性」点「仍要打开」即可。

## 命令行

```
finalmic                         首次运行进入向导，之后直接启动
finalmic run [参数]               启动混音
finalmic devices                 列出所有音频设备
finalmic setup                   重新配置设备
finalmic doctor                  自检
finalmic version                 版本
```

`run` 的常用参数：

| 参数 | 说明 |
| --- | --- |
| `--mic NAME[@dB]` | 说话用的麦克风 |
| `--capture NAME[@dB]` | 采集卡输入 |
| `--in NAME[@dB]` | 额外再混一路，可重复（最多 4 路） |
| `--out NAME` | 输出到哪块虚拟声卡，默认自动挑回环设备 |
| `--record FILE.wav` | 同时录制 32-bit float WAV |
| `--target-ms N` | 抖动缓冲目标，默认 30 ms |
| `--no-meter` | 不显示实时电平表 |
| `--duration 30s` | 跑一段时间后自动停止（方便录测试片段） |
| `--reset` | 忽略已保存配置，重新进入向导 |

设备名可以写 UID、全名、或唯一的一段子串：

```bash
finalmic run --mic "MacBook Pro Microphone" --capture "UGREEN" \
             --out "finalmic" --record mix.wav
```

## 音质设计

| 环节 | 做法 |
| --- | --- |
| 采样率 | 统一 48 kHz；设备原生格式由 CoreAudio 的 AudioUnit 转换（高质量 SRC） |
| 位深 | 全程 32-bit float，混音和限幅都在浮点里做 |
| 混音 | 多路相加 + 每路平滑增益（改增益不爆音） |
| 削波保护 | -1.4 dBFS 以上用 `tanh` 软膝限幅，永远不会有数字硬削波 |
| 时钟漂移 | 每路独立分数读取指针，PI 控制器锁定 30 ms 缓冲；±1000 ppm 内插值误差可忽略 |
| 抖动缓冲 | 30 ms，输出块 512 帧；断流自动重新预充，避免连续爆音 |
| 实时安全 | 音频回调里只用无锁环形缓冲和预分配内存，没有锁、没有 malloc、没有 Go |

整条链路额外延迟约 50~60 ms（30 ms 抖动缓冲 + 驱动 21 ms 预充 + 输出块），直播足够，
不适合实时监听演奏。

## 常见问题

**`finalmic` 设备没出现？**
先 `make devices` 看列表；没有的话执行 `sudo make uninstall-driver && sudo make install-driver`。
仍没有就看 `system_profiler SPAudioDataType`，并确认 `/Library/Audio/Plug-Ins/HAL/finalmic.driver`
的属主是 `root:wheel`。coreaudiod 也可以手动重启：`sudo killall -9 coreaudiod`。

**直播伴侣里没有 `finalmic`？**
驱动装好之后重启一次直播伴侣，它只在启动时枚举设备。

**自己的声音有回声？**
说明电脑外放的声音又被麦克风收了进去。直播时戴耳机，或把系统输出换到别的设备。

**提示没有麦克风权限？**
系统设置 → 隐私与安全性 → 麦克风，把运行 finalmic 的终端（Terminal / iTerm）打开。
`finalmic doctor` 会显示每一路是否真的收到了音频帧。

**采集卡那一路没声音？**
先确认采集卡在别的软件（QuickTime → 新建音频录制）里有电平。`finalmic doctor`
会给出每一路 1 秒采样的峰值。

**能混更多路吗？**
可以，最多 4 路：`--mic A --capture B --in C --in D`。

**为什么输入音量/静音控制没有？**
虚拟声卡本身不提供硬件音量控制，请在直播软件端或 finalmic 的 `@-6dB` 增益里调。

## 项目结构

```
cmd/finalmic/          入口
driver/                AudioServerPlugIn 虚拟声卡（C）+ 离线测试
internal/audio/        cgo 封装 + CoreAudio 混音引擎
internal/cli/          run / setup / devices / doctor 子命令
internal/config/       配置持久化
internal/ui/           终端电平表
```

## 开发

```bash
make test          # go test ./...
make test-driver   # 驱动离线测试（模拟 HAL host，不需要安装）
make vet           # go vet ./...
make fmt           # gofmt
make doctor        # 自检
```

混音引擎是纯 C（`internal/audio/engine.c`），可以脱离 Go 单独编译调试：

```bash
clang -g -fsanitize=address -o /tmp/fmtest your_driver.c internal/audio/engine.c \
  -Iinternal/audio -framework CoreAudio -framework AudioToolbox \
  -framework AudioUnit -framework CoreFoundation
```

## License

MIT © zsh2401
