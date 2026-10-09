package main

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

func TestPersonalizedConfiguration(t *testing.T) {
	config := []byte(`{"url":"https://device/控制台","token":"paired-token"}`)
	footer := make([]byte, 8)
	binary.LittleEndian.PutUint64(footer, uint64(len(config)))
	data := append(append(append([]byte("MZ executable"), config...), footer...), []byte(configMarker)...)
	path := filepath.Join(t.TempDir(), "receiver.exe")
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	paired, found, err := embeddedConfig(path)
	if err != nil || !found || paired.Token != "paired-token" || paired.URL != "https://device/控制台" {
		t.Fatalf("configuration not recovered: %+v, %v, %v", paired, found, err)
	}
	binary.LittleEndian.PutUint64(footer, 1<<40)
	data = append(append([]byte("MZ executable"), footer...), []byte(configMarker)...)
	os.WriteFile(path, data, 0600)
	if _, _, err = embeddedConfig(path); err == nil {
		t.Fatal("oversized configuration accepted")
	}
	os.WriteFile(path, []byte("MZ executable without pairing"), 0600)
	if _, found, err = embeddedConfig(path); err != nil || found {
		t.Fatal("unpaired binary was treated as paired")
	}
}

func runtimeArchive(t *testing.T, name string) []byte {
	t.Helper()
	var buffer bytes.Buffer
	archive := zip.NewWriter(&buffer)
	writer, err := archive.Create(name)
	if err != nil {
		t.Fatal(err)
	}
	writer.Write([]byte("private runtime"))
	if err = archive.Close(); err != nil {
		t.Fatal(err)
	}
	return buffer.Bytes()
}

func TestRuntimeExtraction(t *testing.T) {
	directory := t.TempDir()
	if err := unpackRuntime(runtimeArchive(t, "nested/python.exe"), directory); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(directory, "nested", "python.exe"))
	if err != nil || string(data) != "private runtime" {
		t.Fatal("runtime extraction failed")
	}
	for _, name := range []string{"../escape.exe", "/escape.exe", "..\\escape.exe", "C:\\escape.exe"} {
		if err := unpackRuntime(runtimeArchive(t, name), t.TempDir()); err == nil {
			t.Fatalf("unsafe entry accepted: %s", name)
		}
	}
}

func TestConfigCanBeUpdatedWithoutPartialJSON(t *testing.T) {
	path := filepath.Join(t.TempDir(), "receiver.json")
	first := receiverConfig{URL: "http://device", Token: "old", Directory: "图片保存目录"}
	if err := writeConfig(path, first); err != nil {
		t.Fatal(err)
	}
	first.Token = ""
	first.URL = "http://new-device"
	if err := writeConfig(path, first); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil || !bytes.Contains(data, []byte("new-device")) || !bytes.Contains(data, []byte("图片保存目录")) || bytes.Contains(data, []byte("token")) {
		t.Fatal("configuration update lost address/directory or retained legacy credentials")
	}
}

func TestReceiverAddress(t *testing.T) {
	for _, test := range []struct{ input, expected string }{
		{"192.168.2.41:8080", "http://192.168.2.41:8080"},
		{"192.168.2.41", "http://192.168.2.41"},
		{"device", "http://device"},
		{"device:8080/控制台", "http://device:8080/控制台"},
		{"[::1]:8080", "http://[::1]:8080"},
		{"http://192.168.2.41:8080", "http://192.168.2.41:8080"},
		{"https://device/控制台", "https://device/控制台"},
	} {
		actual, err := normalizeReceiverURL("  " + test.input + "/  ")
		if err != nil || actual != test.expected {
			t.Fatalf("address %q normalized to %q: %v", test.input, actual, err)
		}
	}
	for _, address := range []string{"", "ftp://device", "device:invalid", "http://user:password@device", "http://device?token=x", "http://device#fragment", "device?token=x", "device#fragment"} {
		if _, err := normalizeReceiverURL(address); err == nil {
			t.Fatalf("invalid address accepted: %s", address)
		}
	}
}

func TestAddressChangeKeepsDirectoryForEquivalentInput(t *testing.T) {
	config := receiverConfig{URL: "http://device:8080", Directory: "图片保存目录"}
	if err := config.setURL(" device:8080/ "); err != nil || config.Directory != "图片保存目录" || config.URL != "http://device:8080" {
		t.Fatalf("equivalent address lost directory: %+v, %v", config, err)
	}
	if err := config.setURL("https://device:8080"); err != nil || config.Directory != "" {
		t.Fatalf("different address did not reset directory: %+v, %v", config, err)
	}
	previous := config
	if err := config.setURL("ftp://device"); err == nil || config != previous {
		t.Fatalf("invalid address changed configuration: %+v, %v", config, err)
	}
}
