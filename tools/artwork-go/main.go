// SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda

package main

// G36 Artwork: finds a picture for each game on the G36 card (or in a folder of games on the
// computer) and saves it where the handheld expects it. The G36 shrinks the pictures by itself.

import (
	"bufio"
	"bytes"
	_ "embed"
	"errors"
	"flag"
	"fmt"
	"hash/fnv"
	"image"
	"image/draw"
	"image/jpeg"
	"image/png"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

//go:embed systems.cfg
var embeddedSystems string

//go:embed arcade-names.txt
var embeddedArcade string

const (
	missDays = 60
	maxSide  = 320 // pictures are shrunk to this before saving: the handheld shows them at most 260 x 195
)

var skipExt = map[string]bool{".txt": true, ".png": true, ".jpg": true, ".jpeg": true, ".db": true, ".cfg": true, ".xml": true, ".srm": true, ".state": true, ".sav": true, ".nfo": true, ".pdf": true}

// ---- screen

var interactive bool

var stdin = bufio.NewReader(os.Stdin) // one reader for everything typed, so nothing buffered is lost

func say(format string, a ...any) { fmt.Printf(format+"\n", a...) }

func pause() {
	if interactive {
		fmt.Print("\nPress Enter to close this window. ")
		stdin.ReadString('\n')
	}
}

func fail(format string, a ...any) {
	fmt.Fprintf(os.Stderr, "\n"+format+"\n", a...)
	pause()
	os.Exit(1)
}

func commas(n int) string {
	s := strconv.Itoa(n)
	for i := len(s) - 3; i > 0; i -= 3 {
		s = s[:i] + "," + s[i:]
	}
	return s
}

func ask(prompt string) string {
	fmt.Print(prompt)
	line, _ := stdin.ReadString('\n')
	return strings.TrimSpace(line)
}

// ---- finding things

func isDir(p string) bool  { st, err := os.Stat(p); return err == nil && st.IsDir() }
func exists(p string) bool { _, err := os.Stat(p); return err == nil }

// the G36-GAMES drive: roms/ and some of the folders the G36 makes
func looksLikeCard(p string) bool {
	return isDir(filepath.Join(p, "roms")) &&
		(isDir(filepath.Join(p, "g36")) || isDir(filepath.Join(p, "thumbnails")) || isDir(filepath.Join(p, "bios")) || exists(filepath.Join(p, "README.txt")))
}

func findCards() []string {
	var roots []string
	switch runtime.GOOS {
	case "windows":
		for c := 'C'; c <= 'Z'; c++ { // (A: and B: are floppy drives: nothing to look at, and old Windows asks about an empty one)
			roots = append(roots, string(c)+`:\`)
		}
	case "darwin":
		if es, err := os.ReadDir("/Volumes"); err == nil {
			for _, e := range es {
				roots = append(roots, filepath.Join("/Volumes", e.Name()))
			}
		}
	default:
		for _, g := range []string{"/media/*/*", "/run/media/*/*", "/mnt/*", "/media/*"} {
			m, _ := filepath.Glob(g)
			roots = append(roots, m...)
		}
	}
	var found []string
	for _, r := range roots {
		if looksLikeCard(r) {
			found = append(found, r)
		}
	}
	return found
}

// the card this program sits on (in the card's root, or one or two folders down from it), or ""
func cardAroundProgram() string {
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	if real, err := filepath.EvalSymlinks(exe); err == nil {
		exe = real
	}
	dir := filepath.Dir(exe)
	for i := 0; i < 3; i++ {
		if looksLikeCard(dir) {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			break
		}
		dir = parent
	}
	return ""
}

func knownSystemDirs(p string) int {
	n := 0
	if es, err := os.ReadDir(p); err == nil {
		for _, e := range es {
			if _, ok := serverDirs[e.Name()]; ok && e.IsDir() {
				n++
			}
		}
	}
	return n
}

// what you type or drag into the window: quotes and the backslashes a Mac Terminal adds are removed
func cleanPath(s string) string {
	s = strings.TrimSpace(s)
	s = strings.Trim(s, `"'`)
	if runtime.GOOS != "windows" {
		s = strings.ReplaceAll(s, `\ `, " ")
	}
	return s
}

// ---- systems.cfg and the title list

func parseExtensions(cfg string) map[string]map[string]bool {
	out := map[string]map[string]bool{}
	for _, line := range strings.Split(cfg, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.Split(line, "|")
		if len(parts) < 3 {
			continue
		}
		set := map[string]bool{}
		for _, e := range strings.Split(parts[2], ",") {
			if e = strings.TrimSpace(strings.ToLower(e)); e != "" {
				set["."+e] = true
			}
		}
		out[strings.TrimSpace(parts[0])] = set
	}
	return out
}

func parseArcade(s string) map[string]string {
	m := map[string]string{}
	for _, line := range strings.Split(s, "\n") {
		if i := strings.IndexByte(line, '\t'); i > 0 {
			m[strings.ToLower(line[:i])] = strings.TrimRight(line[i+1:], "\r")
		}
	}
	return m
}

// every game file in the folder and the folders inside it (the menu's rules: not hidden, only the listed extensions)
func gameFiles(folder string, exts map[string]bool) []string {
	var out []string
	filepath.WalkDir(folder, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		name := d.Name()
		if strings.HasPrefix(name, ".") && path != folder {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if d.IsDir() {
			return nil
		}
		ext := strings.ToLower(filepath.Ext(name))
		if exts != nil {
			if !exts[ext] {
				return nil
			}
		} else if skipExt[ext] {
			return nil
		}
		out = append(out, name)
		return nil
	})
	sort.Strings(out)
	return out
}

// ---- the "not found" notes

type missKey struct{ system, stem string }

func loadMisses(path string, retry bool) map[missKey]int64 {
	m := map[missKey]int64{}
	if retry {
		return m
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return m
	}
	lines := strings.Split(string(b), "\n")
	if len(lines) == 0 || strings.TrimRight(lines[0], "\r") != "# matcher "+matcherVersion {
		return m
	}
	now := time.Now().Unix()
	for _, l := range lines[1:] {
		p := strings.Split(strings.TrimRight(l, "\r"), "\t")
		if len(p) != 3 {
			continue
		}
		if t, err := strconv.ParseInt(p[2], 10, 64); err == nil && now-t < missDays*86400 {
			m[missKey{p[0], p[1]}] = t
		}
	}
	return m
}

func saveMisses(path string, m map[missKey]int64) {
	var keys []missKey
	for k := range m {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].system != keys[j].system {
			return keys[i].system < keys[j].system
		}
		return keys[i].stem < keys[j].stem
	})
	var b strings.Builder
	b.WriteString("# matcher " + matcherVersion + "\n")
	for _, k := range keys {
		fmt.Fprintf(&b, "%s\t%s\t%d\n", k.system, k.stem, m[k])
	}
	os.MkdirAll(filepath.Dir(path), 0o755)
	tmp := path + ".part"
	if os.WriteFile(tmp, []byte(b.String()), 0o644) == nil {
		os.Rename(tmp, path)
	}
}

// ---- pictures

// shrink: the picture scaled down to maxSide on its longest side (averaging). Pictures with see-through
// parts stay PNG; solid ones are saved as JPEG (about a sixth of the size). Returns the bytes and the
// file extension; the original if it isn't a PNG.
func shrink(data []byte) ([]byte, string) {
	img, err := png.Decode(bytes.NewReader(data))
	if err != nil {
		return data, ".png"
	}
	b := img.Bounds()
	w, h := b.Dx(), b.Dy()
	nw, nh := w, h
	if w > maxSide || h > maxSide {
		nw, nh = maxSide, h*maxSide/w
		if h > w {
			nw, nh = w*maxSide/h, maxSide
		}
	}
	if nw < 1 {
		nw = 1
	}
	if nh < 1 {
		nh = 1
	}
	src := image.NewNRGBA(image.Rect(0, 0, w, h))
	draw.Draw(src, src.Bounds(), img, b.Min, draw.Src)
	dst := image.NewNRGBA(image.Rect(0, 0, nw, nh))
	opaque := true
	for y := 0; y < nh; y++ {
		sy0, sy1 := y*h/nh, (y+1)*h/nh
		if sy1 <= sy0 {
			sy1 = sy0 + 1
		}
		for x := 0; x < nw; x++ {
			sx0, sx1 := x*w/nw, (x+1)*w/nw
			if sx1 <= sx0 {
				sx1 = sx0 + 1
			}
			var r, g, bl, a, n uint64
			for j := sy0; j < sy1; j++ {
				for i := sx0; i < sx1; i++ {
					p := src.Pix[j*src.Stride+i*4:]
					al := uint64(p[3])
					r += uint64(p[0]) * al
					g += uint64(p[1]) * al
					bl += uint64(p[2]) * al
					a += al
					n++
				}
			}
			o := dst.Pix[y*dst.Stride+x*4:]
			if a > 0 {
				o[0], o[1], o[2], o[3] = uint8(r/a), uint8(g/a), uint8(bl/a), uint8(a/n)
			}
			if o[3] < 250 {
				opaque = false
			}
		}
	}
	var buf bytes.Buffer
	if opaque {
		if jpeg.Encode(&buf, dst, &jpeg.Options{Quality: 90}) == nil {
			return buf.Bytes(), ".jpg"
		}
		buf.Reset()
	}
	enc := png.Encoder{CompressionLevel: png.BestCompression}
	if enc.Encode(&buf, dst) != nil {
		return data, ".png"
	}
	return buf.Bytes(), ".png"
}

func havePicture(thumbs string, subs []string, system, stem string) bool {
	for _, sub := range subs {
		for _, ext := range []string{".png", ".jpg"} {
			if exists(filepath.Join(thumbs, sub, system, stem+ext)) || exists(filepath.Join(thumbs, sub, stem+ext)) {
				return true
			}
		}
	}
	return false
}

func removeParts(dir string) {
	filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err == nil && !d.IsDir() && strings.HasSuffix(p, ".part") {
			os.Remove(p)
		}
		return nil
	})
}

// ---- main

type job struct {
	url, dest string // dest: the path without the extension (.png or .jpg is chosen when the picture is saved)
}

func main() {
	if st, err := os.Stdin.Stat(); err == nil && st.Mode()&os.ModeCharDevice != 0 {
		interactive = true
	}
	dryRun := flag.Bool("dry-run", false, "only say what would be downloaded")
	yes := flag.Bool("yes", false, "don't ask before downloading")
	noPause := flag.Bool("no-pause", false, "don't wait for Enter at the end")
	retry := flag.Bool("retry-misses", false, "look again for games that weren't found before")
	only := flag.String("systems", "", "only these systems, comma-separated (for example gba,gbc)")
	out := flag.String("out", "", "where to save the pictures when there's no card (default: a folder 'G36 Artwork' next to this program)")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: %s [options] [card or games folder]\n\n", filepath.Base(os.Args[0]))
		flag.PrintDefaults()
	}
	flag.Parse()
	if *noPause {
		interactive = false
	}

	say("G36 Artwork")
	say("===========")
	say("Finds a picture for each of your games and gets it ready for the G36.")
	say("This computer needs an internet connection; the G36 doesn't.\n")

	// 1. where are the games?
	target := ""
	if flag.NArg() > 0 {
		target = cleanPath(flag.Arg(0))
	} else if own := cardAroundProgram(); own != "" {
		target = own // started from the card itself
	} else if cards := findCards(); len(cards) > 0 {
		target = cards[0]
		if len(cards) > 1 {
			say("More than one G36 card is plugged in; using %s", target)
		}
	}
	if target == "" {
		if !interactive {
			fail("Couldn't find the G36 card. Plug it into this computer, or give the folder with your games as an argument.")
		}
		say("I couldn't find the G36 card.")
		say("Plug the card into this computer and run this again - or, to get the pictures for games kept on")
		say("this computer, type or drag the folder that holds them (the one with a folder for each system) here.\n")
		target = cleanPath(ask("Folder (or just press Enter to quit): "))
		if target == "" {
			return
		}
	}
	if !isDir(target) {
		fail("%s isn't a folder.", target)
	}

	var roms, thumbs, dest string // dest: where the pictures are saved
	var cardMode bool
	var skipDirs []string // folders to look in for pictures that are already there
	if looksLikeCard(target) {
		cardMode = true
		roms = filepath.Join(target, "roms")
		thumbs = filepath.Join(target, "thumbnails")
		dest = filepath.Join(thumbs, "to-resize")
		skipDirs = []string{"ready", "resized", "to-resize", "large"}
		say("G36 card: %s", target)
	} else {
		roms = target
		if knownSystemDirs(roms) == 0 && isDir(filepath.Join(target, "roms")) {
			roms = filepath.Join(target, "roms")
		}
		if knownSystemDirs(roms) == 0 {
			fail("%s doesn't have folders named after game systems (like gba, snes, atari2600).\nThe G36 card has a folder called roms with them.", roms)
		}
		base := *out
		if base == "" {
			exe, _ := os.Executable()
			base = filepath.Join(filepath.Dir(exe), "G36 Artwork")
			if os.MkdirAll(base, 0o755) != nil {
				home, _ := os.UserHomeDir()
				base = filepath.Join(home, "G36 Artwork")
			}
		}
		thumbs = base
		dest = base
		skipDirs = []string{"."}
		say("Games folder: %s", roms)
	}

	cacheDir, err := os.UserCacheDir()
	if err != nil {
		cacheDir = os.TempDir()
	}
	cacheDir = filepath.Join(cacheDir, "g36-artwork")

	cfg := embeddedSystems
	if cardMode {
		if b, err := os.ReadFile(filepath.Join(target, "g36", "systems.cfg")); err == nil {
			cfg = string(b)
		}
	}
	exts := parseExtensions(cfg)
	arcade := parseArcade(embeddedArcade)
	want := map[string]bool{}
	for _, s := range strings.Split(*only, ",") {
		if s = strings.TrimSpace(s); s != "" {
			want[s] = true
		}
	}

	// the "not found" notes: on the card with the pictures; for a folder on the computer, in this program's cache
	// (not in the output folder, which gets copied to the G36 as it is)
	missPath := filepath.Join(thumbs, "artwork-misses.txt")
	if !cardMode {
		h := fnv.New32a()
		h.Write([]byte(roms))
		missPath = filepath.Join(cacheDir, fmt.Sprintf("misses-%08x.txt", h.Sum32()))
	}
	misses := loadMisses(missPath, *retry)
	newMisses := map[missKey]int64{}
	for k, v := range misses {
		newMisses[k] = v
	}
	visited := map[missKey]bool{}
	now := time.Now().Unix()
	removeParts(dest)

	// 2. what's missing
	say("\nLooking through your games...")
	entries, _ := os.ReadDir(roms)
	var systems []string
	for _, e := range entries {
		if e.IsDir() && !strings.HasPrefix(e.Name(), ".") {
			if _, ok := serverDirs[e.Name()]; ok && (len(want) == 0 || want[e.Name()]) {
				systems = append(systems, e.Name())
			}
		}
	}
	sort.Strings(systems)
	var jobs []job
	totalGames, totalHad, totalFound, totalLost, totalKnown := 0, 0, 0, 0, 0
	var netErr error
	for _, system := range systems {
		files := gameFiles(filepath.Join(roms, system), exts[system])
		if len(files) == 0 {
			continue
		}
		var ix *index
		seen := map[string]bool{}
		had, known, lost := 0, 0, 0
		how := map[string]int{}
		for _, f := range files {
			stem := strings.TrimSuffix(f, filepath.Ext(f))
			if seen[stem] { // the same name in two folders: one picture serves both
				continue
			}
			seen[stem] = true
			k := missKey{system, stem}
			visited[k] = true
			if havePicture(thumbs, skipDirs, system, stem) || (!cardMode && havePicture(dest, []string{"."}, system, stem)) {
				had++
				delete(newMisses, k)
				continue
			}
			if _, ok := misses[k]; ok {
				known++
				continue
			}
			if ix == nil {
				ix, netErr = newIndex(cacheDir, system)
				if netErr != nil {
					break
				}
			}
			alt := ""
			if arcadeSystems[system] {
				alt = arcade[strings.ToLower(stem)]
			}
			e, h, ok := ix.find(stem, alt)
			if !ok {
				lost++
				newMisses[k] = now
				continue
			}
			how[h]++
			jobs = append(jobs, job{pictureURL(e), filepath.Join(dest, system, stem)})
		}
		if netErr != nil {
			break
		}
		found := how["exact"] + how["title"] + how["fuzzy"]
		totalGames += len(seen)
		totalHad += had
		totalFound += found
		totalLost += lost
		totalKnown += known
		line := fmt.Sprintf("  %-13s %6s games: %6s already have a picture, %6s to get, %6s not on the server", system, commas(len(seen)), commas(had), commas(found), commas(lost))
		if known > 0 {
			line += fmt.Sprintf(" (+%s it already looked for)", commas(known))
		}
		say("%s", line)
	}
	if netErr != nil {
		var ne *netError
		if errors.As(netErr, &ne) {
			fail("Couldn't reach thumbnails.libretro.com. Check that this computer is online, then run this again.\nNothing was changed.\n(%s)", ne.msg)
		}
		fail("%v", netErr)
	}
	if totalGames == 0 {
		fail("No games found in %s.", roms)
	}
	say("\n%s games: %s already had a picture, %s to download, %s not on the server%s.", commas(totalGames), commas(totalHad), commas(totalFound), commas(totalLost),
		map[bool]string{true: fmt.Sprintf(", %s looked for before", commas(totalKnown)), false: ""}[totalKnown > 0])
	if len(want) == 0 && len(systems) > 0 { // a full run forgets notes about games that are gone
		for k := range newMisses {
			if !visited[k] {
				delete(newMisses, k)
			}
		}
	}
	if !*dryRun {
		saveMisses(missPath, newMisses)
	}
	if *dryRun {
		say("(Dry run: nothing downloaded.)")
		pause()
		return
	}
	if len(jobs) == 0 {
		say("Nothing to download.")
		pause()
		return
	}
	if interactive && !*yes {
		a := ask(fmt.Sprintf("\nDownload %s pictures now? [Y/n] ", commas(len(jobs))))
		if a != "" && !strings.HasPrefix(strings.ToLower(a), "y") {
			say("Nothing downloaded.")
			pause()
			return
		}
	}

	// 3. download
	say("")
	var done, saved, bytesSaved int64
	var wg sync.WaitGroup
	ch := make(chan job)
	start := time.Now()
	for i := 0; i < 6; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := range ch {
				data, err := get(j.url)
				if err == nil && data != nil {
					data, ext := shrink(data)
					path := j.dest + ext
					if os.MkdirAll(filepath.Dir(path), 0o755) == nil {
						tmp := path + ".part"
						if os.WriteFile(tmp, data, 0o644) == nil && os.Rename(tmp, path) == nil {
							atomic.AddInt64(&saved, 1)
							atomic.AddInt64(&bytesSaved, int64(len(data)))
						}
					}
				}
				n := atomic.AddInt64(&done, 1)
				if n%50 == 0 || int(n) == len(jobs) {
					say("  %s of %s (%d%%)", commas(int(n)), commas(len(jobs)), int(n)*100/len(jobs))
				}
			}
		}()
	}
	for _, j := range jobs {
		ch <- j
	}
	close(ch)
	wg.Wait()

	say("\nSaved %s pictures (%d MB) in %d seconds.", commas(int(saved)), bytesSaved>>20, int(time.Since(start).Seconds()))
	if int(saved) < len(jobs) {
		say("%s didn't download (a connection problem?). Run this again to get them.", commas(len(jobs)-int(saved)))
	}
	if cardMode {
		say("\nDone. Put the card back in your G36 and turn it on. It turns the pictures into thumbnails by itself")
		say("in the background; it can take a few minutes the first time. Games that have no picture on the")
		say("server keep showing \"No image\".")
	} else {
		say("\nDone. The pictures are in:\n  %s", dest)
		say("Now copy everything inside that folder (the folders named gba, gbc, ...) into the folder")
		say("thumbnails/to-resize on the G36-GAMES drive. The G36 turns them into thumbnails by itself.")
	}
	pause()
}
