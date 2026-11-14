package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

func TestHistMatchesCppScheme(t *testing.T) {
	var h Hist
	for v := uint64(0); v < 1<<30; v = v*3/2 + 1 {
		i := hIndex(v)
		if hHigh(i) < v {
			t.Fatalf("bucket %d high %d < %d", i, hHigh(i), v)
		}
		if i > 0 && hHigh(i-1) >= v {
			t.Fatalf("value %d also fits bucket %d", v, i-1)
		}
		h.Record(v)
	}
	if h.Pct(100) != h.Max {
		t.Fatal("p100 != max")
	}
}

func TestSnapshotThenDelta(t *testing.T) {
	s := NewState(Meta{Venue: "test"})
	apply := func(r Record) { s.Apply(&r) }
	apply(Record{Type: recSnapBegin})
	apply(Record{Type: recLevel, Side: 0, A: 100, B: 5})
	apply(Record{Type: recLevel, Side: 1, A: 101, B: 7})
	apply(Record{Type: recSnapEnd})
	apply(Record{Type: recTrade, Side: 1, A: 101, B: 2})
	apply(Record{Type: recLatency, E: 900, U32: 15000})
	snap := s.Snapshot()
	if len(snap.Bids) != 1 || snap.Bids[0] != (Level{100, 5}) || len(snap.Trades) != 1 {
		t.Fatalf("snapshot %+v", snap)
	}
	d := s.Delta()
	if d == nil || len(d.Bids) != 1 || len(d.Trades) != 1 || len(d.LatAdd) != 1 {
		t.Fatalf("first delta %+v", d)
	}
	// Nothing changed: no delta.
	if d := s.Delta(); d != nil {
		t.Fatalf("expected nil delta, got %+v", d)
	}
	// Level removed in the next snapshot -> qty 0 in the delta.
	apply(Record{Type: recSnapBegin})
	apply(Record{Type: recLevel, Side: 1, A: 101, B: 7})
	apply(Record{Type: recSnapEnd})
	d = s.Delta()
	if d == nil || len(d.Bids) != 1 || d.Bids[0] != (Level{100, 0}) || len(d.Asks) != 0 {
		t.Fatalf("removal delta %+v", d)
	}
}

// Writes a ring file in the C++ layout and reads it back, including lapping.
func TestRingReader(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "ring")
	const capRecs = 8
	buf := make([]byte, headerSize+capRecs*recSize)
	binary.LittleEndian.PutUint64(buf[0:], ringMagic)
	binary.LittleEndian.PutUint32(buf[12:], recSize)
	binary.LittleEndian.PutUint64(buf[16:], capRecs)
	if err := os.WriteFile(p, buf, 0o644); err != nil {
		t.Fatal(err)
	}
	r, err := OpenRing(p)
	if err != nil {
		t.Fatal(err)
	}
	// Write through a writable mapping, like the engine does.
	w, err := mapFile(p, 0, true)
	if err != nil {
		t.Fatal(err)
	}
	put := func(i uint64, a int64) {
		off := headerSize + (i%capRecs)*recSize
		b := w[off:]
		b[8] = recTrade
		binary.LittleEndian.PutUint64(b[16:], uint64(a))
		atomic.StoreUint64((*uint64)(unsafe.Pointer(&w[off])), i+1)
		atomic.StoreUint64((*uint64)(unsafe.Pointer(&w[64])), i+1)
	}
	for i := uint64(0); i < 5; i++ {
		put(i, int64(i*10))
	}
	var got []int64
	r.Poll(func(rec *Record) { got = append(got, rec.A) })
	if len(got) != 5 || got[4] != 40 {
		t.Fatalf("got %v", got)
	}
	for i := uint64(5); i < 30; i++ {
		put(i, int64(i))
	}
	got = nil
	r.Poll(func(rec *Record) { got = append(got, rec.A) })
	if len(got) != capRecs || got[0] != 22 || r.Lost != 30-5-capRecs {
		t.Fatalf("lapped read got %v lost %d", got, r.Lost)
	}
}

func TestSignToken(t *testing.T) {
	a := signToken([]byte("secret-secret-secret-secret-1234"), time.Unix(1000, 0))
	if a != signToken([]byte("secret-secret-secret-secret-1234"), time.Unix(1000, 0)) || len(a) != 4+1+64 {
		t.Fatalf("token %q", a)
	}
}
