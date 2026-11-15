package cli

import (
	"fmt"
	"os"
	"time"

	"github.com/zsh2401/finalmic/internal/audio"
	"github.com/zsh2401/finalmic/internal/config"
)

func cmdDoctor(args []string) int {
	fmt.Printf("\n  finalmic %s · 自检\n\n", Version)

	all, err := audio.Devices()
	if err != nil {
		fmt.Fprintf(os.Stderr, "  ✗ 枚举设备失败: %v\n\n", err)
		return 1
	}
	fmt.Printf("  系统设备    %d 个音频设备\n", len(all))

	cfg, cfgErr := config.Load()
	var sources []config.Source
	if cfgErr == nil && len(cfg.Sources) > 0 {
		sources = cfg.Sources
		fmt.Printf("  配置文件    %s\n", mustPath())
	} else {
		fmt.Printf("  配置文件    未配置（只检查全部输入设备）\n")
		for _, d := range all {
			if d.HasInput() {
				sources = append(sources, config.Source{UID: d.UID, Name: d.Name, Role: config.RoleOther})
			}
		}
	}
	fmt.Println()

	fail := 0
	for i, s := range sources {
		d, err := audio.ResolveInput(s.UID)
		if err != nil {
			d, err = audio.ResolveInput(s.Name)
		}
		if err != nil {
			fmt.Printf("  输入 %d      %s  ✗ 设备不在了\n", i+1, s.Name)
			fail++
			continue
		}
		role := config.RoleLabel(s.Role)
		if s.Role == config.RoleOther {
			role = "输入"
		}
		fmt.Printf("  %s %s  %s\n", fit(role, 8), fit(d.Name, 30), describe(d))
		peak, rms, frames, err := audio.Probe(d, 1000)
		if err != nil {
			fmt.Printf("              ✗ 采集失败: %v\n", err)
			fail++
			continue
		}
		verdict := "✓ 有信号"
		if frames == 0 {
			verdict = "✗ 没有收到音频帧（可能是麦克风权限被拒绝）"
			fail++
		} else if peak <= -85 {
			verdict = "⚠ 没有信号（检查设备是否静音 / 是否接通）"
		}
		fmt.Printf("              1 秒采样 %d 帧 · 峰值 %.1f dBFS · RMS %.1f dBFS  %s\n",
			frames, peak, rms, verdict)
	}

	fmt.Println()
	var out audio.Device
	if cfgErr == nil {
		out, err = audio.ResolveOutput(cfg.OutputUID)
		if err != nil {
			out, err = audio.ResolveOutput(cfg.OutputName)
		}
	}
	if err == nil {
		label := "✓ 回环虚拟声卡，可以给直播软件当麦克风"
		if !looksLoopback(out) {
			label = "⚠ 不是回环设备，直播软件不会把它当成麦克风"
			fail++
		}
		fmt.Printf("  输出      %s  %s\n              %s\n", fit(out.Name, 30), describe(out), label)
	} else {
		fmt.Printf("  输出      未配置（运行 finalmic setup）\n")
	}

	if cfgErr == nil && len(cfg.Sources) > 0 {
		fmt.Println()
		fmt.Println("  正在试运行 3 秒，验证完整通路…")
		ac, _, err := resolveConfig(cfg)
		if err != nil {
			fmt.Printf("  ✗ %v\n\n", err)
			return 1
		}
		eng, err := audio.NewEngine(ac)
		if err != nil {
			fmt.Printf("  ✗ 打开设备失败: %v\n\n", err)
			return 1
		}
		defer eng.Close()
		if err := eng.Start(); err != nil {
			fmt.Printf("  ✗ 启动失败: %v\n\n", err)
			return 1
		}
		time.Sleep(3 * time.Second)
		st := eng.Stats()
		eng.Stop()
		fmt.Printf("  输出峰值  %.1f dBFS", st.OutPeakDB)
		if st.OutPeakDB <= -85 {
			fmt.Println("  ⚠ 输出是静音，检查输入电平和音量")
		} else {
			fmt.Println("  ✓ 混音通路正常")
		}
	}

	fmt.Println()
	if fail == 0 {
		fmt.Println("  结论：一切正常。运行 `make run` 开始混音，然后在抖音直播伴侣里选择输出设备。")
	} else {
		fmt.Printf("  结论：发现 %d 个问题，见上面带 ✗ / ⚠ 的行。\n", fail)
	}
	fmt.Println()
	return 0
}

func mustPath() string {
	p, err := config.Path()
	if err != nil {
		return "(未知)"
	}
	return p
}
