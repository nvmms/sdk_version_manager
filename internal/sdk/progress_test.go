package sdk

import (
	"bytes"
	"strings"
	"testing"
	"time"
)

func TestDownloadProgress(t *testing.T) {
	for _, total := range []int64{200, -1} {
		var out bytes.Buffer
		p := downloadProgress{out: &out, total: total, start: time.Now().Add(-time.Second)}
		p.Write(make([]byte, 100))
		text := out.String()
		if !strings.Contains(text, "MiB/s") {
			t.Fatal(text)
		}
		if total > 0 && !strings.Contains(text, "50.0%") {
			t.Fatal(text)
		}
		if total < 0 && strings.Contains(text, "%") {
			t.Fatal("invented progress for unknown length", text)
		}
	}
}
