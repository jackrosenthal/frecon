package main

import (
	"errors"
	"fmt"
	"os"
	"strings"
)

type setKeymapCmd struct {
	Model   string   `help:"Keyboard model, e.g. pc105."`
	Layout  string   `arg:"" optional:"" help:"Layouts, comma separated, each with an optional variant in parentheses, e.g. us, us(3l), or us,de(nodeadkeys)."`
	Variant string   `arg:"" optional:"" help:"Variants, comma separated, one per layout."`
	Options []string `arg:"" optional:"" help:"Options, e.g. grp:alt_shift_toggle."`
}

func (c *setKeymapCmd) Run() error {
	layout, variant, err := splitVariants(c.Layout, c.Variant)
	if err != nil {
		return err
	}

	var params []string
	for _, p := range []struct{ name, value string }{
		{"model", c.Model},
		{"layout", layout},
		{"variant", variant},
		{"options", strings.Join(c.Options, ",")},
	} {
		if p.value == "" {
			continue
		}
		// The escape separates parameters with ';', and frecon only
		// takes printable ASCII.
		for _, r := range p.value {
			if r == ';' || r < ' ' || r > '~' {
				return fmt.Errorf("the %s can't contain %q", p.name, r)
			}
		}
		params = append(params, p.name+"="+p.value)
	}

	tty, err := os.OpenFile("/dev/tty", os.O_WRONLY, 0)
	if err != nil {
		return fmt.Errorf("opening the terminal: %w", err)
	}
	defer tty.Close()

	if _, err := fmt.Fprintf(tty, "\x1b]keymap:%s\x07", strings.Join(params, ";")); err != nil {
		return fmt.Errorf("sending the keymap escape: %w", err)
	}
	return nil
}

// splitVariants moves variants given in parentheses, as in "us,de(nodeadkeys)",
// from the layouts to the variants, as in "us,de" and ",nodeadkeys".
func splitVariants(layout, variant string) (string, string, error) {
	if !strings.Contains(layout, "(") {
		return layout, variant, nil
	}
	if variant != "" {
		return "", "", errors.New("give variants either in parentheses or as an argument, not both")
	}

	layouts := strings.Split(layout, ",")
	variants := make([]string, len(layouts))
	for i, l := range layouts {
		open := strings.IndexByte(l, '(')
		if open < 0 {
			continue
		}
		if !strings.HasSuffix(l, ")") || open == 0 {
			return "", "", fmt.Errorf("bad layout %q, expected layout(variant)", l)
		}
		layouts[i] = l[:open]
		variants[i] = l[open+1 : len(l)-1]
	}
	return strings.Join(layouts, ","), strings.Join(variants, ","), nil
}
