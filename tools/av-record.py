#!/usr/bin/env python3
"""
av-record.py — reference client for the emulator's raw A/V server.

    m2hle --rom sfight.zip --run --headless --av-port 7180 --av-mute
    python tools/av-record.py --port 7180 --seconds 30 out.mp4

The emulator hands over BGRA frames and 16-bit stereo samples on one socket and
does no encoding of its own (see README.md, "Raw A/V out"). This is the other
half: it reads the stream, lays the irregular video cadence onto a constant
60 fps grid using each frame's `sample` stamp, and lets ffmpeg do the rest.

Nothing here is privileged — it is a hundred lines against a documented format,
and the point of it is that reading it is the fastest way to see how the format
works. Take it as a starting point for whatever you actually want to feed.

WHY THE `sample` STAMP AND NOT ARRIVAL TIME. Every packet carries the board's
own 44.1 kHz sample counter, so a picture's presentation time is
`sample / 44100` on exactly the timeline the samples are on. A frame the
emulator dropped leaves a gap in that timeline, and the grid below simply holds
the previous picture across it; a frame that arrives late is still placed where
it belongs. There is nothing to line up by ear and nothing that drifts.
"""
import argparse, os, socket, struct, subprocess, sys, time

FPS = 60                      # the output grid; the board's nominal rate


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output", help="the mp4 to write")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True, help="the emulator's --av-port")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ap.add_argument("--crf", default="18", help="x264 quality (lower is better)")
    ap.add_argument("--preset", default="veryfast")
    ap.add_argument("--keep-dc", action="store_true",
                    help="keep the board's DC offset instead of high-passing it out")
    args = ap.parse_args()

    s = socket.create_connection((args.host, args.port), timeout=15)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    s.settimeout(15)
    buf = bytearray(64 << 20)
    view = memoryview(buf)

    def rd(n):
        got = 0
        while got < n:
            k = s.recv_into(view[got:n], n - got)
            if not k:
                raise EOFError("the emulator closed the stream")
            got += k
        return view[:n]

    h = bytes(rd(32))
    if h[0:4] != b"M2AV":
        sys.exit("not an M2AV stream (got %r)" % h[0:4])
    version, hsize = struct.unpack_from("<HH", h, 4)
    w, height = struct.unpack_from("<HH", h, 8)
    pixfmt = h[12:16]
    rate = struct.unpack_from("<I", h, 24)[0]
    channels, bits = struct.unpack_from("<BB", h, 28)
    if pixfmt != b"BGRA" or bits != 16 or channels != 2:
        sys.exit("unexpected format: pixfmt=%r %dch %dbit" % (pixfmt, channels, bits))
    print("m2av v%d: %dx%d BGRA, %d Hz 16-bit stereo" % (version, w, height, rate))
    vbytes = w * height * 4

    tmp_v = args.output + ".video.mp4"
    tmp_a = args.output + ".audio.raw"
    vproc = subprocess.Popen(
        [args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
         "-f", "rawvideo", "-pix_fmt", "bgra", "-s", "%dx%d" % (w, height),
         "-r", str(FPS), "-i", "pipe:0",
         "-an", "-c:v", "libx264", "-preset", args.preset, "-crf", args.crf,
         "-pix_fmt", "yuv420p", tmp_v],
        stdin=subprocess.PIPE)
    af = open(tmp_a, "wb")

    origin = None       # the sample this recording starts at
    emitted = 0         # constant-rate frames written out
    held = None         # newest picture received
    pictures = dropped = 0
    t0 = time.time()
    try:
        while time.time() - t0 < args.seconds:
            typ, flags, _r, size, frame, sample = struct.unpack("<BBHIQQ", bytes(rd(24)))
            payload = bytes(rd(size))
            if origin is None:
                origin = sample
            if typ == ord("A"):
                af.write(payload)
            else:
                pictures += 1
                dropped += flags & 1
                held = payload
                # Hold the previous picture across anything the emulator dropped:
                # the stamp says where this one belongs, so a gap stays a gap.
                want = int((sample - origin) * FPS / rate)
                while emitted < want:
                    vproc.stdin.write(held)
                    emitted += 1
    except KeyboardInterrupt:
        pass
    finally:
        if held is not None and emitted == 0:
            vproc.stdin.write(held)
            emitted += 1
        vproc.stdin.close()
        vproc.wait()
        af.close()
        s.close()

    samples = os.path.getsize(tmp_a) // 4
    print("%d pictures (%d flagged) -> %d frames at %d fps (%.3f s); "
          "audio %d samples (%.3f s)"
          % (pictures, dropped, emitted, FPS, emitted / float(FPS),
             samples, samples / float(rate)))

    # The board's output carries a DC offset (~5000/32768 in STF, from the DSP
    # path; MAME's WAV has the same one). A real cabinet's amplifier is
    # AC-coupled, so take it out here rather than in the emulator, which keeps
    # its samples exactly as the board made them. See CLAUDE.md, "Sound board".
    mux = [args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
           "-i", tmp_v,
           "-f", "s16le", "-ar", str(rate), "-ac", "2", "-i", tmp_a]
    if not args.keep_dc:
        mux += ["-af", "highpass=f=5"]
    mux += ["-c:v", "copy", "-c:a", "aac", "-b:a", "192k", "-shortest", args.output]
    subprocess.check_call(mux)
    os.remove(tmp_v)
    os.remove(tmp_a)
    print("wrote", args.output)


if __name__ == "__main__":
    main()
