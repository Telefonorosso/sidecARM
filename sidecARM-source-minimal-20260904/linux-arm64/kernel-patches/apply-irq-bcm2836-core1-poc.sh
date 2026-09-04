#!/bin/sh
set -eu
f=drivers/irqchip/irq-bcm2836.c
[ -f "$f" ] || { echo "Run from Linux kernel tree root" >&2; exit 1; }
cp -n "$f" "$f.before-pinux-core1-poc"
python3 - "$f" <<'PY'
import re, sys
p=sys.argv[1]
s=open(p).read()
orig=s
# Restrict replacements to the timer mask/unmask helpers and top-level IRQ handler.
s,n1=re.subn(r'(bcm2836_arm_irqchip_mask_per_cpu_irq\(LOCAL_TIMER_INT_CONTROL0,\s*\n\s*d->hwirq - LOCAL_IRQ_CNTPSIRQ,\s*\n\s*)smp_processor_id\(\)(\);)', r'\g<1>1 /* POC: physical core1 */\2', s, count=1)
s,n2=re.subn(r'(bcm2836_arm_irqchip_unmask_per_cpu_irq\(LOCAL_TIMER_INT_CONTROL0,\s*\n\s*d->hwirq - LOCAL_IRQ_CNTPSIRQ,\s*\n\s*)smp_processor_id\(\)(\);)', r'\g<1>1 /* POC: physical core1 */\2', s, count=1)
s,n3=re.subn(r'(bcm2836_arm_irqchip_handle_irq\(struct pt_regs \*regs\)\s*\n\{\s*\n\s*)int cpu = smp_processor_id\(\);', r'\g<1>int cpu = 1; /* POC: Linux logical CPU0 runs on physical core1 */', s, count=1)
if (n1,n2,n3)!=(1,1,1):
    raise SystemExit(f"Unexpected source layout; replacements mask/unmask/handler={(n1,n2,n3)}. File left untouched.")
open(p,'w').write(s)
print("Patched", p, "(3 replacements)")
PY
