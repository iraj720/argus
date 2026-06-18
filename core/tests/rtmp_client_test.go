package tests

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

type rtmpTestServer struct {
	cmd        *exec.Cmd
	log        bytes.Buffer
	outputRoot string
	port       string
}

func startRTMPTestServer(t *testing.T) *rtmpTestServer {
	t.Helper()

	repoRoot := findRepoRoot(t)
	engineRoot := filepath.Join(repoRoot, "argus")
	binary := buildServerBinaryCached(t, engineRoot)
	outputRoot := t.TempDir()
	port := reservePort(t)

	cmd := exec.Command(
		binary,
		"--server", "rtmp",
		"--host", "127.0.0.1",
		"--port", port,
		"--output-root", outputRoot,
	)
	server := &rtmpTestServer{
		cmd:        cmd,
		outputRoot: outputRoot,
		port:       port,
	}
	cmd.Stdout = &server.log
	cmd.Stderr = &server.log
	if err := cmd.Start(); err != nil {
		t.Fatalf("start RTMP server: %v", err)
	}
	t.Cleanup(func() {
		server.stop(t)
	})

	waitForTCP(t, "127.0.0.1:"+port, 10*time.Second)
	return server
}

func (s *rtmpTestServer) stop(t *testing.T) {
	t.Helper()
	if s.cmd == nil || s.cmd.Process == nil {
		return
	}
	_ = s.cmd.Process.Kill()
	_, _ = s.cmd.Process.Wait()
	s.cmd.Process = nil
}

func (s *rtmpTestServer) addr() string {
	return "127.0.0.1:" + s.port
}

func (s *rtmpTestServer) logs() string {
	return s.log.String()
}

var (
	serverBinaryOnce sync.Once
	serverBinaryPath string
	serverBinaryErr  error
)

func buildServerBinaryCached(t *testing.T, engineRoot string) string {
	t.Helper()

	serverBinaryOnce.Do(func() {
		buildDir, err := os.MkdirTemp("", "argus-build-*")
		if err != nil {
			serverBinaryErr = err
			return
		}
		if err := runCommand(exec.Command("cmake", "-S", engineRoot, "-B", buildDir)); err != nil {
			serverBinaryErr = err
			return
		}
		if err := runCommand(exec.Command("cmake", "--build", buildDir, "--target", "argus")); err != nil {
			serverBinaryErr = err
			return
		}
		serverBinaryPath = filepath.Join(buildDir, "argus")
	})

	if serverBinaryErr != nil {
		t.Fatalf("build argus: %v", serverBinaryErr)
	}
	return serverBinaryPath
}

func runCommand(cmd *exec.Cmd) error {
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("%v: %w\n%s", cmd.Args, err, string(out))
	}
	return nil
}

type rtmpMessage struct {
	typeID          uint8
	timestampMS     uint32
	messageStreamID uint32
	payload         []byte
}

type rtmpChunkState struct {
	initialized     bool
	timestamp       uint32
	timestampDelta  uint32
	messageLength   uint32
	messageTypeID   uint8
	messageStreamID uint32
	bytesRead       uint32
	payload         []byte
}

type rtmpClient struct {
	conn           net.Conn
	inChunkSize    uint32
	outChunkSize   uint32
	inboundStreams map[uint32]*rtmpChunkState
}

func newRTMPClient(t *testing.T, addr string) *rtmpClient {
	t.Helper()

	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		t.Fatalf("dial RTMP server: %v", err)
	}
	client := &rtmpClient{
		conn:           conn,
		inChunkSize:    128,
		outChunkSize:   128,
		inboundStreams: make(map[uint32]*rtmpChunkState),
	}
	t.Cleanup(func() {
		_ = client.Close()
	})
	return client
}

func (c *rtmpClient) Close() error {
	if c.conn == nil {
		return nil
	}
	err := c.conn.Close()
	c.conn = nil
	return err
}

func (c *rtmpClient) Handshake() error {
	c1 := make([]byte, 1536)
	for i := range c1 {
		c1[i] = byte(i % 251)
	}
	if _, err := c.conn.Write(append([]byte{0x03}, c1...)); err != nil {
		return err
	}

	reply := make([]byte, 1+1536+1536)
	if _, err := io.ReadFull(c.conn, reply); err != nil {
		return err
	}
	if reply[0] != 0x03 {
		return fmt.Errorf("unexpected S0 version: %d", reply[0])
	}
	if !bytes.Equal(reply[1+1536:], c1) {
		return errors.New("S2 does not echo C1")
	}
	if _, err := c.conn.Write(reply[1 : 1+1536]); err != nil {
		return err
	}
	return nil
}

func (c *rtmpClient) SetChunkSize(size uint32) error {
	payload := make([]byte, 4)
	binary.BigEndian.PutUint32(payload, size)
	if err := c.writeMessage(2, 1, 0, 0, payload); err != nil {
		return err
	}
	c.outChunkSize = size
	return nil
}

func (c *rtmpClient) Connect(app string, extra map[string]any) error {
	object := amfObject{
		"app":           app,
		"flashVer":      "FMLE/3.0 (compatible; Lavf)",
		"tcUrl":         "rtmp://127.0.0.1/" + app,
		"fpad":          false,
		"capabilities":  15.0,
		"audioCodecs":   4095.0,
		"videoCodecs":   252.0,
		"videoFunction": 1.0,
	}
	for key, value := range extra {
		object[key] = value
	}
	return c.sendCommand(3, 20, 0, 0, "connect", 1, object)
}

func (c *rtmpClient) ConnectMinimal(app string) error {
	return c.sendCommand(3, 20, 0, 0, "connect", 1, amfObject{
		"app": app,
	})
}

func (c *rtmpClient) ReleaseStream(stream string) error {
	return c.sendCommand(3, 20, 0, 0, "releaseStream", 2, nil, stream)
}

func (c *rtmpClient) FCPublish(stream string) error {
	return c.sendCommand(3, 20, 0, 0, "FCPublish", 3, nil, stream)
}

func (c *rtmpClient) CreateStream() error {
	return c.sendCommand(3, 20, 0, 0, "createStream", 4, nil)
}

func (c *rtmpClient) Publish(stream string) error {
	return c.sendCommand(8, 20, 1, 0, "publish", 5, nil, stream, "live")
}

func (c *rtmpClient) UnknownCommand(name string, txid float64) error {
	return c.sendCommand(3, 20, 0, 0, name, txid, nil)
}

func (c *rtmpClient) sendMalformedCommand() error {
	payload := encodeAMF0Number(7)
	return c.writeMessage(3, 20, 0, 0, payload)
}

func (c *rtmpClient) sendCommand(csid uint32, typeID uint8, messageStreamID uint32, timestampMS uint32, name string, txid float64, values ...any) error {
	var payload []byte
	payload = append(payload, encodeAMF0String(name)...)
	payload = append(payload, encodeAMF0Number(txid)...)
	for _, value := range values {
		payload = append(payload, encodeAMF0Value(value)...)
	}
	return c.writeMessage(csid, typeID, messageStreamID, timestampMS, payload)
}

func (c *rtmpClient) writeMessage(csid uint32, typeID uint8, messageStreamID uint32, timestampMS uint32, payload []byte) error {
	if csid >= 64 {
		return fmt.Errorf("extended csid not supported in tests: %d", csid)
	}

	header := make([]byte, 12)
	header[0] = byte(csid)
	writeBE24(header[1:4], timestampMS)
	writeBE24(header[4:7], uint32(len(payload)))
	header[7] = typeID
	binary.LittleEndian.PutUint32(header[8:12], messageStreamID)

	if _, err := c.conn.Write(header); err != nil {
		return err
	}
	offset := uint32(0)
	for offset < uint32(len(payload)) {
		chunkLen := minUint32(uint32(len(payload))-offset, c.outChunkSize)
		if _, err := c.conn.Write(payload[offset : offset+chunkLen]); err != nil {
			return err
		}
		offset += chunkLen
		if offset < uint32(len(payload)) {
			if _, err := c.conn.Write([]byte{byte(0xc0 | csid)}); err != nil {
				return err
			}
		}
	}
	return nil
}

func (c *rtmpClient) ReadMessage(timeout time.Duration) (*rtmpMessage, error) {
	if err := c.conn.SetReadDeadline(time.Now().Add(timeout)); err != nil {
		return nil, err
	}
	msg, err := c.readMessage()
	if err != nil {
		return nil, err
	}
	if msg.typeID == 1 && len(msg.payload) >= 4 {
		c.inChunkSize = binary.BigEndian.Uint32(msg.payload[:4])
	}
	return msg, nil
}

func (c *rtmpClient) readMessage() (*rtmpMessage, error) {
	for {
		fmtType, csid, err := c.readBasicHeader()
		if err != nil {
			return nil, err
		}
		stream := c.inboundStreams[csid]
		if stream == nil {
			stream = &rtmpChunkState{}
			c.inboundStreams[csid] = stream
		}

		headerSize := 0
		switch fmtType {
		case 0:
			headerSize = 11
		case 1:
			headerSize = 7
		case 2:
			headerSize = 3
		case 3:
			headerSize = 0
		default:
			return nil, fmt.Errorf("unexpected fmt: %d", fmtType)
		}

		header := make([]byte, headerSize)
		if _, err := io.ReadFull(c.conn, header); err != nil {
			return nil, err
		}

		var headerTimestamp uint32
		if headerSize >= 3 {
			headerTimestamp = readBE24(header[:3])
		}

		switch fmtType {
		case 0:
			stream.timestamp = headerTimestamp
			stream.timestampDelta = 0
			stream.messageLength = readBE24(header[3:6])
			stream.messageTypeID = header[6]
			stream.messageStreamID = binary.LittleEndian.Uint32(header[7:11])
			stream.bytesRead = 0
			stream.initialized = true
			stream.payload = make([]byte, stream.messageLength)
		case 1:
			if !stream.initialized {
				return nil, errors.New("chunk stream used before initialization")
			}
			stream.timestampDelta = headerTimestamp
			stream.timestamp += stream.timestampDelta
			stream.messageLength = readBE24(header[3:6])
			stream.messageTypeID = header[6]
			stream.bytesRead = 0
			stream.payload = make([]byte, stream.messageLength)
		case 2:
			if !stream.initialized {
				return nil, errors.New("chunk stream used before initialization")
			}
			stream.timestampDelta = headerTimestamp
			stream.timestamp += stream.timestampDelta
			stream.bytesRead = 0
		case 3:
			if !stream.initialized {
				return nil, errors.New("continuation before initialization")
			}
			if stream.bytesRead >= stream.messageLength {
				stream.bytesRead = 0
				stream.timestamp += stream.timestampDelta
			}
		}

		if headerTimestamp == 0xffffff && fmtType <= 2 {
			ext := make([]byte, 4)
			if _, err := io.ReadFull(c.conn, ext); err != nil {
				return nil, err
			}
			stream.timestamp = binary.BigEndian.Uint32(ext)
		}

		toRead := minUint32(stream.messageLength-stream.bytesRead, c.inChunkSize)
		if _, err := io.ReadFull(c.conn, stream.payload[stream.bytesRead:stream.bytesRead+toRead]); err != nil {
			return nil, err
		}
		stream.bytesRead += toRead
		if stream.bytesRead < stream.messageLength {
			continue
		}

		payload := make([]byte, len(stream.payload))
		copy(payload, stream.payload)
		return &rtmpMessage{
			typeID:          stream.messageTypeID,
			timestampMS:     stream.timestamp,
			messageStreamID: stream.messageStreamID,
			payload:         payload,
		}, nil
	}
}

func (c *rtmpClient) readBasicHeader() (uint8, uint32, error) {
	var first [1]byte
	if _, err := io.ReadFull(c.conn, first[:]); err != nil {
		return 0, 0, err
	}
	fmtType := first[0] >> 6
	rawCSID := uint32(first[0] & 0x3f)
	switch rawCSID {
	case 0:
		var second [1]byte
		if _, err := io.ReadFull(c.conn, second[:]); err != nil {
			return 0, 0, err
		}
		return fmtType, uint32(second[0]) + 64, nil
	case 1:
		var ext [2]byte
		if _, err := io.ReadFull(c.conn, ext[:]); err != nil {
			return 0, 0, err
		}
		return fmtType, uint32(ext[0]) + uint32(ext[1])*256 + 64, nil
	default:
		return fmtType, rawCSID, nil
	}
}

type amfObject map[string]any

func encodeAMF0Value(value any) []byte {
	switch typed := value.(type) {
	case nil:
		return []byte{0x05}
	case string:
		return encodeAMF0String(typed)
	case float64:
		return encodeAMF0Number(typed)
	case int:
		return encodeAMF0Number(float64(typed))
	case bool:
		if typed {
			return []byte{0x01, 0x01}
		}
		return []byte{0x01, 0x00}
	case amfObject:
		var out []byte
		out = append(out, 0x03)
		for key, field := range typed {
			out = appendAMF0ObjectProperty(out, key, field)
		}
		out = append(out, 0x00, 0x00, 0x09)
		return out
	default:
		panic(fmt.Sprintf("unsupported AMF value type: %T", value))
	}
}

func encodeAMF0String(value string) []byte {
	out := make([]byte, 3+len(value))
	out[0] = 0x02
	binary.BigEndian.PutUint16(out[1:3], uint16(len(value)))
	copy(out[3:], value)
	return out
}

func encodeAMF0Number(value float64) []byte {
	out := make([]byte, 9)
	out[0] = 0x00
	binary.BigEndian.PutUint64(out[1:], math.Float64bits(value))
	return out
}

func appendAMF0ObjectProperty(dst []byte, key string, value any) []byte {
	keyLen := make([]byte, 2)
	binary.BigEndian.PutUint16(keyLen, uint16(len(key)))
	dst = append(dst, keyLen...)
	dst = append(dst, key...)
	dst = append(dst, encodeAMF0Value(value)...)
	return dst
}

func decodeAMF0Values(payload []byte) ([]any, error) {
	reader := bytes.NewReader(payload)
	var values []any
	for reader.Len() > 0 {
		value, err := decodeAMF0Value(reader)
		if err != nil {
			return nil, err
		}
		values = append(values, value)
	}
	return values, nil
}

func decodeAMF0Value(reader *bytes.Reader) (any, error) {
	marker, err := reader.ReadByte()
	if err != nil {
		return nil, err
	}
	switch marker {
	case 0x00:
		var raw uint64
		if err := binary.Read(reader, binary.BigEndian, &raw); err != nil {
			return nil, err
		}
		return math.Float64frombits(raw), nil
	case 0x01:
		flag, err := reader.ReadByte()
		if err != nil {
			return nil, err
		}
		return flag != 0, nil
	case 0x02:
		var size uint16
		if err := binary.Read(reader, binary.BigEndian, &size); err != nil {
			return nil, err
		}
		data := make([]byte, size)
		if _, err := io.ReadFull(reader, data); err != nil {
			return nil, err
		}
		return string(data), nil
	case 0x03:
		object := map[string]any{}
		for {
			var keySize uint16
			if err := binary.Read(reader, binary.BigEndian, &keySize); err != nil {
				return nil, err
			}
			if keySize == 0 {
				endMarker, err := reader.ReadByte()
				if err != nil {
					return nil, err
				}
				if endMarker == 0x09 {
					return object, nil
				}
				return nil, fmt.Errorf("unexpected object terminator: 0x%x", endMarker)
			}
			key := make([]byte, keySize)
			if _, err := io.ReadFull(reader, key); err != nil {
				return nil, err
			}
			value, err := decodeAMF0Value(reader)
			if err != nil {
				return nil, err
			}
			object[string(key)] = value
		}
	case 0x05:
		return nil, nil
	default:
		return nil, fmt.Errorf("unsupported AMF marker: 0x%x", marker)
	}
}

func readBE24(src []byte) uint32 {
	return uint32(src[0])<<16 | uint32(src[1])<<8 | uint32(src[2])
}

func writeBE24(dst []byte, value uint32) {
	dst[0] = byte(value >> 16)
	dst[1] = byte(value >> 8)
	dst[2] = byte(value)
}

func minUint32(a uint32, b uint32) uint32 {
	if a < b {
		return a
	}
	return b
}
