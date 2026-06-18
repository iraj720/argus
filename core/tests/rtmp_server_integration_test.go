package tests

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"testing"
	"time"
)

type sessionStats struct {
	Session        string `json:"session"`
	Manifest       string `json:"manifest"`
	AudioPackets   uint64 `json:"audio_packets"`
	VideoPackets   uint64 `json:"video_packets"`
	DataPackets    uint64 `json:"data_packets"`
	BytesIn        uint64 `json:"bytes_in"`
	LastTimestamp  uint32 `json:"last_timestamp_ms"`
	FFmpegExitCode int    `json:"ffmpeg_exit_code"`
}

type ffprobeOutput struct {
	Streams []struct {
		CodecType string `json:"codec_type"`
		StartTime string `json:"start_time"`
	} `json:"streams"`
	Format struct {
		StartTime string `json:"start_time"`
		Duration  string `json:"duration"`
	} `json:"format"`
}

func TestRTMPServerPublishesToHLS(t *testing.T) {
	requireBinary(t, "cmake")
	requireBinary(t, "ffmpeg")
	requireBinary(t, "ffprobe")

	repoRoot := findRepoRoot(t)
	engineRoot := filepath.Join(repoRoot, "argus")
	inputFile := filepath.Join(repoRoot, "minion.mp4")
	if _, err := os.Stat(inputFile); err != nil {
		t.Fatalf("missing fixture: %v", err)
	}

	binary := buildServerBinary(t, engineRoot)
	outputRoot := t.TempDir()
	port := reservePort(t)

	serverCmd := exec.Command(binary, "--host", "127.0.0.1", "--port", port, "--source", "live/test", "--sink", outputRoot)
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

	publishCtx, cancelPublish := context.WithTimeout(context.Background(), 20*time.Second)
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
		"-c", "copy",
		"-t", "5",
		"-f", "flv",
		"rtmp://127.0.0.1:"+port+"/live/test",
	)
	var publishLog bytes.Buffer
	publishCmd.Stdout = &publishLog
	publishCmd.Stderr = &publishLog
	if err := publishCmd.Run(); err != nil {
		t.Fatalf("publish failed: %v\nserver:\n%s\npublisher:\n%s\nsink logs:\n%s", err, serverLog.String(), publishLog.String(), collectFiles(outputRoot, ".log"))
	}

	statsPath := waitForFileSuffix(t, outputRoot, "session.json", 10*time.Second)
	manifestPath := waitForFileSuffix(t, outputRoot, "stream.m3u8", 10*time.Second)
	waitForGlob(t, filepath.Dir(manifestPath), "*.ts", 10*time.Second)
	firstSegmentPath := waitForFileSuffix(t, outputRoot, "seg_000000.ts", 10*time.Second)

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
	if stats.Manifest == "" || !strings.HasSuffix(stats.Manifest, "stream.m3u8") {
		t.Fatalf("unexpected manifest in stats: %+v", stats)
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

	firstSegmentProbe := waitForProbeableMediaFile(t, firstSegmentPath, 10*time.Second)
	var firstSegment ffprobeOutput
	if err := json.Unmarshal(firstSegmentProbe, &firstSegment); err != nil {
		t.Fatalf("decode first segment ffprobe output: %v\n%s", err, string(firstSegmentProbe))
	}
	foundVideo = false
	for _, stream := range firstSegment.Streams {
		if stream.CodecType == "video" {
			foundVideo = true
		}
	}
	if !foundVideo {
		t.Fatalf("expected first segment to be independently decodable:\n%s", string(firstSegmentProbe))
	}
}

func TestRTMPServerPublishesToMultipleHLSSinks(t *testing.T) {
	requireBinary(t, "cmake")
	requireBinary(t, "ffmpeg")
	requireBinary(t, "ffprobe")

	repoRoot := findRepoRoot(t)
	engineRoot := filepath.Join(repoRoot, "argus")
	inputFile := filepath.Join(repoRoot, "minion.mp4")
	if _, err := os.Stat(inputFile); err != nil {
		t.Fatalf("missing fixture: %v", err)
	}

	binary := buildServerBinary(t, engineRoot)
	outputRootA := t.TempDir()
	outputRootB := t.TempDir()
	port := reservePort(t)

	serverCmd := exec.Command(
		binary,
		"--host", "127.0.0.1",
		"--port", port,
		"--source", "live/test",
		"--sink", outputRootA,
		"--sink", outputRootB,
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

	waitForTCP(t, "127.0.0.1:"+port, 10*time.Second)

	publishCtx, cancelPublish := context.WithTimeout(context.Background(), 20*time.Second)
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
		"-c", "copy",
		"-t", "5",
		"-f", "flv",
		"rtmp://127.0.0.1:"+port+"/live/test",
	)
	var publishLog bytes.Buffer
	publishCmd.Stdout = &publishLog
	publishCmd.Stderr = &publishLog
	if err := publishCmd.Run(); err != nil {
		t.Fatalf("publish failed: %v\nserver:\n%s\npublisher:\n%s\nsink logs A:\n%s\nsink logs B:\n%s", err, serverLog.String(), publishLog.String(), collectFiles(outputRootA, ".log"), collectFiles(outputRootB, ".log"))
	}

	manifestA := waitForFileSuffix(t, outputRootA, "stream.m3u8", 10*time.Second)
	manifestB := waitForFileSuffix(t, outputRootB, "stream.m3u8", 10*time.Second)
	waitForGlob(t, filepath.Dir(manifestA), "*.ts", 10*time.Second)
	waitForGlob(t, filepath.Dir(manifestB), "*.ts", 10*time.Second)

	probeA := waitForProbeableMediaFile(t, manifestA, 10*time.Second)
	probeB := waitForProbeableMediaFile(t, manifestB, 10*time.Second)
	var outA ffprobeOutput
	var outB ffprobeOutput
	if err := json.Unmarshal(probeA, &outA); err != nil {
		t.Fatalf("decode ffprobe A: %v\n%s", err, string(probeA))
	}
	if err := json.Unmarshal(probeB, &outB); err != nil {
		t.Fatalf("decode ffprobe B: %v\n%s", err, string(probeB))
	}
	if len(outA.Streams) == 0 || len(outB.Streams) == 0 {
		t.Fatalf("expected both sink outputs to be probeable\nA:\n%s\nB:\n%s", string(probeA), string(probeB))
	}
}

func TestRTMPServerPublishesLiveHLSWindow(t *testing.T) {
	requireBinary(t, "cmake")
	requireBinary(t, "ffmpeg")
	requireBinary(t, "ffprobe")

	repoRoot := findRepoRoot(t)
	engineRoot := filepath.Join(repoRoot, "argus")
	inputFile := filepath.Join(repoRoot, "minion.mp4")
	if _, err := os.Stat(inputFile); err != nil {
		t.Fatalf("missing fixture: %v", err)
	}

	binary := buildServerBinary(t, engineRoot)
	outputRoot := t.TempDir()
	port := reservePort(t)

	serverCmd := exec.Command(
		binary,
		"--host", "127.0.0.1",
		"--port", port,
		"--source", "live/test",
		"--live",
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
		"-g", "24",
		"-keyint_min", "24",
		"-sc_threshold", "0",
		"-pix_fmt", "yuv420p",
		"-c:a", "aac",
		"-ar", "44100",
		"-b:a", "128k",
		"-t", "12",
		"-f", "flv",
		"rtmp://127.0.0.1:"+port+"/live/test",
	)
	var publishLog bytes.Buffer
	publishCmd.Stdout = &publishLog
	publishCmd.Stderr = &publishLog
	if err := publishCmd.Run(); err != nil {
		t.Fatalf("publish failed: %v\nserver:\n%s\npublisher:\n%s\nsink logs:\n%s", err, serverLog.String(), publishLog.String(), collectFiles(outputRoot, ".log"))
	}

	manifestPath := waitForFileSuffix(t, outputRoot, "stream.m3u8", 10*time.Second)
	manifestText := waitForManifestWindow(t, manifestPath, 10*time.Second)
	if strings.Count(manifestText, "#EXTINF:") > 3 {
		t.Fatalf("expected live manifest window of at most 3 segments:\n%s", manifestText)
	}
	mediaSequence := manifestMediaSequence(manifestText)
	if mediaSequence <= 0 {
		t.Fatalf("expected live manifest media sequence to advance:\n%s", manifestText)
	}

	sessionDir := filepath.Dir(manifestPath)
	if err := waitForMissingFile(filepath.Join(sessionDir, "seg_000000.ts"), 10*time.Second); err != nil {
		t.Fatalf("expected earliest stale segment to be cleaned up\nmanifest:\n%s\nserver:\n%s\nsink logs:\n%s", manifestText, serverLog.String(), collectFiles(outputRoot, ".log"))
	}
}

func requireBinary(t *testing.T, name string) {
	t.Helper()
	if _, err := exec.LookPath(name); err != nil {
		t.Skipf("%s is required: %v", name, err)
	}
}

func findRepoRoot(t *testing.T) string {
	t.Helper()
	_, currentFile, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller failed")
	}
	return filepath.Clean(filepath.Join(filepath.Dir(currentFile), "..", "..", ".."))
}

func buildServerBinary(t *testing.T, engineRoot string) string {
	t.Helper()
	buildDir := filepath.Join(t.TempDir(), "build")
	runCmd(t, exec.Command("cmake", "-S", engineRoot, "-B", buildDir))
	runCmd(t, exec.Command("cmake", "--build", buildDir, "--target", "argus"))
	return filepath.Join(buildDir, "argus")
}

func runCmd(t *testing.T, cmd *exec.Cmd) {
	t.Helper()
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("command %v failed: %v\n%s", cmd.Args, err, string(out))
	}
}

func reservePort(t *testing.T) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve port: %v", err)
	}
	defer ln.Close()
	_, port, err := net.SplitHostPort(ln.Addr().String())
	if err != nil {
		t.Fatalf("split host port: %v", err)
	}
	return port
}

func waitForTCP(t *testing.T, addr string, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", addr, 500*time.Millisecond)
		if err == nil {
			_ = conn.Close()
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for tcp %s", addr)
}

func waitForFileSuffix(t *testing.T, root string, suffix string, timeout time.Duration) string {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		path, err := findFileSuffix(root, suffix)
		if err == nil {
			return path
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for file suffix %s under %s", suffix, root)
	return ""
}

func findFileSuffix(root string, suffix string) (string, error) {
	var found string
	errStop := errors.New("stop")
	err := filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		if strings.HasSuffix(path, suffix) {
			found = path
			return errStop
		}
		return nil
	})
	if errors.Is(err, errStop) {
		return found, nil
	}
	if err != nil {
		return "", err
	}
	return "", os.ErrNotExist
}

func waitForGlob(t *testing.T, root string, pattern string, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		matches, err := filepath.Glob(filepath.Join(root, pattern))
		if err == nil && len(matches) > 0 {
			return
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for glob %s in %s", pattern, root)
}

func waitForProbeableMediaFile(t *testing.T, path string, timeout time.Duration) []byte {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		probeCtx, cancelProbe := context.WithTimeout(context.Background(), 5*time.Second)
		cmd := exec.CommandContext(probeCtx, "ffprobe", "-v", "error", "-show_streams", "-of", "json", path)
		out, err := cmd.CombinedOutput()
		cancelProbe()
		if err == nil {
			return out
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for probeable media file %s", path)
	return nil
}

func waitForManifestWindow(t *testing.T, path string, timeout time.Duration) string {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		data, err := os.ReadFile(path)
		if err == nil {
			text := string(data)
			if manifestMediaSequence(text) > 0 && strings.Count(text, "#EXTINF:") > 0 {
				return text
			}
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for manifest window %s", path)
	return ""
}

func manifestMediaSequence(manifest string) int {
	for _, line := range strings.Split(manifest, "\n") {
		line = strings.TrimSpace(line)
		const prefix = "#EXT-X-MEDIA-SEQUENCE:"
		if !strings.HasPrefix(line, prefix) {
			continue
		}
		value, err := strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(line, prefix)))
		if err != nil {
			return -1
		}
		return value
	}
	return -1
}

func waitForMissingFile(path string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		_, err := os.Stat(path)
		if errors.Is(err, os.ErrNotExist) {
			return nil
		}
		time.Sleep(200 * time.Millisecond)
	}
	return os.ErrExist
}

func collectFiles(root string, suffix string) string {
	var b strings.Builder
	_ = filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() || !strings.HasSuffix(path, suffix) {
			return nil
		}
		data, readErr := os.ReadFile(path)
		if readErr != nil {
			return nil
		}
		b.WriteString(path)
		b.WriteString(":\n")
		b.Write(data)
		b.WriteString("\n")
		return nil
	})
	return b.String()
}
