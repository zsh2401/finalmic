# finalmic

> 把「你的麦克风」和「采集卡的声音」混成一路虚拟麦克风 —— 给抖音直播伴侣 / OBS / 腾讯会议用。
>
> 一个 macOS 上的命令行混音器，Go + CoreAudio，48 kHz / 32-bit float，带时钟漂移补偿。

```
   麦克风 ─┐
           ├─► finalmic ─► 虚拟声卡（BlackHole / WeMeet）─► 抖音直播伴侣「麦克风」
   采集卡 ─┘
```

## 为什么需要它

抖音直播伴侣一次只能选一个麦克风设备：

- 选电脑麦克风 → 采集卡里的游戏/电脑声音进不来；
- 选采集卡 → 你说话的声音进不去。

finalmic 把两路输入实时混合，写进一块**虚拟声卡**。直播伴侣看到的是一支普通麦克风，
里面同时有你的声音和游戏声音。

> macOS 不允许普通进程直接“创建”一块声卡（那需要安装系统级音频驱动，并要求管理员密码），
> 所以 finalmic 采用业界通用做法：混音写入现成的回环虚拟声卡。推荐
> [BlackHole](https://existential.audio/blackhole/)（免费、开源、采样级精确）。
> 如果你的机器上已经装了腾讯会议/企业微信，通常也会自带一块 `WeMeet Audio Device`，可以直接用。

## 特性

- **两路（或最多四路）实时混音**，每路独立增益，`--mic "设备名@-3"` 这样给单路加减 dB
- **48 kHz / 32-bit float** 全浮点管线，混音不削波；超过 -1.4 dBFS 时平滑软限幅
- **时钟漂移补偿**：两支设备的晶振不可能完全一致，finalmic 用分数重采样（Catmull-Rom）
  配合 PI 控制器把两路始终锁在 30 ms 抖动缓冲上，长跑几小时也不会爆音/断音
- **实时线程里没有 Go 代码**：采集/混音/输出全在 C 里跑，Go 只做控制面和 UI
- 实时电平表、欠载/丢弃计数、时钟漂移 ppm、运行时长
- `--record mix.wav` 可把混音结果录成 32-bit float WAV，方便试听和排查
- `finalmic doctor` 一键自检：设备、麦克风权限、每路电平、输出通路

## 环境要求

- macOS 13 或更新（Apple Silicon / Intel 均可）
- Xcode Command Line Tools：`xcode-select --install`
- Go 1.22+
- 一块回环虚拟声卡（见下）

### 安装虚拟声卡（推荐 BlackHole 2ch）

```bash
brew install --cask blackhole-2ch
```

安装过程会要求输入登录密码（安装系统音频驱动，必须的）。装完不需要重启，
`finalmic devices` 里能看到 `BlackHole 2ch` 即可。

如果机器上已经有 `WeMeet Audio Device`（腾讯会议自带）或 Loopback，也可以直接用，
`finalmic` 会自动优先选择这类回环设备。

## 快速开始

```bash
make build     # 构建当前平台的 bin/finalmic
make run       # 启动（首次运行进入配置向导）
```

首次运行会依次询问：

```
① 哪一个是你的麦克风（你说话的声音）
② 哪一个是采集卡（游戏 / 电脑的声音）
③ 混音输出到哪个设备（虚拟麦克风）      ← 选 BlackHole 2ch / WeMeet Audio Device
```

选择会保存到 `~/Library/Application Support/finalmic/config.json`，以后 `make run` 直接启动。

启动后终端会显示实时电平表：

```
  finalmic 0.1.0  ·  48.0 kHz / 立体声  ·  运行 00:12:03
  麦克风    MacBook Pro Microphone          ███████░░░░░░░   -18.2 dBFS
  采集卡    UGREEN 25173                    ████░░░░░░░░░░   -27.4 dBFS
  ────────────────────────────────────────────────────────────────────────────
  输出      BlackHole 2ch                   █████████░░░░░   -15.1 dBFS
  抖动缓冲  30.0 ms · 漂移 +3 ppm · 欠载 0 · 丢弃 0
```

然后在**抖音直播伴侣**里：设置 → 音频 → 麦克风 → 选择 `BlackHole 2ch`（或你选的虚拟声卡）。
如果直播伴侣还支持单独选择“扬声器/系统声音”，把它关掉，避免重复。

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
             --out "BlackHole 2ch" --record mix.wav
```

## 音质设计

| 环节 | 做法 |
| --- | --- |
| 采样率 | 统一 48 kHz；设备原生格式由 CoreAudio 的 AudioUnit 转换（高质量 SRC） |
| 位深 | 全程 32-bit float，混音和限幅都在浮点里做 |
| 混音 | 多路相加 + 每路平滑增益（改增益不爆音） |
| 削波保护 | -1.4 dBFS 以上用 `tanh` 软膝限幅，永远不会有数字硬削波 |
| 时钟漂移 | 每路独立分数读取指针，PI 控制器锁定 30 ms 缓冲；线性/三次插值误差在 ±1000 ppm 下可忽略 |
| 抖动缓冲 | 30 ms，输出块 512 帧；断流时自动重新预充，避免连续爆音 |
| 实时安全 | 音频回调里只用无锁环形缓冲和预分配内存，没有锁、没有 malloc、没有 Go |

## 常见问题

**选了设备但直播伴侣里没有？**
虚拟声卡要在直播伴侣启动**之前**装好，并且直播伴侣的下拉框里显示的是系统设备名。
装完 BlackHole 后重启一次直播伴侣。

**自己的声音有回声？**
说明电脑外放的声音又被麦克风收了进去。直播时戴耳机，或者在系统设置里把输出设备
从扬声器换到别处。

**提示没有麦克风权限？**
系统设置 → 隐私与安全性 → 麦克风，把运行 finalmic 的终端（Terminal / iTerm）打开。
`finalmic doctor` 会显示每一路是否真的收到了音频帧。

**采集卡那一路没声音？**
先确认采集卡在别的软件（QuickTime → 新建音频录制）里有电平。`finalmic doctor`
会给出每一路 1 秒采样的峰值。

**能混更多路吗？**
可以，最多 4 路：`--mic A --capture B --in C --in D`。

**延迟有多大？**
30 ms 抖动缓冲 + 512 帧输出块，整条链路大约 40~50 ms。对直播足够，不适合实时监听演奏。

## 项目结构

```
cmd/finalmic/          入口
internal/audio/        cgo 封装 + CoreAudio 引擎（engine.c / engine.h）
internal/cli/          run / setup / devices / doctor 子命令
internal/config/       配置持久化
internal/ui/           终端电平表
```

## 开发

```bash
make test      # go test ./...
make vet       # go vet ./...
make fmt       # gofmt
make doctor    # 自检
```

引擎是纯 C（`internal/audio/engine.c`），可以脱离 Go 单独编译调试：

```bash
clang -g -fsanitize=address -o /tmp/fmtest your_driver.c internal/audio/engine.c \
  -Iinternal/audio -framework CoreAudio -framework AudioToolbox \
  -framework AudioUnit -framework CoreFoundation
```

## License

MIT © zsh2401
