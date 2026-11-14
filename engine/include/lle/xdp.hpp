#pragma once
// AF_XDP receive path (ladder step 3b). Linux only, no libbpf/libxdp dependency:
// the XDP program is ten-odd hand-assembled eBPF instructions, loaded and attached
// with the raw bpf() syscall (BPF_LINK_CREATE, so it detaches when the process
// exits or crashes).
//
// The program passes everything to the normal kernel stack except IPv4/UDP (no IP
// options) to one destination port. Those packets get a CLOCK_MONOTONIC stamp
// (bpf_ktime_get_ns) written into the XDP metadata area, then go to the AF_XDP
// socket bound to the receiving queue. The stamp is taken at the XDP hook, earlier
// than the SO_TIMESTAMPING software stamp on the socket path; user space converts
// it to CLOCK_REALTIME so both feed the same kernel_rx_* histograms.
//
// Modes:
//   skb (generic)   works on any interface (veth, Realtek), copies every packet
//   drv (native)    needs driver XDP support (Intel igc/ixgbe/i40e/ice, mlx5, ...)
//   zerocopy        drv mode with XDP_ZEROCOPY: NIC DMA lands directly in the UMEM
//
// XdpReceiver::poll() has the same contract as net::Receiver::poll(): the payload
// stays valid until the next poll(); the frame is handed back to the kernel then.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lle/net.hpp"

#if defined(__linux__)
#include <linux/bpf.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>

#include <atomic>
#endif

namespace lle::xdp {

enum class Mode { Skb, Drv, ZeroCopy };

inline Mode parse_mode(const std::string& s) {
  if (s == "drv") return Mode::Drv;
  if (s == "zerocopy" || s == "zc") return Mode::ZeroCopy;
  return Mode::Skb;
}

inline constexpr std::size_t kEthIpUdp = 14 + 20 + 8;  // headers in front of the UDP payload

#if defined(__linux__)

// ---- tiny eBPF assembler ------------------------------------------------------
namespace asm_ {
inline bpf_insn ins(std::uint8_t code, std::uint8_t dst, std::uint8_t src, std::int16_t off, std::int32_t imm) {
  bpf_insn i{};
  i.code = code;
  i.dst_reg = dst & 0xf;
  i.src_reg = src & 0xf;
  i.off = off;
  i.imm = imm;
  return i;
}
inline bpf_insn mov_reg(int d, int s) { return ins(BPF_ALU64 | BPF_MOV | BPF_X, d, s, 0, 0); }
inline bpf_insn mov_imm(int d, int imm) { return ins(BPF_ALU64 | BPF_MOV | BPF_K, d, 0, 0, imm); }
inline bpf_insn add_imm(int d, int imm) { return ins(BPF_ALU64 | BPF_ADD | BPF_K, d, 0, 0, imm); }
inline bpf_insn ldx(int size, int d, int s, int off) { return ins(BPF_LDX | BPF_MEM | size, d, s, static_cast<std::int16_t>(off), 0); }
inline bpf_insn stx_dw(int d, int s, int off) { return ins(BPF_STX | BPF_MEM | BPF_DW, d, s, static_cast<std::int16_t>(off), 0); }
inline bpf_insn call(int helper) { return ins(BPF_JMP | BPF_CALL, 0, 0, 0, helper); }
inline bpf_insn exit_() { return ins(BPF_JMP | BPF_EXIT, 0, 0, 0, 0); }
}  // namespace asm_

// Builds the filter/redirect program. `map_fd` is the XSKMAP; -1 builds it with a
// placeholder (used by the unprivileged structure test). Jumps to "pass" are
// patched once its position is known.
inline std::vector<bpf_insn> build_program(int map_fd, std::uint16_t udp_port) {
  using namespace asm_;
  std::vector<bpf_insn> p;
  std::vector<std::size_t> to_pass;
  auto jne_pass = [&](int reg, int imm) {
    to_pass.push_back(p.size());
    p.push_back(ins(BPF_JMP | BPF_JNE | BPF_K, reg, 0, 0, imm));
  };
  auto jgt_reg_pass = [&](int a, int b) {
    to_pass.push_back(p.size());
    p.push_back(ins(BPF_JMP | BPF_JGT | BPF_X, a, b, 0, 0));
  };
  // struct xdp_md: data 0, data_end 4, data_meta 8, ingress_ifindex 12, rx_queue_index 16
  p.push_back(mov_reg(6, 1));
  p.push_back(ldx(BPF_W, 2, 6, 0));
  p.push_back(ldx(BPF_W, 3, 6, 4));
  p.push_back(mov_reg(4, 2));
  p.push_back(add_imm(4, static_cast<int>(kEthIpUdp)));
  jgt_reg_pass(4, 3);                                 // too short
  p.push_back(ldx(BPF_H, 5, 2, 12));
  jne_pass(5, 0x0008);                                // ethertype 0x0800, loaded little-endian
  p.push_back(ldx(BPF_B, 5, 2, 14));
  jne_pass(5, 0x45);                                  // IPv4, 20-byte header
  p.push_back(ldx(BPF_B, 5, 2, 23));
  jne_pass(5, 17);                                    // UDP
  p.push_back(ldx(BPF_H, 5, 2, 36));
  jne_pass(5, __builtin_bswap16(udp_port));           // destination port
  // 8 bytes of metadata in front of the packet, then the timestamp.
  p.push_back(mov_reg(1, 6));
  p.push_back(mov_imm(2, -8));
  p.push_back(call(BPF_FUNC_xdp_adjust_meta));
  jne_pass(0, 0);
  p.push_back(call(BPF_FUNC_ktime_get_ns));
  p.push_back(ldx(BPF_W, 2, 6, 8));
  p.push_back(ldx(BPF_W, 3, 6, 0));
  p.push_back(mov_reg(4, 2));
  p.push_back(add_imm(4, 8));
  jgt_reg_pass(4, 3);
  p.push_back(stx_dw(2, 0, 0));
  // bpf_redirect_map(&xsks, rx_queue_index, XDP_PASS): falls back to the stack if
  // no socket is bound to this queue.
  p.push_back(ldx(BPF_W, 2, 6, 16));
  p.push_back(ins(BPF_LD | BPF_DW | BPF_IMM, 1, BPF_PSEUDO_MAP_FD, 0, map_fd));
  p.push_back(ins(0, 0, 0, 0, 0));
  p.push_back(mov_imm(3, XDP_PASS));
  p.push_back(call(BPF_FUNC_redirect_map));
  p.push_back(exit_());
  const std::size_t pass = p.size();
  p.push_back(mov_imm(0, XDP_PASS));
  p.push_back(exit_());
  for (auto i : to_pass) p[i].off = static_cast<std::int16_t>(pass - (i + 1));
  return p;
}

inline long sys_bpf(int cmd, bpf_attr& a) { return ::syscall(__NR_bpf, cmd, &a, sizeof(a)); }

// ---- receiver -----------------------------------------------------------------
struct Config {
  std::string ifname;
  int queue = 0;
  std::uint16_t udp_port = 0;
  Mode mode = Mode::Skb;
  std::uint32_t frames = 4096;  // UMEM frames == fill ring size == rx ring size
  std::uint32_t frame_size = 2048;
};

class XdpReceiver {
 public:
  explicit XdpReceiver(const Config& c) : cfg_(c) {
    ifindex_ = static_cast<int>(::if_nametoindex(c.ifname.c_str()));
    if (!ifindex_) throw std::runtime_error("xdp: no interface " + c.ifname);
    setup_socket();
    setup_program();
    resync_clock();
  }

  // CLOCK_MONOTONIC (bpf_ktime_get_ns) -> CLOCK_REALTIME offset. REALTIME is slewed
  // and stepped by time sync (WSL steps it often), so call this from the idle path
  // a few times a second. Bracketed between two MONOTONIC reads, best of 5.
  void resync_clock() noexcept {
    auto ns = [](clockid_t c) {
      timespec t{};
      ::clock_gettime(c, &t);
      return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
    };
    std::int64_t best_gap = INT64_MAX, off = 0;
    for (int i = 0; i < 5; ++i) {
      const std::int64_t m1 = ns(CLOCK_MONOTONIC), r = ns(CLOCK_REALTIME), m2 = ns(CLOCK_MONOTONIC);
      if (m2 - m1 < best_gap) {
        best_gap = m2 - m1;
        off = r - (m1 + (m2 - m1) / 2);
      }
    }
    mono_to_real_ = off;
  }
  ~XdpReceiver() {
    if (link_fd_ >= 0) ::close(link_fd_);  // detaches the program
    if (prog_fd_ >= 0) ::close(prog_fd_);
    if (map_fd_ >= 0) ::close(map_fd_);
    if (rx_map_) ::munmap(rx_map_, rx_map_len_);
    if (fr_map_) ::munmap(fr_map_, fr_map_len_);
    if (cr_map_) ::munmap(cr_map_, cr_map_len_);
    if (fd_ >= 0) ::close(fd_);
    if (umem_) ::munmap(umem_, umem_len_);
  }
  XdpReceiver(const XdpReceiver&) = delete;
  XdpReceiver& operator=(const XdpReceiver&) = delete;

  LLE_ALWAYS_INLINE bool poll(net::Packet& out) noexcept {
    const std::uint32_t prod = __atomic_load_n(rx_prod_, __ATOMIC_ACQUIRE);
    if (rx_cons_ == prod) return false;
    recycle();
    const xdp_desc d = rx_desc_[rx_cons_ & mask_];
    __atomic_store_n(rx_consp_, ++rx_cons_, __ATOMIC_RELEASE);
    held_ = d.addr;
    held_valid_ = true;
    const unsigned char* f = umem_ + d.addr;
    if (LLE_UNLIKELY(d.len < kEthIpUdp)) return false;
    std::size_t len = load_be<std::uint16_t>(f + 38);  // UDP length (drops Ethernet padding)
    len = len >= 8 ? len - 8 : 0;
    if (len > d.len - kEthIpUdp) len = d.len - kEthIpUdp;
    std::uint64_t mono;
    std::memcpy(&mono, f - 8, sizeof(mono));
    out = net::Packet{f + kEthIpUdp, len, static_cast<std::int64_t>(mono) + mono_to_real_};
    packets_++;
    return true;
  }

  const char* mode_name() const noexcept { return mode_name_; }
  std::uint64_t packets() const noexcept { return packets_; }

 private:
  // Hands the previous frame back to the kernel. One frame in, one frame out, so
  // the fill ring (sized to the UMEM) can never overflow.
  LLE_ALWAYS_INLINE void recycle() noexcept {
    if (!held_valid_) return;
    fr_addr_[fr_prod_ & mask_] = held_ & ~static_cast<std::uint64_t>(cfg_.frame_size - 1);
    __atomic_store_n(fr_prodp_, ++fr_prod_, __ATOMIC_RELEASE);
    held_valid_ = false;
  }

  [[noreturn]] static void fail(const std::string& what) {
    throw std::runtime_error("xdp: " + what + ": " + std::strerror(errno));
  }

  void setup_socket() {
    if ((cfg_.frames & (cfg_.frames - 1)) || (cfg_.frame_size & (cfg_.frame_size - 1)))
      throw std::runtime_error("xdp: frames and frame_size must be powers of two");
    mask_ = cfg_.frames - 1;
    fd_ = ::socket(AF_XDP, SOCK_RAW, 0);
    if (fd_ < 0) fail("socket(AF_XDP) (needs CAP_NET_RAW and CONFIG_XDP_SOCKETS)");
    umem_len_ = static_cast<std::size_t>(cfg_.frames) * cfg_.frame_size;
    void* m = ::mmap(nullptr, umem_len_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (m == MAP_FAILED) fail("mmap umem");
    umem_ = static_cast<unsigned char*>(m);
    xdp_umem_reg reg{};
    reg.addr = reinterpret_cast<std::uint64_t>(umem_);
    reg.len = umem_len_;
    reg.chunk_size = cfg_.frame_size;
    reg.headroom = 0;
    if (::setsockopt(fd_, SOL_XDP, XDP_UMEM_REG, &reg, sizeof(reg)) < 0) fail("XDP_UMEM_REG");
    int n = static_cast<int>(cfg_.frames);
    if (::setsockopt(fd_, SOL_XDP, XDP_UMEM_FILL_RING, &n, sizeof(n)) < 0) fail("fill ring");
    if (::setsockopt(fd_, SOL_XDP, XDP_UMEM_COMPLETION_RING, &n, sizeof(n)) < 0) fail("completion ring");
    if (::setsockopt(fd_, SOL_XDP, XDP_RX_RING, &n, sizeof(n)) < 0) fail("rx ring");
    xdp_mmap_offsets off{};
    socklen_t ol = sizeof(off);
    if (::getsockopt(fd_, SOL_XDP, XDP_MMAP_OFFSETS, &off, &ol) < 0) fail("XDP_MMAP_OFFSETS");

    auto map_ring = [&](std::size_t len, off_t pgoff, std::size_t& out_len) {
      void* r = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd_, pgoff);
      if (r == MAP_FAILED) fail("mmap ring");
      out_len = len;
      return static_cast<unsigned char*>(r);
    };
    rx_map_ = map_ring(off.rx.desc + cfg_.frames * sizeof(xdp_desc), XDP_PGOFF_RX_RING, rx_map_len_);
    fr_map_ = map_ring(off.fr.desc + cfg_.frames * sizeof(std::uint64_t), static_cast<off_t>(XDP_UMEM_PGOFF_FILL_RING), fr_map_len_);
    cr_map_ = map_ring(off.cr.desc + cfg_.frames * sizeof(std::uint64_t), static_cast<off_t>(XDP_UMEM_PGOFF_COMPLETION_RING), cr_map_len_);
    rx_prod_ = reinterpret_cast<std::uint32_t*>(rx_map_ + off.rx.producer);
    rx_consp_ = reinterpret_cast<std::uint32_t*>(rx_map_ + off.rx.consumer);
    rx_desc_ = reinterpret_cast<xdp_desc*>(rx_map_ + off.rx.desc);
    fr_prodp_ = reinterpret_cast<std::uint32_t*>(fr_map_ + off.fr.producer);
    fr_addr_ = reinterpret_cast<std::uint64_t*>(fr_map_ + off.fr.desc);
    rx_cons_ = *rx_consp_;
    fr_prod_ = *fr_prodp_;
    // Give every frame to the kernel.
    for (std::uint32_t i = 0; i < cfg_.frames; ++i) fr_addr_[(fr_prod_ + i) & mask_] = static_cast<std::uint64_t>(i) * cfg_.frame_size;
    fr_prod_ += cfg_.frames;
    __atomic_store_n(fr_prodp_, fr_prod_, __ATOMIC_RELEASE);

    sockaddr_xdp sa{};
    sa.sxdp_family = AF_XDP;
    sa.sxdp_ifindex = static_cast<std::uint32_t>(ifindex_);
    sa.sxdp_queue_id = static_cast<std::uint32_t>(cfg_.queue);
    sa.sxdp_flags = cfg_.mode == Mode::ZeroCopy ? XDP_ZEROCOPY : XDP_COPY;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
      // Drv mode tries zero-copy first and falls back to copy.
      if (cfg_.mode == Mode::ZeroCopy) fail("bind (XDP_ZEROCOPY: driver has no zero-copy support?)");
      fail("bind");
    }
    mode_name_ = cfg_.mode == Mode::ZeroCopy ? "drv+zerocopy" : cfg_.mode == Mode::Drv ? "drv+copy" : "skb+copy";
  }

  void setup_program() {
    bpf_attr a{};
    a.map_type = BPF_MAP_TYPE_XSKMAP;
    a.key_size = 4;
    a.value_size = 4;
    a.max_entries = 64;
    map_fd_ = static_cast<int>(sys_bpf(BPF_MAP_CREATE, a));
    if (map_fd_ < 0) fail("BPF_MAP_CREATE xskmap (needs CAP_BPF/CAP_SYS_ADMIN)");

    const auto prog = build_program(map_fd_, cfg_.udp_port);
    static char log[16384];
    static const char license[] = "GPL";
    a = bpf_attr{};
    a.prog_type = BPF_PROG_TYPE_XDP;
    a.expected_attach_type = BPF_XDP;
    a.insns = reinterpret_cast<std::uint64_t>(prog.data());
    a.insn_cnt = static_cast<std::uint32_t>(prog.size());
    a.license = reinterpret_cast<std::uint64_t>(license);
    a.log_buf = reinterpret_cast<std::uint64_t>(log);
    a.log_size = sizeof(log);
    a.log_level = 1;
    prog_fd_ = static_cast<int>(sys_bpf(BPF_PROG_LOAD, a));
    if (prog_fd_ < 0) {
      std::fprintf(stderr, "xdp verifier log:\n%s\n", log);
      fail("BPF_PROG_LOAD");
    }
    std::uint32_t key = static_cast<std::uint32_t>(cfg_.queue);
    std::uint32_t val = static_cast<std::uint32_t>(fd_);
    a = bpf_attr{};
    a.map_fd = static_cast<std::uint32_t>(map_fd_);
    a.key = reinterpret_cast<std::uint64_t>(&key);
    a.value = reinterpret_cast<std::uint64_t>(&val);
    if (sys_bpf(BPF_MAP_UPDATE_ELEM, a) < 0) fail("xskmap update");

    a = bpf_attr{};
    a.link_create.prog_fd = static_cast<std::uint32_t>(prog_fd_);
    a.link_create.target_ifindex = static_cast<std::uint32_t>(ifindex_);
    a.link_create.attach_type = BPF_XDP;
    a.link_create.flags = cfg_.mode == Mode::Skb ? XDP_FLAGS_SKB_MODE : XDP_FLAGS_DRV_MODE;
    link_fd_ = static_cast<int>(sys_bpf(BPF_LINK_CREATE, a));
    if (link_fd_ < 0) fail(cfg_.mode == Mode::Skb ? "attach (generic)" : "attach (native: driver has no XDP?)");
  }

  Config cfg_;
  int ifindex_ = 0;
  int fd_ = -1, map_fd_ = -1, prog_fd_ = -1, link_fd_ = -1;
  unsigned char* umem_ = nullptr;
  std::size_t umem_len_ = 0;
  unsigned char *rx_map_ = nullptr, *fr_map_ = nullptr, *cr_map_ = nullptr;
  std::size_t rx_map_len_ = 0, fr_map_len_ = 0, cr_map_len_ = 0;
  std::uint32_t* rx_prod_ = nullptr;
  std::uint32_t* rx_consp_ = nullptr;
  xdp_desc* rx_desc_ = nullptr;
  std::uint32_t* fr_prodp_ = nullptr;
  std::uint64_t* fr_addr_ = nullptr;
  std::uint32_t rx_cons_ = 0, fr_prod_ = 0, mask_ = 0;
  std::uint64_t held_ = 0;
  bool held_valid_ = false;
  std::int64_t mono_to_real_ = 0;
  std::uint64_t packets_ = 0;
  const char* mode_name_ = "";
};

#else  // !__linux__

struct Config {
  std::string ifname;
  int queue = 0;
  std::uint16_t udp_port = 0;
  Mode mode = Mode::Skb;
  std::uint32_t frames = 4096;
  std::uint32_t frame_size = 2048;
};
class XdpReceiver {
 public:
  explicit XdpReceiver(const Config&) { throw std::runtime_error("AF_XDP is Linux only"); }
  bool poll(net::Packet&) noexcept { return false; }
  void resync_clock() noexcept {}
  const char* mode_name() const noexcept { return "none"; }
  std::uint64_t packets() const noexcept { return 0; }
};

#endif

}  // namespace lle::xdp
