package sdk

import (
	"bytes"
	"context"
	"crypto/sha256"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDownloadResumeAndRestart(t *testing.T) {
	data := []byte("verified official archive content")
	checksum := fmt.Sprintf("%x", sha256.Sum256(data))
	for _, tc := range []struct {
		name       string
		status     int
		rangeValue string
		body       []byte
		wantErr    bool
	}{
		{"resume", 206, fmt.Sprintf("bytes 8-%d/%d", len(data)-1, len(data)), data[8:], false},
		{"ignore range", 200, "", data, false},
		{"wrong range", 206, fmt.Sprintf("bytes 0-%d/%d", len(data)-1, len(data)), data, true},
		{"bad checksum", 206, fmt.Sprintf("bytes 8-%d/%d", len(data)-1, len(data)), bytes.Repeat([]byte("x"), len(data)-8), true},
		{"range unsatisfiable", 416, "", nil, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "archive.part")
			os.WriteFile(path, data[:8], 0600)
			client := &http.Client{Transport: transport(func(r *http.Request) (*http.Response, error) {
				if r.Header.Get("Range") != "bytes=8-" {
					t.Fatal("missing range", r.Header)
				}
				return &http.Response{StatusCode: tc.status, ContentLength: int64(len(tc.body)), Body: io.NopCloser(bytes.NewReader(tc.body)), Header: http.Header{"Content-Range": []string{tc.rangeValue}}}, nil
			})}
			err := downloadArchive(context.Background(), client, "https://example.invalid/file", path, checksum, io.Discard)
			if (err != nil) != tc.wantErr {
				t.Fatal(err)
			}
			got, _ := os.ReadFile(path)
			if !tc.wantErr && !bytes.Equal(got, data) {
				t.Fatal("wrong assembled archive", string(got))
			}
			if tc.name == "wrong range" && !bytes.Equal(got, data[:8]) {
				t.Fatal("invalid range modified prefix")
			}
			if tc.name == "bad checksum" && len(got) != 0 {
				t.Fatal("invalid archive not reset")
			}
		})
	}
	path := filepath.Join(t.TempDir(), "cached.part")
	os.WriteFile(path, data, 0600)
	client := &http.Client{Transport: transport(func(r *http.Request) (*http.Response, error) {
		t.Fatal("valid cache requested network")
		return nil, nil
	})}
	if err := downloadArchive(context.Background(), client, "https://example.invalid/file", path, checksum, io.Discard); err != nil {
		t.Fatal(err)
	}
}
func TestDownloadInterruptedPrefixIsResumable(t *testing.T) {
	path := filepath.Join(t.TempDir(), "archive.part")
	client := &http.Client{Transport: transport(func(r *http.Request) (*http.Response, error) {
		return &http.Response{StatusCode: 200, ContentLength: 100, Header: make(http.Header), Body: &brokenBody{}}, nil
	})}
	err := downloadArchive(context.Background(), client, "https://example.invalid/file", path, strings.Repeat("0", 64), io.Discard)
	b, _ := os.ReadFile(path)
	if err == nil || string(b) != "prefix" {
		t.Fatal(err, string(b))
	}
}

type brokenBody struct{ read bool }

func (b *brokenBody) Read(p []byte) (int, error) {
	if b.read {
		return 0, io.ErrUnexpectedEOF
	}
	b.read = true
	return copy(p, "prefix"), nil
}
func (*brokenBody) Close() error { return nil }
