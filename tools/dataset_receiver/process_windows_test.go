package main

import (
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func TestWindowsJobLayout(t *testing.T) {
	if unsafe.Sizeof(jobBasicLimits{}) != 64 || unsafe.Sizeof(jobExtendedLimits{}) != 144 {
		t.Fatal("Windows x64 Job Object structure layout mismatch")
	}
}

// Real Windows regression: force-kill a launcher while its child is running.
// The child must terminate too, even though no Go cleanup code can run.
func TestWindowsParentExitKillsChild(t *testing.T) {
	pidFile := filepath.Join(t.TempDir(), "child.pid")
	parent := exec.Command(os.Args[0], "-test.run=^TestWindowsJobHelper$")
	parent.Env = append(os.Environ(), "RECEIVER_JOB_TEST=parent", "RECEIVER_JOB_PID="+pidFile)
	if err := parent.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { parent.Process.Kill(); parent.Wait() }()
	var childPID int
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		data, err := os.ReadFile(pidFile)
		if err == nil {
			childPID, _ = strconv.Atoi(strings.TrimSpace(string(data)))
			if childPID != 0 {
				break
			}
		}
		time.Sleep(10 * time.Millisecond)
	}
	if childPID == 0 {
		t.Fatal("job helper never started its child")
	}
	child, err := syscall.OpenProcess(syscall.SYNCHRONIZE, false, uint32(childPID))
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.CloseHandle(child)
	if err := parent.Process.Kill(); err != nil {
		t.Fatal(err)
	}
	if result, err := syscall.WaitForSingleObject(child, 3000); err != nil || result != syscall.WAIT_OBJECT_0 {
		t.Fatalf("child survived parent termination: result=%d, err=%v", result, err)
	}
}

func TestWindowsConsoleInterruptReachesChild(t *testing.T) {
	console, _, _ := kernel32.NewProc("GetConsoleWindow").Call()
	if console == 0 {
		t.Skip("requires an attached Windows console")
	}
	pidFile := filepath.Join(t.TempDir(), "child.pid")
	parent := exec.Command(os.Args[0], "-test.run=^TestWindowsJobHelper$")
	parent.Env = append(os.Environ(), "RECEIVER_JOB_TEST=supervisor", "RECEIVER_JOB_PID="+pidFile)
	parent.SysProcAttr = &syscall.SysProcAttr{CreationFlags: syscall.CREATE_NEW_PROCESS_GROUP}
	if err := parent.Start(); err != nil {
		t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() { done <- parent.Wait() }()
	defer parent.Process.Kill()
	deadline := time.Now().Add(5 * time.Second)
	ready := false
	for time.Now().Before(deadline) {
		if _, err := os.Stat(pidFile); err == nil {
			ready = true
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if !ready {
		t.Fatal("child did not become ready for console interrupts")
	}
	ok, _, err := kernel32.NewProc("GenerateConsoleCtrlEvent").Call(syscall.CTRL_BREAK_EVENT, uintptr(parent.Process.Pid))
	if ok == 0 {
		t.Fatal(err)
	}
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("interrupted launcher did not exit normally: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("interrupted launcher remained running")
	}
	if _, err := os.Stat(pidFile + ".stopped"); err != nil {
		t.Fatal("child was killed instead of receiving the forwarded interrupt")
	}
}

func TestWindowsJobHelper(t *testing.T) {
	switch os.Getenv("RECEIVER_JOB_TEST") {
	case "supervisor":
		var err error
		receiverJob, err = bindReceiverJob()
		if err != nil {
			os.Exit(2)
		}
		child := exec.Command(os.Args[0], "-test.run=^TestWindowsJobHelper$")
		child.Env = append(os.Environ(), "RECEIVER_JOB_TEST=interrupt-child")
		if runChild(child) == errUserStopped {
			os.Exit(0)
		}
		os.Exit(3)
	case "interrupt-child":
		interrupts := make(chan os.Signal, 1)
		signal.Notify(interrupts, os.Interrupt)
		os.WriteFile(os.Getenv("RECEIVER_JOB_PID"), []byte(fmt.Sprint(os.Getpid())), 0600)
		<-interrupts
		os.WriteFile(os.Getenv("RECEIVER_JOB_PID")+".stopped", []byte("stopped"), 0600)
		os.Exit(0)
	case "parent":
		var err error
		receiverJob, err = bindReceiverJob()
		if err != nil {
			os.Exit(2)
		}
		child := exec.Command(os.Args[0], "-test.run=^TestWindowsJobHelper$")
		child.Env = append(os.Environ(), "RECEIVER_JOB_TEST=child")
		if child.Start() != nil {
			os.Exit(3)
		}
		os.WriteFile(os.Getenv("RECEIVER_JOB_PID"), []byte(fmt.Sprint(child.Process.Pid)), 0600)
		child.Wait()
		os.Exit(4)
	case "child":
		time.Sleep(time.Minute)
		os.Exit(0)
	}
}
