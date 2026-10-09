package main

import (
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"syscall"
	"time"
	"unsafe"
)

type jobBasicLimits struct {
	PerProcessTime, PerJobTime int64
	Flags                      uint32
	MinimumWorkingSet          uintptr
	MaximumWorkingSet          uintptr
	ActiveProcessLimit         uint32
	Affinity                   uintptr
	PriorityClass              uint32
	SchedulingClass            uint32
}

type jobExtendedLimits struct {
	Basic                 jobBasicLimits
	IOCounters            [6]uint64
	ProcessMemoryLimit    uintptr
	JobMemoryLimit        uintptr
	PeakProcessMemoryUsed uintptr
	PeakJobMemoryUsed     uintptr
}

// Bind the launcher itself before creating children, so every subprocess
// inherits job membership with no race between creation and assignment.
// Keep the non-inheritable job handle open until process exit: Windows closes
// it when the launcher dies, killing all members, including orphaned children.
// Closing it explicitly while the launcher is alive would kill the launcher.
func bindReceiverJob() (syscall.Handle, error) {
	job, _, err := kernel32.NewProc("CreateJobObjectW").Call(0, 0)
	if job == 0 {
		return 0, fmt.Errorf("无法创建接收进程组: %w", err)
	}
	limits := jobExtendedLimits{}
	limits.Basic.Flags = 0x2000 // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
	ok, _, err := kernel32.NewProc("SetInformationJobObject").Call(job, 9,
		uintptr(unsafe.Pointer(&limits)), unsafe.Sizeof(limits))
	runtime.KeepAlive(&limits)
	if ok == 0 {
		syscall.CloseHandle(syscall.Handle(job))
		return 0, fmt.Errorf("无法设置接收进程退出保护: %w", err)
	}
	current, _ := syscall.GetCurrentProcess()
	ok, _, err = kernel32.NewProc("AssignProcessToJobObject").Call(job, uintptr(current))
	if ok == 0 {
		syscall.CloseHandle(syscall.Handle(job))
		return 0, fmt.Errorf("无法绑定接收进程退出保护: %w", err)
	}
	return syscall.Handle(job), nil
}

func configureConsoleInput() func() {
	input := syscall.Handle(os.Stdin.Fd())
	var original uint32
	if syscall.GetConsoleMode(input, &original) != nil {
		return func() {} // Redirected input is not a console.
	}
	// Process Ctrl+C as a signal. Disable classic console Quick Edit so selecting
	// output cannot pause the receiver or turn Ctrl+C into a copy operation.
	mode := (original | 0x0001 | 0x0080) &^ uint32(0x0040)
	setMode := kernel32.NewProc("SetConsoleMode")
	setMode.Call(uintptr(input), uintptr(mode))
	return func() { setMode.Call(uintptr(input), uintptr(original)) }
}

func runChild(command *exec.Cmd) error {
	command.SysProcAttr = &syscall.SysProcAttr{CreationFlags: syscall.CREATE_NEW_PROCESS_GROUP}
	interrupts := make(chan os.Signal, 2)
	signal.Notify(interrupts, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(interrupts)
	stopped := make(chan struct{})
	defer close(stopped)
	handler := syscall.NewCallback(func(event uint32) uintptr {
		if event != syscall.CTRL_CLOSE_EVENT {
			return 0 // Keep the Go runtime's Ctrl+C/Ctrl+Break handling.
		}
		waitForConsoleClose(interrupts, stopped, 4*time.Second)
		return 1
	})
	register := kernel32.NewProc("SetConsoleCtrlHandler")
	if ok, _, err := register.Call(handler, 1); ok == 0 {
		return fmt.Errorf("无法设置接收程序关闭处理: %w", err)
	}
	defer register.Call(handler, 0)
	if err := command.Start(); err != nil {
		return err
	}
	done := make(chan error, 1)
	go func() { done <- command.Wait() }()
	return waitForChild(done, interrupts, func() error {
		fmt.Fprintln(os.Stdout, "\n正在停止接收…")
		// CTRL_C cannot target a Windows process group; CTRL_BREAK can.
		ok, _, err := kernel32.NewProc("GenerateConsoleCtrlEvent").Call(syscall.CTRL_BREAK_EVENT, uintptr(command.Process.Pid))
		if ok == 0 {
			return err
		}
		return nil
	}, command.Process.Kill, 3*time.Second)
}
