package cli

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"

	"github.com/zsh2401/finalmic/internal/audio"
	"github.com/zsh2401/finalmic/internal/config"
)

type prompter struct {
	in  *bufio.Reader
	out io.Writer
}

func newPrompter() *prompter {
	return &prompter{in: bufio.NewReader(os.Stdin), out: os.Stdout}
}

func (p *prompter) ask(label, def string) string {
	for {
		if def != "" {
			fmt.Fprintf(p.out, "%s [%s]: ", label, def)
		} else {
			fmt.Fprintf(p.out, "%s: ", label)
		}
		line, err := p.in.ReadString('\n')
		line = strings.TrimSpace(line)
		if line != "" {
			return line
		}
		if def != "" {
			return def
		}
		if err != nil {
			return ""
		}
		fmt.Fprintln(p.out, "  请输入一个值。")
	}
}

func (p *prompter) pick(label string, count, def int) int {
	for {
		v := p.ask(label, strconv.Itoa(def))
		n, err := strconv.Atoi(v)
		if err == nil && n >= 1 && n <= count {
			return n
		}
		fmt.Fprintf(p.out, "  请输入 1 - %d 之间的编号。\n", count)
	}
}

func (p *prompter) confirm(label string, def bool) bool {
	d := "y/N"
	if def {
		d = "Y/n"
	}
	v := strings.ToLower(strings.TrimSpace(p.ask(label, d)))
	if v == "" {
		return def
	}
	return strings.HasPrefix(v, "y")
}

// preferredCaptureIndex guesses which input is the capture card.
func preferredCaptureIndex(inputs []audio.Device, exclude int) int {
	best, bestScore := -1, 0
	for i, d := range inputs {
		if i == exclude {
			continue
		}
		name := strings.ToLower(d.Name)
		score := 1
		switch {
		case strings.Contains(name, "ugreen"):
			score = 100
		case strings.Contains(name, "elgato"), strings.Contains(name, "cam link"),
			strings.Contains(name, "capture"), strings.Contains(name, "采集"):
			score = 90
		case strings.Contains(name, "usb"):
			score = 60
		case d.Transport == "usb ":
			score = 50
		}
		if score > bestScore {
			best, bestScore = i, score
		}
	}
	if best < 0 && len(inputs) > 0 {
		best = 0
	}
	return best
}

func printDeviceList(devs []audio.Device, highlight func(int) bool) {
	for i, d := range devs {
		mark := " "
		if highlight != nil && highlight(i) {
			mark = "★"
		}
		fmt.Printf("  %s %2d) %s %s\n", mark, i+1, fit(d.Name, 32), describe(d))
	}
}

// runWizard interactively builds a configuration.
func runWizard() (*config.Config, error) {
	if !isTTY(os.Stdin) {
		return nil, errors.New("没有可交互的终端，请用 --mic/--capture/--out 参数指定设备")
	}
	inputs, err := audio.Inputs()
	if err != nil {
		return nil, err
	}
	outputs, err := audio.Outputs()
	if err != nil {
		return nil, err
	}
	if len(inputs) == 0 {
		return nil, errors.New("没有找到任何音频输入设备")
	}
	p := newPrompter()

	fmt.Println()
	fmt.Println("  检测到以下输入设备：")
	defaultMic := 0
	for i, d := range inputs {
		if d.IsDefaultInput {
			defaultMic = i
		}
	}
	printDeviceList(inputs, func(i int) bool { return i == defaultMic })

	fmt.Println()
	micIdx := p.pick("① 哪一个是你的麦克风（你说话的声音）", len(inputs), defaultMic+1) - 1
	mic := inputs[micIdx]

	var capture *audio.Device
	if len(inputs) > 1 {
		fmt.Println()
		def := preferredCaptureIndex(inputs, micIdx) + 1
		printDeviceList(inputs, func(i int) bool { return i == def-1 })
		fmt.Println()
		idx := p.pick("② 哪一个是采集卡（游戏 / 电脑的声音）", len(inputs), def) - 1
		c := inputs[idx]
		capture = &c
		if idx == micIdx {
			fmt.Println("  提示：麦克风和采集卡是同一个设备，可能会产生回声。")
		}
	} else {
		fmt.Println("  只检测到一个输入设备，将只混合这一路。")
	}

	fmt.Println()
	outs := sortLoopbackFirst(outputs)
	hasLoopback := false
	for _, d := range outs {
		if looksLoopback(d) {
			hasLoopback = true
			break
		}
	}
	fmt.Println("  检测到以下输出设备（虚拟麦克风要选回环声卡）：")
	printDeviceList(outs, func(i int) bool { return looksLoopback(outs[i]) })
	if !hasLoopback {
		fmt.Println()
		fmt.Println("  ⚠ 没有找到回环虚拟声卡。抖音直播伴侣只能把「真实设备」当成麦克风，")
		fmt.Println("    所以需要先安装一块虚拟声卡。finalmic 自带一个，安装一次即可：")
		fmt.Println("        sudo make install-driver")
		fmt.Println("    （需要管理员密码，装完重新运行 finalmic setup）")
		fmt.Println("    也可以改用现成的 BlackHole：brew install --cask blackhole-2ch")
	}
	fmt.Println()
	pickDef := 1
	outIdx := p.pick("③ 混音输出到哪个设备（虚拟麦克风）", len(outs), pickDef) - 1
	out := outs[outIdx]

	cfg := &config.Config{
		Version:    1,
		OutputUID:  out.UID,
		OutputName: out.Name,
		SampleRate: 48000,
		TargetMS:   30,
	}
	cfg.Sources = append(cfg.Sources, config.Source{
		UID: mic.UID, Name: mic.Name, Role: config.RoleMic, Highpass: true,
	})
	if capture != nil {
		cfg.Sources = append(cfg.Sources, config.Source{
			UID: capture.UID, Name: capture.Name, Role: config.RoleCapture, Highpass: true,
		})
	}

	fmt.Println()
	if looksLoopback(out) {
		fmt.Printf("  ✓ 混音会写入 “%s”，在抖音直播伴侣里把它选成麦克风即可。\n", out.Name)
	} else {
		fmt.Printf("  ⚠ “%s” 不是回环设备，混音只会播放到它，直播软件拿不到。\n", out.Name)
	}
	return cfg, nil
}

func cmdSetup(args []string) int {
	cfg, err := runWizard()
	if err != nil {
		fmt.Fprintf(os.Stderr, "配置失败: %v\n", err)
		return 1
	}
	if err := config.Save(cfg); err != nil {
		fmt.Fprintf(os.Stderr, "保存配置失败: %v\n", err)
		return 1
	}
	path, _ := config.Path()
	fmt.Printf("  ✓ 配置已保存: %s\n\n", path)
	fmt.Println("  直接运行 `make run`（或 `finalmic`）开始混音。")
	return 0
}

func cmdDevices(args []string) int {
	all, err := audio.Devices()
	if err != nil {
		fmt.Fprintf(os.Stderr, "枚举设备失败: %v\n", err)
		return 1
	}
	fmt.Printf("\n  finalmic %s · 共 %d 个音频设备\n\n", Version, len(all))
	for _, d := range all {
		mark := " "
		if looksLoopback(d) {
			mark = "★"
		}
		fmt.Printf("  %s %2d) %s %s\n", mark, d.Index+1, fit(d.Name, 32), describe(d))
	}
	fmt.Println()
	fmt.Println("  ★ = 回环虚拟声卡，可以用作抖音直播伴侣的麦克风")
	fmt.Println("  UID:")
	for _, d := range all {
		if looksLoopback(d) {
			fmt.Printf("    %-32s %s\n", fit(d.Name, 32), d.UID)
		}
	}
	fmt.Println()
	return 0
}

func isTTY(f *os.File) bool {
	st, err := f.Stat()
	if err != nil {
		return false
	}
	return st.Mode()&os.ModeCharDevice != 0
}
