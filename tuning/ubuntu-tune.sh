#!/usr/bin/env bash
# OS tuning for the clean latency runs (optimization ladder step 2), for the GPU PC's
# Ubuntu boot (i7-14700F: 8 P-cores with SMT + 12 E-cores).
#
#   sudo tuning/ubuntu-tune.sh plan            # print what would change (default, read-only)
#   sudo tuning/ubuntu-tune.sh grub            # write the kernel cmdline (needs a reboot)
#   sudo tuning/ubuntu-tune.sh runtime         # apply the runtime settings (until reboot)
#   sudo tuning/ubuntu-tune.sh status          # show the current state
#
# Layout chosen (override with HOT_CORE / HOUSE_CPUS):
#   - hot thread: one P-core, SMT sibling taken offline
#   - telemetry, paper exchange and replayer: other P-cores (never the hot core)
#   - everything else (IRQs, kernel threads, desktop): E-cores
# Each setting is measured as its own ladder step by tuning/run-ladder.sh.
set -euo pipefail
MODE=${1:-plan}

p_cores() { cat /sys/devices/cpu_core/cpus 2>/dev/null || echo "0-$(($(nproc) - 1))"; }
e_cores() { cat /sys/devices/cpu_atom/cpus 2>/dev/null || true; }
siblings() { cat "/sys/devices/system/cpu/cpu$1/topology/thread_siblings_list"; }
expand() { python3 -c "import sys
out=[]
for part in sys.argv[1].split(','):
    if not part: continue
    a,_,b=part.partition('-'); out+=range(int(a), int(b or a)+1)
print(' '.join(map(str,out)))" "$1"; }

P=$(p_cores); E=$(e_cores)
HOT_CORE=${HOT_CORE:-$(expand "$P" | awk '{print $5}')}   # 3rd physical P-core (cpu 4 on a 14700F)
HOT_SIB=$(siblings "$HOT_CORE" | tr ',' '\n' | grep -v "^$HOT_CORE$" | head -1 || true)
ISOLATED="$HOT_CORE${HOT_SIB:+,$HOT_SIB}"
HOUSE_CPUS=${HOUSE_CPUS:-${E:-0-1}}
NIC=${NIC:-$(ip -o route get 1.1.1.1 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p')}

CMDLINE="isolcpus=nohz,domain,managed_irq,$ISOLATED nohz_full=$ISOLATED rcu_nocbs=$ISOLATED irqaffinity=$HOUSE_CPUS \
intel_pstate=active processor.max_cstate=1 intel_idle.max_cstate=0 nosoftlockup tsc=reliable \
transparent_hugepage=madvise hugepagesz=2M hugepages=512 skew_tick=1"

plan() {
  echo "P-cores: $P   E-cores: ${E:-none}"
  echo "hot core: cpu$HOT_CORE   SMT sibling to take offline: ${HOT_SIB:-none}"
  echo "housekeeping (IRQs, kthreads): $HOUSE_CPUS   NIC: ${NIC:-?}"
  echo "kernel cmdline additions:"
  echo "  $CMDLINE"
  echo "runtime: performance governor, sibling offline, irqbalance stopped, IRQs -> housekeeping,"
  echo "         NIC queue IRQ -> cpu$HOT_CORE's neighbour, busy_poll/busy_read=50, swap off, mlock unlimited"
}

grub() {
  local f=/etc/default/grub.d/90-low-latency.cfg
  cp -n /etc/default/grub "/etc/default/grub.bak-$(date +%Y%m%d%H%M%S)" || true
  printf 'GRUB_CMDLINE_LINUX_DEFAULT="$GRUB_CMDLINE_LINUX_DEFAULT %s"\n' "$CMDLINE" > "$f"
  update-grub
  echo "wrote $f; reboot to apply. Remove the file and run update-grub to undo."
}

runtime() {
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g"; done
  [ -n "$HOT_SIB" ] && echo 0 > "/sys/devices/system/cpu/cpu$HOT_SIB/online"
  systemctl stop irqbalance 2>/dev/null || true
  local mask
  mask=$(python3 -c "import sys;print(format(sum(1<<c for c in map(int,sys.argv[1:])),'x'))" $(expand "$HOUSE_CPUS"))
  for irq in /proc/irq/[0-9]*; do echo "$mask" > "$irq/smp_affinity" 2>/dev/null || true; done
  sysctl -qw net.core.busy_poll=50 net.core.busy_read=50 net.core.rmem_max=16777216 net.core.wmem_max=16777216
  sysctl -qw kernel.timer_migration=0 vm.stat_interval=120 kernel.numa_balancing=0 2>/dev/null || true
  swapoff -a || true
  echo "runtime tuning applied (until reboot)"
}

status() {
  echo "cmdline: $(cat /proc/cmdline)"
  echo "isolated: $(cat /sys/devices/system/cpu/isolated)   nohz_full: $(cat /sys/devices/system/cpu/nohz_full 2>/dev/null)"
  echo "governor(cpu$HOT_CORE): $(cat /sys/devices/system/cpu/cpu$HOT_CORE/cpufreq/scaling_governor 2>/dev/null)"
  [ -n "$HOT_SIB" ] && echo "sibling cpu$HOT_SIB online: $(cat /sys/devices/system/cpu/cpu$HOT_SIB/online)"
  echo "busy_poll: $(sysctl -n net.core.busy_poll) busy_read: $(sysctl -n net.core.busy_read)"
  echo "hugepages: $(grep HugePages_Total /proc/meminfo)"
}

case "$MODE" in plan) plan ;; grub) grub ;; runtime) runtime ;; status) status ;; *) echo "unknown mode $MODE"; exit 2 ;; esac
