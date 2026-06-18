package tests

import (
	"encoding/binary"
	"errors"
	"io"
	"net"
	"sync"
	"testing"
	"time"
)

func TestRTMPHandshakeRejectsInvalidVersion(t *testing.T) {
	server := startRTMPTestServer(t)
	conn, err := net.DialTimeout("tcp", server.addr(), 5*time.Second)
	if err != nil {
		t.Fatalf("dial RTMP server: %v", err)
	}
	defer conn.Close()

	if err := conn.SetDeadline(time.Now().Add(2 * time.Second)); err != nil {
		t.Fatalf("set deadline: %v", err)
	}
	if _, err := conn.Write([]byte{0x01}); err != nil {
		t.Fatalf("write invalid version: %v", err)
	}

	var buf [1]byte
	_, err = conn.Read(buf[:])
	if err == nil {
		t.Fatalf("expected server to close invalid handshake connection")
	}
}

func TestRTMPConnectReturnsExpectedControlAndResult(t *testing.T) {
	server := startRTMPTestServer(t)
	client := newRTMPClient(t, server.addr())
	if err := client.Handshake(); err != nil {
		t.Fatalf("handshake: %v", err)
	}
	if err := client.Connect("live", nil); err != nil {
		t.Fatalf("connect command: %v", err)
	}

	windowAck := mustReadRTMPMessage(t, client, server, 2*time.Second)
	if windowAck.typeID != 5 {
		t.Fatalf("expected Window Acknowledgement Size, got type=%d", windowAck.typeID)
	}
	if got := binary.BigEndian.Uint32(windowAck.payload); got != 5000000 {
		t.Fatalf("unexpected window ack size: %d", got)
	}

	peerBandwidth := mustReadRTMPMessage(t, client, server, 2*time.Second)
	if peerBandwidth.typeID != 6 {
		t.Fatalf("expected Set Peer Bandwidth, got type=%d", peerBandwidth.typeID)
	}
	if got := binary.BigEndian.Uint32(peerBandwidth.payload[:4]); got != 5000000 {
		t.Fatalf("unexpected peer bandwidth size: %d", got)
	}
	if peerBandwidth.payload[4] != 2 {
		t.Fatalf("unexpected peer bandwidth limit type: %d", peerBandwidth.payload[4])
	}

	chunkSize := mustReadRTMPMessage(t, client, server, 2*time.Second)
	if chunkSize.typeID != 1 {
		t.Fatalf("expected Set Chunk Size, got type=%d", chunkSize.typeID)
	}
	if got := binary.BigEndian.Uint32(chunkSize.payload); got != 4096 {
		t.Fatalf("unexpected outbound chunk size: %d", got)
	}

	connectResult := mustReadRTMPMessage(t, client, server, 2*time.Second)
	if connectResult.typeID != 20 {
		t.Fatalf("expected connect command result, got type=%d", connectResult.typeID)
	}
	values := mustDecodeAMF0Values(t, connectResult.payload)
	if got := values[0]; got != "_result" {
		t.Fatalf("unexpected command name: %#v", got)
	}
	if got := values[1]; got != float64(1) {
		t.Fatalf("unexpected transaction id: %#v", got)
	}
	status, ok := values[3].(map[string]any)
	if !ok {
		t.Fatalf("unexpected status object: %#v", values[3])
	}
	if status["code"] != "NetConnection.Connect.Success" {
		t.Fatalf("unexpected connect status code: %#v", status["code"])
	}
}

func TestRTMPPublishRequiresConnectAndCreateStream(t *testing.T) {
	t.Run("publish before connect", func(t *testing.T) {
		server := startRTMPTestServer(t)
		client := newRTMPClient(t, server.addr())
		if err := client.Handshake(); err != nil {
			t.Fatalf("handshake: %v", err)
		}
		if err := client.Publish("test"); err != nil {
			t.Fatalf("publish command: %v", err)
		}
		expectRTMPSessionClose(t, client, server)
	})

	t.Run("publish before createStream", func(t *testing.T) {
		server := startRTMPTestServer(t)
		client := newRTMPClient(t, server.addr())
		if err := client.Handshake(); err != nil {
			t.Fatalf("handshake: %v", err)
		}
		if err := client.Connect("live", nil); err != nil {
			t.Fatalf("connect command: %v", err)
		}
		mustDrainConnectResponses(t, client, server)
		if err := client.Publish("test"); err != nil {
			t.Fatalf("publish command: %v", err)
		}
		expectRTMPSessionClose(t, client, server)
	})
}

func TestRTMPPublishFlowReturnsExpectedStatuses(t *testing.T) {
	requireBinary(t, "ffmpeg")

	server := startRTMPTestServer(t)
	client := newRTMPClient(t, server.addr())
	if err := client.Handshake(); err != nil {
		t.Fatalf("handshake: %v", err)
	}
	if err := client.Connect("live", nil); err != nil {
		t.Fatalf("connect command: %v", err)
	}
	mustDrainConnectResponses(t, client, server)
	if err := client.ReleaseStream("test"); err != nil {
		t.Fatalf("releaseStream: %v", err)
	}
	if err := client.FCPublish("test"); err != nil {
		t.Fatalf("FCPublish: %v", err)
	}

	fcPublish := mustReadRTMPMessage(t, client, server, 2*time.Second)
	fcValues := mustDecodeAMF0Values(t, fcPublish.payload)
	if fcValues[0] != "onFCPublish" {
		t.Fatalf("unexpected FCPublish response: %#v", fcValues)
	}

	if err := client.CreateStream(); err != nil {
		t.Fatalf("createStream: %v", err)
	}
	createStream := mustReadRTMPMessage(t, client, server, 2*time.Second)
	createValues := mustDecodeAMF0Values(t, createStream.payload)
	if createValues[0] != "_result" || createValues[3] != float64(1) {
		t.Fatalf("unexpected createStream response: %#v", createValues)
	}

	if err := client.Publish("test"); err != nil {
		t.Fatalf("publish: %v", err)
	}
	streamBegin := mustReadRTMPMessage(t, client, server, 2*time.Second)
	if streamBegin.typeID != 4 {
		t.Fatalf("expected StreamBegin user control, got type=%d", streamBegin.typeID)
	}
	if event := binary.BigEndian.Uint16(streamBegin.payload[:2]); event != 0 {
		t.Fatalf("unexpected user control event type: %d", event)
	}
	if streamID := binary.BigEndian.Uint32(streamBegin.payload[2:6]); streamID != 1 {
		t.Fatalf("unexpected publish stream id: %d", streamID)
	}

	publishStatus := mustReadRTMPMessage(t, client, server, 2*time.Second)
	statusValues := mustDecodeAMF0Values(t, publishStatus.payload)
	if statusValues[0] != "onStatus" {
		t.Fatalf("unexpected publish status command: %#v", statusValues)
	}
	status, ok := statusValues[3].(map[string]any)
	if !ok {
		t.Fatalf("unexpected publish status payload: %#v", statusValues[3])
	}
	if status["code"] != "NetStream.Publish.Start" {
		t.Fatalf("unexpected publish status code: %#v", status["code"])
	}
}

func TestRTMPUnknownCommandDoesNotBreakSession(t *testing.T) {
	server := startRTMPTestServer(t)
	client := newRTMPClient(t, server.addr())
	if err := client.Handshake(); err != nil {
		t.Fatalf("handshake: %v", err)
	}
	if err := client.Connect("live", nil); err != nil {
		t.Fatalf("connect command: %v", err)
	}
	mustDrainConnectResponses(t, client, server)

	if err := client.UnknownCommand("noSuchCommand", 9); err != nil {
		t.Fatalf("send unknown command: %v", err)
	}
	if err := client.CreateStream(); err != nil {
		t.Fatalf("createStream: %v", err)
	}
	result := mustReadRTMPMessage(t, client, server, 2*time.Second)
	values := mustDecodeAMF0Values(t, result.payload)
	if values[0] != "_result" || values[1] != float64(4) {
		t.Fatalf("unexpected createStream after unknown command: %#v", values)
	}
}

func TestRTMPFragmentedConnectAtDefaultChunkSize(t *testing.T) {
	server := startRTMPTestServer(t)
	client := newRTMPClient(t, server.addr())
	if err := client.Handshake(); err != nil {
		t.Fatalf("handshake: %v", err)
	}
	if err := client.Connect("live", map[string]any{
		"pageUrl": "https://example.invalid/publishers/with/a/very/long/path/for/default-chunk-fragmentation/testing",
		"swfUrl":  "https://example.invalid/player/assets/really-long-path/to-force-a-second-chunk/player.swf",
	}); err != nil {
		t.Fatalf("connect command: %v", err)
	}
	mustDrainConnectResponses(t, client, server)
}

func TestRTMPSetChunkSizeControlIsAccepted(t *testing.T) {
	server := startRTMPTestServer(t)
	client := newRTMPClient(t, server.addr())
	if err := client.Handshake(); err != nil {
		t.Fatalf("handshake: %v", err)
	}
	if err := client.SetChunkSize(64); err != nil {
		t.Fatalf("set chunk size: %v", err)
	}
	if err := client.ConnectMinimal("live"); err != nil {
		t.Fatalf("minimal connect command: %v", err)
	}
	mustDrainConnectResponses(t, client, server)
}

func TestRTMPMalformedCommandClosesSession(t *testing.T) {
	server := startRTMPTestServer(t)
	client := newRTMPClient(t, server.addr())
	if err := client.Handshake(); err != nil {
		t.Fatalf("handshake: %v", err)
	}
	if err := client.sendMalformedCommand(); err != nil {
		t.Fatalf("send malformed command: %v", err)
	}
	expectRTMPSessionClose(t, client, server)
}

func TestRTMPConcurrentSessionsStayIndependent(t *testing.T) {
	server := startRTMPTestServer(t)

	const sessions = 4
	var wg sync.WaitGroup
	errCh := make(chan error, sessions)

	for i := 0; i < sessions; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			client, err := dialAndPrimeRTMP(server.addr())
			if err != nil {
				errCh <- err
				return
			}
			defer client.Close()

			if err := client.ReleaseStream("stream"); err != nil {
				errCh <- err
				return
			}
			if err := client.FCPublish("stream"); err != nil {
				errCh <- err
				return
			}
			msg, err := client.ReadMessage(2 * time.Second)
			if err != nil {
				errCh <- err
				return
			}
			values, err := decodeAMF0Values(msg.payload)
			if err != nil {
				errCh <- err
				return
			}
			if values[0] != "onFCPublish" {
				errCh <- errors.New("unexpected FCPublish response")
				return
			}

			if err := client.CreateStream(); err != nil {
				errCh <- err
				return
			}
			msg, err = client.ReadMessage(2 * time.Second)
			if err != nil {
				errCh <- err
				return
			}
			values, err = decodeAMF0Values(msg.payload)
			if err != nil {
				errCh <- err
				return
			}
			if values[0] != "_result" || values[3] != float64(1) {
				errCh <- errors.New("unexpected createStream result")
				return
			}
		}()
	}

	wg.Wait()
	close(errCh)
	for err := range errCh {
		if err != nil {
			t.Fatalf("concurrent session failed: %v\nserver:\n%s", err, server.logs())
		}
	}
}

func dialAndPrimeRTMP(addr string) (*rtmpClient, error) {
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return nil, err
	}
	client := &rtmpClient{
		conn:           conn,
		inChunkSize:    128,
		outChunkSize:   128,
		inboundStreams: make(map[uint32]*rtmpChunkState),
	}
	if err := client.Handshake(); err != nil {
		_ = client.Close()
		return nil, err
	}
	if err := client.Connect("live", nil); err != nil {
		_ = client.Close()
		return nil, err
	}
	for i := 0; i < 4; i++ {
		if _, err := client.ReadMessage(2 * time.Second); err != nil {
			_ = client.Close()
			return nil, err
		}
	}
	return client, nil
}

func mustDrainConnectResponses(t *testing.T, client *rtmpClient, server *rtmpTestServer) {
	t.Helper()
	for i := 0; i < 4; i++ {
		_ = mustReadRTMPMessage(t, client, server, 2*time.Second)
	}
}

func mustReadRTMPMessage(t *testing.T, client *rtmpClient, server *rtmpTestServer, timeout time.Duration) *rtmpMessage {
	t.Helper()
	msg, err := client.ReadMessage(timeout)
	if err != nil {
		t.Fatalf("read RTMP message: %v\nserver:\n%s", err, server.logs())
	}
	return msg
}

func mustDecodeAMF0Values(t *testing.T, payload []byte) []any {
	t.Helper()
	values, err := decodeAMF0Values(payload)
	if err != nil {
		t.Fatalf("decode AMF payload: %v", err)
	}
	return values
}

func expectRTMPSessionClose(t *testing.T, client *rtmpClient, server *rtmpTestServer) {
	t.Helper()
	_, err := client.ReadMessage(2 * time.Second)
	if err == nil {
		t.Fatalf("expected RTMP session close\nserver:\n%s", server.logs())
	}
	if !errors.Is(err, io.EOF) && !isTimeoutClose(err) {
		t.Fatalf("unexpected session close error: %v\nserver:\n%s", err, server.logs())
	}
}

func isTimeoutClose(err error) bool {
	var netErr net.Error
	return errors.As(err, &netErr) && !netErr.Timeout()
}
