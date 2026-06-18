package tests

import (
	"bytes"
	"context"
	"encoding/json"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

func TestWebRTCServerPublishesToHLS(t *testing.T) {
	requireBinary(t, "cmake")
	requireBinary(t, "ffmpeg")
	requireBinary(t, "ffprobe")
	requireWHIPMuxer(t)

	repoRoot := findRepoRoot(t)
	engineRoot := filepath.Join(repoRoot, "argus")
	inputFile := filepath.Join(repoRoot, "minion.mp4")
	if _, err := os.Stat(inputFile); err != nil {
		t.Fatalf("missing fixture: %v", err)
	}

	binary := buildServerBinary(t, engineRoot)
	outputRoot := t.TempDir()
	port := reservePort(t)

	serverCmd := exec.Command(binary, "--server", "webrtc", "--host", "127.0.0.1", "--port", port, "--source", "live/test", "--sink", outputRoot)
	var serverLog bytes.Buffer
	serverCmd.Stdout = &serverLog
	serverCmd.Stderr = &serverLog
	if err := serverCmd.Start(); err != nil {
		t.Fatalf("start server: %v", err)
	}
	defer func() {
		if serverCmd.Process != nil {
			_ = serverCmd.Process.Kill()
			_, _ = serverCmd.Process.Wait()
		}
	}()

	waitForTCP(t, "127.0.0.1:"+port, 10*time.Second)

	publishCtx, cancelPublish := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancelPublish()
	publishCmd := exec.CommandContext(
		publishCtx,
		"ffmpeg",
		"-hide_banner",
		"-loglevel", "error",
		"-re",
		"-stream_loop", "-1",
		"-i", inputFile,
		"-map", "0:v:0",
		"-map", "0:a:0",
		"-c:v", "libx264",
		"-preset", "ultrafast",
		"-tune", "zerolatency",
		"-pix_fmt", "yuv420p",
		"-profile:v", "baseline",
		"-level:v", "3.1",
		"-g", "30",
		"-keyint_min", "30",
		"-sc_threshold", "0",
		"-c:a", "libopus",
		"-ar", "48000",
		"-ac", "2",
		"-t", "5",
		"-f", "whip",
		"-handshake_timeout", "10000",
		"http://127.0.0.1:"+port+"/live/test/whip",
	)
	var publishLog bytes.Buffer
	publishCmd.Stdout = &publishLog
	publishCmd.Stderr = &publishLog
	if err := publishCmd.Run(); err != nil {
		t.Fatalf("publish failed: %v\nserver:\n%s\npublisher:\n%s\nsink logs:\n%s", err, serverLog.String(), publishLog.String(), collectFiles(outputRoot, ".log"))
	}

	statsPath := waitForFileSuffix(t, outputRoot, "session.json", 15*time.Second)
	manifestPath := waitForFileSuffix(t, outputRoot, "stream.m3u8", 15*time.Second)
	waitForGlob(t, filepath.Dir(manifestPath), "*.m4s", 15*time.Second)

	var stats sessionStats
	statsData, err := os.ReadFile(statsPath)
	if err != nil {
		t.Fatalf("read stats: %v", err)
	}
	if err := json.Unmarshal(statsData, &stats); err != nil {
		t.Fatalf("decode stats: %v\n%s", err, string(statsData))
	}
	if stats.VideoPackets == 0 {
		t.Fatalf("expected video packets, got %+v\nserver:\n%s", stats, serverLog.String())
	}
	if stats.AudioPackets == 0 {
		t.Fatalf("expected audio packets, got %+v\nserver:\n%s", stats, serverLog.String())
	}
	if stats.BytesIn == 0 {
		t.Fatalf("expected bytes_in > 0, got %+v", stats)
	}
	if stats.FFmpegExitCode != 0 {
		t.Fatalf("expected sink ffmpeg exit 0, got %+v\nserver:\n%s\nsink logs:\n%s", stats, serverLog.String(), collectFiles(outputRoot, ".log"))
	}

	probeCtx, cancelProbe := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancelProbe()
	probeCmd := exec.CommandContext(probeCtx, "ffprobe", "-v", "error", "-show_streams", "-of", "json", manifestPath)
	probeOut, err := probeCmd.CombinedOutput()
	if err != nil {
		t.Fatalf("ffprobe failed: %v\noutput:\n%s\nserver:\n%s\nsink logs:\n%s", err, string(probeOut), serverLog.String(), collectFiles(outputRoot, ".log"))
	}

	var probe ffprobeOutput
	if err := json.Unmarshal(probeOut, &probe); err != nil {
		t.Fatalf("decode ffprobe output: %v\n%s", err, string(probeOut))
	}

	var foundVideo bool
	var foundAudio bool
	for _, stream := range probe.Streams {
		switch stream.CodecType {
		case "video":
			foundVideo = true
		case "audio":
			foundAudio = true
		}
	}
	if !foundVideo {
		t.Fatalf("expected video stream in ffprobe output:\n%s", string(probeOut))
	}
	if !foundAudio {
		t.Fatalf("expected audio stream in ffprobe output:\n%s", string(probeOut))
	}

	for _, stream := range probe.Streams {
		if stream.StartTime == "" {
			continue
		}
		startTime, err := strconv.ParseFloat(stream.StartTime, 64)
		if err != nil {
			t.Fatalf("parse ffprobe stream start_time %q: %v\n%s", stream.StartTime, err, string(probeOut))
		}
		if math.Abs(startTime) > 5 {
			t.Fatalf("expected near-zero stream start_time, got %f\n%s", startTime, string(probeOut))
		}
	}
}

func requireWHIPMuxer(t *testing.T) {
	t.Helper()
	cmd := exec.Command("ffmpeg", "-hide_banner", "-h", "muxer=whip")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Skipf("ffmpeg whip muxer unavailable: %v\n%s", err, string(out))
	}
	if !strings.Contains(string(out), "Muxer whip") {
		t.Skipf("ffmpeg whip muxer unavailable:\n%s", string(out))
	}
}
