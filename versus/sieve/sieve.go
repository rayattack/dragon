package main

import "fmt"

func sieve(limit int) int {
	isPrime := make([]bool, limit+1)
	for i := range isPrime {
		isPrime[i] = true
	}
	isPrime[0] = false
	isPrime[1] = false
	for i := 2; i*i <= limit; i++ {
		if isPrime[i] {
			for j := i * i; j <= limit; j += i {
				isPrime[j] = false
			}
		}
	}
	count := 0
	for _, v := range isPrime {
		if v {
			count++
		}
	}
	return count
}

func main() {
	fmt.Println(sieve(1000000))
}
