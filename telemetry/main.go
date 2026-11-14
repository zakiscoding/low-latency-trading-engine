// Telemetry service: reads the engine's shared-memory ring (on its own cores, never
// the engine's), aggregates, and streams a snapshot plus 10-20 Hz deltas.
//
// Outputs:
//   - Uplink: an outbound-only WebSocket to the Cloudflare Worker's Durable Object
//     (no inbound ports on this machine). The DO fans it out to every viewer and
//     caches the latest snapshot at the edge. Admin commands come back down the
//     same connection and are written to the engine's control block.
//   - Local: ws://127.0.0.1:8090/stream and /snapshot for development.
//
// Usage:
//
//	telemetry --ring /dev/shm/lle-telemetry-crypto --control /dev/shm/lle-control \
//	          --venue bitstamp --product BTC-USD \
//	          --uplink wss://<worker>/ingest --uplink-secret-file /run/secrets/ingest
package main

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/coder/websocket"
)

type AdminCmd struct {
	T      string  `json:"t"` // "admin"
	ID     string  `json:"id"`
	Op     string  `json:"op"` // kill | unkill | start | stop | limits | replay | status
	Limits *Limits `json:"limits,omitempty"`
}

type AdminAck struct {
	T     string `json:"t"` // "ack"
	ID    string `json:"id"`
	OK    bool   `json:"ok"`
	Error string `json:"error,omitempty"`
	Kill  bool   `json:"kill"`
	Trade bool   `json:"trading"`
}

type hub struct {
	mu   sync.Mutex
	subs map[chan []byte]struct{}
}

func (h *hub) add() chan []byte {
	c := make(chan []byte, 64)
	h.mu.Lock()
	h.subs[c] = struct{}{}
	h.mu.Unlock()
	return c
}
func (h *hub) del(c chan []byte) { h.mu.Lock(); delete(h.subs, c); h.mu.Unlock() }
func (h *hub) pub(b []byte) {
	h.mu.Lock()
	for c := range h.subs {
		select {
		case c <- b:
		default: // slow local viewer: drop, it resyncs from the next snapshot
		}
	}
	h.mu.Unlock()
}

type server struct {
	st      *State
	ctrl    *Control
	hub     *hub
	replay  string
	paused  bool
	pauseMu sync.Mutex
}

func (s *server) handleAdmin(c AdminCmd) AdminAck {
	ack := AdminAck{T: "ack", ID: c.ID, OK: true}
	if s.ctrl == nil {
		ack.OK, ack.Error = false, "no control block attached"
		return ack
	}
	t, f := true, false
	switch c.Op {
	case "kill":
		s.ctrl.Apply(&t, nil, nil, false)
	case "unkill":
		s.ctrl.Apply(&f, nil, nil, false)
	case "start":
		s.ctrl.Apply(nil, &t, nil, false)
	case "stop":
		s.ctrl.Apply(nil, &f, nil, false)
	case "limits":
		if c.Limits == nil {
			ack.OK, ack.Error = false, "limits missing"
		} else {
			s.ctrl.Apply(nil, nil, c.Limits, false)
		}
	case "replay":
		s.ctrl.Apply(nil, nil, nil, true)
		if s.replay != "" {
			// Fixed command from the service's own flags; nothing from the request is
			// passed to the shell.
			parts := strings.Fields(s.replay)
			cmd := exec.Command(parts[0], parts[1:]...)
			if err := cmd.Start(); err != nil {
				ack.OK, ack.Error = false, err.Error()
			} else {
				go cmd.Wait()
			}
		}
	case "status":
	default:
		ack.OK, ack.Error = false, "unknown op"
	}
	ack.Kill, ack.Trade = s.ctrl.State()
	log.Printf("admin %s id=%s ok=%v", c.Op, c.ID, ack.OK)
	return ack
}

// signToken: HMAC-SHA256(secret, "ingest:"+unix_seconds), sent as
// "Authorization: Bearer <ts>.<hex>". The Worker checks it within +-120 s.
func signToken(secret []byte, now time.Time) string {
	ts := strconv.FormatInt(now.Unix(), 10)
	m := hmac.New(sha256.New, secret)
	m.Write([]byte("ingest:" + ts))
	return ts + "." + hex.EncodeToString(m.Sum(nil))
}

func (s *server) uplink(ctx context.Context, url string, secret []byte, rate time.Duration) {
	backoff := time.Second
	for ctx.Err() == nil {
		hdr := http.Header{}
		hdr.Set("Authorization", "Bearer "+signToken(secret, time.Now()))
		c, _, err := websocket.Dial(ctx, url, &websocket.DialOptions{HTTPHeader: hdr})
		if err != nil {
			log.Printf("uplink: dial failed: %v (retry in %v)", err, backoff)
			select {
			case <-time.After(backoff):
			case <-ctx.Done():
				return
			}
			if backoff < 30*time.Second {
				backoff *= 2
			}
			continue
		}
		backoff = time.Second
		log.Printf("uplink: connected to %s", url)
		c.SetReadLimit(1 << 20)
		s.runUplink(ctx, c, rate)
		c.CloseNow()
	}
}

func (s *server) runUplink(ctx context.Context, c *websocket.Conn, rate time.Duration) {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	send := func(v any) error {
		b, _ := json.Marshal(v)
		wctx, wc := context.WithTimeout(ctx, 5*time.Second)
		defer wc()
		return c.Write(wctx, websocket.MessageText, b)
	}
	// Reader: admin commands and flow control from the DO.
	go func() {
		defer cancel()
		for {
			_, b, err := c.Read(ctx)
			if err != nil {
				return
			}
			var m AdminCmd
			if json.Unmarshal(b, &m) != nil {
				continue
			}
			switch m.T {
			case "admin":
				if err := send(s.handleAdmin(m)); err != nil {
					return
				}
			case "pause", "resume":
				s.pauseMu.Lock()
				s.paused = m.T == "pause"
				s.pauseMu.Unlock()
				log.Printf("uplink: %s", m.T)
			case "snap?":
				send(s.st.Snapshot())
			}
		}
	}()
	if send(s.st.Snapshot()) != nil {
		return
	}
	tick := time.NewTicker(rate)
	defer tick.Stop()
	snapEvery := time.NewTicker(30 * time.Second)
	defer snapEvery.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-snapEvery.C:
			if send(s.st.Snapshot()) != nil {
				return
			}
		case <-tick.C:
			s.pauseMu.Lock()
			p := s.paused
			s.pauseMu.Unlock()
			if p {
				continue
			}
			if d := s.st.Delta(); d != nil {
				if send(d) != nil {
					return
				}
			}
		}
	}
}

func main() {
	ringPath := flag.String("ring", "/dev/shm/lle-telemetry-crypto", "engine telemetry ring")
	ctrlPath := flag.String("control", "", "engine control block (admin); empty = read-only")
	venue := flag.String("venue", "bitstamp", "venue name shown to viewers")
	product := flag.String("product", "BTC-USD", "product")
	listen := flag.String("listen", "127.0.0.1:8090", "local debug listener")
	uplinkURL := flag.String("uplink", "", "wss://.../ingest of the Worker (empty = local only)")
	secretFile := flag.String("uplink-secret-file", "", "file holding the shared ingest secret")
	hz := flag.Int("hz", 10, "delta rate (10-20)")
	replayCmd := flag.String("replay-cmd", "", "command run for the admin 'replay' op")
	flag.Parse()

	if *venue == "coinbase" && *uplinkURL != "" {
		log.Fatal("refusing to publish Coinbase data: its market data terms forbid showing it to others")
	}
	attribution := map[string]string{
		"bitstamp": "Market data: Bitstamp (bitstamp.net)",
		"itch":     "Replay of a NASDAQ TotalView-ITCH 5.0 sample file",
	}[*venue]
	st := NewState(Meta{Venue: *venue, Product: *product, PriceDec: 2, QtyDec: 8, Attribution: attribution})
	if *venue == "itch" {
		st.Meta.QtyDec = 0
	}
	srv := &server{st: st, hub: &hub{subs: map[chan []byte]struct{}{}}, replay: *replayCmd}
	if *ctrlPath != "" {
		c, err := OpenControl(*ctrlPath)
		if err != nil {
			log.Fatalf("control: %v", err)
		}
		srv.ctrl = c
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	// Ring reader: polls every 2 ms; reopens if the engine restarts.
	go func() {
		var ring *Ring
		for ctx.Err() == nil {
			if ring == nil {
				r, err := OpenRing(*ringPath)
				if err != nil {
					time.Sleep(time.Second)
					continue
				}
				ring = r
				log.Printf("ring: attached %s (capacity %d)", *ringPath, r.cap)
			}
			ring.Poll(st.Apply)
			if srv.ctrl != nil {
				k, t := srv.ctrl.State()
				st.SetControlState(k, t, ring.Lost)
			}
			time.Sleep(2 * time.Millisecond)
		}
	}()

	rate := time.Second / time.Duration(max(1, min(*hz, 20)))
	if *uplinkURL != "" {
		secret, err := os.ReadFile(*secretFile)
		if err != nil || len(strings.TrimSpace(string(secret))) < 32 {
			log.Fatalf("uplink needs --uplink-secret-file with >= 32 chars")
		}
		go srv.uplink(ctx, *uplinkURL, []byte(strings.TrimSpace(string(secret))), rate)
	}
	// Local publisher for the debug listener.
	go func() {
		t := time.NewTicker(rate)
		for range t.C {
			if d := st.Delta(); d != nil {
				b, _ := json.Marshal(d)
				srv.hub.pub(b)
			}
		}
	}()

	mux := http.NewServeMux()
	mux.HandleFunc("/snapshot", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(st.Snapshot())
	})
	mux.HandleFunc("/stream", func(w http.ResponseWriter, r *http.Request) {
		c, err := websocket.Accept(w, r, &websocket.AcceptOptions{OriginPatterns: []string{"localhost:*", "127.0.0.1:*"}})
		if err != nil {
			return
		}
		defer c.CloseNow()
		b, _ := json.Marshal(st.Snapshot())
		if c.Write(r.Context(), websocket.MessageText, b) != nil {
			return
		}
		ch := srv.hub.add()
		defer srv.hub.del(ch)
		for {
			select {
			case <-r.Context().Done():
				return
			case m := <-ch:
				if c.Write(r.Context(), websocket.MessageText, m) != nil {
					return
				}
			}
		}
	})
	if !strings.HasPrefix(*listen, "127.0.0.1:") && !strings.HasPrefix(*listen, "localhost:") {
		log.Fatal("--listen must be loopback; public access goes through the Worker")
	}
	hs := &http.Server{Addr: *listen, Handler: mux, ReadHeaderTimeout: 5 * time.Second}
	go func() {
		<-ctx.Done()
		hs.Close()
	}()
	fmt.Fprintf(os.Stderr, "telemetry: ring=%s venue=%s listen=%s uplink=%v\n", *ringPath, *venue, *listen, *uplinkURL != "")
	if err := hs.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
