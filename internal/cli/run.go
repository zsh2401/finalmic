package cli

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/zsh2401/finalmic/internal/audio"
	"github.com/zsh2401/finalmic/internal/config"
	"github.com/zsh2401/finalmic/internal/ui"
)

type multiFlag []string

func (m *multiFlag) String() string { return strings.Join(*m, ",") }
func (m *multiFlag) Set(v string) error {
	*m = append(*m, v)
	return nil
}

type runOptions struct {
	mic      string
	capture  string
	inputs   multiFlag
	out      string
	record   string
	targetMS float64
	noMeter  bool
	reset    bool
	duration time.Duration
}

func parseRunFlags(args []string) (*runOptions, error) {
	o := &runOptions{targetMS: 30}
	fs := flag.NewFlagSet("finalmic run", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	fs.StringVar(&o.mic, "mic", "", "说话用的麦克风")
	fs.StringVar(&o.capture, "capture", "", "采集卡输入")
	fs.Var(&o.inputs, "in", "额外输入，可重复")
	fs.StringVar(&o.out, "out", "", "虚拟麦克风输出设备")
	fs.StringVar(&o.record, "record", "", "同时录制为 WAV")
	fs.Float64Var(&o.targetMS, "target-ms", 30, "抖动缓冲毫秒")
	fs.BoolVar(&o.noMeter, "no-meter", false, "不显示电平表")
	fs.BoolVar(&o.reset, "reset", false, "忽略已保存配置")
	fs.DurationVar(&o.duration, "duration", 0, "运行一段时间后自动停止，例如 30s")
	fs.Usage = usage
	if err := fs.Parse(args); err != nil {
		return nil, err
	}
	if fs.NArg() > 0 {
		return nil, fmt.Errorf("多余的参数: %s", strings.Join(fs.Args(), " "))
	}
	return o, nil
}

// parseInputSpec understands "name", "name@-6" and "name@-6dB".
func parseInputSpec(spec string) (string, float64, bool) {
	i := strings.LastIndex(spec, "@")
	if i > 0 && i < len(spec)-1 {
		raw := strings.TrimSuffix(strings.TrimSuffix(spec[i+1:], "dB"), "db")
		if g, err := strconv.ParseFloat(raw, 64); err == nil {
			return strings.TrimSpace(spec[:i]), g, true
		}
	}
	return strings.TrimSpace(spec), 0, false
}

// autoOutput finds a loopback device to render into.
func autoOutput() (audio.Device, error) {
	outs, err := audio.Outputs()
	if err != nil {
		return audio.Device{}, err
	}
	for _, d := range sortLoopbackFirst(outs) {
		if looksLoopback(d) {
			return d, nil
		}
	}
	return audio.Device{}, errors.New(
		"没有找到回环虚拟声卡；请安装 BlackHole 2ch（brew install --cask blackhole-2ch）" +
			"或用 --out 指定一个虚拟声卡")
}

func planFromFlags(o *runOptions) (*config.Config, error) {
	type item struct {
		spec string
		role string
	}
	var items []item
	if o.mic != "" {
		items = append(items, item{o.mic, config.RoleMic})
	}
	if o.capture != "" {
		items = append(items, item{o.capture, config.RoleCapture})
	}
	for _, s := range o.inputs {
		items = append(items, item{s, config.RoleOther})
	}
	if len(items) == 0 && o.out == "" {
		return nil, nil
	}
	if len(items) == 0 {
		return nil, errors.New("用 --mic / --capture 至少指定一路输入")
	}

	cfg := &config.Config{Version: 1, SampleRate: 48000, TargetMS: o.targetMS}
	for _, it := range items {
		name, gain, _ := parseInputSpec(it.spec)
		d, err := audio.ResolveInput(name)
		if err != nil {
			return nil, err
		}
		cfg.Sources = append(cfg.Sources, config.Source{
			UID: d.UID, Name: d.Name, GainDB: gain, Role: it.role, Highpass: true,
		})
	}
	if o.out != "" {
		d, err := audio.ResolveOutput(o.out)
		if err != nil {
			return nil, err
		}
		cfg.OutputUID, cfg.OutputName = d.UID, d.Name
	} else {
		d, err := autoOutput()
		if err != nil {
			return nil, err
		}
		cfg.OutputUID, cfg.OutputName = d.UID, d.Name
	}
	return cfg, nil
}

// resolveConfig maps names/UIDs in the config to live devices.
func resolveConfig(cfg *config.Config) (audio.Config, []audio.Device, error) {
	var ac audio.Config
	var devs []audio.Device
	for i := range cfg.Sources {
		s := &cfg.Sources[i]
		d, err := audio.ResolveInput(s.UID)
		if err != nil {
			d, err = audio.ResolveInput(s.Name)
		}
		if err != nil {
			return ac, nil, fmt.Errorf("输入设备 %q 不可用: %w", s.Name, err)
		}
		s.UID, s.Name = d.UID, d.Name
		devs = append(devs, d)
		ac.Sources = append(ac.Sources, audio.Source{Device: d, GainDB: s.GainDB, Highpass: s.Highpass})
	}
	out, err := audio.ResolveOutput(cfg.OutputUID)
	if err != nil {
		out, err = audio.ResolveOutput(cfg.OutputName)
	}
	if err != nil {
		return ac, nil, fmt.Errorf("输出设备 %q 不可用: %w", cfg.OutputName, err)
	}
	cfg.OutputUID, cfg.OutputName = out.UID, out.Name
	devs = append(devs, out)
	ac.Output = out
	ac.SampleRate = cfg.SampleRate
	ac.TargetMS = cfg.TargetMS
	return ac, devs, nil
}

func choosePlan(o *runOptions) (*config.Config, error) {
	fromFlags, err := planFromFlags(o)
	if err != nil {
		return nil, err
	}
	if fromFlags != nil {
		return fromFlags, nil
	}
	if o.reset {
		return runWizard()
	}
	cfg, err := config.Load()
	if err != nil {
		if !errors.Is(err, os.ErrNotExist) {
			fmt.Fprintf(os.Stderr, "  (读取配置失败: %v，重新配置)\n", err)
		}
		return runWizard()
	}
	preferOwnDevice(cfg)
	return cfg, nil
}

// preferOwnDevice switches a saved configuration over to finalmic's own
// virtual sound card once the driver is installed. Only virtual loopbacks are
// replaced, never a real speaker or interface the user picked on purpose.
func preferOwnDevice(cfg *config.Config) {
	own, ok := findOwnDevice()
	if !ok || cfg.OutputUID == own.UID {
		return
	}
	cur, err := audio.ResolveOutput(cfg.OutputUID)
	if err != nil {
		cur, err = audio.ResolveOutput(cfg.OutputName)
	}
	if err != nil || !looksLoopback(cur) || isOwnDevice(cur) {
		return
	}
	fmt.Printf("  检测到 finalmic 虚拟声卡，输出已从 “%s” 切换到 “%s”\n", cur.Name, own.Name)
	cfg.OutputUID, cfg.OutputName = own.UID, own.Name
	if err := config.Save(cfg); err != nil {
		fmt.Fprintf(os.Stderr, "  (配置未保存: %v)\n", err)
	}
}

func findOwnDevice() (audio.Device, bool) {
	outs, err := audio.Outputs()
	if err != nil {
		return audio.Device{}, false
	}
	for _, d := range outs {
		if isOwnDevice(d) {
			return d, true
		}
	}
	return audio.Device{}, false
}

func cmdRun(args []string) int {
	o, err := parseRunFlags(args)
	if err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	cfg, err := choosePlan(o)
	if err != nil {
		fmt.Fprintf(os.Stderr, "\n  ✗ %v\n\n", err)
		return 1
	}

	ac, devs, err := resolveConfig(cfg)
	if err != nil {
		if isTTY(os.Stdin) {
			fmt.Fprintf(os.Stderr, "\n  %v\n  重新选择设备…\n", err)
			cfg, err = runWizard()
			if err == nil {
				ac, devs, err = resolveConfig(cfg)
				if err == nil {
					config.Save(cfg)
				}
			}
		}
		if err != nil {
			fmt.Fprintf(os.Stderr, "\n  ✗ %v\n\n", err)
			return 1
		}
	}
	if cfg.TargetMS <= 0 {
		cfg.TargetMS = o.targetMS
		ac.TargetMS = o.targetMS
	}

	eng, err := audio.NewEngine(ac)
	if err != nil {
		fmt.Fprintf(os.Stderr, "\n  ✗ 打开音频设备失败: %v\n\n", err)
		return 1
	}
	defer eng.Close()

	if err := eng.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "\n  ✗ 启动音频失败: %v\n\n", err)
		return 1
	}
	if o.record != "" {
		if err := eng.StartRecording(o.record); err != nil {
			fmt.Fprintf(os.Stderr, "  ⚠ 无法开始录音: %v\n", err)
		} else {
			fmt.Printf("  ● 正在录音 → %s\n", o.record)
		}
	}

	printStartup(cfg, ac, o)

	started := time.Now()
	st, total := runMeterLoop(eng, cfg, ac, o, started, devs)
	if total == 0 {
		total = st.Frames
	}
	eng.StopRecording()
	eng.Stop()

	elapsed := time.Since(started)
	fmt.Printf("\n  ✓ 已停止 · 运行 %s · 共混音 %s 帧\n", fmtDuration(elapsed), comma(total))
	if st.XRuns != nil {
		var xr, dr int64
		for i := range st.XRuns {
			xr += st.XRuns[i]
			dr += st.Drops[i]
		}
		if xr > 0 || dr > 0 {
			fmt.Printf("    欠载 %d 次 · 丢弃 %d 次\n", xr, dr)
		}
	}
	if o.record != "" {
		if fi, err := os.Stat(o.record); err == nil {
			fmt.Printf("    录音已保存: %s (%.1f MB)\n", o.record, float64(fi.Size())/1024/1024)
		}
	}
	fmt.Println()
	return 0
}

func printStartup(cfg *config.Config, ac audio.Config, o *runOptions) {
	fmt.Printf("\n  finalmic %s · %.1f kHz · 立体声 · 抖动缓冲 %.0f ms\n\n",
		Version, ac.SampleRate/1000, ac.TargetMS)
	for i, s := range cfg.Sources {
		fmt.Printf("  %s  %s  %s\n",
			fit(config.RoleLabel(s.Role), 6), fit(s.Name, 30),
			describe(ac.Sources[i].Device))
	}
	fmt.Printf("  %s  %s  %s\n", fit("输出", 6), fit(ac.Output.Name, 30), describe(ac.Output))
	fmt.Println()
	if looksLoopback(ac.Output) {
		fmt.Printf("  → 打开抖音直播伴侣，把「麦克风」选成 “%s”\n", ac.Output.Name)
	} else {
		fmt.Printf("  ⚠ “%s” 不是回环设备，直播软件无法把它当成麦克风。\n", ac.Output.Name)
	}
	fmt.Println("  → Ctrl-C 停止")
	fmt.Println()
}

func runMeterLoop(eng *audio.Engine, cfg *config.Config, ac audio.Config, o *runOptions,
	started time.Time, devs []audio.Device) (audio.Stats, int64) {

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if o.duration > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, o.duration)
		defer cancel()
	}

	tty := ui.IsTTY(os.Stdout)
	renderer := ui.NewRenderer(os.Stdout, tty)
	if o.noMeter || !tty {
		fmt.Println("  混音运行中…（Ctrl-C 停止）")
	}

	tick := time.NewTicker(100 * time.Millisecond)
	defer tick.Stop()
	lastCheck := time.Now()
	var last audio.Stats
	var total int64

	for {
		select {
		case <-ctx.Done():
			renderer.Finish()
			return last, total
		case now := <-tick.C:
			st := eng.Stats()
			last = st
			total += st.Frames
			if tty && !o.noMeter {
				renderer.Draw(meterLines(renderer, st, cfg, ac, started, o.record))
			}
			if now.Sub(lastCheck) >= time.Second {
				lastCheck = now
				if bad := firstDeadDevice(devs); bad != nil {
					renderer.Finish()
					fmt.Printf("\n  ✗ 设备已断开: %s\n  请重新连接后运行 finalmic setup\n\n", bad.Name)
					return st, total
				}
			}
		}
	}
}

func firstDeadDevice(devs []audio.Device) *audio.Device {
	for i := range devs {
		if !audio.DeviceAlive(devs[i]) {
			return &devs[i]
		}
	}
	return nil
}

func meterLines(r *ui.Renderer, st audio.Stats, cfg *config.Config, ac audio.Config,
	started time.Time, recPath string) []string {

	lines := []string{
		fmt.Sprintf("  %s %s  ·  %.1f kHz / 立体声  ·  %s  %s",
			r.Title("finalmic"), r.Note(Version), ac.SampleRate/1000,
			r.Note("运行"), fmtDuration(time.Since(started))),
	}
	for i, s := range cfg.Sources {
		if i >= len(st.PeakDB) {
			break
		}
		bar := r.Bar(st.PeakDB[i], 14)
		state := ""
		if !st.Primed[i] {
			state = r.Note("  缓冲中…")
		}
		gain := ""
		if s.GainDB != 0 {
			gain = fmt.Sprintf("  %+.1f dB", s.GainDB)
		}
		lines = append(lines, fmt.Sprintf("  %s  %s  %s  %s%s%s",
			r.Label(ui.Pad(config.RoleLabel(s.Role), 8)),
			ui.Pad(ui.Short(s.Name, 30), 30),
			bar, ui.DB(st.PeakDB[i]), r.Note(gain), state))
	}
	lines = append(lines, r.Line(76))
	lines = append(lines, fmt.Sprintf("  %s  %s  %s  %s",
		r.Label(ui.Pad("输出", 8)), ui.Pad(ui.Short(ac.Output.Name, 30), 30),
		r.Bar(st.OutPeakDB, 14), ui.DB(st.OutPeakDB)))

	var xr, dr int64
	buf := 0.0
	drift := 0.0
	for i := range st.XRuns {
		xr += st.XRuns[i]
		dr += st.Drops[i]
		buf += st.FillMS[i]
		drift += st.DriftPPM[i]
	}
	if n := float64(len(st.XRuns)); n > 0 {
		buf /= n
		drift /= n
	}
	if time.Since(started) > 3*time.Second && !anyPrimed(st) {
		lines = append(lines, r.Warn("  ⚠ 3 秒内没有收到音频数据：检查麦克风权限，或设备是否被静音"))
	} else {
		lines = append(lines, fmt.Sprintf("  %s  %.1f ms · %s %+.0f ppm · %s %d · %s %d",
			r.Note("抖动缓冲"), buf, r.Note("漂移"), drift,
			r.Note("欠载"), xr, r.Note("丢弃"), dr))
	}

	if recPath != "" {
		lines = append(lines, fmt.Sprintf("  %s  %s", r.Note("录音"), recPath))
	}
	return lines
}

func anyPrimed(st audio.Stats) bool {
	for _, p := range st.Primed {
		if p {
			return true
		}
	}
	return false
}

func fmtDuration(d time.Duration) string {
	d = d.Round(time.Second)
	h := int(d.Hours())
	m := int(d.Minutes()) % 60
	s := int(d.Seconds()) % 60
	if h > 0 {
		return fmt.Sprintf("%02d:%02d:%02d", h, m, s)
	}
	return fmt.Sprintf("%02d:%02d", m, s)
}

func comma(n int64) string {
	s := strconv.FormatInt(n, 10)
	var b strings.Builder
	for i, c := range s {
		if i > 0 && (len(s)-i)%3 == 0 {
			b.WriteByte(',')
		}
		b.WriteRune(c)
	}
	return b.String()
}
