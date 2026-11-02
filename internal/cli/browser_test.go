package cli

import (
	"bytes"
	"fmt"
	"strings"
	"testing"

	"github.com/nvmms/sdk_version_manager/internal/sdk"
)

func TestBrowserPagingAndActions(t *testing.T) {
	var versions []sdk.Version
	for i := 0; i < 100; i++ {
		versions = append(versions, sdk.Version{Number: fmt.Sprintf("24.0.%d", i), Installed: i == 15})
	}
	b := browserModel{versions: versions}
	b.move(15, 10)
	if b.selected != 15 || b.top != 6 {
		t.Fatal("selection did not scroll into view", b)
	}
	var out bytes.Buffer
	if err := b.render(&out, 80, 17, "node"); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "✓") || strings.Contains(out.String(), "24.0.99") || strings.Count(out.String(), "24.0.") != 10 {
		t.Fatal("viewport is not bounded", out.String())
	}
	b.move(200, 10)
	if b.selected != 99 {
		t.Fatal("selection escaped list")
	}
	b.move(-200, 10)
	if b.selected != 0 || b.top != 0 {
		t.Fatal("home did not restore viewport")
	}
	b.selected = 15
	out.Reset()
	b.render(&out, 40, 10, "node")
	if !strings.Contains(out.String(), "24.0.15") {
		t.Fatal("resize hid selected row")
	}
}
func TestBrowserSanitizesTextAndEmptyList(t *testing.T) {
	b := browserModel{status: "failure\x1b[2J\nsecret"}
	var out bytes.Buffer
	b.render(&out, 40, 12, "node")
	if !strings.Contains(out.String(), "No matching versions") || strings.Contains(out.String(), "failure\x1b") {
		t.Fatal(out.String())
	}
	b.move(1, 10)
	if b.selected != 0 {
		t.Fatal("empty list selection moved")
	}
}

func TestDownloadScreenHidesList(t *testing.T) {
	b := browserModel{versions: []sdk.Version{{Number: "24.0.0"}, {Number: "22.0.0"}}, busy: true, status: "Downloading 42.0%  12 MiB / 30 MiB"}
	var out bytes.Buffer
	b.render(&out, 80, 24, "node")
	text := out.String()
	if strings.Contains(text, "22.0.0") || strings.Contains(text, "INSTALLED") || !strings.Contains(text, "42.0%") {
		t.Fatal(text)
	}
}
