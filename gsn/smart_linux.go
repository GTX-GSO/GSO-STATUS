//go:build linux

package main

import (
	"errors"
	"os"
	"syscall"
	"unsafe"
)

const sgIO = 0x2285

// sg_io_hdr from <scsi/sg.h>. Go's natural alignment matches the C struct on the 64-bit targets GSN is built for.
type sgIOHdr struct {
	interfaceID    int32
	dxferDirection int32
	cmdLen         uint8
	mxSbLen        uint8
	iovecCount     uint16
	dxferLen       uint32
	dxferp         unsafe.Pointer
	cmdp           unsafe.Pointer
	sbp            unsafe.Pointer
	timeout        uint32
	flags          uint32
	packID         int32
	usrPtr         unsafe.Pointer
	status         uint8
	maskedStatus   uint8
	msgStatus      uint8
	sbLenWr        uint8
	hostStatus     uint16
	driverStatus   uint16
	resid          int32
	duration       uint32
	info           uint32
}

// ataPassThrough sends one ATA PASS-THROUGH(16) command (SAT; libata implements it for SATA disks and most USB-SATA bridges do too).
func ataPassThrough(f *os.File, cdb [16]byte, data []byte) (sense [32]byte, hdr sgIOHdr, err error) {
	return ataPassThroughLen(f, cdb, data, 16)
}

func ataPassThroughLen(f *os.File, cdb [16]byte, data []byte, cdbLen uint8) (sense [32]byte, hdr sgIOHdr, err error) {
	hdr = sgIOHdr{interfaceID: 'S', dxferDirection: -1, cmdLen: cdbLen, mxSbLen: uint8(len(sense)), cmdp: unsafe.Pointer(&cdb[0]), sbp: unsafe.Pointer(&sense[0]), timeout: 5000}
	if len(data) > 0 {
		hdr.dxferDirection, hdr.dxferLen, hdr.dxferp = -3, uint32(len(data)), unsafe.Pointer(&data[0])
	}
	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, f.Fd(), sgIO, uintptr(unsafe.Pointer(&hdr))); errno != 0 {
		return sense, hdr, errno
	}
	if hdr.hostStatus != 0 || hdr.resid != 0 && len(data) > 0 {
		return sense, hdr, errors.New("pass-through failed")
	}
	return sense, hdr, nil
}

// checkPowerMode asks the drive whether it is spinning (ATA CHECK POWER MODE, which by specification does not wake it).
// The answer comes back as an ATA status return descriptor in the sense data: descriptor code 0x09 at byte 8, sector count
// (the power mode: 0x00 standby, 0x80 idle, 0xFF active) at byte 12. Bridges implement either the 16-byte or the 12-byte
// SAT command, so the second form is tried when the first gets no descriptor back.
func checkPowerMode(f *os.File) string {
	cdbs := [][16]byte{
		{0x85, 3 << 1, 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xE5, 0},
		{0xA1, 3 << 1, 0x20, 0, 0, 0, 0, 0, 0xA0, 0xE5},
	}
	for i, cdb := range cdbs {
		sense, hdr, err := ataPassThroughLen(f, cdb, nil, uint8(16-4*i))
		if err != nil || hdr.sbLenWr < 14 || (sense[0] != 0x72 && sense[0] != 0x73) || sense[8] != 0x09 {
			continue
		}
		if sense[12] == 0x00 {
			return diskStandby
		}
		return diskActive
	}
	return diskUnknown
}

// readDiskPower opens the device and returns its power state; it never wakes the drive.
func readDiskPower(device string) string {
	f, err := os.OpenFile(device, os.O_RDONLY, 0)
	if err != nil {
		return diskUnknown
	}
	defer f.Close()
	return checkPowerMode(f)
}

// smartFailing asks for the drive's own verdict (SMART RETURN STATUS): the drive puts 0x4F/0xC2 in the LBA registers when
// all is well and 0xF4/0x2C when a threshold has been exceeded. ok is false when the bridge does not pass the answer on.
func smartFailing(f *os.File) (failing, ok bool) {
	cdbs := [][16]byte{
		{0x85, 3 << 1, 0x20, 0, 0xDA, 0, 0, 0, 0, 0, 0x4F, 0, 0xC2, 0, 0xB0, 0},
		{0xA1, 3 << 1, 0x20, 0xDA, 0, 0, 0x4F, 0xC2, 0xA0, 0xB0},
	}
	for i, cdb := range cdbs {
		sense, hdr, err := ataPassThroughLen(f, cdb, nil, uint8(16-4*i))
		if err != nil || hdr.sbLenWr < 20 || (sense[0] != 0x72 && sense[0] != 0x73) || sense[8] != 0x09 {
			continue
		}
		switch mid, high := sense[16], sense[18]; {
		case mid == 0x4F && high == 0xC2:
			return false, true
		case mid == 0xF4 && high == 0x2C:
			return true, true
		}
	}
	return false, false
}

// readSmart reads the SMART attribute table with the read-only SMART READ DATA command, and the drive's own status with the
// non-data SMART RETURN STATUS. Callers only use it while the drive is spinning.
func readSmart(device string) (smartReading, error) {
	f, err := os.OpenFile(device, os.O_RDONLY, 0)
	if err != nil {
		return smartReading{}, err
	}
	defer f.Close()
	cdb := [16]byte{0x85, 4 << 1, 0x0e, 0, 0xD0, 0, 1, 0, 0, 0, 0x4F, 0, 0xC2, 0, 0xB0, 0}
	var data [512]byte
	if _, _, err := ataPassThrough(f, cdb, data[:]); err != nil {
		return smartReading{}, err
	}
	r, err := parseSmart(data[:])
	if err != nil {
		return r, err
	}
	r.health.failing, r.health.statusKnown = smartFailing(f)
	return r, nil
}
