// Command finalmic mixes a microphone and a capture card into one virtual
// microphone on macOS.
package main

import (
	"os"

	"github.com/zsh2401/finalmic/internal/cli"
)

func main() {
	os.Exit(cli.Main(os.Args[1:]))
}
