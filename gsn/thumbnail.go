package main

import (
	"bytes"
	"compress/flate"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"image"
	"image/draw"
	"image/png"
	"io"
	"math"
	"regexp"
	"strconv"
)

// The model picture of the running job. The printer keeps the job's .3mf (a zip) in /cache; the pictures sit at the very
// start of the archive, in front of the large G-code, so the first part of the file is enough. The archive's central
// directory is at the end and cannot be read without downloading everything, so the local entry headers are walked in
// order instead. The picture is cropped to the model, scaled to the size the display draws it at, and sent as raw
// ARGB8888 (blue, green, red, alpha bytes, straight alpha) so the display needs no decoder and no scaling.
const (
	thumbnailSize   = 120
	thumbnailBytes  = thumbnailSize * thumbnailSize * 4
	thumbnailPrefix = 256 << 10 // how much of the archive is read while looking for the picture
	maxPicture      = 2 << 20   // compressed size of one picture
	maxPicturePx    = 4096
)

var (
	platePictureName = regexp.MustCompile(`^Metadata/plate_([0-9]+)\.png$`)
	platePattern     = regexp.MustCompile(`plate_([0-9]+)\.gcode`)
)

// plateNumber reads the plate from the printer's gcode_file ("/data/Metadata/plate_2.gcode"); 1 when it is not there.
func plateNumber(gcodeFile string) int {
	if m := platePattern.FindStringSubmatch(gcodeFile); m != nil {
		if n, err := strconv.Atoi(m[1]); err == nil && n > 0 && n < 1000 {
			return n
		}
	}
	return 1
}

func thumbnailKey(job string, plate int) string {
	sum := sha256.Sum256([]byte(job + "\x00" + strconv.Itoa(plate)))
	return hex.EncodeToString(sum[:8])
}

// zipPicture walks the local file headers at the start of a zip and returns the PNG named Metadata/plate_<plate>.png.
func zipPicture(prefix []byte, plate int) ([]byte, error) {
	want := "Metadata/plate_" + strconv.Itoa(plate) + ".png"
	for pos := 0; pos+30 <= len(prefix); {
		if binary.LittleEndian.Uint32(prefix[pos:]) != 0x04034b50 {
			break // central directory or anything else: no more entries in front
		}
		flags := binary.LittleEndian.Uint16(prefix[pos+6:])
		method := binary.LittleEndian.Uint16(prefix[pos+8:])
		compressed := int64(binary.LittleEndian.Uint32(prefix[pos+18:]))
		nameLen := int(binary.LittleEndian.Uint16(prefix[pos+26:]))
		extraLen := int(binary.LittleEndian.Uint16(prefix[pos+28:]))
		start := pos + 30 + nameLen + extraLen
		if start > len(prefix) {
			break
		}
		name := string(prefix[pos+30 : pos+30+nameLen])
		end := int64(start) + compressed
		descriptor := flags&8 != 0
		if descriptor {
			// sizes follow the data in a descriptor: find the signature whose recorded size matches the distance
			end = -1
			for i := start; i+16 <= len(prefix); i++ {
				if binary.LittleEndian.Uint32(prefix[i:]) == 0x08074b50 && int64(binary.LittleEndian.Uint32(prefix[i+8:])) == int64(i-start) {
					end = int64(i)
					break
				}
			}
			if end < 0 {
				if name == want {
					return nil, errors.New("thumbnail_beyond_prefix")
				}
				break // an entry that runs past the prefix (the G-code): nothing wanted lies behind it
			}
		}
		if end > int64(len(prefix)) {
			if name == want {
				return nil, errors.New("thumbnail_beyond_prefix")
			}
			break
		}
		if name == want {
			data := prefix[start:end]
			switch method {
			case 0:
				return data, nil
			case 8:
				out, err := io.ReadAll(io.LimitReader(flate.NewReader(bytes.NewReader(data)), maxPicture+1))
				if err != nil || len(out) > maxPicture {
					return nil, errors.New("thumbnail_bad_data")
				}
				return out, nil
			}
			return nil, errors.New("thumbnail_unsupported_method")
		}
		next := end
		if descriptor {
			next += 16 // signature, crc, two sizes
		}
		pos = int(next)
	}
	return nil, errors.New("thumbnail_not_found")
}

// renderThumbnail turns the printer's picture (a render of the plate on a transparent background) into thumbnailSize
// square pixels of the model alone: crop to the visible part, pad to a square, average down.
func renderThumbnail(data []byte) ([]byte, error) {
	cfg, err := png.DecodeConfig(bytes.NewReader(data))
	if err != nil || cfg.Width < 1 || cfg.Height < 1 || cfg.Width > maxPicturePx || cfg.Height > maxPicturePx {
		return nil, errors.New("thumbnail_bad_picture")
	}
	decoded, err := png.Decode(bytes.NewReader(data))
	if err != nil {
		return nil, errors.New("thumbnail_bad_picture")
	}
	b := decoded.Bounds()
	src := image.NewNRGBA(image.Rect(0, 0, b.Dx(), b.Dy()))
	draw.Draw(src, src.Bounds(), decoded, b.Min, draw.Src)
	minX, minY, maxX, maxY := b.Dx(), b.Dy(), -1, -1
	for y := 0; y < b.Dy(); y++ {
		for x := 0; x < b.Dx(); x++ {
			if src.NRGBAAt(x, y).A > 16 {
				minX, maxX = min(minX, x), max(maxX, x)
				minY, maxY = min(minY, y), max(maxY, y)
			}
		}
	}
	if maxX < 0 {
		return nil, errors.New("thumbnail_empty")
	}
	w, h := float64(maxX-minX+1), float64(maxY-minY+1)
	side := math.Max(w, h) * 1.12 // a little margin around the model
	left := float64(minX) + w/2 - side/2
	top := float64(minY) + h/2 - side/2
	step := side / thumbnailSize
	out := make([]byte, thumbnailBytes)
	for dy := 0; dy < thumbnailSize; dy++ {
		for dx := 0; dx < thumbnailSize; dx++ {
			x0, x1 := left+float64(dx)*step, left+float64(dx+1)*step
			y0, y1 := top+float64(dy)*step, top+float64(dy+1)*step
			var r, g, bl, a, n float64
			for sy := int(math.Floor(y0)); sy < max(int(math.Ceil(y1)), int(math.Floor(y0))+1); sy++ {
				for sx := int(math.Floor(x0)); sx < max(int(math.Ceil(x1)), int(math.Floor(x0))+1); sx++ {
					n++
					if sx < 0 || sy < 0 || sx >= src.Bounds().Dx() || sy >= src.Bounds().Dy() {
						continue
					}
					p := src.NRGBAAt(sx, sy)
					alpha := float64(p.A)
					r += float64(p.R) * alpha
					g += float64(p.G) * alpha
					bl += float64(p.B) * alpha
					a += alpha
				}
			}
			o := (dy*thumbnailSize + dx) * 4
			if a > 0 {
				out[o], out[o+1], out[o+2] = byte(math.Round(bl/a)), byte(math.Round(g/a)), byte(math.Round(r/a))
				out[o+3] = byte(math.Round(a / n))
			}
		}
	}
	return out, nil
}
