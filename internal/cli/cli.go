// Package cli implements the finalmic command line interface.
package cli

import (
	"fmt"
	"os"
	"sort"
	"strings"

	"github.com/zsh2401/finalmic/internal/audio"
	"github.com/zsh2401/finalmic/internal/ui"
)

// Version is stamped at link time.
var Version = "dev"

const banner = `  ███████╗██╗███╗   ██╗ █████╗ ██╗     ███╗   ███╗██╗ ██████╗██████╗  ██████╗
  ██╔════╝██║████╗  ██║██╔══██╗██║     ████╗ ████║██║██╔════╝██╔══██╗██╔═══██╗
  █████╗  ██║██╔██╗ ██║███████║██║     ██╔████╔██║██║██║     ██████╔╝██║   ██║
  ██╔══╝  ██║██║╚██╗██║██╔══██║██║     ██║╚██╔╝██║██║██║     ██╔══██╗██║   ██║
  ██║     ██║██║ ╚████║██║  ██║███████╗██║ ╚═╝ ██║██║╚██████╗██║  ██║╚██████╔╝
  ╚═╝     ╚═╝╚═╝  ╚═══╝╚═╝  ╚═╝╚══════╝╚═╝     ╚═╝╚═╝ ╚═════╝╚═╝  ╚═╝ ╚═════╝`

// Main dispatches subcommands.
func Main(args []string) int {
	if len(args) == 0 {
		return cmdRun(nil)
	}
	switch args[0] {
	case "run", "start":
		return cmdRun(args[1:])
	case "devices", "ls", "list":
		return cmdDevices(args[1:])
	case "setup", "init", "wizard":
		return cmdSetup(args[1:])
	case "doctor", "check":
		return cmdDoctor(args[1:])
	case "version", "--version", "-v":
		fmt.Printf("finalmic %s\n", Version)
		return 0
	case "help", "-h", "--help":
		usage()
		return 0
	default:
		if strings.HasPrefix(args[0], "-") {
			return cmdRun(args)
		}
		fmt.Fprintf(os.Stderr, "未知命令: %s\n\n", args[0])
		usage()
		return 2
	}
}

func usage() {
	fmt.Print(banner)
	fmt.Printf("\n\n  finalmic %s — 把麦克风和采集卡混成一路虚拟麦克风\n\n", Version)
	fmt.Print(`  用法:
    finalmic                     首次运行进入配置向导，之后直接启动
    finalmic run [参数]          启动混音
    finalmic devices             列出所有音频设备
    finalmic setup               重新配置设备
    finalmic doctor              检查设备、权限和电平
    finalmic version             显示版本

  run 参数:
    --mic NAME[@dB]                说话用的麦克风（默认读取配置文件）
    --capture NAME[@dB]            采集卡输入（默认读取配置文件）
    --in NAME[@dB]                 额外再接入一路输入，可重复
    --out NAME                     输出到哪个虚拟声卡（默认自动挑回环设备）
    --record FILE.wav              把混音结果同时录成 32-bit float WAV
    --target-ms N                  抖动缓冲目标，默认 30 毫秒
    --no-meter                     不显示实时电平表
    --reset                        忽略已保存的配置，重新进入向导

  例子:
    finalmic run --mic "MacBook Pro Microphone" --capture "UGREEN 25173" \
                   --out "BlackHole 2ch" --record mix.wav
`)
}

// loopbackKeywords mark devices that can be used as a virtual microphone.
var loopbackKeywords = []string{
	"blackhole", "loopback", "wemeet", "soundflower", "existential",
	"vb-cable", "virtual audio",
}

// looksLoopback reports whether d is very likely a loopback virtual device.
func looksLoopback(d audio.Device) bool {
	if !d.HasOutput() || !d.HasInput() {
		return false
	}
	name := strings.ToLower(d.Name)
	for _, k := range loopbackKeywords {
		if strings.Contains(name, k) {
			return true
		}
	}
	return d.Virtual()
}

// sortLoopbackFirst moves loopback candidates to the front, preserving order.
func sortLoopbackFirst(devs []audio.Device) []audio.Device {
	out := append([]audio.Device(nil), devs...)
	sort.SliceStable(out, func(i, j int) bool {
		return looksLoopback(out[i]) && !looksLoopback(out[j])
	})
	return out
}

// fit truncates and pads s to exactly n display cells for table output.
func fit(s string, n int) string {
	out := ui.Short(s, n)
	w := 0
	for _, r := range out {
		w += ui.RuneWidth(r)
	}
	if w < n {
		out += strings.Repeat(" ", n-w)
	}
	return out
}

func describe(d audio.Device) string {
	parts := []string{d.TransportLabel()}
	if d.HasInput() {
		parts = append(parts, fmt.Sprintf("%d ch 输入", d.InChannels))
	}
	if d.HasOutput() {
		parts = append(parts, fmt.Sprintf("%d ch 输出", d.OutChannels))
	}
	if d.SampleRate > 0 {
		parts = append(parts, fmt.Sprintf("%.0f Hz", d.SampleRate))
	}
	if looksLoopback(d) {
		parts = append(parts, "回环虚拟声卡")
	}
	if d.IsDefaultInput {
		parts = append(parts, "默认输入")
	}
	if d.IsDefaultOutput {
		parts = append(parts, "默认输出")
	}
	return strings.Join(parts, " · ")
}
