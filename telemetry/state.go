package main

// Aggregated view of one engine, built from ring records, serialised as a full
// snapshot (for new viewers) or as a delta since the last tick (10-20 Hz).

import (
	"math/big"
	"math/bits"
	"sort"
	"sync"
	"time"
)

// Log-linear histogram, same bucket scheme as the C++ one (exact below 128, then
// 64 buckets per power of two).
const (
	hSubBits = 7
	hSub     = 1 << hSubBits
	hHalf    = hSub / 2
	hMaxExp  = 40
	hBuckets = hSub + hMaxExp*hHalf
)

type Hist struct {
	Counts [hBuckets]uint64
	N      uint64
	Max    uint64
}

func hIndex(v uint64) int {
	if v < hSub {
		return int(v)
	}
	msb := 63 - bits.LeadingZeros64(v)
	e := msb - hSubBits + 1
	if e > hMaxExp {
		return hBuckets - 1
	}
	return hSub + (e-1)*hHalf + int(v>>uint(e)) - hHalf
}

func hHigh(i int) uint64 {
	if i < hSub {
		return uint64(i)
	}
	j := i - hSub
	e := j/hHalf + 1
	sub := uint64(j%hHalf + hHalf)
	return (sub << uint(e)) + (1<<uint(e) - 1)
}

func (h *Hist) Record(v uint64) {
	h.Counts[hIndex(v)]++
	h.N++
	if v > h.Max {
		h.Max = v
	}
}

func (h *Hist) Pct(p float64) uint64 {
	if h.N == 0 {
		return 0
	}
	want := uint64(p/100*float64(h.N) + 0.5)
	if want < 1 {
		want = 1
	}
	var seen uint64
	for i, c := range h.Counts {
		seen += c
		if seen >= want {
			if hv := hHigh(i); hv < h.Max {
				return hv
			}
			return h.Max
		}
	}
	return h.Max
}

type HistJSON struct {
	N       uint64      `json:"n"`
	P50     uint64      `json:"p50"`
	P99     uint64      `json:"p99"`
	P999    uint64      `json:"p999"`
	Max     uint64      `json:"max"`
	Buckets [][2]uint64 `json:"buckets,omitempty"` // [upper_ns, count]
}

func (h *Hist) JSON(withBuckets bool) HistJSON {
	j := HistJSON{N: h.N, P50: h.Pct(50), P99: h.Pct(99), P999: h.Pct(99.9), Max: h.Max}
	if withBuckets {
		for i, c := range h.Counts {
			if c > 0 {
				j.Buckets = append(j.Buckets, [2]uint64{hHigh(i), c})
			}
		}
	}
	return j
}

type Trade struct {
	Ts    int64  `json:"ts"`
	Price int64  `json:"p"`
	Qty   int64  `json:"q"`
	Side  string `json:"s"` // side of the resting order that was hit: "b" or "a"
}

type Fill struct {
	Ts    int64  `json:"ts"`
	Side  string `json:"s"`
	Price int64  `json:"p"`
	Qty   int64  `json:"q"`
	Fee   int64  `json:"fee"` // micro-dollars
	Liq   string `json:"liq"`
}

type Level [2]int64 // price ticks, qty units

type Meta struct {
	Venue       string `json:"venue"`
	Product     string `json:"product"`
	PriceDec    int    `json:"price_dec"`
	QtyDec      int    `json:"qty_dec"`
	Attribution string `json:"attribution"`
}

type Stats struct {
	Messages  int64  `json:"messages"`
	Orders    int64  `json:"orders"`
	Fills     int64  `json:"fills"`
	Rejects   int64  `json:"rejects"`
	Position  int64  `json:"position"`
	CashMicro int64  `json:"cash_micros"` // realised cash incl. fees; P&L = cash + position*mid
	MarkMid   int64  `json:"mark_mid"`
	FeesMicro int64  `json:"fees_micros"`
	Lost      uint64 `json:"ring_lost"`
	Kill      bool   `json:"kill"`
	Trading   bool   `json:"trading"`
	EngineUp  bool   `json:"engine_up"`
}

type State struct {
	mu   sync.Mutex
	Meta Meta

	bids, asks         map[int64]int64 // current published depth (top N)
	pendBids, pendAsks map[int64]int64 // snapshot being assembled
	sentBids, sentAsks map[int64]int64 // what viewers have (for deltas)

	trades    []Trade // ring of recent trades
	newTrades []Trade
	fills     []Fill
	newFills  []Fill

	latT2T, latWire, latParse, latBook, latDecide, latSend Hist
	latNew                                                 map[int]uint64 // t2t bucket increments since last delta

	stats         Stats
	lastSentStats Stats
	lastRecord    time.Time
	seq           uint64
}

func NewState(m Meta) *State {
	return &State{
		Meta: m, bids: map[int64]int64{}, asks: map[int64]int64{},
		sentBids: map[int64]int64{}, sentAsks: map[int64]int64{}, latNew: map[int]uint64{},
	}
}

const keepTrades = 60
const keepFills = 40

func sideStr(s uint8) string {
	if s == 0 {
		return "b"
	}
	return "a"
}

// Apply ingests one ring record.
func (s *State) Apply(r *Record) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.lastRecord = time.Now()
	switch r.Type {
	case recSnapBegin:
		s.pendBids, s.pendAsks = map[int64]int64{}, map[int64]int64{}
	case recLevel:
		if s.pendBids == nil {
			return
		}
		if r.Side == 0 {
			s.pendBids[r.A] = r.B
		} else {
			s.pendAsks[r.A] = r.B
		}
	case recSnapEnd:
		if s.pendBids != nil {
			s.bids, s.asks = s.pendBids, s.pendAsks
			s.pendBids, s.pendAsks = nil, nil
		}
	case recTrade:
		t := Trade{Ts: r.TsNs, Price: r.A, Qty: r.B, Side: sideStr(r.Side)}
		s.trades = append(s.trades, t)
		if len(s.trades) > keepTrades {
			s.trades = s.trades[len(s.trades)-keepTrades:]
		}
		s.newTrades = append(s.newTrades, t)
	case recFill:
		liq := "T"
		if r.U32 == 'M' {
			liq = "M"
		}
		f := Fill{Ts: r.TsNs, Side: sideStr(r.Side), Price: r.A, Qty: r.B, Fee: r.D, Liq: liq}
		s.fills = append(s.fills, f)
		if len(s.fills) > keepFills {
			s.fills = s.fills[len(s.fills)-keepFills:]
		}
		s.newFills = append(s.newFills, f)
		// Cash in micro-dollars: price*qty*1e6 / 10^(price_dec+qty_dec).
		div := int64(1)
		for i := 0; i < s.Meta.PriceDec+s.Meta.QtyDec; i++ {
			div *= 10
		}
		n := new(big.Int).Mul(big.NewInt(r.A), big.NewInt(r.B))
		n.Mul(n, big.NewInt(1_000_000)).Quo(n, big.NewInt(div))
		if r.Side == 0 {
			s.stats.CashMicro -= n.Int64()
		} else {
			s.stats.CashMicro += n.Int64()
		}
		s.stats.CashMicro -= r.D
		s.stats.FeesMicro += r.D
	case recLatency:
		// a=parse b=book c=decide d=(build<<32|send) e=total, u32=wire (kernel RX -> after send)
		s.latParse.Record(uint64(r.A))
		s.latBook.Record(uint64(r.B))
		s.latDecide.Record(uint64(r.C))
		s.latSend.Record(uint64(r.D) & 0xFFFFFFFF)
		s.latT2T.Record(uint64(r.E))
		s.latNew[hIndex(uint64(r.E))]++
		if r.U32 > 0 {
			s.latWire.Record(uint64(r.U32))
		}
	case recStats:
		s.stats.Messages, s.stats.Orders, s.stats.Fills, s.stats.Rejects, s.stats.Position = r.A, r.B, r.C, r.D, r.E
		if s.bestMid() > 0 {
			s.stats.MarkMid = s.bestMid()
		}
	}
}

func (s *State) bestMid() int64 {
	var bb, ba int64 = 0, 0
	for p := range s.bids {
		if p > bb {
			bb = p
		}
	}
	for p := range s.asks {
		if ba == 0 || p < ba {
			ba = p
		}
	}
	if bb == 0 || ba == 0 {
		return 0
	}
	return (bb + ba) / 2
}

func levelsSorted(m map[int64]int64, desc bool) []Level {
	out := make([]Level, 0, len(m)) // never nil: JSON [] not null
	for p, q := range m {
		out = append(out, Level{p, q})
	}
	sort.Slice(out, func(i, j int) bool {
		if desc {
			return out[i][0] > out[j][0]
		}
		return out[i][0] < out[j][0]
	})
	return out
}

func diff(cur, sent map[int64]int64) []Level {
	var d []Level
	for p, q := range cur {
		if sent[p] != q {
			d = append(d, Level{p, q})
		}
	}
	for p := range sent {
		if _, ok := cur[p]; !ok {
			d = append(d, Level{p, 0})
		}
	}
	return d
}

func clone(m map[int64]int64) map[int64]int64 {
	c := make(map[int64]int64, len(m))
	for k, v := range m {
		c[k] = v
	}
	return c
}

type Snapshot struct {
	T       string              `json:"t"` // "snap"
	Seq     uint64              `json:"seq"`
	Ts      int64               `json:"ts"`
	Meta    Meta                `json:"meta"`
	Bids    []Level             `json:"bids"`
	Asks    []Level             `json:"asks"`
	Trades  []Trade             `json:"trades"`
	Fills   []Fill              `json:"fills"`
	Latency map[string]HistJSON `json:"latency"`
	Stats   Stats               `json:"stats"`
}

type Delta struct {
	T      string      `json:"t"` // "d"
	Seq    uint64      `json:"seq"`
	Ts     int64       `json:"ts"`
	Bids   []Level     `json:"bids,omitempty"` // qty 0 = remove
	Asks   []Level     `json:"asks,omitempty"`
	Trades []Trade     `json:"trades,omitempty"`
	Fills  []Fill      `json:"fills,omitempty"`
	LatAdd [][2]uint64 `json:"lat_add,omitempty"` // [bucket_upper_ns, count] added to t2t
	Lat    *HistJSON   `json:"lat,omitempty"`     // current t2t summary
	Stats  Stats       `json:"stats"`
}

func (s *State) latencyJSON() map[string]HistJSON {
	return map[string]HistJSON{
		"tick_to_trade":     s.latT2T.JSON(true),
		"kernel_rx_to_send": s.latWire.JSON(true),
		"parse":             s.latParse.JSON(false),
		"book":              s.latBook.JSON(false),
		"decide":            s.latDecide.JSON(false),
		"send":              s.latSend.JSON(false),
	}
}

func (s *State) engineUp() bool { return time.Since(s.lastRecord) < 3*time.Second }

// Snapshot returns the full state (for a new viewer / the edge cache).
func (s *State) Snapshot() Snapshot {
	s.mu.Lock()
	defer s.mu.Unlock()
	st := s.stats
	st.EngineUp = s.engineUp()
	return Snapshot{
		T: "snap", Seq: s.seq, Ts: time.Now().UnixNano(), Meta: s.Meta,
		Bids: levelsSorted(s.bids, true), Asks: levelsSorted(s.asks, false),
		Trades: append([]Trade{}, s.trades...), Fills: append([]Fill{}, s.fills...),
		Latency: s.latencyJSON(), Stats: st,
	}
}

// Delta returns what changed since the previous Delta call and advances the
// baseline. Returns nil if nothing changed.
func (s *State) Delta() *Delta {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := &Delta{T: "d", Ts: time.Now().UnixNano()}
	d.Bids = diff(s.bids, s.sentBids)
	d.Asks = diff(s.asks, s.sentAsks)
	d.Trades, s.newTrades = s.newTrades, nil
	d.Fills, s.newFills = s.newFills, nil
	for i, c := range s.latNew {
		d.LatAdd = append(d.LatAdd, [2]uint64{hHigh(i), c})
	}
	if len(d.LatAdd) > 0 {
		j := s.latT2T.JSON(false)
		d.Lat = &j
	}
	s.latNew = map[int]uint64{}
	st := s.stats
	st.EngineUp = s.engineUp()
	d.Stats = st
	if len(d.Bids)+len(d.Asks)+len(d.Trades)+len(d.Fills)+len(d.LatAdd) == 0 && st == s.lastSentStats {
		return nil
	}
	s.lastSentStats = st
	s.sentBids, s.sentAsks = clone(s.bids), clone(s.asks)
	s.seq++
	d.Seq = s.seq
	return d
}

func (s *State) SetControlState(kill, trading bool, lost uint64) {
	s.mu.Lock()
	s.stats.Kill, s.stats.Trading, s.stats.Lost = kill, trading, lost
	s.mu.Unlock()
}
