#!/usr/bin/env python3
"""Subtitles for videos, made on this Mac with Whisper (whisper.cpp, large-v3-turbo).

    python3 subtitle.py VIDEO_OR_FOLDER [...]   [--force] [--lang en] [--out FOLDER]

Writes "<video name>.srt" beside each video, which the G36 Video Player picks up
(Menu > Subtitles). Folders are searched for .mp4 .mkv .mov .m4v .avi .webm; videos
that already have a .srt are skipped unless --force. --out puts the .srt files in
FOLDER instead (named the same), leaving the videos' folders untouched.

Timing: Whisper's own lines often start where the previous one ended, so a pause in
the speech puts the next line on screen early. Instead every word is timed (DTW
alignment) and the lines are built here: each starts at its first spoken word, a
pause starts a new line, and a line ends shortly after its last word.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.join(HERE, "bin", "whisper-cli")
MODEL = os.path.join(HERE, "models", "ggml-large-v3-turbo.bin")
VIDEO_EXT = (".mp4", ".mkv", ".mov", ".m4v", ".avi", ".webm")

PAUSE = 0.6        # seconds of silence that start a new line
MAX_CHARS = 84     # per subtitle (two lines of about 42)
LINE_CHARS = 42
MAX_DUR = 6.0      # seconds on screen at most
LEAD = 0.3         # shown this much before the first word (a moment to start reading)
TAIL = 0.5         # and kept this long after the last
MIN_DUR = 0.9


def words_from_json(path):
    """[(start, end, text)] in seconds, one per word."""
    with open(path, encoding="utf-8", errors="replace") as f:
        data = json.load(f)
    words = []
    for seg in data.get("transcription", []):
        for tok in seg.get("tokens", []):
            text = tok.get("text", "")
            if not text or text.startswith("[_") or text.startswith("<|"):
                continue
            off = tok.get("offsets", {})
            t0 = off.get("from", 0) / 1000.0
            t1 = off.get("to", 0) / 1000.0
            dtw = tok.get("t_dtw", -1)
            if dtw is not None and dtw >= 0:
                t0 = dtw / 100.0   # where the word is actually heard (10 ms units)
            if text.startswith(" ") or not words:
                words.append([t0, max(t1, t0), text.strip()])
            else:
                words[-1][1] = max(words[-1][1], t1)
                words[-1][2] += text
    # Only the starts are reliable (Whisper's own word ends are rough guesses): a word
    # lasts about as long as it takes to say, and ends by the time the next one starts
    words = [w for w in words if w[2]]
    for i, w in enumerate(words):
        say = 0.25 + 0.065 * len(w[2])
        end = w[0] + say
        if i + 1 < len(words):
            end = min(end, words[i + 1][0])
        w[1] = max(end, w[0] + 0.05)
    return words


def group(words):
    """Words into subtitles: (start, end, text)."""
    cues, cur = [], []
    for w in words:
        if cur:
            text = " ".join(x[2] for x in cur)
            gap = w[0] - cur[-1][1]
            ends_sentence = cur[-1][2][-1:] in ".?!"
            # a new line at a pause, when it's full, or at the end of a sentence followed
            # by a breath (often the next speaker) or once the line is long enough
            if (gap > PAUSE or len(text) + 1 + len(w[2]) > MAX_CHARS or w[1] - cur[0][0] > MAX_DUR
                    or (ends_sentence and (gap > 0.15 or len(text) > LINE_CHARS))):
                cues.append(cur)
                cur = []
        cur.append(w)
    if cur:
        cues.append(cur)
    out = []
    for i, c in enumerate(cues):
        start = max(0.0, c[0][0] - LEAD)
        end = max(c[-1][1] + TAIL, start + MIN_DUR)
        if i + 1 < len(cues):
            end = min(end, cues[i + 1][0][0] - LEAD - 0.02)
        out.append((start, max(end, start + 0.3), " ".join(x[2] for x in c)))
    return out


def two_lines(text):
    if len(text) <= LINE_CHARS:
        return text
    mid = len(text) // 2
    spaces = [i for i, ch in enumerate(text) if ch == " "]
    if not spaces:
        return text
    cut = min(spaces, key=lambda i: abs(i - mid))
    return text[:cut] + "\n" + text[cut + 1:]


def stamp(t):
    ms = int(round(t * 1000))
    return "%02d:%02d:%02d,%03d" % (ms // 3600000, ms // 60000 % 60, ms // 1000 % 60, ms % 1000)


def subtitle(video, lang, force, out=None):
    srt = os.path.splitext(video)[0] + ".srt"
    if out:
        srt = os.path.join(out, os.path.basename(srt))
    if os.path.exists(srt) and not force:
        print("skip (has subtitles):", video)
        return
    print("subtitling:", video, flush=True)
    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "audio.wav")
        subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", video, wav], check=True)
        base = os.path.join(tmp, "out")
        subprocess.run([CLI, "-m", MODEL, "-f", wav, "-l", lang, "--dtw", "large.v3.turbo",
                        "-nfa", "-ojf", "-of", base, "-np"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cues = group(words_from_json(base + ".json"))
    with open(srt + ".tmp", "w", encoding="utf-8") as f:
        for i, (a, b, text) in enumerate(cues, 1):
            f.write("%d\n%s --> %s\n%s\n\n" % (i, stamp(a), stamp(b), two_lines(text)))
    os.replace(srt + ".tmp", srt)
    print("  %d lines -> %s" % (len(cues), srt))


def main():
    args = sys.argv[1:]
    force = "--force" in args
    lang = "en"
    out = None
    if "--lang" in args:
        lang = args[args.index("--lang") + 1]
    if "--out" in args:
        out = args[args.index("--out") + 1]
        os.makedirs(out, exist_ok=True)
    paths = [a for i, a in enumerate(args) if not a.startswith("--") and (i == 0 or args[i - 1] not in ("--lang", "--out"))]
    if not paths:
        print(__doc__)
        return 1
    videos = []
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                dirs.sort()
                for n in sorted(files):
                    if n.lower().endswith(VIDEO_EXT) and not n.startswith("._"):
                        videos.append(os.path.join(root, n))
        else:
            videos.append(p)
    for v in videos:
        subtitle(v, lang, force, out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
