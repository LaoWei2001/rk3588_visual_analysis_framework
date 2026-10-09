package main

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net/url"
	"os"
	"path/filepath"
	"strings"
)

// Older downloads appended configuration. Read it only for backwards compatibility.
const configMarker = "RKDATASETCFGv1!!"
const maxConfigBytes = 16 * 1024

type receiverConfig struct {
	URL       string `json:"url"`
	Token     string `json:"token,omitempty"` // Legacy configuration; no longer used to connect.
	Directory string `json:"directory,omitempty"`
}

func normalizeReceiverURL(value string) (string, error) {
	value = strings.TrimRight(strings.TrimSpace(value), "/")
	if value != "" && !strings.Contains(value, "://") {
		value = "http://" + value
	}
	parsed, err := url.Parse(value)
	if err != nil || (parsed.Scheme != "http" && parsed.Scheme != "https") || parsed.Hostname() == "" || parsed.User != nil || parsed.RawQuery != "" || parsed.Fragment != "" {
		return "", fmt.Errorf("请输入有效的设备地址，如 192.168.2.41:8080；HTTPS 请填写完整地址")
	}
	return value, nil
}

func (config *receiverConfig) setURL(value string) error {
	address, err := normalizeReceiverURL(value)
	if err != nil {
		return err
	}
	previous, _ := normalizeReceiverURL(config.URL)
	if previous != address {
		config.Directory = ""
	}
	config.URL = address
	return nil
}

func embeddedConfig(path string) (receiverConfig, bool, error) {
	var config receiverConfig
	file, err := os.Open(path)
	if err != nil {
		return config, false, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return config, false, err
	}
	footerSize := int64(len(configMarker) + 8)
	if info.Size() < footerSize {
		return config, false, nil
	}
	footer := make([]byte, footerSize)
	if _, err = file.ReadAt(footer, info.Size()-footerSize); err != nil {
		return config, false, err
	}
	if string(footer[8:]) != configMarker {
		return config, false, nil
	}
	length := binary.LittleEndian.Uint64(footer[:8])
	if length == 0 || length > maxConfigBytes || int64(length) > info.Size()-footerSize {
		return config, false, fmt.Errorf("接收程序中的配置损坏，请重新下载")
	}
	data := make([]byte, length)
	if _, err = file.ReadAt(data, info.Size()-footerSize-int64(length)); err != nil {
		return config, false, err
	}
	if err = json.Unmarshal(data, &config); err != nil {
		return config, false, fmt.Errorf("接收配置无效: %w", err)
	}
	if config.URL == "" || config.Token == "" {
		return config, false, fmt.Errorf("接收配置缺少地址或凭据")
	}
	return config, true, nil
}

func unpackRuntime(payload []byte, destination string) error {
	archive, err := zip.NewReader(bytes.NewReader(payload), int64(len(payload)))
	if err != nil {
		return err
	}
	for _, entry := range archive.File {
		name := strings.ReplaceAll(entry.Name, "\\", "/")
		clean := filepath.Clean(filepath.FromSlash(name))
		if filepath.IsAbs(clean) || clean == "." || clean == ".." ||
			strings.HasPrefix(clean, ".."+string(filepath.Separator)) || strings.Contains(clean, ":") ||
			entry.Mode()&os.ModeSymlink != 0 {
			return fmt.Errorf("运行时包含非法路径")
		}
		target := filepath.Join(destination, clean)
		if entry.FileInfo().IsDir() {
			if err = os.MkdirAll(target, 0700); err != nil {
				return err
			}
			continue
		}
		if err = os.MkdirAll(filepath.Dir(target), 0700); err != nil {
			return err
		}
		input, err := entry.Open()
		if err != nil {
			return err
		}
		output, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			input.Close()
			return err
		}
		_, copyErr := io.Copy(output, input)
		input.Close()
		closeErr := output.Close()
		if copyErr != nil {
			return copyErr
		}
		if closeErr != nil {
			return closeErr
		}
	}
	return nil
}

func writeConfig(path string, config receiverConfig) error {
	data, err := json.MarshalIndent(config, "", "  ")
	if err != nil {
		return err
	}
	file, err := os.CreateTemp(filepath.Dir(path), ".receiver-config-*")
	if err != nil {
		return err
	}
	temporary := file.Name()
	defer os.Remove(temporary)
	if _, err = file.Write(data); err == nil {
		err = file.Sync()
	}
	closeErr := file.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	return os.Rename(temporary, path)
}
