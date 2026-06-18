package tests

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func TestRuntimeHTTPUpsertManifestAddsSink(t *testing.T) {
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
	runtimePort := reservePort(t)

	serverCmd := exec.Command(
		binary,
		"--host", "127.0.0.1",
		"--port", port,
		"--runtime-port", runtimePort,
		"--source", "live/test",
		"--sink", outputRootA,
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
	waitForTCP(t, "127.0.0.1:"+runtimePort, 10*time.Second)

	publishCtx, cancelPublish := context.WithTimeout(context.Background(), 25*time.Second)
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
		"-t", "8",
		"-f", "flv",
		"rtmp://127.0.0.1:"+port+"/live/test",
	)
	var publishLog bytes.Buffer
	publishCmd.Stdout = &publishLog
	publishCmd.Stderr = &publishLog
	if err := publishCmd.Start(); err != nil {
		t.Fatalf("start publisher: %v", err)
	}

	manifestA := waitForFileSuffix(t, outputRootA, "stream.m3u8", 10*time.Second)
	waitForGlob(t, filepath.Dir(manifestA), "*.ts", 10*time.Second)

	requestBody := map[string]any{
		"sources": []map[string]any{
			{"source_id": "live/test"},
		},
		"sinks": []map[string]any{
			{
				"sink_id":     "sink-a",
				"source_id":   "live/test",
				"output_root": outputRootA,
				"mode":        "record",
			},
			{
				"sink_id":     "sink-b",
				"source_id":   "live/test",
				"output_root": outputRootB,
				"mode":        "record",
			},
		},
	}
	bodyBytes, err := json.Marshal(requestBody)
	if err != nil {
		t.Fatalf("marshal request: %v", err)
	}

	resp, err := http.Post("http://127.0.0.1:"+runtimePort+"/upsert_manifest", "application/json", bytes.NewReader(bodyBytes))
	if err != nil {
		t.Fatalf("post upsert_manifest: %v\nserver:\n%s", err, serverLog.String())
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("unexpected status %d\nserver:\n%s", resp.StatusCode, serverLog.String())
	}

	manifestB := waitForFileSuffix(t, outputRootB, "stream.m3u8", 10*time.Second)
	waitForGlob(t, filepath.Dir(manifestB), "*.ts", 10*time.Second)

	if err := publishCmd.Wait(); err != nil {
		t.Fatalf("publish failed: %v\nserver:\n%s\npublisher:\n%s\nsink logs A:\n%s\nsink logs B:\n%s",
			err,
			serverLog.String(),
			publishLog.String(),
			collectFiles(outputRootA, ".log"),
			collectFiles(outputRootB, ".log"),
		)
	}

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
	var foundVideoA bool
	var foundVideoB bool
	for _, stream := range outA.Streams {
		if stream.CodecType == "video" {
			foundVideoA = true
		}
	}
	for _, stream := range outB.Streams {
		if stream.CodecType == "video" {
			foundVideoB = true
		}
	}
	if !foundVideoA || !foundVideoB {
		t.Fatalf("expected both manifests to contain video streams\nA:\n%s\nB:\n%s", string(probeA), string(probeB))
	}
}
