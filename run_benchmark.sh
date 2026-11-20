policies=("lru" "clock" "lfu" "fifo")
max_alloc=400

for (( n=20; n<=max_alloc; n+=20 )); do
  echo "Page Allocation: $n"

  for policy in "${policies[@]}"; do
    echo "--- Policy: $policy ---"
    ./vmtrace -q -p "$policy" -n "$n" grep -i -w main ./main.cpp 
    echo ""
  done
  echo ""
done
