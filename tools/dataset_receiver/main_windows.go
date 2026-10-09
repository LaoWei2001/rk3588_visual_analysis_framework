package main

import (
	"bufio"
	_ "embed"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"unsafe"
)

// Both are copied into a temporary build directory by build_windows.py.
//
//go:embed payload/python-runtime.zip
var pythonRuntime []byte

//go:embed payload/dataset_receiver.py
var receiverScript []byte

//go:embed payload/GO-LICENSE.txt
var goLicense []byte

var kernel32 = syscall.NewLazyDLL("kernel32.dll")
var receiverJob syscall.Handle

func chooseDirectory() (string, error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	ole32 := syscall.NewLazyDLL("ole32.dll")
	hresult, _, _ := ole32.NewProc("CoInitializeEx").Call(0, 2) // STA
	if int32(hresult) < 0 {
		return "", fmt.Errorf("无法打开文件夹选择器")
	}
	defer ole32.NewProc("CoUninitialize").Call()
	title, _ := syscall.UTF16PtrFromString("选择电脑上的图片保存文件夹")
	displayName := make([]uint16, 260)
	owner, _, _ := kernel32.NewProc("GetConsoleWindow").Call()
	info := struct {
		Owner       uintptr
		Root        uintptr
		DisplayName *uint16
		Title       *uint16
		Flags       uint32
		Callback    uintptr
		Parameter   uintptr
		Image       int32
	}{Owner: owner, DisplayName: &displayName[0], Title: title, Flags: 0x41}
	shell32 := syscall.NewLazyDLL("shell32.dll")
	pidl, _, _ := shell32.NewProc("SHBrowseForFolderW").Call(uintptr(unsafe.Pointer(&info)))
	runtime.KeepAlive(info)
	if pidl == 0 {
		return "", nil
	} // Cancel is a normal exit.
	defer ole32.NewProc("CoTaskMemFree").Call(pidl)
	path := make([]uint16, 32768)
	ok, _, _ := shell32.NewProc("SHGetPathFromIDListEx").Call(pidl, uintptr(unsafe.Pointer(&path[0])), uintptr(len(path)), 0)
	if ok == 0 {
		return "", fmt.Errorf("请选择本地磁盘或有效的共享文件夹")
	}
	return syscall.UTF16ToString(path), nil
}

func ownsConsole() bool {
	processes := make([]uint32, 16)
	count, _, _ := kernel32.NewProc("GetConsoleProcessList").Call(uintptr(unsafe.Pointer(&processes[0])), uintptr(len(processes)))
	return count == 1
}

func run() error {
	executable, err := os.Executable()
	if err != nil {
		return err
	}
	defaultConfig := strings.TrimSuffix(executable, filepath.Ext(executable)) + ".json"
	flags := flag.NewFlagSet("dataset_receiver", flag.ContinueOnError)
	configPath := flags.String("config", defaultConfig, "接收配置路径（默认自动创建在 EXE 旁）")
	targetURL := flags.String("url", "", "设备地址，如 192.168.2.41:8080（省略协议时默认 HTTP）")
	output := flags.String("output", "", "电脑图片保存目录")
	choose := flags.Bool("choose-directory", false, "重新选择图片保存目录")
	start := flags.Bool("start", false, "直接使用已保存配置开始接收，跳过启动菜单")
	check := flags.Bool("check-runtime", false, "检查内置运行时，不连接设备")
	licenses := flags.Bool("licenses", false, "显示内置组件许可证")
	if err = flags.Parse(os.Args[1:]); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}
	if flags.NArg() != 0 {
		return fmt.Errorf("不支持的位置参数")
	}
	// A private runtime directory prevents dependencies on PATH or installed Python.
	directory, err := os.MkdirTemp("", "dataset-receiver-*")
	if err != nil {
		return fmt.Errorf("无法准备接收程序: %w", err)
	}
	defer os.RemoveAll(directory)
	if err = unpackRuntime(pythonRuntime, directory); err != nil {
		return fmt.Errorf("内置运行时释放失败: %w", err)
	}
	scriptPath := filepath.Join(directory, "dataset_receiver.py")
	if err = os.WriteFile(scriptPath, receiverScript, 0600); err != nil {
		return err
	}
	python := filepath.Join(directory, "python.exe")
	if *licenses {
		pythonLicense, err := os.ReadFile(filepath.Join(directory, "LICENSE.txt"))
		if err != nil {
			return err
		}
		fmt.Printf("Go runtime license:\n%s\nPython runtime license:\n%s\n", goLicense, pythonLicense)
		return nil
	}
	if *check {
		command := exec.Command(python, "-I", "-u", "-c", "import ssl,sqlite3,urllib.request,hashlib,json,msvcrt,ctypes;ctypes.WinDLL('kernel32');print('Bundled runtime OK')")
		command.Stdout, command.Stderr = os.Stdout, os.Stderr
		return runChild(command)
	}
	config := receiverConfig{}
	data, readErr := os.ReadFile(*configPath)
	if readErr == nil {
		if err = json.Unmarshal(data, &config); err != nil {
			return fmt.Errorf("接收配置损坏: %w", err)
		}
	} else if !os.IsNotExist(readErr) {
		return readErr
	}
	if *configPath == defaultConfig && config.URL == "" {
		paired, found, pairErr := embeddedConfig(executable)
		if pairErr != nil {
			return pairErr
		}
		if found {
			if err = config.setURL(paired.URL); err != nil {
				return err
			}
		}
	}
	config, err = setupReceiver(config, setupOptions{
		URL: *targetURL, Directory: *output, ChooseDirectory: *choose, Start: *start,
	}, os.Stdin, os.Stdout, chooseDirectory)
	if err != nil {
		return err
	}
	config.Directory, err = filepath.Abs(config.Directory)
	if err != nil {
		return err
	}
	if err = writeConfig(*configPath, config); err != nil {
		return fmt.Errorf("配置保存失败，请把 EXE 放到可写目录: %w", err)
	}
	fmt.Fprintf(os.Stdout, "\n开始接收：%s\n保存目录：%s\n按 Ctrl+C 停止。\n", config.URL, config.Directory)
	command := exec.Command(python, "-I", "-u", scriptPath, "--config", *configPath, "--output", config.Directory)
	command.Stdin, command.Stdout, command.Stderr = os.Stdin, os.Stdout, os.Stderr
	return runChild(command)
}

func mainExitCode() int {
	kernel32.NewProc("SetConsoleOutputCP").Call(65001)
	kernel32.NewProc("SetConsoleCP").Call(65001)
	restoreInput := configureConsoleInput()
	defer restoreInput()
	pause := ownsConsole()
	var err error
	receiverJob, err = bindReceiverJob()
	if err == nil {
		err = run()
	}
	if errors.Is(err, errUserStopped) {
		return 0 // Ctrl+C/closing the window must never ask for another Enter.
	}
	code := 0
	if err != nil {
		fmt.Fprintln(os.Stderr, "接收程序已停止：", err)
		code = 1
	}
	if pause && err != nil {
		fmt.Print("按回车关闭窗口…")
		bufio.NewReader(os.Stdin).ReadString('\n')
	}
	return code
}

func main() {
	os.Exit(mainExitCode())
}
