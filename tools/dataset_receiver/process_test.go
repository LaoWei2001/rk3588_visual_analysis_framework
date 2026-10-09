package main

import (
	"errors"
	"os"
	"testing"
	"time"
)

func TestChildNormalExitPreservesResult(t *testing.T) {
	for _, result := range []error{nil, errors.New("startup failed")} {
		done := make(chan error, 1)
		done <- result
		unexpected := func() error { t.Fatal("normal exit triggered cancellation"); return nil }
		if actual := waitForChild(done, make(chan os.Signal), unexpected, unexpected, time.Hour); actual != result {
			t.Fatalf("result lost: %v", actual)
		}
	}
}

func TestChildCancellation(t *testing.T) {
	for _, scenario := range []string{"graceful", "timeout", "second-interrupt", "forward-failed"} {
		t.Run(scenario, func(t *testing.T) {
			done := make(chan error, 1)
			interrupts := make(chan os.Signal, 2)
			interrupts <- os.Interrupt
			grace := time.Hour
			if scenario == "timeout" {
				grace = 5 * time.Millisecond
			}
			if scenario == "second-interrupt" {
				interrupts <- os.Interrupt
			}
			forwards, kills := 0, 0
			forward := func() error {
				forwards++
				if scenario == "graceful" {
					done <- nil
				}
				if scenario == "forward-failed" {
					return errors.New("console unavailable")
				}
				return nil
			}
			kill := func() error { kills++; done <- errors.New("terminated"); return nil }
			if err := waitForChild(done, interrupts, forward, kill, grace); !errors.Is(err, errUserStopped) {
				t.Fatalf("cancellation reported as error: %v", err)
			}
			expectedKills := 1
			if scenario == "graceful" {
				expectedKills = 0
			}
			if forwards != 1 || kills != expectedKills {
				t.Fatalf("forwarded %d times, killed %d times", forwards, kills)
			}
		})
	}
}

func TestUnresponsiveChildCannotHoldLauncherOpen(t *testing.T) {
	interrupts := make(chan os.Signal, 1)
	interrupts <- os.Interrupt
	start := time.Now()
	failed := func() error { return errors.New("child unavailable") }
	if err := waitForChild(make(chan error), interrupts, failed, failed, time.Hour); !errors.Is(err, errUserStopped) {
		t.Fatal(err)
	}
	if time.Since(start) > 3*time.Second {
		t.Fatal("launcher remained stuck after cancellation")
	}
}

func TestSimultaneousChildExitAndInterruptIsNormalStop(t *testing.T) {
	done := make(chan error, 1)
	done <- errors.New("console closed child")
	interrupts := make(chan os.Signal, 1)
	interrupts <- os.Interrupt
	noop := func() error { return nil }
	if err := waitForChild(done, interrupts, noop, noop, time.Hour); !errors.Is(err, errUserStopped) {
		t.Fatalf("user stop would display startup error and wait for Enter: %v", err)
	}
}

func TestConsoleCloseWaitsForChildCleanup(t *testing.T) {
	interrupts := make(chan os.Signal, 1)
	stopped := make(chan struct{})
	returned := make(chan struct{})
	go func() {
		waitForConsoleClose(interrupts, stopped, time.Second)
		close(returned)
	}()
	select {
	case <-interrupts:
	case <-time.After(time.Second):
		t.Fatal("console close did not request child shutdown")
	}
	select {
	case <-returned:
		t.Fatal("launcher would kill the child before disconnect cleanup")
	default:
	}
	close(stopped)
	select {
	case <-returned:
	case <-time.After(time.Second):
		t.Fatal("console close kept waiting after child cleanup")
	}
}

func TestConsoleCloseHasBoundedWaitEvenWithFullSignalQueue(t *testing.T) {
	interrupts := make(chan os.Signal, 1)
	interrupts <- os.Interrupt
	returned := make(chan struct{})
	go func() {
		waitForConsoleClose(interrupts, make(chan struct{}), 5*time.Millisecond)
		close(returned)
	}()
	select {
	case <-returned:
	case <-time.After(time.Second):
		t.Fatal("console close blocked indefinitely")
	}
}
