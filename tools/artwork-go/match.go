// SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda

package main

// Finding a game's picture on thumbnails.libretro.com: what the server has, and which of it is this game.

import (
	"fmt"
	"html"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

const (
	baseURL   = "https://thumbnails.libretro.com/"
	userAgent = "G36-artwork/1.0 (handheld front end; personal use)"
	// bump when the matching gets better: older "not found" notes are then ignored
	matcherVersion = "2"
)

// the kinds of picture, best first: the first one the server has for a game is used
var artTypes = []string{"Named_Boxarts", "Named_Snaps", "Named_Titles"}

// G36 system folder -> the server's folders to look in (first match wins)
var serverDirs = map[string][]string{
	"gba":          {"Nintendo - Game Boy Advance"},
	"gbc":          {"Nintendo - Game Boy Color", "Nintendo - Game Boy"},
	"gb":           {"Nintendo - Game Boy", "Nintendo - Game Boy Color"},
	"nes":          {"Nintendo - Nintendo Entertainment System"},
	"famicom":      {"Nintendo - Nintendo Entertainment System", "Nintendo - Family Computer Disk System"},
	"fds":          {"Nintendo - Family Computer Disk System"},
	"snes":         {"Nintendo - Super Nintendo Entertainment System"},
	"sfc":          {"Nintendo - Super Nintendo Entertainment System"},
	"megadrive":    {"Sega - Mega Drive - Genesis"},
	"genesis":      {"Sega - Mega Drive - Genesis"},
	"mastersystem": {"Sega - Master System - Mark III"},
	"gamegear":     {"Sega - Game Gear"},
	"sg-1000":      {"Sega - SG-1000"},
	"segacd":       {"Sega - Mega-CD - Sega CD"},
	"psx":          {"Sony - PlayStation"},
	"pcengine":     {"NEC - PC Engine - TurboGrafx 16"},
	"tg16":         {"NEC - PC Engine - TurboGrafx 16"},
	"pcenginecd":   {"NEC - PC Engine CD - TurboGrafx-CD"},
	"ngp":          {"SNK - Neo Geo Pocket", "SNK - Neo Geo Pocket Color"},
	"ngpc":         {"SNK - Neo Geo Pocket Color", "SNK - Neo Geo Pocket"},
	"neogeo":       {"SNK - Neo Geo", "FBNeo - Arcade Games", "MAME"},
	"neogeocd":     {"SNK - Neo Geo CD"},
	"arcade":       {"FBNeo - Arcade Games", "MAME"},
	"fbneo":        {"FBNeo - Arcade Games"},
	"mame":         {"MAME", "FBNeo - Arcade Games"},
	"cps1":         {"FBNeo - Arcade Games", "MAME"},
	"cps2":         {"FBNeo - Arcade Games", "MAME"},
	"cps3":         {"FBNeo - Arcade Games", "MAME"},
	"atari2600":    {"Atari - 2600"},
	"atari5200":    {"Atari - 5200"},
	"atari7800":    {"Atari - 7800"},
	"atari800":     {"Atari - 8-bit"},
	"atarilynx":    {"Atari - Lynx"},
}

// arcade games are named by short MAME names (2020bb.zip); the title list turns them into names
var arcadeSystems = map[string]bool{"neogeo": true, "arcade": true, "fbneo": true, "mame": true, "cps1": true, "cps2": true, "cps3": true}

var badChars = regexp.MustCompile("[&*/:`<>?\\\\|\"]") // the server writes these as "_"

// ---- the network

type netError struct{ msg string }

func (e *netError) Error() string { return e.msg }

var httpClient = &http.Client{Timeout: 45 * time.Second}

// get returns the bytes, nil if the server says there is no such file, or a *netError if it can't be reached
func get(u string) ([]byte, error) {
	var last error
	for i := 0; i < 3; i++ {
		req, err := http.NewRequest("GET", u, nil)
		if err != nil {
			return nil, &netError{err.Error()}
		}
		req.Header.Set("User-Agent", userAgent)
		resp, err := httpClient.Do(req)
		if err == nil {
			data, rerr := io.ReadAll(resp.Body)
			resp.Body.Close()
			if resp.StatusCode == 404 {
				return nil, nil
			}
			if resp.StatusCode == 200 && rerr == nil {
				return data, nil
			}
			if rerr != nil {
				last = rerr
			} else {
				last = fmt.Errorf("server answered %d", resp.StatusCode)
			}
		} else {
			last = err
		}
		time.Sleep(time.Duration(1+i) * time.Second)
	}
	return nil, &netError{fmt.Sprintf("%s: %v", u, last)}
}

func pictureURL(e entry) string {
	return baseURL + url.PathEscape(e.sdir) + "/" + e.art + "/" + url.PathEscape(e.name)
}

var hrefPNG = regexp.MustCompile(`href="([^"?/][^"]*\.png)"`)

// listing: the picture names on the server in <sdir>/<art>/ (kept in the cache folder for 30 days)
func listing(cache, sdir, art string) ([]string, error) {
	path := filepath.Join(cache, strings.ReplaceAll(sdir+"__"+art, "/", "_")+".txt")
	if st, err := os.Stat(path); err == nil && time.Since(st.ModTime()) < 30*24*time.Hour {
		if b, err := os.ReadFile(path); err == nil {
			if len(b) == 0 {
				return nil, nil
			}
			return strings.Split(string(b), "\n"), nil
		}
	}
	data, err := get(baseURL + url.PathEscape(sdir) + "/" + art + "/")
	if err != nil {
		return nil, err
	}
	var names []string
	for _, m := range hrefPNG.FindAllStringSubmatch(string(data), -1) {
		n, err := url.PathUnescape(m[1])
		if err != nil {
			n = m[1]
		}
		names = append(names, html.UnescapeString(n))
	}
	os.MkdirAll(cache, 0o755)
	os.WriteFile(path, []byte(strings.Join(names, "\n")), 0o644)
	return names, nil
}

// ---- names

var (
	reTags     = regexp.MustCompile(`\(([^)]*)\)`)
	reGroups   = regexp.MustCompile(`\([^)]*\)|\[[^\]]*\]`)
	reThe      = regexp.MustCompile(`^(.*?),\s*(The|A|An)\s*(?:-\s*(.*))?$`)
	reAnd      = regexp.MustCompile(`(?i)\band\b`)
	reNotAlnum = regexp.MustCompile(`[^a-z0-9]+`)
	reNumber   = regexp.MustCompile(`^\s*\d{1,3}\s*[-._ ]\s*`)
	reRev      = regexp.MustCompile(`rev\s*(\w+)`)
)

func tagsOf(name string) []string {
	var out []string
	for _, m := range reTags.FindAllStringSubmatch(name, -1) {
		out = append(out, strings.ToLower(m[1]))
	}
	return out
}

// titleKey: the title without the (region) [flags] bits, "Name, The" -> "the name", "&" / "and" / "_" alike, letters and digits only
func titleKey(name string) string {
	t := reGroups.ReplaceAllString(name, " ")
	t = reAnd.ReplaceAllString(strings.ReplaceAll(t, "&", " "), " ")
	if m := reThe.FindStringSubmatch(strings.TrimSpace(t)); m != nil {
		t = m[2] + " " + m[1]
		if m[3] != "" {
			t += " " + m[3]
		}
	}
	return reNotAlnum.ReplaceAllString(strings.ToLower(t), "")
}

var regionRank = []string{"usa", "world", "europe", "en", "japan"}

func tagSet(name string) map[string]bool {
	set := map[string]bool{}
	for _, p := range strings.Split(strings.ReplaceAll(strings.Join(tagsOf(name), ","), " ", ""), ",") {
		set[p] = true
	}
	return set
}

// pick: the best of several versions of one title: the region of the ROM first, then USA / World / Europe, then the newest revision
func pick(cands []string, romName string) string {
	want := tagSet(romName)
	type scored struct {
		name                    string
		overlap, proto, rank, r int
	}
	list := make([]scored, 0, len(cands))
	for _, c := range cands {
		parts := tagSet(c)
		joined := strings.ReplaceAll(strings.Join(tagsOf(c), ","), " ", "")
		s := scored{name: c, rank: 9}
		for p := range parts {
			if want[p] {
				s.overlap++
			}
		}
		for i, r := range regionRank {
			if parts[r] {
				s.rank = i
				break
			}
		}
		if strings.Contains(joined, "proto") || strings.Contains(joined, "beta") || strings.Contains(joined, "demo") || strings.Contains(joined, "sample") {
			s.proto = 1
		}
		if m := reRev.FindStringSubmatch(joined); m != nil {
			s.r = int(m[1][0])
		}
		list = append(list, s)
	}
	sort.Slice(list, func(i, j int) bool {
		a, b := list[i], list[j]
		if a.overlap != b.overlap {
			return a.overlap > b.overlap
		}
		if a.proto != b.proto {
			return a.proto < b.proto
		}
		if a.rank != b.rank {
			return a.rank < b.rank
		}
		if a.r != b.r {
			return a.r > b.r
		}
		return a.name < b.name
	})
	return list[0].name
}

// ---- what the server has for one G36 system

type entry struct{ sdir, art, name string }

type index struct {
	exact map[string]entry   // lower-case name -> entry
	byKey map[string][]entry // title key -> entries
}

func newIndex(cache, system string) (*index, error) {
	ix := &index{exact: map[string]entry{}, byKey: map[string][]entry{}}
	for _, art := range artTypes {
		for _, sdir := range serverDirs[system] {
			names, err := listing(cache, sdir, art)
			if err != nil {
				return nil, err
			}
			for _, n := range names {
				stem := strings.TrimSuffix(n, ".png")
				e := entry{sdir, art, n}
				// a better kind of picture (earlier in artTypes) is never replaced by a later one
				if _, ok := ix.exact[strings.ToLower(stem)]; !ok {
					ix.exact[strings.ToLower(stem)] = e
				}
				k := titleKey(stem)
				ix.byKey[k] = append(ix.byKey[k], e)
			}
		}
	}
	return ix, nil
}

func artRank(art string) int {
	for i, a := range artTypes {
		if a == art {
			return i
		}
	}
	return len(artTypes)
}

// find returns the entry and how it matched ("exact", "title", "fuzzy"), or ok=false
func (ix *index) find(romStem, altTitle string) (entry, string, bool) {
	for _, c := range []struct{ name, how string }{{romStem, "exact"}, {altTitle, "title"}} {
		if c.name == "" {
			continue
		}
		if e, ok := ix.exact[strings.ToLower(badChars.ReplaceAllString(c.name, "_"))]; ok {
			return e, c.how, true
		}
	}
	// last resorts: the title, the name as it is, then without a leading number ("01 Pokemon - Ruby Version")
	for _, name := range []string{altTitle, romStem, reNumber.ReplaceAllString(romStem, "")} {
		if name == "" {
			continue
		}
		cands := ix.byKey[titleKey(name)]
		if len(cands) == 0 {
			continue
		}
		best := len(artTypes)
		for _, c := range cands {
			if r := artRank(c.art); r < best {
				best = r
			}
		}
		var same []entry
		var names []string
		for _, c := range cands {
			if artRank(c.art) == best {
				same = append(same, c)
				names = append(names, c.name)
			}
		}
		top := pick(names, name)
		for _, c := range same {
			if c.name == top {
				return c, "fuzzy", true
			}
		}
	}
	return entry{}, "", false
}
