package cli

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
	"syscall"
	"time"
	"unicode"

	"github.com/nvmms/sdk_version_manager/internal/sdk"
	"github.com/nvmms/sdk_version_manager/internal/terminal"
)

// browserModel contains selection state only; rendering and SDK operations are separate.
type browserModel struct {
	versions      []sdk.Version
	selected, top int
	status        string
	busy          bool
}

func (b *browserModel) move(delta, rows int) {
	if len(b.versions) == 0 {
		return
	}
	b.selected = max(0, min(len(b.versions)-1, b.selected+delta))
	b.top = min(b.top, b.selected)
	if b.selected >= b.top+rows {
		b.top = b.selected - rows + 1
	}
}
func cleanText(s string) string {
	return strings.Map(func(r rune) rune {
		if unicode.IsControl(r) {
			return ' '
		}
		return r
	}, s)
}
func fit(s string, width int) string {
	r := []rune(cleanText(s))
	if len(r) > width {
		r = r[:max(0, width)]
	}
	return string(r)
}
func (b *browserModel) render(out io.Writer, width, height int, provider string) error {
	rows := max(1, height-7)
	if b.selected < b.top {
		b.top = b.selected
	}
	if b.selected >= b.top+rows {
		b.top = b.selected - rows + 1
	}
	var screen strings.Builder
	screen.WriteString("\x1b[H\x1b[2J")
	line := func(s string) { screen.WriteString(fit(s, max(1, width-1))); screen.WriteString("\r\n") }
	if b.busy {
		line("Installing " + provider + " " + b.versions[b.selected].Number)
		line("")
		line(b.status)
		line("")
		line("Ctrl+C Cancel · q/Esc Cancel and exit")
		_, err := io.WriteString(out, screen.String())
		return err
	}
	line("SVM / " + provider + " versions")
	line("↑/↓ Select · PgUp/PgDn Page · Home/End · Enter Download · q/Esc Quit")
	line("   INSTALLED  VERSION          LTS           RELEASED")
	for i := b.top; i < min(len(b.versions), b.top+rows); i++ {
		v := b.versions[i]
		mark := "-"
		if v.Installed {
			mark = "✓"
		}
		cursor := "  "
		if i == b.selected {
			cursor = "> "
		}
		row := fit(fmt.Sprintf("%s %-9s  %-15s  %-12s  %s", cursor, mark, v.Number, v.LTS, v.Date), max(1, width-1))
		if i == b.selected {
			screen.WriteString("\x1b[7m" + row + "\x1b[0m\r\n")
		} else {
			line(row)
		}
	}
	if len(b.versions) == 0 {
		line("No matching versions.")
	}
	line(fmt.Sprintf("%d / %d · ✓ Installed", min(len(b.versions), b.selected+1), len(b.versions)))
	status := b.status
	line(status)
	_, err := io.WriteString(out, screen.String())
	return err
}

type progressLine struct {
	mu   sync.Mutex
	text string
}

func (p *progressLine) Write(b []byte) (int, error) {
	p.mu.Lock()
	defer p.mu.Unlock()
	s := strings.TrimSpace(string(b))
	lines := strings.Split(s, "\n")
	if len(lines) > 0 {
		p.text = fit(lines[len(lines)-1], 240)
	}
	return len(b), nil
}
func (p *progressLine) Text() string { p.mu.Lock(); defer p.mu.Unlock(); return p.text }

type actionResult struct {
	code    int
	err     error
	message string
}

func browse(ctx context.Context, dir string, m *sdk.Manager, provider string, versions []sdk.Version, out *os.File) (code int, err error) {
	session, err := terminal.Open(out)
	if err != nil {
		return 1, err
	}
	completedVersion := ""
	defer func() {
		if completedVersion != "" {
			fmt.Fprintf(out, "Installed %s %s.\n\nUse in this project:\n  svm use %s %s\n\nRun commands:\n  svm node --version\n  svm npm --version\n", provider, completedVersion, provider, completedVersion)
		}
	}()
	defer func() {
		if e := session.Close(); e != nil {
			err = errors.Join(err, e)
			if code == 0 {
				code = 1
			}
		}
	}()
	// Own the input descriptor, so leaving the screen can unblock its reader.
	stop := make(chan struct{})
	readerDone := make(chan struct{})
	defer func() { close(stop); <-readerDone }()
	input := make(chan byte, 64)
	readErrors := make(chan error, 1)
	go func() {
		defer close(readerDone)
		var data [64]byte
		for {
			select {
			case <-stop:
				return
			default:
			}
			n, e := session.Read(data[:])
			for _, c := range data[:n] {
				select {
				case input <- c:
				case <-stop:
					return
				}
			}
			if errors.Is(e, syscall.EAGAIN) || errors.Is(e, syscall.EINTR) {
				select {
				case <-time.After(10 * time.Millisecond):
					continue
				case <-stop:
					return
				}
			}
			if e != nil {
				select {
				case readErrors <- e:
				case <-stop:
				}
				return
			}
		}
	}()
	b := browserModel{versions: versions}
	ticker := time.NewTicker(80 * time.Millisecond)
	defer ticker.Stop()
	var sequence string
	var escapeAt time.Time
	results := make(chan actionResult, 1)
	var cancel context.CancelFunc
	var progress *progressLine
	var exiting bool
	defer func() {
		if cancel != nil {
			cancel()
			<-results
		}
	}()
	handle := func(key string) bool {
		if b.busy {
			if key == "quit" || key == "escape" {
				exiting = true
				cancel()
			}
			if key == "interrupt" {
				cancel()
			}
			return false
		}
		_, height := session.Size()
		rows := max(1, height-7)
		switch key {
		case "interrupt", "quit":
			return true
		case "escape":
			return true
		case "up", "left":
			b.move(-1, rows)
		case "down", "right":
			b.move(1, rows)
		case "pageup":
			b.move(-rows, rows)
		case "pagedown":
			b.move(rows, rows)
		case "home":
			b.move(-len(b.versions), rows)
		case "end":
			b.move(len(b.versions), rows)
		case "enter":
			if len(b.versions) == 0 {
				return false
			}
			version := b.versions[b.selected].Number
			args := []string{"install", provider, version}
			message := "Installed " + provider + " " + version
			var operationCtx context.Context
			operationCtx, cancel = context.WithCancel(ctx)
			progress = &progressLine{}
			b.busy = true
			b.status = "Preparing download…"
			go func() {
				code, err := run(operationCtx, args, dir, m, strings.NewReader(""), progress, progress)
				results <- actionResult{code, err, message}
			}()
		}
		return false
	}
	dirty := true
	lastWidth, lastHeight := 0, 0
	for {
		if dirty {
			w, h := session.Size()
			lastWidth, lastHeight = w, h
			if err = b.render(out, w, h, provider); err != nil {
				return 1, err
			}
			dirty = false
		}
		select {
		case <-ctx.Done():
			return 130, ctx.Err()
		case err := <-readErrors:
			if errors.Is(err, io.EOF) {
				return 0, nil
			}
			return 1, err
		case r := <-results:
			cancel()
			cancel = nil
			b.busy = false
			if r.err != nil {
				b.status = "Failed: " + r.err.Error()
			} else if r.code != 0 {
				b.status = fmt.Sprintf("Operation failed (exit %d)", r.code)
			} else {
				completedVersion = b.versions[b.selected].Number
				return 0, nil
			}
			for i := range b.versions {
				b.versions[i].Installed = m.Installed(provider, b.versions[i].Number)
			}
			if exiting {
				return 0, nil
			}
			dirty = true
		case <-ticker.C:
			if sequence != "" && time.Since(escapeAt) > 120*time.Millisecond {
				sequence = ""
				if handle("escape") {
					return 0, nil
				}
				dirty = true
			}
			if b.busy {
				text := progress.Text()
				if b.status != text {
					b.status = text
					dirty = true
				}
			}
			w, h := session.Size()
			if w != lastWidth || h != lastHeight {
				dirty = true
			}
		case c := <-input:
			key := ""
			if sequence != "" {
				sequence += string(c)
				switch sequence {
				case "\x1b[A", "\x1bOA":
					key = "up"
				case "\x1b[B", "\x1bOB":
					key = "down"
				case "\x1b[C", "\x1bOC":
					key = "right"
				case "\x1b[D", "\x1bOD":
					key = "left"
				case "\x1b[5~":
					key = "pageup"
				case "\x1b[6~":
					key = "pagedown"
				case "\x1b[H", "\x1bOH", "\x1b[1~":
					key = "home"
				case "\x1b[F", "\x1bOF", "\x1b[4~":
					key = "end"
				}
				if key != "" || len(sequence) > 8 {
					sequence = ""
				}
			} else {
				switch c {
				case 27:
					sequence = "\x1b"
					escapeAt = time.Now()
				case 3:
					key = "interrupt"
				case 'q':
					key = "quit"
				case 'k':
					key = "up"
				case 'j':
					key = "down"
				case '\r', '\n':
					key = "enter"
				}
			}
			if key != "" {
				if handle(key) {
					return 0, nil
				}
				dirty = true
			}
		}
	}
}
