package main

import (
	"fmt"
	"sync"
)

const WORKERS = 8
const CHUNK = 30000000
const M = 1000000007

func work(lo, hi int64) int64 {
	var acc int64 = 0
	for i := lo; i < hi; i++ {
		acc = (acc + i) % M
	}
	return acc
}

func main() {
	res := make([]int64, WORKERS)
	var wg sync.WaitGroup
	for w := 0; w < WORKERS; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			res[w] = work(int64(w)*CHUNK, int64(w)*CHUNK+CHUNK)
		}(w)
	}
	wg.Wait()
	var total int64 = 0
	for w := 0; w < WORKERS; w++ {
		total += res[w]
	}
	fmt.Println(total)
}
