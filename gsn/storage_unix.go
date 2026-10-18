//go:build !windows

package main

import "syscall"

// Capacity of the filesystem that holds path, as seen by an unprivileged process.
func diskUsage(path string) (total, free uint64, err error) {
	var st syscall.Statfs_t
	if err = syscall.Statfs(path, &st); err != nil {
		return 0, 0, err
	}
	size := uint64(st.Bsize)
	return uint64(st.Blocks) * size, uint64(st.Bavail) * size, nil
}
