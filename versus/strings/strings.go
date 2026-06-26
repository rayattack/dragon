package main

import "fmt"

func main() {
	s := ""
	for i := 0; i < 10000; i++ {
		s += "hello"
	}
	fmt.Println(len(s))
}
