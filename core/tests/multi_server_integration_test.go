package tests

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestBothServersPublishToHLS(t *testing.T) {
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
	rtmpPort := reservePort(t)
	webrtcPort := reservePort(t)

	serverCmd := exec.Command(
		binary,
		"--server", "both",
		"--host", "127.0.0.1",
		"--rtmp-port", rtmpPort,
		"--webrtc-port", webrtcPort,
		"--source", "live/rtmp-test",
		"--sink", outputRoot,
		"--source", "live/webrtc-test",
		"--sink", outputRoot,
	)
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

	waitForTCP(t, "127.0.0.1:"+rtmpPort, 10*time.Second)
	waitForTCP(t, "127.0.0.1:"+webrtcPort, 10*time.Second)

	rtmpCtx, cancelRTMP := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancelRTMP()
	rtmpCmd := exec.CommandContext(
		rtmpCtx,
		"ffmpeg",
		"-hide_banner",
		"-loglevel", "error",
		"-re",
		"-stream_loop", "-1",
		"-i", inputFile,
		"-map", "0:v:0",
		"-map", "0:a:0",
		"-c", "copy",
		"-t", "5",
		"-f", "flv",
		"rtmp://127.0.0.1:"+rtmpPort+"/live/rtmp-test",
	)
	var rtmpLog bytes.Buffer
	rtmpCmd.Stdout = &rtmpLog
	rtmpCmd.Stderr = &rtmpLog
	if err := rtmpCmd.Run(); err != nil {
		t.Fatalf("rtmp publish failed: %v\nserver:\n%s\npublisher:\n%s\nsink logs:\n%s", err, serverLog.String(), rtmpLog.String(), collectFiles(outputRoot, ".log"))
	}

	whipCtx, cancelWHIP := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancelWHIP()
	whipCmd := exec.CommandContext(
		whipCtx,
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
		"http://127.0.0.1:"+webrtcPort+"/live/webrtc-test/whip",
	)
	var whipLog bytes.Buffer
	whipCmd.Stdout = &whipLog
	whipCmd.Stderr = &whipLog
	if err := whipCmd.Run(); err != nil {
		t.Fatalf("whip publish failed: %v\nserver:\n%s\npublisher:\n%s\nsink logs:\n%s", err, serverLog.String(), whipLog.String(), collectFiles(outputRoot, ".log"))
	}

	waitForFileCountSuffix(t, outputRoot, "session.json", 2, 15*time.Second)
	waitForFileCountSuffix(t, outputRoot, "stream.m3u8", 2, 15*time.Second)
	waitForFileCountSuffix(t, outputRoot, ".ts", 1, 15*time.Second)
	waitForFileCountSuffix(t, outputRoot, ".m4s", 1, 15*time.Second)

	statsPaths := findFilesWithSuffix(t, outputRoot, "session.json")
	if len(statsPaths) != 2 {
		t.Fatalf("expected 2 stats files, got %d\nserver:\n%s", len(statsPaths), serverLog.String())
	}
	for _, statsPath := range statsPaths {
		var stats sessionStats
		statsData, err := os.ReadFile(statsPath)
		if err != nil {
			t.Fatalf("read stats %s: %v", statsPath, err)
		}
		if err := json.Unmarshal(statsData, &stats); err != nil {
			t.Fatalf("decode stats %s: %v\n%s", statsPath, err, string(statsData))
		}
		if stats.VideoPackets == 0 || stats.AudioPackets == 0 || stats.BytesIn == 0 || stats.FFmpegExitCode != 0 {
			t.Fatalf("unexpected stats in %s: %+v\nserver:\n%s\nsink logs:\n%s", statsPath, stats, serverLog.String(), collectFiles(outputRoot, ".log"))
		}
	}
}

func waitForFileCountSuffix(t *testing.T, root string, suffix string, expected int, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		paths := findFilesWithSuffix(t, root, suffix)
		if len(paths) >= expected {
			return
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %d files with suffix %s under %s", expected, suffix, root)
}

func findFilesWithSuffix(t *testing.T, root string, suffix string) []string {
	t.Helper()
	var paths []string
	err := filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() || !strings.HasSuffix(path, suffix) {
			return nil
		}
		paths = append(paths, path)
		return nil
	})
	if err != nil {
		t.Fatalf("walk %s: %v", root, err)
	}
	return paths
}
