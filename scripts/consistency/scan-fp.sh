#!/usr/bin/env bash
# Count fused-multiply-add instructions left in a built binary.
#
# usage: scan-fp.sh <binary> <out-file>
#
# Roadmap §5.1 item 3 asks the controlled build to check "the actual compile commands, explicit
# FMA, and remaining vectorisation". build-info.sh covers the compile commands; this covers the
# other two, and the distinction matters:
#
#   -ffp-contract=off only stops the *compiler* from fusing a*b+c on its own. It does not touch
#   an explicit intrinsic. libear's per-arch objects are compiled with -mfma and call xsimd's
#   fma directly, SAF calls into Accelerate/OpenBLAS, and neither is reachable from a project
#   compile option. So a non-zero count here after config B is the concrete evidence that build
#   flags alone cannot close the gap — which is exactly what phase 0 is trying to establish.
#
# Best effort by design: if no disassembler is present the scan is skipped and the run still
# succeeds, because the baseline itself does not depend on it.
#
# Caveat recorded in the output: mnemonics are matched textually over the disassembly, so a
# symbol name containing e.g. "fmadd" would be counted. Treat the number as an indicator to
# investigate, not as an exact instruction census.

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <binary> <out-file>" >&2
    exit 2
fi

bin="$1"
out_file="$2"

if [ ! -f "$bin" ]; then
    echo "error: no such binary: $bin" >&2
    exit 2
fi

mkdir -p "$(dirname "$out_file")"

disasm=""
for candidate in llvm-objdump objdump gobjdump; do
    if command -v "$candidate" >/dev/null 2>&1; then
        disasm="$candidate"
        break
    fi
done

if [ -z "$disasm" ]; then
    {
        echo "# FMA scan"
        echo "scan.version=1"
        echo "scan.binary=$(basename "$bin")"
        echo "disasm.tool=none"
        echo "disasm.note=no llvm-objdump/objdump on this runner; scan skipped"
    } >"$out_file"
    echo "wrote ${out_file} (skipped: no disassembler)"
    exit 0
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

if ! "$disasm" -d "$bin" >"$tmp" 2>/dev/null; then
    {
        echo "# FMA scan"
        echo "scan.version=1"
        echo "scan.binary=$(basename "$bin")"
        echo "disasm.tool=${disasm}"
        echo "disasm.note=disassembly failed; scan skipped"
    } >"$out_file"
    echo "wrote ${out_file} (skipped: disassembly failed)"
    exit 0
fi

# x86: vfmadd/vfmsub/vfnmadd/vfnmsub (+addsub/subadd forms). aarch64: fmadd/fmsub/fnmadd/fnmsub
# and the vector fmla/fmls. Anchored on a word boundary so "fmla" does not match "fmlal2" only
# by prefix — the trailing class covers the real suffixes.
pattern='\b(vfn?m(add|sub)(sub|add)?[0-9a-z]*|fn?m(add|sub)[0-9a-z]*|fml[as][0-9a-z.]*)\b'

hits="$(grep -coE "$pattern" "$tmp" || true)"
lines="$(wc -l <"$tmp" | tr -d '[:space:]')"

{
    echo "# FMA scan"
    echo "scan.version=1"
    echo "scan.binary=$(basename "$bin")"
    echo "disasm.tool=${disasm}"
    echo "disasm.lines=${lines}"
    echo "fma.total=${hits}"
    echo "fma.note=textual mnemonic match over disassembly; symbol names can inflate the count"
    if [ "${hits:-0}" -gt 0 ]; then
        grep -oE "$pattern" "$tmp" | sort | uniq -c | sort -rn | head -n 20 |
            while read -r n mnemonic; do
                printf 'fma.mnemonic.%s=%s\n' "$mnemonic" "$n"
            done
    fi
} >"$out_file"

echo "wrote ${out_file} (fma.total=${hits})"
