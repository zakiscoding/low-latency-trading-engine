package main

// Reader for the engine's shared-memory telemetry ring and writer for its control
// block. Layouts mirror engine/include/lle/shm.hpp exactly (x86-64, little endian).

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"sync/atomic"
	"syscall"
	"unsafe"
)

const (
	ringMagic  = 0x4C4C4554454C3031 // "LLETEL01"
	ctrlMagic  = 0x4C4C4543544C3031 // "LLECTL01"
	headerSize = 128
	recSize    = 64
)

// Record types (shm::RecType).
const (
	recTop       = 1
	recLevel     = 2
	recTrade     = 3
	recOrder     = 4
	recFill      = 5
	recLatency   = 6
	recStats     = 7
	recSnapBegin = 8
	recSnapEnd   = 9
)

type Record struct {
	Type uint8
	Side uint8
	Book uint16
	U32  uint32
	A, B int64
	C, D int64
	E    int64
	TsNs int64
}

type Ring struct {
	mem  []byte
	cap  uint64
	next uint64
	Lost uint64 // records overwritten before we read them
}

func mapFile(path string, size int, write bool) ([]byte, error) {
	flag := os.O_RDONLY
	prot := syscall.PROT_READ
	if write {
		flag = os.O_RDWR
		prot |= syscall.PROT_WRITE
	}
	f, err := os.OpenFile(path, flag, 0)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	if size == 0 {
		st, err := f.Stat()
		if err != nil {
			return nil, err
		}
		size = int(st.Size())
	}
	return syscall.Mmap(int(f.Fd()), 0, size, prot, syscall.MAP_SHARED)
}

func OpenRing(path string) (*Ring, error) {
	mem, err := mapFile(path, 0, false)
	if err != nil {
		return nil, err
	}
	if len(mem) < headerSize {
		return nil, errors.New("ring file too small")
	}
	if binary.LittleEndian.Uint64(mem[0:8]) != ringMagic {
		return nil, fmt.Errorf("%s: bad magic (engine not started yet?)", path)
	}
	if rs := binary.LittleEndian.Uint32(mem[12:16]); rs != recSize {
		return nil, fmt.Errorf("record size %d, want %d", rs, recSize)
	}
	c := binary.LittleEndian.Uint64(mem[16:24])
	if uint64(len(mem)) < headerSize+c*recSize {
		return nil, errors.New("ring file shorter than its capacity")
	}
	r := &Ring{mem: mem, cap: c}
	r.next = r.head() // start from now, not from history
	return r, nil
}

func (r *Ring) u64(off uint64) *uint64 { return (*uint64)(unsafe.Pointer(&r.mem[off])) }
func (r *Ring) head() uint64           { return atomic.LoadUint64(r.u64(64)) }

// Poll calls f for each new record. Seqlock: a record is valid only if its
// sequence number is index+1 both before and after copying the payload.
func (r *Ring) Poll(f func(*Record)) int {
	h := r.head()
	if h-r.next > r.cap { // lapped: skip what was overwritten
		r.Lost += h - r.next - r.cap
		r.next = h - r.cap
	}
	n := 0
	var rec Record
	for ; r.next < h; r.next++ {
		off := headerSize + (r.next%r.cap)*recSize
		seq := r.u64(off)
		s1 := atomic.LoadUint64(seq)
		if s1 != r.next+1 {
			r.Lost++
			continue
		}
		b := r.mem[off+8 : off+64]
		rec.Type = b[0]
		rec.Side = b[1]
		rec.Book = binary.LittleEndian.Uint16(b[2:4])
		rec.U32 = binary.LittleEndian.Uint32(b[4:8])
		rec.A = int64(binary.LittleEndian.Uint64(b[8:16]))
		rec.B = int64(binary.LittleEndian.Uint64(b[16:24]))
		rec.C = int64(binary.LittleEndian.Uint64(b[24:32]))
		rec.D = int64(binary.LittleEndian.Uint64(b[32:40]))
		rec.E = int64(binary.LittleEndian.Uint64(b[40:48]))
		rec.TsNs = int64(binary.LittleEndian.Uint64(b[48:56]))
		if atomic.LoadUint64(seq) != s1 {
			r.Lost++
			continue
		}
		f(&rec)
		n++
	}
	return n
}

// Control block (shm::ControlBlock, 128 bytes).
type Control struct{ mem []byte }

const (
	cOffVersion  = 8
	cOffKill     = 16
	cOffTrading  = 20
	cOffMaxQty   = 24
	cOffMaxPos   = 32
	cOffNotional = 40
	cOffRate     = 48
	cOffPerDay   = 56
	cOffLoss     = 64
	cOffImbNum   = 72
	cOffImbDen   = 80
	cOffOrderQty = 88
	cOffReplay   = 96
)

func OpenControl(path string) (*Control, error) {
	mem, err := mapFile(path, 128, true)
	if err != nil {
		return nil, err
	}
	if binary.LittleEndian.Uint64(mem[0:8]) != ctrlMagic {
		return nil, fmt.Errorf("%s: bad control magic", path)
	}
	return &Control{mem: mem}, nil
}

func (c *Control) p64(off int) *int64  { return (*int64)(unsafe.Pointer(&c.mem[off])) }
func (c *Control) p32(off int) *uint32 { return (*uint32)(unsafe.Pointer(&c.mem[off])) }

type Limits struct {
	MaxOrderQty, MaxPosition, MaxNotional, MaxOrdersPerSec, MaxOrdersPerDay, MaxDailyLoss int64
	ImbalanceNum, ImbalanceDen, OrderQty                                                  int64
}

// Apply writes fields and then bumps the version (release), which is what the
// engine polls. Zero values mean "leave unchanged".
func (c *Control) Apply(kill, trading *bool, l *Limits, replay bool) {
	if kill != nil {
		v := uint32(0)
		if *kill {
			v = 1
		}
		atomic.StoreUint32(c.p32(cOffKill), v)
	}
	if trading != nil {
		v := uint32(0)
		if *trading {
			v = 1
		}
		atomic.StoreUint32(c.p32(cOffTrading), v)
	}
	if l != nil {
		set := func(off int, v int64) {
			if v > 0 {
				atomic.StoreInt64(c.p64(off), v)
			}
		}
		set(cOffMaxQty, l.MaxOrderQty)
		set(cOffMaxPos, l.MaxPosition)
		set(cOffNotional, l.MaxNotional)
		set(cOffRate, l.MaxOrdersPerSec)
		set(cOffPerDay, l.MaxOrdersPerDay)
		set(cOffLoss, l.MaxDailyLoss)
		set(cOffImbNum, l.ImbalanceNum)
		set(cOffImbDen, l.ImbalanceDen)
		set(cOffOrderQty, l.OrderQty)
	}
	if replay {
		atomic.AddInt64(c.p64(cOffReplay), 1)
	}
	atomic.AddUint64((*uint64)(unsafe.Pointer(&c.mem[cOffVersion])), 1)
}

func (c *Control) State() (kill, trading bool) {
	return atomic.LoadUint32(c.p32(cOffKill)) != 0, atomic.LoadUint32(c.p32(cOffTrading)) != 0
}
