package main

import (
	"bytes"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSavedSettingsCanBeEditedAtStartup(t *testing.T) {
	for _, test := range []struct {
		name, input, address, directory string
		choices                         int
	}{
		{"reuse", "\n", "http://old-device:8080", "原图片目录", 0},
		{"device", "1\nnew-device:8080\n", "http://new-device:8080", "原图片目录", 0},
		{"folder", "2\n", "http://old-device:8080", "新图片目录", 1},
		{"both", "3\nhttps://new-device\n", "https://new-device", "新图片目录", 1},
		{"keep-address", "1\n\n", "http://old-device:8080", "原图片目录", 0},
		{"invalid-menu-retry", "9\n2\n", "http://old-device:8080", "新图片目录", 1},
		{"invalid-address-retry", "1\nftp://device\nnew-device:8080\n", "http://new-device:8080", "原图片目录", 0},
	} {
		t.Run(test.name, func(t *testing.T) {
			config := receiverConfig{URL: "http://old-device:8080", Directory: "原图片目录", Token: "obsolete"}
			var output bytes.Buffer
			choices := 0
			actual, err := setupReceiver(config, setupOptions{}, strings.NewReader(test.input), &output, func() (string, error) {
				choices++
				return "新图片目录", nil
			})
			if err != nil || actual.URL != test.address || actual.Directory != test.directory || actual.Token != "" || choices != test.choices {
				t.Fatalf("startup edit failed: %+v, choices=%d, error=%v", actual, choices, err)
			}
			if !strings.Contains(output.String(), "当前设备：http://old-device:8080") || !strings.Contains(output.String(), "2 更换保存目录") {
				t.Fatal("saved configuration or edit entry missing from startup")
			}
			path := filepath.Join(t.TempDir(), "receiver.json")
			if err := writeConfig(path, actual); err != nil {
				t.Fatal(err)
			}
			data, err := os.ReadFile(path)
			if err != nil || bytes.Contains(data, []byte("token")) || !bytes.Contains(data, []byte(test.directory)) {
				t.Fatal("edited configuration was not persisted cleanly")
			}
		})
	}
}

func TestSetupCancellationDoesNotSaveOrConnect(t *testing.T) {
	for _, input := range []string{"0\n", "2\n", "3\nnew-device\n", ""} {
		t.Run(strings.ReplaceAll(input, "\n", "/"), func(t *testing.T) {
			original := receiverConfig{URL: "http://old-device", Directory: "原图片目录"}
			path := filepath.Join(t.TempDir(), "receiver.json")
			if err := writeConfig(path, original); err != nil {
				t.Fatal(err)
			}
			before, _ := os.ReadFile(path)
			config, err := setupReceiver(original, setupOptions{}, strings.NewReader(input), io.Discard, func() (string, error) {
				return "", nil // Cancelling the folder dialog.
			})
			connected := false
			if err == nil {
				writeConfig(path, config)
				connected = true
			}
			after, _ := os.ReadFile(path)
			if connected || !(errors.Is(err, errUserStopped) || errors.Is(err, io.EOF)) || !bytes.Equal(before, after) {
				t.Fatalf("cancelled startup changed saved settings or connected: %+v, %v", config, err)
			}
		})
	}
}

func TestSetupSupportsFirstLaunchAndExplicitOptions(t *testing.T) {
	for _, test := range []struct {
		name     string
		config   receiverConfig
		options  setupOptions
		input    string
		expected receiverConfig
		choices  int
	}{
		{"first", receiverConfig{}, setupOptions{}, "device:8080\n", receiverConfig{URL: "http://device:8080", Directory: "选择的目录"}, 1},
		{"direct", receiverConfig{URL: "http://device", Directory: "原目录"}, setupOptions{Start: true}, "", receiverConfig{URL: "http://device", Directory: "原目录"}, 0},
		{"command-line", receiverConfig{URL: "http://old", Directory: "原目录"}, setupOptions{URL: "device:8080", Directory: "指定的目录"}, "", receiverConfig{URL: "http://device:8080", Directory: "指定的目录"}, 0},
		{"choose-directory", receiverConfig{URL: "http://device", Directory: "原目录"}, setupOptions{ChooseDirectory: true}, "", receiverConfig{URL: "http://device", Directory: "选择的目录"}, 1},
		{"new-cli-device", receiverConfig{URL: "http://old", Directory: "原目录"}, setupOptions{URL: "device:8080"}, "", receiverConfig{URL: "http://device:8080", Directory: "选择的目录"}, 1},
		{"explicit-folder-wins", receiverConfig{URL: "http://device", Directory: "原目录"}, setupOptions{Directory: "指定的目录", ChooseDirectory: true}, "", receiverConfig{URL: "http://device", Directory: "指定的目录"}, 0},
	} {
		t.Run(test.name, func(t *testing.T) {
			var output bytes.Buffer
			choices := 0
			actual, err := setupReceiver(test.config, test.options, strings.NewReader(test.input), &output, func() (string, error) {
				choices++
				return "选择的目录", nil
			})
			if err != nil || actual != test.expected || choices != test.choices || strings.Contains(output.String(), "请选择") {
				t.Fatalf("non-menu startup failed: %+v, choices=%d, error=%v", actual, choices, err)
			}
		})
	}
}

func TestFolderDialogFailureAllowsManualPath(t *testing.T) {
	config := receiverConfig{URL: "http://device", Directory: "原目录"}
	actual, err := setupReceiver(config, setupOptions{}, strings.NewReader("2\n\"新图片目录\"\n"), io.Discard, func() (string, error) {
		return "", errors.New("dialog unavailable")
	})
	if err != nil || actual.URL != config.URL || actual.Directory != "新图片目录" {
		t.Fatalf("manual folder fallback failed: %+v, %v", actual, err)
	}
}
