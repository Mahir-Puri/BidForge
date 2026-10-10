package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"math/rand"
	"net"
	"net/http"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

type campaign struct {
	ID         string   `json:"id"`
	Active     bool     `json:"active"`
	Categories []string `json:"categories"`
	Width      int      `json:"width"`
	Height     int      `json:"height"`
	Bid        int64    `json:"bid_milli_cpm"`
	Budget     int64    `json:"budget_micros"`
	Creative   string   `json:"creative_id"`
}
type banner struct {
	W int `json:"w"`
	H int `json:"h"`
}
type impression struct {
	ID     string  `json:"id"`
	Banner banner  `json:"banner"`
	Floor  float64 `json:"bidfloor"`
}
type site struct {
	Categories []string `json:"cat"`
}
type bidRequest struct {
	ID   string       `json:"id"`
	Tmax int          `json:"tmax"`
	Imp  []impression `json:"imp"`
	Site site         `json:"site"`
}
type bid struct {
	ID       string  `json:"id"`
	ImpID    string  `json:"impid"`
	Price    float64 `json:"price"`
	Campaign string  `json:"cid"`
}
type bidResponse struct {
	ID       string `json:"id"`
	Currency string `json:"cur"`
	Seats    []struct {
		Bids []bid `json:"bid"`
	} `json:"seatbid"`
}
type auction struct {
	Body                      []byte
	ID                        string
	Competitor                int64
	Missing, DuplicateOutcome bool
}
type attempt struct {
	Status                                                  int
	Latency, Processing                                     float64
	Timeout, Transport, BadResponse                         bool
	Won, Lost, Omitted, Notifications, NotificationFailures int
}
type report struct {
	ClientDeadlineMisses int                `json:"client_over_tmax"`
	Seed                 int64              `json:"seed"`
	UniqueInputs         int                `json:"unique_inputs"`
	UniqueDecisions      int                `json:"unique_decisions"`
	Attempts             int                `json:"bid_attempts"`
	HTTPCompleted        int                `json:"http_completed"`
	Bids                 int                `json:"bid_responses_including_duplicates"`
	NoBids               int                `json:"no_bid_responses_including_duplicates"`
	Overload             int                `json:"http_503"`
	OtherHTTP            int                `json:"other_http_errors"`
	Timeouts             int                `json:"timeouts"`
	TransportErrors      int                `json:"transport_errors"`
	BadResponses         int                `json:"invalid_bid_responses"`
	DuplicateMismatches  int                `json:"duplicate_decision_mismatches"`
	Won                  int                `json:"unique_wins"`
	Lost                 int                `json:"unique_losses"`
	Omitted              int                `json:"omitted_outcomes"`
	Notifications        int                `json:"notification_attempts"`
	NotificationFailures int                `json:"notification_failures"`
	Seconds              float64            `json:"workload_seconds"`
	CompletedRPS         float64            `json:"completed_requests_per_second"`
	Latency              map[string]float64 `json:"client_latency_ms"`
	Processing           map[string]float64 `json:"server_processing_ms"`
	StatsBefore          json.RawMessage    `json:"stats_before"`
	StatsAfter           json.RawMessage    `json:"stats_after"`
	DeadlineNoBids       int64              `json:"deadline_no_bids_delta"`
	InvariantViolations  int64              `json:"invariant_violations"`
}

func save(path string, value any) error {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, append(data, '\n'), 0644)
}
func generate(args []string) error {
	f := flag.NewFlagSet("generate", flag.ContinueOnError)
	count := f.Int("campaigns", 1000, "campaign count")
	cats := f.Int("categories", 100, "category count")
	seed := f.Int64("seed", 1, "input seed")
	budget := f.Int64("budget", 50000000, "microdollars per campaign")
	output := f.String("output", "campaigns.json", "output file")
	if err := f.Parse(args); err != nil {
		return err
	}
	if *count < 1 || *count > 1000000 || *cats < 1 || *budget < 5000 {
		return errors.New("campaigns 1..1000000, categories positive, budget >= 5000 required")
	}
	rng := rand.New(rand.NewSource(*seed))
	cs := make([]campaign, *count)
	for i := range cs {
		cs[i] = campaign{fmt.Sprintf("campaign-%06d", i), true, []string{fmt.Sprintf("cat-%d", i%*cats)}, 300, 250, int64(1000 + rng.Intn(4001)), *budget, fmt.Sprintf("creative-%d", i)}
	}
	return save(*output, cs)
}
func post(client *http.Client, url string, body []byte) (int, []byte, float64, error) {
	req, err := http.NewRequestWithContext(context.Background(), http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return 0, nil, 0, err
	}
	req.Header.Set("Content-Type", "application/json")
	res, err := client.Do(req)
	if err != nil {
		return 0, nil, 0, err
	}
	defer res.Body.Close()
	data, err := io.ReadAll(io.LimitReader(res.Body, 1024*1024+1))
	if err != nil {
		return 0, nil, 0, err
	}
	if len(data) > 1024*1024 {
		return 0, nil, 0, errors.New("oversized response")
	}
	micros, _ := strconv.ParseFloat(res.Header.Get("X-Processing-Micros"), 64)
	return res.StatusCode, data, micros / 1000, nil
}
func getStats(client *http.Client, url string) (json.RawMessage, error) {
	res, err := client.Get(url + "/stats")
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	data, err := io.ReadAll(io.LimitReader(res.Body, 64*1024*1024))
	if err != nil {
		return nil, err
	}
	if res.StatusCode != 200 || !json.Valid(data) {
		return nil, errors.New("invalid stats response")
	}
	return data, nil
}
func quantiles(values []float64) map[string]float64 {
	out := map[string]float64{"p50": 0, "p95": 0, "p99": 0}
	if len(values) == 0 {
		return out
	}
	sort.Float64s(values)
	for key, p := range map[string]float64{"p50": .50, "p95": .95, "p99": .99} {
		out[key] = values[int(math.Ceil(p*float64(len(values))))-1]
	}
	return out
}
func validateBid(body []byte, id string) (bid, error) {
	var r bidResponse
	if err := json.Unmarshal(body, &r); err != nil {
		return bid{}, err
	}
	if r.ID != id || r.Currency != "USD" || len(r.Seats) != 1 || len(r.Seats[0].Bids) != 1 {
		return bid{}, errors.New("unexpected bid envelope")
	}
	b := r.Seats[0].Bids[0]
	scaled := b.Price * 1000
	if b.ID == "" || b.ImpID != "imp-1" || b.Campaign == "" || b.Price <= 0 || b.Price > 1000000 || math.Abs(scaled-math.Round(scaled)) > 1e-6 {
		return bid{}, errors.New("invalid bid fields")
	}
	return b, nil
}

type decisionHistory struct {
	mutex      sync.Mutex
	decisions  map[string]string
	mismatches int
}

func submit(client *http.Client, url string, a auction, delay time.Duration, history *decisionHistory) attempt {
	r := attempt{}
	began := time.Now()
	status, body, processing, err := post(client, url+"/bid", a.Body)
	r.Latency = float64(time.Since(began).Microseconds()) / 1000
	r.Status = status
	r.Processing = processing
	if err != nil {
		var ne net.Error
		if errors.As(err, &ne) && ne.Timeout() {
			r.Timeout = true
		} else {
			r.Transport = true
		}
		return r
	}
	if status != 200 && status != 204 {
		return r
	}
	var b bid
	if status == 200 {
		b, err = validateBid(body, a.ID)
		if err != nil {
			r.BadResponse = true
			return r
		}
	}
	signature := fmt.Sprintf("%d:%s:%s:%.3f", status, b.ID, b.Campaign, b.Price)
	history.mutex.Lock()
	old, exists := history.decisions[a.ID]
	if exists && old != signature {
		history.mismatches++
	}
	if !exists {
		history.decisions[a.ID] = signature
	}
	history.mutex.Unlock()
	if exists || status == 204 {
		return r
	}
	// Ties go to the synthetic competitor. Cost is our submitted bid.
	outcome := "loss"
	if int64(math.Round(b.Price*1000)) > a.Competitor {
		outcome = "win"
		r.Won++
	} else {
		r.Lost++
	}
	if a.Missing {
		r.Omitted++
		return r
	}
	time.Sleep(delay)
	notification, _ := json.Marshal(map[string]string{"bid_id": b.ID, "outcome": outcome})
	repeats := 1
	if a.DuplicateOutcome {
		repeats = 2
	}
	for n := 0; n < repeats; n++ {
		r.Notifications++
		status, _, _, err := post(client, url+"/outcome", notification)
		if err != nil || status != 200 {
			r.NotificationFailures++
		}
	}
	return r
}

func run(args []string) error {
	f := flag.NewFlagSet("run", flag.ContinueOnError)
	url := f.String("url", "http://127.0.0.1:8080", "bidder URL")
	count := f.Int("requests", 1000, "unique auctions")
	workers := f.Int("concurrency", 8, "bounded workers")
	seed := f.Int64("seed", 1, "input seed")
	cats := f.Int("categories", 100, "category count")
	distribution := f.String("distribution", "uniform", "uniform or hot (80% category 0)")
	duplicate := f.Float64("duplicate-percent", 10, "chance of one extra request attempt")
	missing := f.Float64("missing-percent", 0, "chance of omitted outcome")
	duplicateOutcome := f.Float64("duplicate-outcome-percent", 10, "chance of one extra notification")
	delay := f.Duration("outcome-delay", 0, "delay before notification; worker stays occupied")
	timeout := f.Duration("timeout", 2*time.Second, "HTTP client timeout")
	tmax := f.Int("tmax", 100, "handler deadline milliseconds")
	prefix := f.String("prefix", "run", "request ID prefix; change between runs on same server")
	output := f.String("output", "results.json", "output file")
	if err := f.Parse(args); err != nil {
		return err
	}
	if *count < 1 || *count > 10000000 || *workers < 1 || *workers > 4096 || *cats < 1 || *timeout <= 0 || *delay < 0 || *tmax < 1 || *tmax > 60000 || len(*prefix) > 100 {
		return errors.New("invalid workload bounds")
	}
	for _, p := range []float64{*duplicate, *missing, *duplicateOutcome} {
		if math.IsNaN(p) || p < 0 || p > 100 {
			return errors.New("percentages must be 0..100")
		}
	}
	if *distribution != "uniform" && *distribution != "hot" {
		return errors.New("distribution must be uniform or hot")
	}
	*url = strings.TrimRight(*url, "/")
	rng := rand.New(rand.NewSource(*seed))
	auctions := make([]auction, *count)
	jobs := make([]int, 0, *count*2)
	for i := range auctions {
		category := rng.Intn(*cats)
		if *distribution == "hot" && rng.Intn(100) < 80 {
			category = 0
		}
		id := fmt.Sprintf("%s-%d", *prefix, i)
		body, err := json.Marshal(bidRequest{id, *tmax, []impression{{"imp-1", banner{300, 250}, 0}}, site{[]string{fmt.Sprintf("cat-%d", category)}}})
		if err != nil {
			return err
		}
		auctions[i] = auction{body, id, int64(rng.Intn(6001)), rng.Float64()*100 < *missing, rng.Float64()*100 < *duplicateOutcome}
		jobs = append(jobs, i)
		if rng.Float64()*100 < *duplicate {
			jobs = append(jobs, i)
		}
	}
	rng.Shuffle(len(jobs), func(i, j int) { jobs[i], jobs[j] = jobs[j], jobs[i] })
	// Reuse connections, but cap both idle and active connections.
	transport := &http.Transport{MaxIdleConns: *workers, MaxIdleConnsPerHost: *workers, MaxConnsPerHost: *workers, IdleConnTimeout: time.Second, DialContext: (&net.Dialer{Timeout: *timeout}).DialContext}
	defer transport.CloseIdleConnections()
	client := &http.Client{Transport: transport, Timeout: *timeout}
	before, err := getStats(client, *url)
	if err != nil {
		return fmt.Errorf("stats before: %w", err)
	}
	history := decisionHistory{decisions: make(map[string]string)}
	queue := make(chan int, *workers)
	results := make(chan attempt, *workers)
	var wg sync.WaitGroup
	start := time.Now()
	for worker := 0; worker < *workers; worker++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for index := range queue {
				results <- submit(client, *url, auctions[index], *delay, &history)
			}
		}()
	}
	go func() {
		for _, index := range jobs {
			queue <- index
		}
		close(queue)
		wg.Wait()
		close(results)
	}()
	report := report{Seed: *seed, UniqueInputs: *count, StatsBefore: before}
	latencies := make([]float64, 0, len(jobs))
	processing := make([]float64, 0, len(jobs))
	for r := range results {
		report.Attempts++
		if r.Latency > float64(*tmax) {
			report.ClientDeadlineMisses++
		}
		latencies = append(latencies, r.Latency)
		if r.Timeout {
			report.Timeouts++
			continue
		}
		if r.Transport {
			report.TransportErrors++
			continue
		}
		report.HTTPCompleted++
		if r.BadResponse {
			report.BadResponses++
		}
		switch r.Status {
		case 200:
			report.Bids++
			processing = append(processing, r.Processing)
		case 204:
			report.NoBids++
			processing = append(processing, r.Processing)
		case 503:
			report.Overload++
		default:
			report.OtherHTTP++
		}
		report.Won += r.Won
		report.Lost += r.Lost
		report.Omitted += r.Omitted
		report.Notifications += r.Notifications
		report.NotificationFailures += r.NotificationFailures
	}
	report.Seconds = time.Since(start).Seconds()
	report.CompletedRPS = float64(report.HTTPCompleted) / report.Seconds
	report.Latency = quantiles(latencies)
	report.Processing = quantiles(processing)
	report.UniqueDecisions = len(history.decisions)
	report.DuplicateMismatches = history.mismatches
	after, err := getStats(client, *url)
	if err != nil {
		return fmt.Errorf("stats after: %w", err)
	}
	report.StatsAfter = after
	var beforeCounters, afterCounters struct {
		Deadline   int64 `json:"deadline_no_bids"`
		Violations int64 `json:"invariant_violations"`
	}
	if err = json.Unmarshal(before, &beforeCounters); err != nil {
		return err
	}
	if err = json.Unmarshal(after, &afterCounters); err != nil {
		return err
	}
	report.DeadlineNoBids = afterCounters.Deadline - beforeCounters.Deadline
	report.InvariantViolations = afterCounters.Violations
	if err = save(*output, report); err != nil {
		return err
	}
	fmt.Printf("%d attempts, %d unique decisions, %.1f completed requests/s, p99 %.3f ms; saved %s\n", report.Attempts, report.UniqueDecisions, report.CompletedRPS, report.Latency["p99"], *output)
	if report.InvariantViolations != 0 || report.DuplicateMismatches != 0 || report.BadResponses != 0 {
		return errors.New("correctness check failed; see result file")
	}
	return nil
}
func main() {
	var err error
	if len(os.Args) < 2 {
		err = errors.New("usage: loadgen generate|run [flags]")
	} else {
		switch os.Args[1] {
		case "generate":
			err = generate(os.Args[2:])
		case "run":
			err = run(os.Args[2:])
		default:
			err = errors.New("unknown command")
		}
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
