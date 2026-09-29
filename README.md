# frecon: the Freon Console

This is a terminal emulator that replaces the kernel Virtual Terminal (VT)
console.  It uses the Kernel Mode Setting (KMS) support in the kernel.  It
is similar to the kmscon project:
	https://www.freedesktop.org/wiki/Software/kmscon

A new project was created (rather than re-using kmscon) because:
* We show a boot splash animation during early boot.
* Dynamic VT initialization.
* Integration with CrOS dev mode checks.
* A D-Bus IPC protocol for Chrome to control startup/shutdown.
* VT switching negotiation with Chrome.
* We don't need all the seat logic that kmscon includes.

However, it's not as bad as it sounds!  We haven't re-implemented our own
terminal emulator -- we use the same library that kmscon does:
[libtsm](https://www.freedesktop.org/wiki/Software/libtsm/).  That's the
much more tricky part too.

For even more details, check out the [design doc](./DESIGN-DOC.md) in this
repo.

## Command line options

* `--clear=color`
	Specify clear color for splash screen terminal. The color is 32-bit
integer in a framebuffer format (ARGB) in any format supported by strtoul.
* `--daemon`
	Daemonize frecon.
* `--enable-gfx`
	Enable image and box drawing OSC escape codes.
* `--enable-mouse`
	Enable the mouse on all terminals.  See [Mouse](#mouse).
* `--enable-vts`
	Enable additional terminals in addition to splash screen.
* `--enable-vt1`
	Enable switching to VT1 (aka splash screen) and keep a terminal on it
after finishing splash animation.
* `--frame-interval=N`
	Specify default time (in milliseconds) between frames of splash screen
animation.
* `--login-cmd=/path/to/program`
	Run this program in interactive terminals instead of `agetty`, for
example a login manager.  It is run without arguments, and is restarted when
it exits.
* `--loop-start=N`
	Specify frame to start splash animation loop. This option also enables
the animation loop.
* `--loop-count=N`
	Specify number of splash animation loop repetitions.  Default of 0
repeats forever.  Looping has to be enabled using `--loop-start`.
* `--loop-interval=N`
	Specify default time (in milliseconds) between frames of loop animation.
* `--loop-offset=x,y`
	Specify default offset to centered image in loop.
* `--no-login`
	When additional terminals are enabled do not display login prompt on
them. Can be used by scripts to display debugging information and logs on
additional terminals.
* `--num-vts=N`
	Specify number of enabled VTs. The default is 4, the maximum is 12.
* `--offset=x,y`
	Specify absolute location of the splash image on screen.
* `--pre-create-vts`
	Normally VTs are create on demand the the user switches to a VT.
In some cases it may be necessary to pre-create them at startup, for instance
to write a log or debug output for them while they are not active so it can be
examined later. This option allows for that. This option also ensures daemon
parent waits for daemon child to finish initalization so consoles are created
by the time daemon parent exits.
* `--print-resolution`
	Print detected screen resolution and exit. Deprecated.
* `--scale=N`
	Set default scale for splash screen images. The scale is a positive
integer number. Default scale is 1. 0 has a special meaning - using scale 1
for screens with horizontal resolution lower and equal than 1920 and 2
otherwise.  Scale affects image/box size and offset.
* `--splash-only`
	Exit immediately after finishing splash animation. Otherwise frecon
will wait for DBUS signal (LoginScreenVisible) from Chrome before exiting
when extra terminals are not enabled.
* `--image=/path/to/image.png`
* `--image-hires=/path/to/image.png`
or any image file name specified after options
	Add image to splash screen animation. `--image` and `--image-hires` are
added conditionally depending whether horizontal or vertical screen resolution
is above 1920. This allows frecon to make runtime decision which set of images
to use instead running frecon first with `--print-resolution` option and making
this decision in a script that invokes frecon.
Free form image file name in the command line are added unconditionally.
* `--wait-drop-master`
    Wait to call drmDropMaster until prompted by the caller with the escape
code: `drmdropmaster:`.

## Imaging escape codes

Frecon implements rudimentary functionality to display images and draw
single color boxes on the terminal screen using OSC (Operating System Command)
based terminal escape codes.

The OSC sequences always start with ESC+] (`\033]`) and end with either the
String Terminator (ST) sequence (`\033\\`) or a BEL (`\a`) character.

Two escapes are implemented, all escape parameters can be specified in any
order.

`image:file=/full/path/to/file.png;location=x,y;offset=x,y;scale=s`

`box:size=w,h;color=c;location=x,y;offset=x,y;scale=s`

* `location` is the absolute location on screen.
* `offset` is an image offset starting with image centered on screen.
* `color` is a 32-bit number in the same format as `--clear` command line
  argument, it defaults to `0`.
* `size` is two integer numbers.
* `scale` is integer scaling factor applied to image size or box size.

Examples:
```sh
printf "\033]image:file=/usr/share/chromeos-assets/images_100_percent/boot_splash_frame18.png\a" > /dev/pts/1
printf "\033]box:color=0xFFFFFFFF;size=100,100\a" > /dev/pts/1
```

A box is drawn on top of the text written before it.  Text redrawn later
paints over the box in the cells it changes.  To place boxes on the text, the
terminal's size in pixels is in the `ws_xpixel` and `ws_ypixel` fields of its
window size, so a cell is `ws_xpixel / ws_col` pixels wide and
`ws_ypixel / ws_row` pixels high.

## Input escape code

An escape code can be used to enable/disable keyboard input processing on
a terminal. The setting is stored per terminal and applies to input "within"
terminal. Switching between terminals, scrolling, backlight control etc remains
operational.

`input:onoff`

* where `onoff` is one of: on,1,true or off,0,false

Examples:
```sh
printf "\033]input:on\a" > /run/frecon/vt0
printf "\033]input:off\a" > /run/frecon/vt1
```

## Mouse

With the mouse enabled, frecon shows a pointer when the mouse is used, and
hides it after 5 seconds without mouse activity.  Mice, touchpads,
touchscreens, and tablets are supported:

* Left button: drag to select text, double click to select a word, and triple
  click to select a line.  The selection is copied when the button is released.
* Middle button: paste the last selection.  It is shared by all terminals.
* Right button: extend the selection to the pointer.
* Wheel, or two fingers on a touchpad: scroll the scrollback.  On the alternate
  screen, used by full screen programs such as `less`, the Up and Down arrow
  keys are sent instead.

When the program in the terminal requests mouse events (xterm mouse tracking),
they are sent to the program instead.  Hold Shift to select, paste, and scroll
anyway.

The mouse is enabled on all terminals with `--enable-mouse`, or on one
terminal with an escape code.  The mouse escape code does not require
`--enable-osc`.

`mouse:onoff`

* where `onoff` is one of: on,1,true or off,0,false

Examples:
```sh
printf "\033]mouse:on\a"
printf "\033]mouse:off\a" > /run/frecon/vt1
```

## SwitchVT escape code

An escape code that can be used to switch currently active terminal.
`switchvt:termnum`

* where termnum is an integer from 0 to num-vts-1

Example:
```sh
printf "\033]switchvt:1\a" > /run/frecon/current
```

## Keymap escape code

An escape code that can be used to change the xkb keyboard layout.  The
keymap is shared by all terminals of a frecon process.  Parameters can be
specified in any order, and parameters that are omitted or empty use the
libxkbcommon defaults.  If the keymap cannot be compiled, the current one is
kept.

`keymap:model=m;layout=l;variant=v;options=o`

Example:
```sh
printf "\033]keymap:layout=us,de;variant=,nodeadkeys;options=grp:alt_shift_toggle\a" > /run/frecon/current
```

From a frecon terminal, `frectl` takes the layout, variant, and options like
`setxkbmap`:

```sh
frectl set-keymap "us,de(nodeadkeys)" "" grp:alt_shift_toggle
```

## Screenshots

With `--enable-osc`, Print Screen captures the active terminal's screen as it
is shown, including the text cursor, the selection, and the mouse pointer, as a
PNG.  The capture replaces any earlier one on that terminal, and is kept until
a program on the same terminal reads it with the escape code:

`screenshot`

frecon writes back `\033]screenshot:<len>;` and `<len>` bytes of PNG to the
terminal's input, with a `<len>` of 0 if there is no capture.  The PNG is
binary, so the terminal must be in raw mode to read it.  The capture is
discarded once it is read.

`frectl` does this:

```sh
frectl save-screenshot screenshot.png
```

## Files

Frecon creates the following files and links in `/run/frecon` directory:

- `/run/frecon/pid` which contains pid for frecon daemon process (only when
  `--daemon` is specified on command line).

- for every VT it creates, a link from `/run/frecon/vt%u` to `/dev/pts/X`
  where `%u` is terminal number from 0 to num-vts - 1 so the user can determine
  which VT uses which pts since pts number assignment is not deterministic.

- /run/frecon/current is a symlink to currently active terminal
  (/run/frecon/vtX). It can be used to discover which terminal is currently
  active or to write text to currently active terminal.


## Example Usage

For the sake of an example let's assume you wish to switch between vt1 -> vt0(normal screen) -> vt1
we can do so using the following steps:

1. start off by linking /run/frecon/current to /run/frecon/vt1
2. use exit code to actually switch to vt1 i.e. 'printf "\\033]switchvt:1\\a" > /run/frecon/current'
3. switch to vt0 now by using the following command  'printf "\\033]switchvt:0\\a" > /run/frecon/current'
4. repeat steps 1 and 2 since switching to vt0 breaks the link

## DRM Drop Master

An escape code can be used to tell frecon to drop DRM master. This is useful
when `frecon` is invoked with `--wait-drop-master`.

Example:
```sh
printf "\033]drmdropmaster\a" > /run/frecon/current
```
