package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"strconv"
	"time"

	"golang.org/x/term"
)

const (
	// frecon replies with replyPrefix, the length of the PNG in decimal,
	// ';' and the PNG. A length of 0 means there is no screenshot.
	screenshotQuery = "\x1b]screenshot\x07"
	replyPrefix     = "\x1b]screenshot:"

	// How long to wait for each read of the reply.
	replyTimeout = 5 * time.Second
)

type saveScreenshotCmd struct {
	Output string `arg:"" type:"path" help:"PNG file to write."`
}

func (c *saveScreenshotCmd) Run() error {
	tty, err := os.OpenFile("/dev/tty", os.O_RDWR, 0)
	if err != nil {
		return fmt.Errorf("opening the terminal: %w", err)
	}
	defer tty.Close()

	// The reply is binary, so the terminal must pass it through unchanged
	// and not echo it.
	restore, err := makeRaw(tty)
	if err != nil {
		return err
	}
	defer restore()

	if _, err := tty.WriteString(screenshotQuery); err != nil {
		return fmt.Errorf("sending the screenshot escape: %w", err)
	}

	r := deadlineReader{tty}
	n, err := readReplyHeader(r)
	if errors.Is(err, os.ErrDeadlineExceeded) {
		return errors.New("no reply, is this a frecon terminal with --enable-osc?")
	} else if err != nil {
		return fmt.Errorf("reading the reply: %w", err)
	}
	if n == 0 {
		return errors.New("no screenshot on this terminal, press Print Screen first")
	}

	// Read all of it before writing, so none is left in the terminal's
	// input if the write fails.
	png, err := io.ReadAll(io.LimitReader(r, n))
	if err == nil && int64(len(png)) < n {
		err = io.ErrUnexpectedEOF
	}
	if err != nil {
		return fmt.Errorf("reading the screenshot: %w", err)
	}

	return os.WriteFile(c.Output, png, 0o666)
}

// makeRaw puts tty in raw mode and returns a function to restore it.
func makeRaw(tty *os.File) (func(), error) {
	// tty.Fd() would make tty blocking, which disables read deadlines.
	conn, err := tty.SyscallConn()
	if err != nil {
		return nil, err
	}

	var state *term.State
	var rawErr error
	if err := conn.Control(func(fd uintptr) {
		state, rawErr = term.MakeRaw(int(fd))
	}); err != nil {
		return nil, err
	}
	if rawErr != nil {
		return nil, fmt.Errorf("putting the terminal in raw mode: %w", rawErr)
	}

	return func() {
		_ = conn.Control(func(fd uintptr) {
			_ = term.Restore(int(fd), state)
		})
	}, nil
}

// readReplyHeader reads up to the ';' after the length, a byte at a time so
// that keys typed after the reply are left for the next program. Keys typed
// before the reply are skipped.
func readReplyHeader(r io.Reader) (int64, error) {
	var b [1]byte
	var digits []byte

	for matched := 0; matched < len(replyPrefix); {
		if _, err := io.ReadFull(r, b[:]); err != nil {
			return 0, err
		}
		if b[0] == replyPrefix[matched] {
			matched++
		} else if b[0] == replyPrefix[0] {
			matched = 1
		} else {
			matched = 0
		}
	}

	for {
		if _, err := io.ReadFull(r, b[:]); err != nil {
			return 0, err
		}
		if b[0] == ';' {
			break
		}
		if b[0] < '0' || b[0] > '9' || len(digits) == 19 {
			return 0, fmt.Errorf("bad length in reply: %q", append(digits, b[0]))
		}
		digits = append(digits, b[0])
	}

	return strconv.ParseInt(string(digits), 10, 64)
}

// deadlineReader times out each read after replyTimeout.
type deadlineReader struct {
	f *os.File
}

func (d deadlineReader) Read(p []byte) (int, error) {
	if err := d.f.SetReadDeadline(time.Now().Add(replyTimeout)); err != nil {
		return 0, err
	}
	return d.f.Read(p)
}
