package main

import (
	"errors"
	"os"
	"time"
)

var errUserStopped = errors.New("接收已停止")

// Keep the console-close handler alive while the child reports its disconnect.
// Returning immediately lets Windows kill the launcher and its whole Job Object.
func waitForConsoleClose(interrupts chan<- os.Signal, stopped <-chan struct{}, timeout time.Duration) {
	select {
	case interrupts <- os.Interrupt:
	default:
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case <-stopped:
	case <-timer.C:
	}
}

// Wait for the child normally; on cancellation, allow graceful cleanup, then
// force termination. The launcher must not hang on a stuck network request.
func waitForChild(done <-chan error, interrupts <-chan os.Signal, interrupt func() error, kill func() error, grace time.Duration) error {
	select {
	case err := <-done:
		select {
		case <-interrupts:
			return errUserStopped // Concurrent console close is not a startup failure.
		default:
			return err
		}
	case <-interrupts:
	}
	if interrupt() == nil {
		timer := time.NewTimer(grace)
		defer timer.Stop()
		select {
		case <-done:
			return errUserStopped
		case <-interrupts: // A second Ctrl+C forces immediate exit.
		case <-timer.C:
		}
	}
	_ = kill()
	// Windows Job Object cleanup on launcher exit is the final fallback even
	// if Wait or termination fails. Keep total shutdown below the close timeout.
	timer := time.NewTimer(time.Second)
	defer timer.Stop()
	select {
	case <-done:
	case <-timer.C:
	}
	return errUserStopped
}
