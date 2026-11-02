package sdk

import (
	"fmt"
	"io"
	"time"
)

// downloadProgress reports transferred bytes without buffering the SDK in memory.
type downloadProgress struct {
	out         io.Writer
	total, done int64
	initial     int64
	start, last time.Time
}

func (p *downloadProgress) Write(b []byte) (int, error) {
	p.done += int64(len(b))
	now := time.Now()
	if now.Sub(p.last) >= 200*time.Millisecond {
		p.report(now)
	}
	return len(b), nil
}
func (p *downloadProgress) report(now time.Time) {
	elapsed := now.Sub(p.start).Seconds()
	if elapsed <= 0 {
		elapsed = 0.001
	}
	speed := float64(p.done-p.initial) / elapsed / (1 << 20)
	if p.total > 0 {
		percent := min(100, float64(p.done)*100/float64(p.total))
		fmt.Fprintf(p.out, "Downloading %5.1f%%  %.2f / %.2f MiB  %.2f MiB/s\n", percent, float64(p.done)/(1<<20), float64(p.total)/(1<<20), speed)
	} else {
		fmt.Fprintf(p.out, "Downloading %.2f MiB  %.2f MiB/s\n", float64(p.done)/(1<<20), speed)
	}
	p.last = now
}
