package main

import (
	"fmt"
	"strconv"
)

func main() {
	var n, k int64 = 3000000, 200000
	counts := make(map[string]int64)
	for i := int64(0); i < n; i++ {
		counts[strconv.FormatInt(i%k, 10)]++
	}
	var total int64 = 0
	for j := int64(0); j < k; j++ {
		total += j * counts[strconv.FormatInt(j, 10)]
	}
	fmt.Println(total)
}
