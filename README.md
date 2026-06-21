# dvtm - dynamic virtual terminal manager

`det`, a better fork  of [dvtm](https://www.brain-dump.org/projects/dvtm/), brings the concept
of tiling window management. It fixes 

1. the DV1/DV2 issue that caused latency on precesses(e.g. nvim quit). [24b4c8bfa6c8] 
2. Fixed DSR that glitches the cursor position(e.g. lazygit quit). [816eccbd4a1a]
3. A color patch. [0eeb25ba90cf]

## TODO

1. A More Customizable Status Bar

## Download

Either Download the latest [source tarball](https://github.com/martanne/dvtm/releases),
compile (you will need curses headers) and install it

    $EDITOR config.mk && $EDITOR config.def.h && make && sudo make install

or use one of the distribution provided
[binary packages](https://repology.org/project/dvtm/packages).

## Why dvtm? The philosophy behind

dvtm strives to adhere to the
[Unix philosophy](http://www.catb.org/esr/writings/taoup/html/ch01s06.html).
It tries to do one thing, *dynamic* window management on the console,
and to do it well.

As such dvtm does *not* implement [session management](#faq) but instead
delegates this task to a separate tool called
[abduco](https://www.brain-dump.org/projects/abduco/).

Similarly dvtm's copy mode is implemented by piping the scroll back buffer
content to an external editor and only storing whatever the editor writes
to `stdout`. Hence the selection process is delegated to the editor
where powerful features such as regular expression search are available.

As a result dvtm's source code is relatively small
([~4000 lines of C](https://www.ohloh.net/p/dvtm/analyses/latest/languages_summary)),
simple and therefore easy to hack on.

## Quickstart

All of `det` keybindings start with a common modifier which from now
on is refered to as `MOD`. By default `MOD` is set to `CTRL+c` however
this can be changed at runttime with the `-m` command line option.
For example setting `MOD` to `CTRL-b` is accomplished by starting
`det -m ^b`.

### Windows

New windows are created with `MOD+n` and closed with `MOD+x`.
To switch among the windows use `MOD+j` and `MOD+k` or `MOD+[1..9]`
where the digit corresponds to the window number which is displayed
in the title bar. Windows can be minimized and restored with `MOD+.`.
Input can be directed to all visible window by pressing `MOD+a`,
issuing the same key combination again restores normal behaviour
i.e. only the currently focused window will receive input.

### Layouts

Visible Windows are arranged by a layout. Each layout consists of a
master and a tile area. Typically the master area occupies the largest
part of the screen and is intended for the currently most important
window. The size of the master area can be shrunk with `MOD+h`
and enlarged with `MOD-l` respectively. Windows can be zoomed into
the master area with `MOD+Enter`. The number of windows in the
master area can be increased and decreased with `MOD+i` and `MOD+d`.

By default dvtm comes with 4 different layouts which can be cycled
through via `MOD+Space`

 * vertical stack: master area on the left half, other clients
   stacked on the right
 * bottom stack: master area on the top half, other clients stacked below
 * grid: every window gets an equally sized portion of the screen
 * fullscreen: only the selected window is shown and occupies the
   whole available display area `MOD+f`

Further layouts are included in the source tarball but disabled by
default.

### Tagging

Each window has a non empty set of tags [1..n] associated with it. A view
consists of a number of tags. The current view includes all windows
which are tagged with the currently active tags. The following key
bindings are used to manipulate the tagsets.

- `MOD-0`  view all windows with any tag
- `Mod-v-Tab` toggles to the previously selected tags
- `MOD-v-[1..n]` view all windows with nth tag
- `Mod-V-[1..n]` add/remove all windows with nth tag to/from the view
- `Mod-t-[1..n]` apply nth tag to focused window
- `Mod-T-[1..n]` add/remove nth tag to/from focused window

### Statusbar

dvtm can be instructed to read and display status messages from a named
pipe. As an example the
[`det-status` script](https://raw.githubusercontent.com/martanne/dvtm/master/dvtm-status)
is provided which shows the current time.

### Copymode ###

`MOD+e` pipes the whole scroll buffer content to an external editor.
What ever the editor writes to `stdout` is remembered by det and can
later be pasted with `MOD+p`.

In order for this to work the editor needs to be usable as a filter
and should use `stderr` for its user interface. Examples where this is
the case include `sandy(1)` and [vis](https://www.brain-dump.org/projects/vis).

    $ echo Hello World | vis - | cat

## Patches

There exist a number of out of tree patches which customize dvtm's
behaviour:

 - [pertag](http://waxandwane.org/dvtm.html) (see also the corresponding
   [mailing list post](https://lists.suckless.org/hackers/1510/8186.html))

## FAQ

### How to change the key bindings?

The configuration of det is done by creating a custom `config.h`
and (re)compiling the source code.
You basically define a set of layouts and keys which dvtm will use.
There are some pre defined macros to ease configuration.

### WARNING: terminal is not fully functional

This means you haven't installed the `det.info` terminfo description
which can be done with `tic -s det.info`. If for some reason you
can't install new terminfo descriptions set the `det_TERM` environment
variable to a known terminal when starting `det` as in

    $ det_TERM=rxvt det

This will instruct det to use rxvt as `$TERM` value within its windows.

### How to set the window title?

The window title can be changed by means of a
[xterm extension](https://tldp.org/HOWTO/Xterm-Title-3.html#ss3.2)
terminal escape sequence

    $ echo -ne "\033]0;Your title here\007"

So for example in `bash` if you want to display the current working
directory in the window title this can be accomplished by means of
the following section in your startup files.

    # If this is an xterm set the title to user@host:dir
    case "$TERM" in
    det*|xterm*|rxvt*)
        PROMPT_COMMAND='echo -ne "\033]0;${USER}@${HOSTNAME}: ${PWD/$HOME/~}\007"'
        ;;
    *)
        ;;
    esac

Other shells provide similar functionality, zsh as an example has a
[precmd function](http://zsh.sourceforge.net/Doc/Release/Functions.html#Hook-Functions)
which can be used to achieve the same effect.

### Something is wrong with the displayed colors

Make sure you have set `$TERM` correctly for example if you want to
use 256 color profiles you probably have to append `-256color` to
your regular terminal name. Also due to limitations of ncurses by
default you can only use 255 color pairs simultaneously. If you
need more than 255 different color pairs at the same time, then you
have to rebuild ncurses with

    $ ./configure ... --enable-ext-colors

Note that this changes the ABI and therefore sets SONAME of the
library to 6 (i.e. you have to link against `libncursesw.so.6`).

### Some characters are displayed like garbage

Make sure you compiled det against a unicode aware curses library
(in case of ncurses this would be `libncursesw`). Also make sure
that your locale settings contain UTF-8.

### The numeric keypad does not work with Putty

Disable [application keypad mode](https://the.earth.li/~sgtatham/putty/0.64/htmldoc/Chapter4.html#config-features-application)
in the Putty configuration under `Terminal => Features => Disable application keypad mode`.

### Unicode characters do not work within Putty

You have to tell Putty in which
[character encoding](https://the.earth.li/~sgtatham/putty/0.64/htmldoc/Chapter4.html#config-translation)
the received data is. Set the dropdown box under `Window => Translation`
to UTF-8. In order to get proper line drawing characters you proabably
also want to set the TERM environment variable to `putty` or `putty-256color`.
If that still doesn't do the trick then try running det with the
following ncurses related environment variable set `NCURSES_NO_UTF8_ACS=1`.

## License

dvtm reuses some code of dwm and is released under the same
[MIT/X11 license](https://raw.githubusercontent.com/martanne/dvtm/master/LICENSE).
The terminal emulation part is licensed under the ISC license.
