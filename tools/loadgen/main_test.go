package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"
)

func TestGenerateReproducible(t *testing.T) {
	dir := t.TempDir()
	a, b := filepath.Join(dir, "a.json"), filepath.Join(dir, "b.json")
	for _, path := range []string{a, b} {
		if err := generate([]string{"-campaigns", "20", "-seed", "7", "-output", path}); err != nil {
			t.Fatal(err)
		}
	}
	first, _ := os.ReadFile(a)
	second, _ := os.ReadFile(b)
	if string(first) != string(second) {
		t.Fatal("seed mismatch")
	}
	var cs []campaign
	if err := json.Unmarshal(first, &cs); err != nil {
		t.Fatal(err)
	}
	if len(cs) != 20 {
		t.Fatal("wrong campaign count")
	}
}
func TestQuantiles(t *testing.T) {
	q := quantiles([]float64{4, 1, 2, 3})
	if q["p50"] != 2 || q["p99"] != 4 {
		t.Fatal(q)
	}
}
func TestInvalidBid(t *testing.T) {
	if _, err := validateBid([]byte(`{"id":"r","cur":"USD","seatbid":[]}`), "r"); err == nil {
		t.Fatal("accepted missing bid")
	}
}
func TestDuplicateAuctionsSettleOnce(t *testing.T) {
	var notifications atomic.Int64
	s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		switch r.URL.Path {
		case "/stats":
			w.Write([]byte(`{"deadline_no_bids":0,"invariant_violations":0}`))
		case "/bid":
			var req bidRequest
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
				t.Error(err)
				return
			}
			json.NewEncoder(w).Encode(map[string]any{"id": req.ID, "cur": "USD", "seatbid": []any{map[string]any{"bid": []any{map[string]any{"id": "bid-" + req.ID, "impid": "imp-1", "cid": "a", "price": 3}}}}})
		case "/outcome":
			notifications.Add(1)
			w.Write([]byte(`{"state":"won"}`))
		}
	}))
	defer s.Close()
	output := filepath.Join(t.TempDir(), "report.json")
	if err := run([]string{"-url", s.URL, "-requests", "10", "-concurrency", "8", "-duplicate-percent", "100", "-duplicate-outcome-percent", "0", "-output", output}); err != nil {
		t.Fatal(err)
	}
	if notifications.Load() != 10 {
		t.Fatalf("got %d notifications", notifications.Load())
	}
	data, _ := os.ReadFile(output)
	var r report
	if err := json.Unmarshal(data, &r); err != nil {
		t.Fatal(err)
	}
	if r.Attempts != 20 || r.UniqueDecisions != 10 || r.DuplicateMismatches != 0 {
		t.Fatal(r)
	}
}
