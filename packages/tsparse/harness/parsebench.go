// parsebench - single-file parse benchmark over typescript-go's real parser,
// mirroring the setup of internal/parser/parser_test.go BenchmarkParse.
package main

import (
	"fmt"
	"os"
	"strconv"
	"time"

	"github.com/microsoft/typescript-go/internal/ast"
	"github.com/microsoft/typescript-go/internal/core"
	"github.com/microsoft/typescript-go/internal/parser"
	"github.com/microsoft/typescript-go/internal/tspath"
	"github.com/microsoft/typescript-go/internal/vfs/osvfs"
)

func main() {
	file := os.Args[1]
	iters := 20
	if len(os.Args) > 2 {
		iters, _ = strconv.Atoi(os.Args[2])
	}
	data, err := os.ReadFile(file)
	if err != nil {
		panic(err)
	}
	sourceText := string(data)
	fileName := tspath.GetNormalizedAbsolutePath(file, "/")
	path := tspath.ToPath(fileName, "/", osvfs.FS().UseCaseSensitiveFileNames())
	scriptKind := core.GetScriptKindFromFileName(fileName)
	opts := ast.SourceFileParseOptions{FileName: fileName, Path: path}

	best := time.Duration(1 << 62)
	total := time.Duration(0)
	stmts := 0
	for i := 0; i < iters; i++ {
		t0 := time.Now()
		sf := parser.ParseSourceFile(opts, sourceText, scriptKind)
		dt := time.Since(t0)
		if dt < best {
			best = dt
		}
		total += dt
		stmts = len(sf.Statements.Nodes)
	}
	mb := float64(len(data)) / (1024 * 1024)
	fmt.Printf("statements: %d\n", stmts)
	fmt.Printf("best: %.3f ms  avg: %.3f ms\n",
		float64(best.Nanoseconds())/1e6,
		float64(total.Nanoseconds())/1e6/float64(iters))
	fmt.Printf("throughput: %.1f MB/s\n", mb/best.Seconds())
}
