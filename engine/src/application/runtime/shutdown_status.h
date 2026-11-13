#pragma once

// Low frequency lifecycle reporting only; never called from a signal handler.
void shutdown_status_begin();
void shutdown_status_stage(const char *stage);
void shutdown_status_complete(int exit_code, unsigned long persistence_errors = 0);
