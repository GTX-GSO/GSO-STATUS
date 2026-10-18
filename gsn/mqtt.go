package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"time"
)

// The smallest MQTT 3.1.1 client the printer source needs: connect with a user name and password, subscribe at QoS 0,
// receive PUBLISH packets (acknowledging QoS 1), publish at QoS 0 and keep the session alive. No dependency is pulled in
// for it; the printer speaks plain MQTT over TLS and nothing else is needed.

const (
	mqttConnect     = 1
	mqttConnack     = 2
	mqttPublish     = 3
	mqttPuback      = 4
	mqttSubscribe   = 8
	mqttSuback      = 9
	mqttPingreq     = 12
	mqttPingresp    = 13
	mqttMaxPacket   = 2 << 20 // a full printer report is tens of KB; anything beyond 2 MiB is not one
	mqttKeepAliveS  = 30
	mqttWriteTimout = 5 * time.Second
)

var errMQTTRefused = errors.New("mqtt_connection_refused")

type mqttPacket struct {
	kind    byte
	flags   byte
	payload []byte
}

type mqttConn struct {
	c   net.Conn
	r   *bufio.Reader
	id  uint16
	wmu sync.Mutex // the keep-alive goroutine and the read loop both write
}

func newMQTTConn(c net.Conn) *mqttConn { return &mqttConn{c: c, r: bufio.NewReaderSize(c, 16<<10)} }

func mqttString(b []byte, s string) []byte {
	return append(append(b, byte(len(s)>>8), byte(len(s))), s...)
}

func mqttLength(n int) []byte {
	var out []byte
	for {
		d := byte(n % 128)
		n /= 128
		if n > 0 {
			d |= 0x80
		}
		out = append(out, d)
		if n == 0 {
			return out
		}
	}
}

func (m *mqttConn) write(kind, flags byte, body []byte) error {
	packet := append([]byte{kind<<4 | flags}, mqttLength(len(body))...)
	packet = append(packet, body...)
	m.wmu.Lock()
	defer m.wmu.Unlock()
	m.c.SetWriteDeadline(time.Now().Add(mqttWriteTimout))
	_, err := m.c.Write(packet)
	return err
}

// read returns the next packet; the caller sets the read deadline.
func (m *mqttConn) read() (mqttPacket, error) {
	first, err := m.r.ReadByte()
	if err != nil {
		return mqttPacket{}, err
	}
	length, shift := 0, 0
	for i := 0; ; i++ {
		d, err := m.r.ReadByte()
		if err != nil {
			return mqttPacket{}, err
		}
		length |= int(d&0x7f) << shift
		if d&0x80 == 0 {
			break
		}
		if shift += 7; i == 3 {
			return mqttPacket{}, errors.New("mqtt_bad_length")
		}
	}
	if length > mqttMaxPacket {
		return mqttPacket{}, errors.New("mqtt_packet_too_large")
	}
	body := make([]byte, length)
	if _, err = io.ReadFull(m.r, body); err != nil {
		return mqttPacket{}, err
	}
	return mqttPacket{kind: first >> 4, flags: first & 0x0f, payload: body}, nil
}

// connect performs CONNECT/CONNACK. A refused login reports errMQTTRefused with the broker's return code.
func (m *mqttConn) connect(clientID, user, password string, timeout time.Duration) error {
	body := mqttString(nil, "MQTT")
	body = append(body, 4, 0xC2, 0, mqttKeepAliveS)
	body = mqttString(body, clientID)
	body = mqttString(body, user)
	body = mqttString(body, password)
	if err := m.write(mqttConnect, 0, body); err != nil {
		return err
	}
	m.c.SetReadDeadline(time.Now().Add(timeout))
	p, err := m.read()
	if err != nil {
		return err
	}
	if p.kind != mqttConnack || len(p.payload) != 2 {
		return errors.New("mqtt_unexpected_reply")
	}
	if p.payload[1] != 0 {
		return fmt.Errorf("%w: %d", errMQTTRefused, p.payload[1])
	}
	return nil
}

func (m *mqttConn) subscribe(filter string, timeout time.Duration) error {
	m.id++
	body := mqttString([]byte{byte(m.id >> 8), byte(m.id)}, filter)
	body = append(body, 0)
	if err := m.write(mqttSubscribe, 2, body); err != nil {
		return err
	}
	deadline := time.Now().Add(timeout)
	for {
		m.c.SetReadDeadline(deadline)
		p, err := m.read()
		if err != nil {
			return err
		}
		if p.kind == mqttSuback {
			if len(p.payload) < 3 || p.payload[2] == 0x80 {
				return errors.New("mqtt_subscribe_refused")
			}
			return nil
		}
		// A retained or early PUBLISH can arrive before SUBACK; it is dropped here, the printer repeats its state.
	}
}

func (m *mqttConn) publish(topic string, payload []byte) error {
	return m.write(mqttPublish, 0, append(mqttString(nil, topic), payload...))
}

func (m *mqttConn) ping() error { return m.write(mqttPingreq, 0, nil) }

// message splits a PUBLISH packet, acknowledging QoS 1.
func (m *mqttConn) message(p mqttPacket) (topic string, payload []byte, ok bool) {
	if p.kind != mqttPublish || len(p.payload) < 2 {
		return "", nil, false
	}
	n := int(p.payload[0])<<8 | int(p.payload[1])
	rest := p.payload[2:]
	if len(rest) < n {
		return "", nil, false
	}
	topic, rest = string(rest[:n]), rest[n:]
	if qos := (p.flags >> 1) & 3; qos > 0 {
		if len(rest) < 2 {
			return "", nil, false
		}
		if qos == 1 {
			m.write(mqttPuback, 0, rest[:2])
		}
		rest = rest[2:]
	}
	return topic, rest, true
}
