package sdk

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"
)

// downloadArchive verifies the whole file, including any resumed prefix. A server
// ignoring Range restarts cleanly; an invalid range never gets appended.
func downloadArchive(ctx context.Context, client *http.Client, url, path, expected string, out io.Writer) error {
	file, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return err
	}
	offset := info.Size()
	hash := sha256.New()
	if _, err = io.Copy(hash, file); err != nil {
		return err
	}
	if offset > 0 && strings.EqualFold(hex.EncodeToString(hash.Sum(nil)), expected) {
		fmt.Fprintln(out, "Using verified cached archive")
		return nil
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	request.Header.Set("Accept-Encoding", "identity")
	if offset > 0 {
		request.Header.Set("Range", fmt.Sprintf("bytes=%d-", offset))
	}
	response, err := client.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	total := response.ContentLength
	switch response.StatusCode {
	case http.StatusOK:
		offset = 0
		hash.Reset()
		if err = file.Truncate(0); err != nil {
			return err
		}
		if _, err = file.Seek(0, io.SeekStart); err != nil {
			return err
		}
	case http.StatusPartialContent:
		value := response.Header.Get("Content-Range")
		parts := strings.Split(strings.TrimPrefix(value, "bytes "), "/")
		if len(parts) != 2 {
			return errors.New("invalid Content-Range; download was not modified")
		}
		bounds := strings.Split(parts[0], "-")
		if len(bounds) != 2 {
			return errors.New("invalid download range")
		}
		start, e1 := strconv.ParseInt(bounds[0], 10, 64)
		end, e2 := strconv.ParseInt(bounds[1], 10, 64)
		size, e3 := strconv.ParseInt(parts[1], 10, 64)
		if e1 != nil || e2 != nil || e3 != nil || start != offset || end < start || end != size-1 || size <= offset || response.ContentLength >= 0 && response.ContentLength != end-start+1 {
			return errors.New("server returned an inconsistent range; retry download")
		}
		total = size
	case http.StatusRequestedRangeNotSatisfiable:
		// The prefix was not a valid complete archive. Restart on the next attempt.
		if err = file.Truncate(0); err != nil {
			return err
		}
		return errors.New("partial archive no longer matches server; retry to restart download")
	default:
		return fmt.Errorf("download: HTTP %d", response.StatusCode)
	}
	if offset > 0 {
		fmt.Fprintf(out, "Resuming at %.2f MiB\n", float64(offset)/(1<<20))
	}
	transfer := &downloadProgress{out: out, total: total, done: offset, initial: offset, start: time.Now()}
	_, err = io.Copy(io.MultiWriter(file, hash, transfer), response.Body)
	transfer.report(time.Now())
	if err != nil {
		return err
	}
	if err = ctx.Err(); err != nil {
		return err
	}
	if err = file.Sync(); err != nil {
		return err
	}
	if !strings.EqualFold(hex.EncodeToString(hash.Sum(nil)), expected) {
		if err = file.Truncate(0); err != nil {
			return err
		}
		return errors.New("SHA-256 mismatch; installation aborted, partial archive reset; retry svm install")
	}
	return nil
}
