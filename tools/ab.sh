#!/usr/bin/env bash
# Interleaved A/B benchmark helper. usage: ab.sh CASE N BIN_A BIN_B...
# Reports the median of N interleaved runs per binary.
export LC_ALL=C
cd "$(dirname "$0")/.."
c=$1; N=$2; shift 2
bins=("$@"); nb=${#bins[@]}
tag=$$
for ((j=0;j<nb;j++)); do : > "/tmp/ab_r$j$tag"; done
for ((i=0;i<N;i++)); do
  for ((j=0;j<nb;j++)); do
    taskset -c 6 "${bins[j]}" "$c" 42 65536 2048 | tail -1 | awk '{print $3}' >> "/tmp/ab_r$j$tag"
  done
done
printf "%-10s base=%s" "$c" "$(sort -n /tmp/ab_r0$tag | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')"
for ((j=1;j<nb;j++)); do
  m=$(sort -n "/tmp/ab_r$j$tag" | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
  b=$(sort -n "/tmp/ab_r0$tag" | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
  printf " %s=%s(%+.2f%%)" "$(basename "${bins[j]}")" "$m" "$(awk -v a="$m" -v b="$b" 'BEGIN{print (a/b-1)*100}')"
done
echo
for ((j=0;j<nb;j++)); do rm -f "/tmp/ab_r$j$tag"; done
