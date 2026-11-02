package sdk

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"time"

	"github.com/nvmms/sdk_version_manager/internal/config"
	"github.com/nvmms/sdk_version_manager/internal/filelock"
)

type networkSettings struct {
	SchemaVersion int    `json:"schemaVersion"`
	Proxy         string `json:"proxy"`
}

func validateProxy(value string) error {
	if value == "" {
		return nil
	}
	u, err := url.Parse(value)
	if err != nil || u.Scheme != "http" || u.Hostname() == "" || u.User != nil || u.RawQuery != "" || u.Fragment != "" || u.Path != "" && u.Path != "/" {
		return errors.New("proxy must be an HTTP URL without credentials, query, or path, e.g. http://127.0.0.1:7890")
	}
	return nil
}
func (m *Manager) Proxy() (string, error) {
	b, err := os.ReadFile(filepath.Join(m.Root, "settings", "network.json"))
	if errors.Is(err, os.ErrNotExist) {
		return "", nil
	}
	if err != nil {
		return "", err
	}
	var settings networkSettings
	if err = json.Unmarshal(b, &settings); err != nil {
		return "", errors.New("invalid network configuration")
	}
	if settings.SchemaVersion != 1 {
		return "", errors.New("unsupported network configuration schema")
	}
	if err = validateProxy(settings.Proxy); err != nil {
		return "", err
	}
	return settings.Proxy, nil
}
func (m *Manager) SetProxy(value string) error {
	if err := validateProxy(value); err != nil {
		return err
	}
	lock, err := filelock.Acquire(filepath.Join(m.Root, "locks", "settings.lock"), false)
	if err != nil {
		return err
	}
	defer lock.Close()
	return config.Write(filepath.Join(m.Root, "settings", "network.json"), networkSettings{1, value})
}
func (m *Manager) Client(timeout time.Duration) (*http.Client, error) {
	proxy, err := m.Proxy()
	if err != nil {
		return nil, err
	}
	transport := http.DefaultTransport.(*http.Transport).Clone()
	if proxy != "" {
		u, _ := url.Parse(proxy)
		transport.Proxy = http.ProxyURL(u)
	}
	return &http.Client{Transport: transport, Timeout: timeout, CheckRedirect: func(req *http.Request, via []*http.Request) error {
		if len(via) >= 10 {
			return errors.New("too many redirects")
		}
		if req.URL.Scheme != "https" {
			return errors.New("refusing a non-HTTPS download redirect")
		}
		return nil
	}}, nil
}
