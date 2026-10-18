//go:build !linux

package main

import "errors"

func readDiskPower(string) string { return diskUnknown }

func readSmart(string) (smartReading, error) {
	return smartReading{}, errors.New("disk temperature needs Linux")
}
