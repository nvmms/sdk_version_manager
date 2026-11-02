package sdk

import (
	"context"
	"fmt"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

func checkNodeExecutable(ctx context.Context, root, version string) error {
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	entry, _ := (Node{}).Entry("node")
	command := exec.CommandContext(ctx, filepath.Join(root, entry.Executable), "--version")
	command.Dir = root
	output, err := command.Output()
	if err != nil {
		return fmt.Errorf("node %s health check failed: %w; uninstall and then run svm use node %s", version, err, version)
	}
	if strings.TrimSpace(string(output)) != "v"+version {
		return fmt.Errorf("installed executable version mismatch (expected v%s); uninstall and then run svm use node %s", version, version)
	}
	return nil
}
