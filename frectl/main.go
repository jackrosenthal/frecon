// frectl controls frecon from a program running on one of its terminals.
package main

import (
	"github.com/alecthomas/kong"
	"github.com/jackrosenthal/fay"
)

type cli struct {
	SaveScreenshot saveScreenshotCmd `cmd:"" help:"Save the screenshot taken with Print Screen on this terminal as a PNG."`
	SetKeymap      setKeymapCmd      `cmd:"" help:"Set frecon's keyboard layout, like setxkbmap.  Anything omitted uses the libxkbcommon default."`
}

func main() {
	var c cli
	ctx := kong.Parse(&c,
		kong.Name("frectl"),
		kong.Description("Control frecon from a program running on its terminal."),
		fay.Register(),
	)
	ctx.FatalIfErrorf(ctx.Run())
}
